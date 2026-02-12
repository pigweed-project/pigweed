// Copyright 2026 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_bluetooth_proxy/hci/sniff_offload_manager.h"

#include "lib/stdcompat/utility.h"
#include "pw_assert/check.h"
#include "pw_async2/channel.h"
#include "pw_async2/future.h"
#include "pw_async2/future_timeout.h"
#include "pw_async2/try.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/hci_android.emb.h"
#include "pw_bluetooth/hci_commands.emb.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/hci_events.emb.h"
#include "pw_log/log.h"
#include "pw_multibuf/v2/multibuf.h"
#include "pw_span/span.h"
#include "pw_status/try.h"
#include "pw_sync/lock_annotations.h"

namespace pw::bluetooth::proxy::hci {
namespace {

constexpr SniffOffloadManager::HandlerAction kIgnorePacket{
    .intercept = SniffOffloadManager::HandlerAction::Interception::kPassthrough,
    .resume = SniffOffloadManager::HandlerAction::Resumption::kStop,
};

constexpr SniffOffloadManager::HandlerAction kBlockPacket{
    .intercept = SniffOffloadManager::HandlerAction::Interception::kIntercept,
    .resume = SniffOffloadManager::HandlerAction::Resumption::kResume,
};

constexpr auto kWriteSniffOffloadEnableOpcode =
    cpp23::to_underlying(emboss::OpCode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE);
constexpr auto kWriteSniffOffloadParametersOpcode = cpp23::to_underlying(
    emboss::OpCode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS);
constexpr auto kSniffModeOpcode =
    cpp23::to_underlying(emboss::OpCode::SNIFF_MODE);
constexpr auto kExitSniffModeOpcode =
    cpp23::to_underlying(emboss::OpCode::EXIT_SNIFF_MODE);
constexpr auto kSniffSubratingOpcode =
    cpp23::to_underlying(emboss::OpCode::SNIFF_SUBRATING);

constexpr auto kModeChangeEventCode =
    cpp23::to_underlying(emboss::EventCode::MODE_CHANGE);
constexpr auto kConnectionCompleteEventCode =
    cpp23::to_underlying(emboss::EventCode::CONNECTION_COMPLETE);
constexpr auto kDisconnectionCompleteEventCode =
    cpp23::to_underlying(emboss::EventCode::DISCONNECTION_COMPLETE);
constexpr auto kSniffSubratingEventCode =
    cpp23::to_underlying(emboss::EventCode::SNIFF_SUBRATING);

namespace WriteSniffOffloadEnableCommand =
    vendor::android_hci::WriteSniffOffloadEnableCommand;
namespace WriteSniffOffloadParametersCommand =
    vendor::android_hci::WriteSniffOffloadParametersCommand;
namespace ConnectionCompleteEvent = emboss::ConnectionCompleteEvent;
namespace DisconnectionCompleteEvent = emboss::DisconnectionCompleteEvent;
namespace CommandCompleteEvent = emboss::CommandCompleteEvent;
namespace CommandStatusEvent = emboss::CommandStatusEvent;
namespace EventHeader = emboss::EventHeader;

using emboss::CommandCompleteEventWriter;
using emboss::CommandStatusEventWriter;
using emboss::ConnectionCompleteEventView;
using emboss::DisconnectionCompleteEventView;
using vendor::android_hci::WriteSniffOffloadEnableCommandView;
using vendor::android_hci::WriteSniffOffloadParametersCommandView;

template <size_t kSize>
using Scratch = std::array<std::byte, kSize>;

// State that a connection can be in under the sniff offload spec.
enum class ConnectionState : bool {
  kPendingParameters,
  kControlStarted,
};

// Sniff modes and transitional states between them.
enum class ConnectionMode : uint8_t {
  kActive,
  kTransitionActiveToSniff,
  kSniff,
  kTransitionSniffToActive,
};

}  // namespace

bool SniffOffloadManager::HandlerAction::operator==(
    const HandlerAction& other) const {
  return intercept == other.intercept && resume == other.resume;
}

class SniffOffloadManager::ConnectionFsm final : public ConnectionMap::Item {
 public:
  ConnectionFsm(SniffOffloadManager& manager, ConnectionHandle handle)
      : manager_(manager), handle_(handle) {}

  // External FSM input events.
  using EnableInput = SniffOffloadManager::Enabled;
  using DisableInput = SniffOffloadManager::Disabled;
  struct SniffOffloadParametersInput final {
    uint16_t sniff_max_interval;
    uint16_t sniff_min_interval;
    uint16_t sniff_attempts;
    uint16_t sniff_timeout;
    uint16_t link_inactivity_timeout;
    uint16_t subrating_max_latency;
    uint16_t subrating_min_remote_timeout;
    uint16_t subrating_min_local_timeout;
    bool allow_exit_sniff_on_rx;
    bool allow_exit_sniff_on_tx;
  };
  struct ModeChangeInput final {
    emboss::AclConnectionMode current_mode;
  };
  struct AclActivity final {
    Direction direction;
  };

  using Input = std::variant<EnableInput,
                             DisableInput,
                             SniffOffloadParametersInput,
                             ModeChangeInput,
                             AclActivity>;
  // End external FSM input events.

  void Start();
  void Stop();
  void OnInput(Input&& input) PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);
  void AssertLockHeld(SniffOffloadManager& caller) const
      PW_ASSERT_EXCLUSIVE_LOCK(manager_.mutex_)
          PW_EXCLUSIVE_LOCKS_REQUIRED(caller.mutex_) {
    // Should be impossible to ever be unsatisfied, but assert to satisfy the
    // thread safety analysis.
    PW_DCHECK(&caller == &manager_);
  }

  ConnectionHandle handle() const { return handle_; }
  uint16_t key() const { return cpp23::to_underlying(handle_); }

 private:
  void HandleInput(EnableInput&& input)
      PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);
  void HandleInput(DisableInput&& input)
      PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);
  void HandleInput(SniffOffloadParametersInput&& input)
      PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);
  void HandleInput(ModeChangeInput&& input)
      PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);
  void HandleInput(AclActivity&& input)
      PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);

  void HandleTimeout() PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);

  void ResetTimer() PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);

  void SendEnterSniffMode() PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);
  void SendSniffSubrating() PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);
  void SendExitSniffMode() PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);

  bool ShouldExitSniff(Direction direction)
      PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_);

  ConnectionState connection_state() const
      PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_) {
    return ConnectionState{parameters_.has_value()};
  }
  bool should_control() PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_) {
    return enabled_ && connection_state() == ConnectionState::kControlStarted;
  }

  class TimeoutTask final : public async2::Task {
   public:
    TimeoutTask(SniffOffloadManager::ConnectionFsm& fsm)
        : Task(PW_ASYNC_TASK_NAME("SniffOffloadManager::Connection::Timeout")),
          fsm_(fsm) {}

    async2::Poll<> DoPend(async2::Context&) override;

    // Wait for the specified time, overriding any previous wait.
    void WaitFor(chrono::SystemClock::duration duration)
        PW_EXCLUSIVE_LOCKS_REQUIRED(fsm_.manager_.mutex_) {
      timeout_future_ = fsm_.manager_.time_provider_.WaitFor(duration);
    }
    void AssertLockHeld(ConnectionFsm& caller) const
        PW_ASSERT_EXCLUSIVE_LOCK(fsm_.manager_.mutex_)
            PW_EXCLUSIVE_LOCKS_REQUIRED(caller.manager_.mutex_) {
      // Should be impossible to ever be unsatisfied, but assert to satisfy the
      // thread safety analysis.
      PW_DCHECK(&caller == &fsm_);
    }

   private:
    SniffOffloadManager::ConnectionFsm& fsm_;
    async2::TimeFuture<chrono::SystemClock> timeout_future_
        PW_GUARDED_BY(fsm_.manager_.mutex_);
  };

  using SniffOffloadParameters = SniffOffloadParametersInput;

  SniffOffloadManager& manager_;
  const ConnectionHandle handle_;

  std::optional<SniffOffloadParameters> parameters_
      PW_GUARDED_BY(manager_.mutex_);

  bool enabled_ PW_GUARDED_BY(manager_.mutex_) = false;
  ConnectionMode connection_mode_ PW_GUARDED_BY(manager_.mutex_) =
      ConnectionMode::kActive;

  TimeoutTask timeout_task_{*this};
};

SniffOffloadManager::SniffOffloadManager(
    allocator::Allocator& allocator,
    async2::Dispatcher& dispatcher,
    SendCommandFunc&& send_command,
    SendEventFunc&& send_event,
    OnErrorFunc&& on_error,
    async2::TimeProvider<chrono::SystemClock>& time_provider)
    : allocator_(allocator),
      dispatcher_(dispatcher),
      time_provider_(time_provider),
      send_command_(std::move(send_command)),
      send_event_(std::move(send_event)),
      on_error_(std::move(on_error)) {}

SniffOffloadManager::~SniffOffloadManager() {
  for (auto iter = connections_.begin(); iter != connections_.end();) {
    iter->Stop();
    auto* fsm = &*iter;
    iter = connections_.erase(iter);
    allocator_.Delete(fsm);
  }
}

void SniffOffloadManager::ProcessAclPacket(const MultiBuf& acl_packet,
                                           Direction direction) {
  Scratch<emboss::AclDataFrameHeader::IntrinsicSizeInBytes()> scratch;
  auto span = acl_packet.Get(scratch);
  auto view = MakeEmbossView<emboss::AclDataFrameHeaderView>(span);
  std::lock_guard lock(mutex_);

  if (!view.ok() || !view->Ok()) {
    PW_LOG_ERROR("Invalid ACL packet.");
    OnError(ErrorReason::kInvalidPacket);
    return;
  }

  ConnectionHandle connection_handle{view->handle().Read()};
  auto iter = connections_.find(cpp23::to_underlying(connection_handle));
  if (iter == connections_.end()) {
    PW_LOG_WARN("Connection handle 0x%04x not found.",
                cpp23::to_underlying(connection_handle));
    OnError(ErrorReason::kConnectionNotFound, connection_handle);
    return;
  }

  iter->AssertLockHeld(*this);
  iter->OnInput(ConnectionFsm::AclActivity{direction});
}

SniffOffloadManager::HandlerAction SniffOffloadManager::ProcessCommand(
    const MultiBuf& command_packet, CommandOpcode opcode) {
  std::lock_guard lock(mutex_);
  switch (cpp23::to_underlying(opcode)) {
    case kWriteSniffOffloadEnableOpcode:
      return ProcessWriteSniffOffloadEnable(command_packet);
    case kWriteSniffOffloadParametersOpcode:
      return ProcessWriteSniffOffloadParameters(command_packet);
    case kSniffModeOpcode:
      return ProcessSniffMode(command_packet);
    case kExitSniffModeOpcode:
      return ProcessExitSniffMode(command_packet);
    default:
      return kIgnorePacket;
  }
}

Status SniffOffloadManager::ProcessCommandStatus(const MultiBuf& event_packet) {
  Scratch<emboss::CommandStatusEvent::IntrinsicSizeInBytes()> scratch;
  auto span = event_packet.Get(scratch);
  auto view = MakeEmbossView<emboss::CommandStatusEventView>(span);
  std::lock_guard lock(mutex_);

  if (!view.ok() || !view->Ok()) {
    PW_LOG_ERROR("Invalid CommandStatusEvent packet.");
    OnError(ErrorReason::kInvalidPacket);
    return Status::InvalidArgument();
  }

  auto opcode = view->command_opcode_enum().Read();
  switch (cpp23::to_underlying(opcode)) {
    case kSniffSubratingOpcode:
    case kSniffModeOpcode:
    case kExitSniffModeOpcode:
      break;
    default:
      return Status::FailedPrecondition();
  }

  auto status = view->status().Read();
  if (status != emboss::StatusCode::SUCCESS) {
    PW_LOG_ERROR("Command failed (opcode: 0x%04x, status: 0x%04x)",
                 cpp23::to_underlying(opcode),
                 cpp23::to_underlying(status));
    OnError(ErrorReason::kCommandStatusFailed);
  }

  return OkStatus();
}

SniffOffloadManager::HandlerAction SniffOffloadManager::ProcessEvent(
    const MultiBuf& event_packet, EventCode event_code) {
  std::lock_guard lock(mutex_);
  switch (cpp23::to_underlying(event_code)) {
    case kModeChangeEventCode:
      return ProcessModeChange(event_packet);
    case kConnectionCompleteEventCode:
      return ProcessConnectionComplete(event_packet);
    case kDisconnectionCompleteEventCode:
      return ProcessDisconnectionComplete(event_packet);
    case kSniffSubratingEventCode:
      return ProcessSniffSubrating(event_packet);
    default:
      return kIgnorePacket;
  }
}

SniffOffloadManager::HandlerAction
SniffOffloadManager::ProcessWriteSniffOffloadEnable(
    const MultiBuf& command_packet) {
  Scratch<WriteSniffOffloadEnableCommand::IntrinsicSizeInBytes()> scratch;
  auto span = command_packet.Get(scratch);
  auto view = MakeEmbossView<WriteSniffOffloadEnableCommandView>(span);

  if (!view.ok() || view->header().opcode().Read() !=
                        emboss::OpCode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE) {
    PW_LOG_ERROR("Invalid WriteSniffOffloadEnable packet.");
    OnError(ErrorReason::kInvalidPacket);
    SendCommandStatus(kWriteSniffOffloadEnableOpcode,
                      CommandStatus::kBadArguments);
    return kBlockPacket;
  }

  auto enable = cpp23::to_underlying(view->enable().Read());

  if (!enable) {
    DoDisable();
  } else {
    suppress_mode_change_event_ =
        cpp23::to_underlying(view->suppress_mode_change_event().Read());
    suppress_sniff_subrating_event_ =
        cpp23::to_underlying(view->suppress_sniff_subrating_event().Read());

    DoEnable({
        .subrating_max_latency = view->max_latency().Read(),
        .subrating_min_remote_timeout = view->min_remote_timeout().Read(),
        .subrating_min_local_timeout = view->min_local_timeout().Read(),
    });
  }

  SendCommandComplete(kWriteSniffOffloadEnableOpcode);
  return kBlockPacket;
}

SniffOffloadManager::HandlerAction
SniffOffloadManager::ProcessWriteSniffOffloadParameters(
    const MultiBuf& command_packet) {
  Scratch<WriteSniffOffloadParametersCommand::IntrinsicSizeInBytes()> scratch;
  auto span = command_packet.Get(scratch);
  auto view = MakeEmbossView<WriteSniffOffloadParametersCommandView>(span);

  if (!view.ok() ||
      view->header().opcode().Read() !=
          emboss::OpCode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS) {
    PW_LOG_ERROR("Invalid WriteSniffOffloadParameters packet.");
    OnError(ErrorReason::kInvalidPacket);
    SendCommandStatus(kWriteSniffOffloadParametersOpcode,
                      CommandStatus::kBadArguments);
    return kBlockPacket;
  }

  ConnectionHandle handle = ConnectionHandle(view->connection_handle().Read());
  auto iter = connections_.find(cpp23::to_underlying(handle));
  if (iter == connections_.end()) {
    PW_LOG_WARN(
        "Cannot set sniff offload parameters for unknown connection 0x%04x.",
        cpp23::to_underlying(handle));
    OnError(ErrorReason::kConnectionNotFound, handle);
    SendCommandStatus(kWriteSniffOffloadParametersOpcode,
                      CommandStatus::kUnknownConnection);
    return kBlockPacket;
  }

  iter->AssertLockHeld(*this);
  iter->OnInput(ConnectionFsm::SniffOffloadParametersInput{
      .sniff_max_interval = view->max_interval().Read(),
      .sniff_min_interval = view->min_interval().Read(),
      .sniff_attempts = view->attempts().Read(),
      .sniff_timeout = view->sniff_timeout().Read(),
      .link_inactivity_timeout = view->link_inactivity_timeout().Read(),
      .subrating_max_latency = view->max_latency().Read(),
      .subrating_min_remote_timeout = view->min_remote_timeout().Read(),
      .subrating_min_local_timeout = view->min_local_timeout().Read(),
      .allow_exit_sniff_on_rx = bool(view->allow_exit_sniff_on_rx().Read()),
      .allow_exit_sniff_on_tx = bool(view->allow_exit_sniff_on_tx().Read()),
  });

  SendCommandComplete(kWriteSniffOffloadParametersOpcode);
  return kBlockPacket;
}

SniffOffloadManager::HandlerAction SniffOffloadManager::ProcessSniffMode(
    const MultiBuf& command_packet) {
  Scratch<WriteSniffOffloadParametersCommand::IntrinsicSizeInBytes()> scratch;
  auto span = command_packet.Get(scratch);
  auto view = MakeEmbossView<emboss::SniffModeCommandView>(span);

  if (!view.ok() ||
      view->header().opcode().Read() != emboss::OpCode::SNIFF_MODE) {
    PW_LOG_ERROR("Invalid SniffMode packet.");
    OnError(ErrorReason::kInvalidPacket);
    return {};
  }

  if (std::holds_alternative<Enabled>(state_)) {
    PW_LOG_WARN("Received sniff mode command from host while enabled.");
  }
  return {};
}

SniffOffloadManager::HandlerAction SniffOffloadManager::ProcessModeChange(
    const MultiBuf& event_packet) {
  Scratch<emboss::ModeChangeEvent::IntrinsicSizeInBytes()> scratch;
  auto span = event_packet.Get(scratch);
  auto view = MakeEmbossView<emboss::ModeChangeEventView>(span);

  const HandlerAction action = {
      .intercept = HandlerAction::Interception{suppress_mode_change_event_}};

  if (!view.ok() ||
      view->header().event_code().Read() != emboss::EventCode::MODE_CHANGE) {
    PW_LOG_ERROR("Invalid ModeChangeEvent packet.");
    OnError(ErrorReason::kInvalidPacket);
    return action;
  }

  ConnectionHandle connection_handle{view->connection_handle().Read()};
  auto iter = connections_.find(cpp23::to_underlying(connection_handle));
  if (iter == connections_.end()) {
    PW_LOG_WARN("Connection handle 0x%04x not found.",
                cpp23::to_underlying(connection_handle));
    OnError(ErrorReason::kConnectionNotFound, connection_handle);
    return action;
  }

  auto current_mode = view->current_mode().Read();
  iter->AssertLockHeld(*this);
  iter->OnInput(ConnectionFsm::ModeChangeInput{current_mode});

  return action;
}

SniffOffloadManager::HandlerAction SniffOffloadManager::ProcessExitSniffMode(
    const MultiBuf& command_packet) {
  Scratch<emboss::ExitSniffModeCommand::IntrinsicSizeInBytes()> scratch;
  auto span = command_packet.Get(scratch);
  auto view = MakeEmbossView<emboss::ExitSniffModeCommandView>(span);

  if (!view.ok() ||
      view->header().opcode().Read() != emboss::OpCode::EXIT_SNIFF_MODE) {
    PW_LOG_ERROR("Invalid ExitSniffMode packet.");
    OnError(ErrorReason::kInvalidPacket);
    return {};
  }

  if (std::holds_alternative<Enabled>(state_)) {
    PW_LOG_WARN("Received exit sniff mode command from host while enabled.");
  }
  return {};
}

SniffOffloadManager::HandlerAction
SniffOffloadManager::ProcessConnectionComplete(const MultiBuf& event_packet) {
  Scratch<ConnectionCompleteEvent::IntrinsicSizeInBytes()> scratch;
  auto span = event_packet.Get(scratch);
  auto view = MakeEmbossView<ConnectionCompleteEventView>(span);
  if (!view.ok() || view->header().event_code().Read() !=
                        emboss::EventCode::CONNECTION_COMPLETE) {
    PW_LOG_ERROR("Invalid ConnectionCompleteEvent packet.");
    OnError(ErrorReason::kInvalidPacket);
    return {};
  }

  if (view->link_type().Read() != emboss::LinkType::ACL) {
    return {};
  }

  if (view->status().Read() != emboss::StatusCode::SUCCESS) {
    return {};
  }

  ConnectionHandle handle = ConnectionHandle(view->connection_handle().Read());
  auto iter = connections_.find(cpp23::to_underlying(handle));
  if (iter != connections_.end()) {
    PW_LOG_WARN("Connection handle 0x%04x already exists.",
                cpp23::to_underlying(handle));
    OnError(ErrorReason::kConnectionAlreadyExists, handle);
    return {};
  }

  auto fsm = allocator_.New<ConnectionFsm>(*this, handle);
  if (fsm == nullptr) {
    PW_LOG_WARN("Failed to allocate connection data for handle 0x%04x.",
                cpp23::to_underlying(handle));
    OnError(ErrorReason::kCannotAllocateConnection, handle);
    return {};
  }

  fsm->AssertLockHeld(*this);
  connections_.insert(*fsm);
  if (std::holds_alternative<Enabled>(state_)) {
    fsm->OnInput(std::get<Enabled>(state_));
  }
  fsm->Start();
  return {};
}

SniffOffloadManager::HandlerAction
SniffOffloadManager::ProcessDisconnectionComplete(
    const MultiBuf& event_packet) {
  Scratch<DisconnectionCompleteEvent::IntrinsicSizeInBytes()> scratch;
  auto span = event_packet.Get(scratch);
  auto view = MakeEmbossView<DisconnectionCompleteEventView>(span);
  if (!view.ok() || view->header().event_code().Read() !=
                        emboss::EventCode::DISCONNECTION_COMPLETE) {
    PW_LOG_ERROR("Invalid DisconnectionCompleteEvent packet.");
    OnError(ErrorReason::kInvalidPacket);
    return {};
  }

  ConnectionHandle handle = ConnectionHandle(view->connection_handle().Read());
  auto iter = connections_.find(cpp23::to_underlying(handle));
  if (iter == connections_.end()) {
    PW_LOG_WARN("Connection handle 0x%04x not found.",
                cpp23::to_underlying(handle));
    OnError(ErrorReason::kConnectionNotFound, handle);
    return {};
  }

  auto* fsm = &*iter;
  connections_.erase(iter);
  fsm->Stop();
  allocator_.Delete(fsm);
  return {};
}

SniffOffloadManager::HandlerAction SniffOffloadManager::ProcessSniffSubrating(
    const MultiBuf& event_packet) {
  Scratch<emboss::EventHeader::IntrinsicSizeInBytes()> scratch;
  auto span = event_packet.Get(scratch);
  auto view = MakeEmbossView<emboss::EventHeaderView>(span);

  if (!view.ok() ||
      view->event_code().Read() != emboss::EventCode::SNIFF_SUBRATING) {
    PW_LOG_ERROR("Invalid SniffSubratingEvent packet.");
    OnError(ErrorReason::kInvalidPacket);
    return {};
  }

  // Do nothing and respect sniff subrating suppression.
  return {.intercept =
              HandlerAction::Interception{suppress_sniff_subrating_event_}};
}

void SniffOffloadManager::SendCommandComplete(uint16_t opcode) {
  Scratch<CommandCompleteEvent::IntrinsicSizeInBytes()> scratch;
  auto writer = MakeEmbossWriter<CommandCompleteEventWriter>(scratch);
  PW_CHECK_OK(writer.status());

  writer->header().event_code().Write(emboss::EventCode::COMMAND_COMPLETE);
  writer->header().parameter_total_size().Write(
      CommandCompleteEvent::IntrinsicSizeInBytes() -
      EventHeader::IntrinsicSizeInBytes());
  writer->num_hci_command_packets().Write(1);
  writer->command_opcode_uint().Write(opcode);

  PW_CHECK(writer->Ok());

  auto buf = AllocateBuffer(scratch);

  if (!buf.ok()) {
    PW_LOG_WARN("Failed to allocate buffer for CommandComplete event.");
    OnError(ErrorReason::kCannotAllocateBuffer);
    return;
  }

  SendEvent(std::move(*buf));
}

void SniffOffloadManager::SendCommandStatus(uint16_t opcode,
                                            CommandStatus status) {
  Scratch<CommandStatusEvent::IntrinsicSizeInBytes()> scratch;
  auto writer = MakeEmbossWriter<CommandStatusEventWriter>(scratch);
  PW_CHECK_OK(writer.status());

  emboss::StatusCode status_code;

  switch (status) {
    case CommandStatus::kSuccess:
      status_code = emboss::StatusCode::SUCCESS;
      break;
    case CommandStatus::kBadArguments:
      status_code = emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS;
      break;
    case CommandStatus::kUnknownConnection:
      status_code = emboss::StatusCode::UNKNOWN_CONNECTION_ID;
      break;
  }

  writer->header().event_code().Write(emboss::EventCode::COMMAND_STATUS);
  writer->header().parameter_total_size().Write(
      CommandStatusEvent::IntrinsicSizeInBytes() -
      EventHeader::IntrinsicSizeInBytes());
  writer->status().Write(status_code);
  writer->num_hci_command_packets().Write(1);
  writer->command_opcode_uint().Write(opcode);

  PW_CHECK(writer->Ok());

  auto buf = AllocateBuffer(scratch);
  if (!buf.ok()) {
    PW_LOG_WARN("Failed to allocate buffer for CommandStatus event.");
    OnError(ErrorReason::kCannotAllocateBuffer);
    return;
  }

  SendEvent(std::move(*buf));
}

void SniffOffloadManager::SendCommand(
    MultiBuf::Instance&& command_packet,
    CompletionEvent completion,
    std::optional<ConnectionHandle> connection_handle) {
  if (!send_command_(std::move(command_packet), completion).ok()) {
    PW_LOG_WARN("Failed to send command.");
    OnError(ErrorReason::kSendCommandFailed, connection_handle);
  }
}

void SniffOffloadManager::SendEvent(
    MultiBuf::Instance&& event_packet,
    std::optional<ConnectionHandle> connection_handle) {
  if (!send_event_(std::move(event_packet)).ok()) {
    PW_LOG_WARN("Failed to send event.");
    OnError(ErrorReason::kSendEventFailed, connection_handle);
  }
}

void SniffOffloadManager::OnError(
    ErrorReason reason, std::optional<ConnectionHandle> connection_handle) {
  if (on_error_(reason, connection_handle) == ErrorAction::kDisable) {
    DoDisable();
  }
}

void SniffOffloadManager::DoDisable() {
  for (auto& fsm : connections_) {
    fsm.AssertLockHeld(*this);
    fsm.OnInput(Disabled{});
  }

  state_ = Disabled();
}

void SniffOffloadManager::DoEnable(Enabled&& enabled) {
  for (auto& fsm : connections_) {
    fsm.AssertLockHeld(*this);
    fsm.OnInput(Enabled{enabled});
  }

  state_ = std::move(enabled);
}

Result<MultiBuf::Instance> SniffOffloadManager::AllocateBuffer(
    ConstByteSpan span) {
  auto alloc = allocator_.MakeUnique<std::byte[]>(span.size());
  if (alloc == nullptr) {
    return Status::ResourceExhausted();
  }

  memcpy(alloc.get(), span.data(), span.size());
  MultiBuf::Instance buf(allocator_);

  if (!buf->TryReserveForPushBack()) {
    return Status::ResourceExhausted();
  }

  buf->PushBack(std::move(alloc));
  return buf;
}

void SniffOffloadManager::ConnectionFsm::Start() {
  manager_.dispatcher_.Post(timeout_task_);
}

void SniffOffloadManager::ConnectionFsm::Stop() { timeout_task_.Deregister(); }

void SniffOffloadManager::ConnectionFsm::OnInput(Input&& input) {
  std::visit(
      [this](auto&& value) PW_EXCLUSIVE_LOCKS_REQUIRED(manager_.mutex_) {
        HandleInput(std::forward<decltype(value)>(value));
      },
      std::move(input));
}

void SniffOffloadManager::ConnectionFsm::HandleInput(EnableInput&& input) {
  [[maybe_unused]] auto _ = std::move(input);
  enabled_ = true;
}

void SniffOffloadManager::ConnectionFsm::HandleInput(DisableInput&& input) {
  [[maybe_unused]] auto _ = std::move(input);
  enabled_ = false;
}

void SniffOffloadManager::ConnectionFsm::HandleInput(
    SniffOffloadParametersInput&& input) {
  auto previous_connection_state = connection_state();
  parameters_ = std::move(input);
  PW_CHECK(connection_state() == ConnectionState::kControlStarted);
  if (previous_connection_state == ConnectionState::kPendingParameters) {
    ResetTimer();
  }
  SendSniffSubrating();
}

void SniffOffloadManager::ConnectionFsm::HandleInput(ModeChangeInput&& input) {
  auto mode_change = std::move(input);
  switch (mode_change.current_mode) {
    case emboss::AclConnectionMode::ACTIVE:
      connection_mode_ = ConnectionMode::kActive;
      ResetTimer();
      break;
    case emboss::AclConnectionMode::SNIFF:
      connection_mode_ = ConnectionMode::kSniff;
      break;
    case emboss::AclConnectionMode::HOLD:
      connection_mode_ = ConnectionMode::kSniff;
      PW_LOG_WARN("Got unexpected 'HOLD' mode.");
      break;
    default:
      PW_LOG_WARN("Got unexpected ACL connection mode (0x%02" PRIX8 ")",
                  cpp23::to_underlying(mode_change.current_mode));
      break;
  }
}

void SniffOffloadManager::ConnectionFsm::HandleTimeout() {
  switch (connection_mode_) {
    case ConnectionMode::kActive:
      if (should_control()) {
        SendEnterSniffMode();
        connection_mode_ = ConnectionMode::kTransitionActiveToSniff;
      }
      break;
    case ConnectionMode::kTransitionActiveToSniff:
    case ConnectionMode::kSniff:
    case ConnectionMode::kTransitionSniffToActive:
      break;
  }
}

void SniffOffloadManager::ConnectionFsm::HandleInput(AclActivity&& input) {
  auto activity = std::move(input);
  switch (connection_mode_) {
    case ConnectionMode::kActive:
      break;
    case ConnectionMode::kTransitionActiveToSniff:
    case ConnectionMode::kSniff:
      if (ShouldExitSniff(activity.direction)) {
        SendExitSniffMode();
        connection_mode_ = ConnectionMode::kTransitionSniffToActive;
      }
      break;
    case ConnectionMode::kTransitionSniffToActive:
      break;
  }

  ResetTimer();
}

void SniffOffloadManager::ConnectionFsm::ResetTimer() {
  if (!should_control()) {
    return;
  }

  timeout_task_.AssertLockHeld(*this);
  timeout_task_.WaitFor(
      std::chrono::milliseconds(parameters_->link_inactivity_timeout));
}

void SniffOffloadManager::ConnectionFsm::SendEnterSniffMode() {
  Scratch<emboss::SniffModeCommand::IntrinsicSizeInBytes()> scratch;

  PW_CHECK(should_control());

  auto writer = MakeEmbossWriter<emboss::SniffModeCommandWriter>(scratch);
  PW_CHECK_OK(writer);

  writer->header().opcode().Write(emboss::OpCode::SNIFF_MODE);
  writer->header().parameter_total_size().Write(
      emboss::SniffModeCommand::IntrinsicSizeInBytes() -
      emboss::CommandHeader::IntrinsicSizeInBytes());
  writer->connection_handle().Write(cpp23::to_underlying(handle_));
  writer->sniff_max_interval().Write(parameters_->sniff_max_interval);
  writer->sniff_min_interval().Write(parameters_->sniff_min_interval);
  writer->sniff_attempt().Write(parameters_->sniff_attempts);
  writer->sniff_timeout().Write(parameters_->sniff_timeout);

  PW_CHECK(writer->Ok());

  auto buf = manager_.AllocateBuffer(scratch);
  if (!buf.ok()) {
    PW_LOG_WARN("Failed to allocate buffer for EnterSniffMode command.");
    manager_.OnError(ErrorReason::kCannotAllocateBuffer, handle_);
    return;
  }

  manager_.SendCommand(std::move(*buf), CompletionEvent::kCommandStatus);
}

void SniffOffloadManager::ConnectionFsm::SendSniffSubrating() {
  Scratch<emboss::SniffSubratingCommand::IntrinsicSizeInBytes()> scratch;

  PW_CHECK(should_control());

  auto writer = MakeEmbossWriter<emboss::SniffSubratingCommandWriter>(scratch);
  PW_CHECK_OK(writer);

  writer->header().opcode().Write(emboss::OpCode::SNIFF_SUBRATING);
  writer->header().parameter_total_size().Write(
      emboss::SniffSubratingCommand::IntrinsicSizeInBytes() -
      emboss::CommandHeader::IntrinsicSizeInBytes());
  writer->connection_handle().Write(cpp23::to_underlying(handle_));
  writer->max_latency().Write(parameters_->subrating_max_latency);
  writer->min_remote_timeout().Write(parameters_->subrating_min_remote_timeout);
  writer->min_local_timeout().Write(parameters_->subrating_min_local_timeout);

  PW_CHECK(writer->Ok());

  auto buf = manager_.AllocateBuffer(scratch);
  if (!buf.ok()) {
    PW_LOG_WARN("Failed to allocate buffer for SniffSubrating command.");
    manager_.OnError(ErrorReason::kCannotAllocateBuffer, handle_);
    return;
  }

  manager_.SendCommand(std::move(*buf), CompletionEvent::kCommandStatus);
}

void SniffOffloadManager::ConnectionFsm::SendExitSniffMode() {
  Scratch<emboss::ExitSniffModeCommand::IntrinsicSizeInBytes()> scratch;

  PW_CHECK(should_control());

  auto writer = MakeEmbossWriter<emboss::ExitSniffModeCommandWriter>(scratch);
  PW_CHECK_OK(writer);

  writer->header().opcode().Write(emboss::OpCode::EXIT_SNIFF_MODE);
  writer->header().parameter_total_size().Write(
      emboss::ExitSniffModeCommand::IntrinsicSizeInBytes() -
      emboss::CommandHeader::IntrinsicSizeInBytes());
  writer->connection_handle().Write(cpp23::to_underlying(handle_));
  PW_CHECK(writer->Ok());

  auto buf = manager_.AllocateBuffer(scratch);
  if (!buf.ok()) {
    PW_LOG_WARN("Failed to allocate buffer for ExitSniffMode command.");
    manager_.OnError(ErrorReason::kCannotAllocateBuffer, handle_);
    return;
  }

  manager_.SendCommand(std::move(*buf), CompletionEvent::kCommandStatus);
}

bool SniffOffloadManager::ConnectionFsm::ShouldExitSniff(Direction direction) {
  return should_control() && (direction == Direction::kHostToController)
             ? (parameters_->allow_exit_sniff_on_tx)
             : (parameters_->allow_exit_sniff_on_rx);
}

async2::Poll<> SniffOffloadManager::ConnectionFsm::TimeoutTask::DoPend(
    async2::Context& cx) {
  while (true) {
    std::lock_guard lock(fsm_.manager_.mutex_);
    if (!timeout_future_.is_pendable() || timeout_future_.is_complete()) {
      timeout_future_ = fsm_.manager_.time_provider_.WaitUntil(
          chrono::SystemClock::time_point::max());
    }
    PW_TRY_READY(timeout_future_.Pend(cx));
    fsm_.HandleTimeout();
  }
}

}  // namespace pw::bluetooth::proxy::hci
