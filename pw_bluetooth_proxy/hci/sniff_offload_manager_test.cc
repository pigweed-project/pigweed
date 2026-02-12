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

#include <cstddef>
#include <cstdint>

#include "lib/stdcompat/utility.h"
#include "pw_allocator/synchronized_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_async2/dispatcher_for_test.h"
#include "pw_async2/simulated_time_provider.h"
#include "pw_bluetooth/hci_android.emb.h"
#include "pw_bluetooth/hci_commands.emb.h"
#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/hci_events.emb.h"
#include "pw_chrono/system_clock.h"
#include "pw_log/log.h"
#include "pw_status/status.h"
#include "pw_sync/mutex.h"
#include "pw_thread/sleep.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"
#include "pw_unit_test/status_macros.h"

namespace pw::bluetooth::proxy::hci {
namespace {

using HandlerAction = SniffOffloadManager::HandlerAction;

constexpr size_t kAllocatorSize = 1024;
constexpr size_t kInternalAllocatorSize = 256;
constexpr auto kDefaultErrorAction = SniffOffloadManager::ErrorAction::kIgnore;
constexpr auto kDefaultLinkInactivityTimeout = std::chrono::milliseconds(1000);
constexpr auto kTick = chrono::SystemClock::duration(1);

constexpr HandlerAction kPassthroughResume = {
    .intercept = HandlerAction::Interception::kPassthrough,
    .resume = HandlerAction::Resumption::kResume,
};

constexpr HandlerAction kInterceptResume = {
    .intercept = HandlerAction::Interception::kIntercept,
    .resume = HandlerAction::Resumption::kResume,
};

class SniffOffloadManagerTest : public ::testing::Test {
 protected:
  using EventCode = SniffOffloadManager::EventCode;
  using CommandOpcode = SniffOffloadManager::CommandOpcode;
  using ErrorReason = SniffOffloadManager::ErrorReason;

  struct HostCommand final {
    MultiBuf::Instance buf;
    SniffOffloadManager::CommandOpcode opcode;
  };

  struct ControllerEvent final {
    MultiBuf::Instance buf;
    SniffOffloadManager::EventCode event_code;
  };

  struct SentCommand final {
    MultiBuf::Instance buf;
    SniffOffloadManager::CompletionEvent completion;
    std::optional<CommandOpcode> opcode();
  };

  struct Error final {
    ErrorReason reason;
    std::optional<ConnectionHandle> handle = std::nullopt;
    bool operator==(const Error& other) const;
  };

  struct SniffOffloadParameters final {
    uint16_t sniff_max_interval = 0x0010;
    uint16_t sniff_min_interval = 0x0008;
    uint16_t sniff_attempts = 0x0004;
    uint16_t sniff_timeout = 0x0008;
    uint16_t link_inactivity_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            kDefaultLinkInactivityTimeout)
            .count();
    uint16_t subrating_max_latency = 0x0008;
    uint16_t subrating_min_remote_timeout = 0x0000;
    uint16_t subrating_min_local_timeout = 0x0000;
    bool allow_exit_sniff_on_rx = true;
    bool allow_exit_sniff_on_tx = true;

    static SniffOffloadParameters Default() { return {}; }
  };

  SniffOffloadManager& MakeSniffOffloadWithHandlers(
      SniffOffloadManager::SendCommandFunc&& send_command,
      SniffOffloadManager::SendEventFunc&& send_event,
      SniffOffloadManager::OnErrorFunc&& on_error) {
    return sniff_offload_manager_.emplace(allocator_,
                                          dispatcher_,
                                          std::move(send_command),
                                          std::move(send_event),
                                          std::move(on_error),
                                          time_provider_);
  }

  void DestroySniffOffload() { sniff_offload_manager_.reset(); }

  SniffOffloadManager& sniff_offload_manager() {
    if (!sniff_offload_manager_.has_value()) {
      return MakeSniffOffloadWithHandlers(MakeDefaultSendCommand(),
                                          MakeDefaultSendEvent(),
                                          MakeDefaultOnError());
    }
    return *sniff_offload_manager_;
  }

  Allocator& allocator() { return allocator_; }
  async2::DispatcherForTest& dispatcher() { return dispatcher_; }

  template <typename Fn>
  auto WithTestAllocator(Fn&& fn) {
    auto borrow = allocator_.Borrow();
    return fn(static_cast<decltype(test_allocator_)&>(*borrow));
  }

  DynamicDeque<MultiBuf::Instance>& packets_to_host() {
    return packets_to_host_;
  }
  DynamicDeque<SentCommand>& packets_to_controller() {
    return packets_to_controller_;
  }
  DynamicDeque<Error>& errors() { return errors_; }

  HandlerAction SimulateCommand(MultiBuf::Instance&& buf,
                                CommandOpcode opcode) {
    auto action =
        sniff_offload_manager().ProcessCommand(std::move(buf), opcode);
    SyncWithDispatcher();
    return action;
  }

  HandlerAction SimulateEvent(MultiBuf::Instance&& buf, EventCode event_code) {
    auto action =
        sniff_offload_manager().ProcessEvent(std::move(buf), event_code);
    SyncWithDispatcher();
    return action;
  }

  Status SimulateCommandStatus(MultiBuf::Instance&& buf) {
    auto status = sniff_offload_manager().ProcessCommandStatus(std::move(buf));
    SyncWithDispatcher();
    return status;
  }

  HandlerAction Simulate(HostCommand&& command) {
    return SimulateCommand(std::move(command.buf), command.opcode);
  }

  HandlerAction Simulate(ControllerEvent&& event) {
    return SimulateEvent(std::move(event.buf), event.event_code);
  }

  void SimulateAclActivity(uint16_t handle,
                           SniffOffloadManager::Direction direction,
                           bool valid = true);

  void AdvanceTime(chrono::SystemClock::duration duration) {
    time_provider_.AdvanceTime(duration);
    SyncWithDispatcher();
  }

  SniffOffloadManager::SendCommandFunc MakeDefaultSendCommand() {
    return [this](MultiBuf::Instance&& buf,
                  SniffOffloadManager::CompletionEvent completion) -> Status {
      return packets_to_controller_.try_push_back({std::move(buf), completion})
                 ? OkStatus()
                 : Status::ResourceExhausted();
    };
  }

  SniffOffloadManager::SendEventFunc MakeDefaultSendEvent() {
    return [this](MultiBuf::Instance&& buf) -> Status {
      return packets_to_host_.try_push_back(std::move(buf))
                 ? OkStatus()
                 : Status::ResourceExhausted();
    };
  }

  SniffOffloadManager::OnErrorFunc MakeDefaultOnError() {
    return [this](ErrorReason reason, std::optional<ConnectionHandle> handle)
               -> SniffOffloadManager::ErrorAction {
      errors_.push_back({reason, handle});
      return kDefaultErrorAction;
    };
  }

  ControllerEvent ConnectionComplete(
      ConnectionHandle handle,
      emboss::StatusCode status = emboss::StatusCode::SUCCESS,
      emboss::LinkType link_type = emboss::LinkType::ACL);
  ControllerEvent ConnectionComplete(
      std::underlying_type_t<ConnectionHandle> handle,
      std::underlying_type_t<emboss::StatusCode> status =
          cpp23::to_underlying(emboss::StatusCode::SUCCESS),
      std::underlying_type_t<emboss::LinkType> link_type =
          cpp23::to_underlying(emboss::LinkType::ACL)) {
    return ConnectionComplete(ConnectionHandle(handle),
                              emboss::StatusCode(status),
                              emboss::LinkType(link_type));
  }

  ControllerEvent DisconnectionComplete(
      ConnectionHandle handle,
      emboss::StatusCode status = emboss::StatusCode::SUCCESS,
      emboss::StatusCode reason =
          emboss::StatusCode::CONNECTION_TERMINATED_BY_LOCAL_HOST);
  ControllerEvent DisconnectionComplete(
      std::underlying_type_t<ConnectionHandle> handle,
      std::underlying_type_t<emboss::StatusCode> status =
          cpp23::to_underlying(emboss::StatusCode::SUCCESS),
      std::underlying_type_t<emboss::StatusCode> reason = cpp23::to_underlying(
          emboss::StatusCode::CONNECTION_TERMINATED_BY_LOCAL_HOST)) {
    return DisconnectionComplete(ConnectionHandle(handle),
                                 emboss::StatusCode(status),
                                 emboss::StatusCode(reason));
  }

  HostCommand WriteSniffOffloadEnable(bool enable,
                                      bool suppress_mode_change_event = false,
                                      bool suppress_sniff_subrating = false);
  HostCommand WriteSniffOffloadParameters(
      ConnectionHandle handle,
      SniffOffloadParameters&& parameters = SniffOffloadParameters::Default());
  HostCommand WriteSniffOffloadParameters(
      std::underlying_type_t<ConnectionHandle> handle,
      SniffOffloadParameters&& parameters = SniffOffloadParameters::Default()) {
    return WriteSniffOffloadParameters(ConnectionHandle(handle),
                                       std::move(parameters));
  }

  ControllerEvent ModeChangeEvent(
      uint16_t connection_handle,
      emboss::AclConnectionMode mode = emboss::AclConnectionMode::SNIFF);
  ControllerEvent SniffSubratingEvent(uint16_t connection_handle);

  MultiBuf::Instance MakeCommandStatus(CommandOpcode opcode,
                                       emboss::StatusCode status);
  MultiBuf::Instance MakeZeroBuffer(size_t size = 0);

  bool CheckSniffSubratingSent(
      std::optional<uint16_t> connection_handle = std::nullopt);
  bool CheckSniffModeSent(
      std::optional<uint16_t> connection_handle = std::nullopt);
  bool CheckSniffExitSent(
      std::optional<uint16_t> connection_handle = std::nullopt);
  bool CheckCommandCompleteEventSent(
      std::optional<CommandOpcode> opcode = std::nullopt);
  bool CheckCommandStatusEventSent(
      std::optional<CommandOpcode> opcode = std::nullopt,
      std::optional<emboss::StatusCode> status = std::nullopt);

  bool NoErrors() {
    auto was_empty = errors_.empty();
    errors_.clear();
    return was_empty;
  }

  std::optional<Error> PopLastError() {
    if (errors_.empty()) {
      return std::nullopt;
    }
    auto last = std::move(errors_.back());
    errors_.pop_back();
    return last;
  }

  void SyncWithDispatcher() { dispatcher().RunUntilStalled(); }

 private:
  void TearDown() override { DestroySniffOffload(); }

  Allocator& internal_allocator() { return internal_allocator_; }

  allocator::test::AllocatorForTest<kAllocatorSize> test_allocator_;
  allocator::SynchronizedAllocator<sync::Mutex> allocator_{test_allocator_};
  allocator::test::AllocatorForTest<kInternalAllocatorSize> internal_allocator_;
  allocator::SynchronizedAllocator<sync::Mutex> sync_internal_allocator_{
      internal_allocator_};
  async2::DispatcherForTest dispatcher_;
  async2::SimulatedTimeProvider<chrono::SystemClock> time_provider_;
  std::optional<SniffOffloadManager> sniff_offload_manager_;

  pw::DynamicDeque<MultiBuf::Instance> packets_to_host_{
      sync_internal_allocator_};
  pw::DynamicDeque<SentCommand> packets_to_controller_{
      sync_internal_allocator_};
  pw::DynamicDeque<Error> errors_{sync_internal_allocator_};
};

TEST_F(SniffOffloadManagerTest, CreationAndDestruction) {
  DestroySniffOffload();

  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, CreationAndDestructionActiveConnection) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x0123)), kPassthroughResume);
  EXPECT_EQ(Simulate(ConnectionComplete(0x0456)), kPassthroughResume);
  DestroySniffOffload();

  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, CreationAndDestructionAllConnectionsClosed) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x0123)), kPassthroughResume);
  EXPECT_EQ(Simulate(ConnectionComplete(0x0456)), kPassthroughResume);
  EXPECT_EQ(Simulate(DisconnectionComplete(0x0123)), kPassthroughResume);
  EXPECT_EQ(Simulate(DisconnectionComplete(0x0456)), kPassthroughResume);
  DestroySniffOffload();

  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, EnableSniffBeforeConnection) {
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(DisconnectionComplete(0x123)), kPassthroughResume);

  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, EnableSniffAfterConnection) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(DisconnectionComplete(0x123)), kPassthroughResume);

  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutosniffOnTimeout) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout + kTick);

  EXPECT_TRUE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutoSniffOnTimeoutExitOnRx) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout + kTick);

  EXPECT_TRUE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());

  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kRx);
  EXPECT_TRUE(CheckSniffExitSent(0x123));
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutoSniffOnTimeoutExitOnTx) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout + kTick);

  EXPECT_TRUE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());

  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kTx);
  EXPECT_TRUE(CheckSniffExitSent(0x123));
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutoSniffOnTimeoutNoExitOnRx) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(
                0x123, {.allow_exit_sniff_on_rx = false})),
            kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout + kTick);

  EXPECT_TRUE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());

  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kRx);
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutoSniffOnTimeoutNoExitOnTx) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(
                0x123, {.allow_exit_sniff_on_tx = false})),
            kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout + kTick);

  EXPECT_TRUE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());

  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kTx);
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutoSniffDelaysOnAclActivityTx) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kTx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kTx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kTx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kTx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kTx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);

  EXPECT_TRUE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutoSniffDelaysOnAclActivityRx) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kRx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kRx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kRx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kRx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kRx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);

  EXPECT_TRUE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutoSniffDelaysOnAclActivityMixed) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kRx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kTx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kTx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kRx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kTx);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);

  EXPECT_TRUE(CheckSniffModeSent(0x123));
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutosniffResetsTimerOnModeChangeActive) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout * 2 / 3 + kTick);
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  EXPECT_EQ(Simulate(ModeChangeEvent(0x123, emboss::AclConnectionMode::ACTIVE)),
            kPassthroughResume);
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout * 2 / 3 + kTick);
  EXPECT_FALSE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout * 2 / 3 + kTick);
  EXPECT_TRUE(CheckSniffModeSent(0x123));
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutosniffDoesNotSendSniffModeIfAlreadySniff) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout * 2 / 3 + kTick);
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  EXPECT_EQ(Simulate(ModeChangeEvent(0x123, emboss::AclConnectionMode::SNIFF)),
            kPassthroughResume);
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout * 2 / 3 + kTick);
  EXPECT_FALSE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout * 2 / 3 + kTick);
  EXPECT_FALSE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutosniffTreatsHoldModeAsSniff) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout * 2 / 3 + kTick);
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  EXPECT_EQ(Simulate(ModeChangeEvent(0x123, emboss::AclConnectionMode::HOLD)),
            kPassthroughResume);
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout * 2 / 3 + kTick);
  EXPECT_FALSE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout * 2 / 3 + kTick);
  EXPECT_FALSE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, AutosniffDisable) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(false)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());

  AdvanceTime(kDefaultLinkInactivityTimeout / 2 + kTick);

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, SuppressModeChange) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true, true, true)),
            kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(ModeChangeEvent(0x123)), kInterceptResume);
  EXPECT_TRUE(NoErrors());
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true, true, false)),
            kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(ModeChangeEvent(0x123)), kInterceptResume);
  EXPECT_TRUE(NoErrors());
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true, false, true)),
            kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(ModeChangeEvent(0x123)), kPassthroughResume);
  EXPECT_TRUE(NoErrors());
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true, false, false)),
            kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(ModeChangeEvent(0x123)), kPassthroughResume);
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, SuppressSniffSubrating) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true, true, true)),
            kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(SniffSubratingEvent(0x123)), kInterceptResume);
  EXPECT_TRUE(NoErrors());
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true, true, false)),
            kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(SniffSubratingEvent(0x123)), kPassthroughResume);
  EXPECT_TRUE(NoErrors());
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true, false, true)),
            kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(SniffSubratingEvent(0x123)), kInterceptResume);
  EXPECT_TRUE(NoErrors());
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true, false, false)),
            kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(SniffSubratingEvent(0x123)), kPassthroughResume);
  EXPECT_TRUE(NoErrors());
}

TEST_F(SniffOffloadManagerTest, ErrorCannotAllocateConnection) {
  WithTestAllocator([](auto& allocator) { allocator.Exhaust(); });
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(
      PopLastError(),
      Error({ErrorReason::kCannotAllocateConnection, ConnectionHandle{0x123}}));
}

TEST_F(SniffOffloadManagerTest, ErrorCannotAllocateBuffer) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  WithTestAllocator([](auto& allocator) { allocator.Exhaust(); });
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_EQ(PopLastError(),
            Error({ErrorReason::kCannotAllocateBuffer, std::nullopt}));
  EXPECT_FALSE(CheckCommandCompleteEventSent());

  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_EQ(PopLastError(),
            Error({ErrorReason::kCannotAllocateBuffer, std::nullopt}));
  EXPECT_FALSE(CheckCommandCompleteEventSent());
  EXPECT_EQ(
      PopLastError(),
      Error({ErrorReason::kCannotAllocateBuffer, ConnectionHandle{0x123}}));
  EXPECT_FALSE(CheckSniffSubratingSent());

  AdvanceTime(kDefaultLinkInactivityTimeout + kTick);

  EXPECT_EQ(
      PopLastError(),
      Error({ErrorReason::kCannotAllocateBuffer, ConnectionHandle{0x123}}));
  EXPECT_FALSE(CheckSniffModeSent());

  EXPECT_TRUE(NoErrors());
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(packets_to_host().empty());
}

TEST_F(SniffOffloadManagerTest, ErrorConnectionAlreadyExists) {
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_TRUE(NoErrors());
  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(
      PopLastError(),
      Error({ErrorReason::kConnectionAlreadyExists, ConnectionHandle{0x123}}));

  EXPECT_TRUE(NoErrors());
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(packets_to_host().empty());
}

TEST_F(SniffOffloadManagerTest, ErrorConnectionNotFound) {
  SimulateAclActivity(0x123, SniffOffloadManager::Direction::kRx);
  EXPECT_EQ(PopLastError(),
            Error({ErrorReason::kConnectionNotFound, ConnectionHandle{0x123}}));

  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x456)), kInterceptResume);
  EXPECT_EQ(PopLastError(),
            Error({ErrorReason::kConnectionNotFound, ConnectionHandle{0x456}}));
  EXPECT_TRUE(CheckCommandStatusEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS,
      emboss::StatusCode::UNKNOWN_CONNECTION_ID));

  EXPECT_EQ(Simulate(DisconnectionComplete(0x555)), kPassthroughResume);
  EXPECT_EQ(PopLastError(),
            Error({ErrorReason::kConnectionNotFound, ConnectionHandle{0x555}}));

  EXPECT_EQ(Simulate(ModeChangeEvent(0x333)), kPassthroughResume);
  EXPECT_EQ(PopLastError(),
            Error({ErrorReason::kConnectionNotFound, ConnectionHandle{0x333}}));

  EXPECT_TRUE(NoErrors());
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(packets_to_host().empty());
}

TEST_F(SniffOffloadManagerTest, ErrorInvalidPacket) {
  constexpr size_t kWriteSniffOffloadEnableSize = vendor::android_hci::
      WriteSniffOffloadEnableCommand::IntrinsicSizeInBytes();
  constexpr size_t kWriteSniffOffloadParametersSize = vendor::android_hci::
      WriteSniffOffloadParametersCommand::IntrinsicSizeInBytes();
  constexpr size_t kSniffModeSize =
      emboss::SniffModeCommand::IntrinsicSizeInBytes();
  constexpr size_t kExitSniffModeSize =
      emboss::ExitSniffModeCommand::IntrinsicSizeInBytes();

  EXPECT_EQ(SimulateCommand(MakeZeroBuffer(),
                            CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE),
            kInterceptResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  EXPECT_TRUE(CheckCommandStatusEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE,
      emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS));
  EXPECT_EQ(SimulateCommand(MakeZeroBuffer(kWriteSniffOffloadEnableSize),
                            CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE),
            kInterceptResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  EXPECT_TRUE(CheckCommandStatusEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE,
      emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS));

  EXPECT_EQ(
      SimulateCommand(MakeZeroBuffer(),
                      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS),
      kInterceptResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  EXPECT_TRUE(CheckCommandStatusEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS,
      emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS));
  EXPECT_EQ(
      SimulateCommand(MakeZeroBuffer(kWriteSniffOffloadParametersSize),
                      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS),
      kInterceptResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  EXPECT_TRUE(CheckCommandStatusEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS,
      emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS));

  EXPECT_EQ(SimulateCommand(MakeZeroBuffer(), CommandOpcode::SNIFF_MODE),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  EXPECT_EQ(SimulateCommand(MakeZeroBuffer(kSniffModeSize),
                            CommandOpcode::SNIFF_MODE),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));

  EXPECT_EQ(SimulateCommand(MakeZeroBuffer(), CommandOpcode::EXIT_SNIFF_MODE),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  EXPECT_EQ(SimulateCommand(MakeZeroBuffer(kExitSniffModeSize),
                            CommandOpcode::EXIT_SNIFF_MODE),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));

  EXPECT_TRUE(NoErrors());
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(packets_to_host().empty());

  constexpr size_t kModeChangeEventSize =
      emboss::ModeChangeEvent::IntrinsicSizeInBytes();
  constexpr size_t kConnectionCompleteEventSize =
      emboss::ConnectionCompleteEvent::IntrinsicSizeInBytes();
  constexpr size_t kDisconnectionCompleteEventSize =
      emboss::DisconnectionCompleteEvent::IntrinsicSizeInBytes();
  constexpr size_t kSniffSubratingEventSize =
      emboss::EventHeader::IntrinsicSizeInBytes();

  EXPECT_EQ(SimulateEvent(MakeZeroBuffer(), EventCode::MODE_CHANGE),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  EXPECT_EQ(SimulateEvent(MakeZeroBuffer(kModeChangeEventSize),
                          EventCode::MODE_CHANGE),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));

  EXPECT_EQ(SimulateEvent(MakeZeroBuffer(), EventCode::CONNECTION_COMPLETE),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  EXPECT_EQ(SimulateEvent(MakeZeroBuffer(kConnectionCompleteEventSize),
                          EventCode::CONNECTION_COMPLETE),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));

  EXPECT_EQ(SimulateEvent(MakeZeroBuffer(), EventCode::DISCONNECTION_COMPLETE),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  EXPECT_EQ(SimulateEvent(MakeZeroBuffer(kDisconnectionCompleteEventSize),
                          EventCode::DISCONNECTION_COMPLETE),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));

  EXPECT_EQ(SimulateEvent(MakeZeroBuffer(), EventCode::SNIFF_SUBRATING),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  EXPECT_EQ(SimulateEvent(MakeZeroBuffer(kSniffSubratingEventSize),
                          EventCode::SNIFF_SUBRATING),
            kPassthroughResume);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));

  EXPECT_TRUE(NoErrors());
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(packets_to_host().empty());

  SimulateAclActivity(0, SniffOffloadManager::Direction::kRx, false);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  SimulateAclActivity(0, SniffOffloadManager::Direction::kTx, false);
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));

  EXPECT_TRUE(NoErrors());
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(packets_to_host().empty());
}

TEST_F(SniffOffloadManagerTest, ErrorSendCommandFailed) {
  MakeSniffOffloadWithHandlers([](auto&&, auto) { return Status::Internal(); },
                               MakeDefaultSendEvent(),
                               MakeDefaultOnError());

  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_EQ(PopLastError(),
            Error({ErrorReason::kSendCommandFailed, std::nullopt}));
}

TEST_F(SniffOffloadManagerTest, ErrorSendEventFailed) {
  MakeSniffOffloadWithHandlers(
      MakeDefaultSendCommand(),
      [](auto&&) { return Status::Internal(); },
      MakeDefaultOnError());

  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_FALSE(CheckCommandCompleteEventSent());
  EXPECT_EQ(PopLastError(),
            Error({ErrorReason::kSendEventFailed, std::nullopt}));
}

TEST_F(SniffOffloadManagerTest, ProcessCommandStatus) {
  EXPECT_EQ(SimulateCommandStatus(MakeZeroBuffer()), Status::InvalidArgument());
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kInvalidPacket, std::nullopt}));
  EXPECT_EQ(SimulateCommandStatus(MakeCommandStatus(
                CommandOpcode::INQUIRY, emboss::StatusCode::SUCCESS)),
            Status::FailedPrecondition());

  EXPECT_TRUE(NoErrors());

  EXPECT_EQ(Simulate(ConnectionComplete(0x123)), kPassthroughResume);
  EXPECT_EQ(Simulate(WriteSniffOffloadEnable(true)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE));
  EXPECT_EQ(Simulate(WriteSniffOffloadParameters(0x123)), kInterceptResume);
  EXPECT_TRUE(CheckCommandCompleteEventSent(
      CommandOpcode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS));

  EXPECT_TRUE(CheckSniffSubratingSent(0x123));
  EXPECT_TRUE(NoErrors());

  EXPECT_EQ(SimulateCommandStatus(MakeCommandStatus(
                CommandOpcode::SNIFF_SUBRATING, emboss::StatusCode::SUCCESS)),
            OkStatus());

  AdvanceTime(kDefaultLinkInactivityTimeout + kTick);

  EXPECT_TRUE(CheckSniffModeSent());
  EXPECT_TRUE(NoErrors());

  EXPECT_EQ(SimulateCommandStatus(
                MakeCommandStatus(CommandOpcode::SNIFF_MODE,
                                  emboss::StatusCode::UNKNOWN_CONNECTION_ID)),
            OkStatus());
  EXPECT_EQ(PopLastError(), Error({ErrorReason::kCommandStatusFailed}));

  EXPECT_TRUE(NoErrors());
  EXPECT_TRUE(packets_to_controller().empty());
  EXPECT_TRUE(packets_to_host().empty());
}

std::optional<SniffOffloadManager::CommandOpcode>
SniffOffloadManagerTest::SentCommand::opcode() {
  std::array<std::byte, emboss::CommandHeader::IntrinsicSizeInBytes()> scratch;
  auto span = buf->Get(scratch);
  emboss::CommandHeaderView view(span.data(), span.size());
  if (!view.Ok()) {
    return std::nullopt;
  }

  return CommandOpcode{cpp23::to_underlying(view.opcode().Read())};
}

bool SniffOffloadManagerTest::Error::operator==(const Error& other) const {
  return reason == other.reason && handle == other.handle;
}

SniffOffloadManagerTest::ControllerEvent
SniffOffloadManagerTest::ConnectionComplete(ConnectionHandle handle,
                                            emboss::StatusCode status,
                                            emboss::LinkType link_type) {
  auto alloc = internal_allocator().MakeUnique<std::byte[]>(
      emboss::ConnectionCompleteEvent::IntrinsicSizeInBytes());
  PW_CHECK(alloc != nullptr, "Test alloc failed");
  memset(alloc.get(), 0, alloc.size());

  MultiBuf::Instance buf{internal_allocator_};
  PW_CHECK(buf->TryReserveForPushBack());

  emboss::ConnectionCompleteEventWriter writer(alloc.get(), alloc.size());
  PW_CHECK(writer.IsComplete());
  writer.header().event_code().Write(emboss::EventCode::CONNECTION_COMPLETE);
  writer.header().parameter_total_size().Write(
      emboss::ConnectionCompleteEvent::IntrinsicSizeInBytes() -
      emboss::EventHeader::IntrinsicSizeInBytes());
  writer.link_type().Write(link_type);
  writer.connection_handle().Write(cpp23::to_underlying(handle));
  writer.status().Write(status);
  PW_CHECK(writer.Ok());

  buf->PushBack(std::move(alloc));
  EventCode event_code{
      cpp23::to_underlying(emboss::EventCode::CONNECTION_COMPLETE)};

  return {std::move(buf), event_code};
}

SniffOffloadManagerTest::ControllerEvent
SniffOffloadManagerTest::DisconnectionComplete(ConnectionHandle handle,
                                               emboss::StatusCode status,
                                               emboss::StatusCode reason) {
  auto alloc = internal_allocator().MakeUnique<std::byte[]>(
      emboss::DisconnectionCompleteEvent::IntrinsicSizeInBytes());
  PW_CHECK(alloc != nullptr, "Test alloc failed");
  memset(alloc.get(), 0, alloc.size());

  MultiBuf::Instance buf{internal_allocator_};
  PW_CHECK(buf->TryReserveForPushBack());

  emboss::DisconnectionCompleteEventWriter writer(alloc.get(), alloc.size());
  PW_CHECK(writer.IsComplete());
  writer.header().event_code().Write(emboss::EventCode::DISCONNECTION_COMPLETE);
  writer.header().parameter_total_size().Write(
      emboss::DisconnectionCompleteEvent::IntrinsicSizeInBytes() -
      emboss::EventHeader::IntrinsicSizeInBytes());
  writer.connection_handle().Write(cpp23::to_underlying(handle));
  writer.status().Write(status);
  writer.reason().Write(reason);
  PW_CHECK(writer.Ok());

  buf->PushBack(std::move(alloc));
  EventCode event_code{
      cpp23::to_underlying(emboss::EventCode::DISCONNECTION_COMPLETE)};

  return {std::move(buf), event_code};
}

SniffOffloadManagerTest::HostCommand
SniffOffloadManagerTest::WriteSniffOffloadEnable(
    bool enable,
    bool suppress_mode_change_event,
    bool suppress_sniff_subrating) {
  auto alloc = internal_allocator().MakeUnique<std::byte[]>(
      vendor::android_hci::WriteSniffOffloadEnableCommand::
          IntrinsicSizeInBytes());
  PW_CHECK(alloc != nullptr, "Test alloc failed");
  memset(alloc.get(), 0, alloc.size());

  MultiBuf::Instance buf{internal_allocator_};
  PW_CHECK(buf->TryReserveForPushBack());

  vendor::android_hci::WriteSniffOffloadEnableCommandWriter writer(
      alloc.get(), alloc.size());
  PW_CHECK(writer.IsComplete());
  writer.header().opcode().Write(
      emboss::OpCode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE);
  writer.header().parameter_total_size().Write(
      vendor::android_hci::WriteSniffOffloadEnableCommand::
          IntrinsicSizeInBytes() -
      emboss::CommandHeader::IntrinsicSizeInBytes());
  writer.enable().Write(emboss::GenericEnableParam{enable});
  writer.suppress_mode_change_event().Write(
      vendor::android_hci::SniffOffloadEventSuppression{
          suppress_mode_change_event});
  writer.suppress_sniff_subrating_event().Write(
      vendor::android_hci::SniffOffloadEventSuppression{
          suppress_sniff_subrating});
  writer.max_latency().Write(0x0002);
  writer.min_remote_timeout().Write(0);
  writer.min_local_timeout().Write(0);
  PW_CHECK(writer.Ok());

  buf->PushBack(std::move(alloc));
  SniffOffloadManager::CommandOpcode opcode{
      cpp23::to_underlying(emboss::OpCode::ANDROID_WRITE_SNIFF_OFFLOAD_ENABLE)};

  return {std::move(buf), opcode};
}

SniffOffloadManagerTest::HostCommand
SniffOffloadManagerTest::WriteSniffOffloadParameters(
    ConnectionHandle handle, SniffOffloadParameters&& parameters) {
  auto alloc = internal_allocator().MakeUnique<std::byte[]>(
      vendor::android_hci::WriteSniffOffloadParametersCommand::
          IntrinsicSizeInBytes());
  PW_CHECK(alloc != nullptr, "Test alloc failed");
  memset(alloc.get(), 0, alloc.size());

  MultiBuf::Instance buf{internal_allocator_};
  PW_CHECK(buf->TryReserveForPushBack());

  vendor::android_hci::WriteSniffOffloadParametersCommandWriter writer(
      alloc.get(), alloc.size());
  PW_CHECK(writer.IsComplete());
  writer.header().opcode().Write(
      emboss::OpCode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS);
  writer.header().parameter_total_size().Write(
      vendor::android_hci::WriteSniffOffloadParametersCommand::
          IntrinsicSizeInBytes() -
      emboss::CommandHeader::IntrinsicSizeInBytes());
  writer.connection_handle().Write(cpp23::to_underlying(handle));
  writer.max_interval().Write(parameters.sniff_max_interval);
  writer.min_interval().Write(parameters.sniff_min_interval);
  writer.attempts().Write(parameters.sniff_attempts);
  writer.sniff_timeout().Write(parameters.sniff_timeout);
  writer.link_inactivity_timeout().Write(parameters.link_inactivity_timeout);
  writer.max_latency().Write(parameters.subrating_max_latency);
  writer.min_remote_timeout().Write(parameters.subrating_min_local_timeout);
  writer.min_local_timeout().Write(parameters.subrating_min_local_timeout);
  writer.allow_exit_sniff_on_rx().Write(
      vendor::android_hci::SniffOffloadAllowExit{
          parameters.allow_exit_sniff_on_rx});
  writer.allow_exit_sniff_on_tx().Write(
      vendor::android_hci::SniffOffloadAllowExit{
          parameters.allow_exit_sniff_on_tx});
  PW_CHECK(writer.Ok());

  buf->PushBack(std::move(alloc));
  SniffOffloadManager::CommandOpcode opcode{cpp23::to_underlying(
      emboss::OpCode::ANDROID_WRITE_SNIFF_OFFLOAD_PARAMETERS)};

  return {std::move(buf), opcode};
}

SniffOffloadManagerTest::ControllerEvent
SniffOffloadManagerTest::ModeChangeEvent(uint16_t connection_handle,
                                         emboss::AclConnectionMode mode) {
  auto alloc = internal_allocator().MakeUnique<std::byte[]>(
      emboss::ModeChangeEvent::IntrinsicSizeInBytes());
  PW_CHECK(alloc != nullptr, "Test alloc failed");
  memset(alloc.get(), 0, alloc.size());

  MultiBuf::Instance buf{internal_allocator_};
  PW_CHECK(buf->TryReserveForPushBack());

  emboss::ModeChangeEventWriter writer(alloc.get(), alloc.size());
  PW_CHECK(writer.IsComplete());
  writer.header().event_code().Write(emboss::EventCode::MODE_CHANGE);
  writer.header().parameter_total_size().Write(
      emboss::ModeChangeEvent::IntrinsicSizeInBytes() -
      emboss::EventHeader::IntrinsicSizeInBytes());
  writer.connection_handle().Write(connection_handle);
  writer.current_mode().Write(mode);
  PW_CHECK(writer.Ok());

  buf->PushBack(std::move(alloc));
  EventCode event_code{cpp23::to_underlying(emboss::EventCode::MODE_CHANGE)};

  return {std::move(buf), event_code};
}

SniffOffloadManagerTest::ControllerEvent
SniffOffloadManagerTest::SniffSubratingEvent(
    [[maybe_unused]] uint16_t connection_handle) {
  // TODO: pwbug.dev/482481726 - Need emboss definition for this.
  auto alloc = internal_allocator().MakeUnique<std::byte[]>(
      emboss::EventHeader::IntrinsicSizeInBytes());
  PW_CHECK(alloc != nullptr, "Test alloc failed");
  memset(alloc.get(), 0, alloc.size());

  MultiBuf::Instance buf{internal_allocator_};
  PW_CHECK(buf->TryReserveForPushBack());

  emboss::EventHeaderWriter writer(alloc.get(), alloc.size());
  PW_CHECK(writer.IsComplete());
  writer.event_code().Write(emboss::EventCode::SNIFF_SUBRATING);
  writer.parameter_total_size().Write(0);
  PW_CHECK(writer.Ok());

  buf->PushBack(std::move(alloc));
  EventCode event_code{
      cpp23::to_underlying(emboss::EventCode::SNIFF_SUBRATING)};

  return {std::move(buf), event_code};
}

MultiBuf::Instance SniffOffloadManagerTest::MakeCommandStatus(
    SniffOffloadManager::CommandOpcode opcode, emboss::StatusCode status) {
  auto alloc = internal_allocator().MakeUnique<std::byte[]>(
      emboss::CommandStatusEvent::IntrinsicSizeInBytes());
  PW_CHECK(alloc != nullptr, "Test alloc failed");
  memset(alloc.get(), 0, alloc.size());

  MultiBuf::Instance buf{internal_allocator_};
  PW_CHECK(buf->TryReserveForPushBack());

  emboss::CommandStatusEventWriter writer(alloc.get(), alloc.size());
  PW_CHECK(writer.IsComplete());
  writer.header().event_code().Write(emboss::EventCode::COMMAND_STATUS);
  writer.header().parameter_total_size().Write(
      emboss::CommandStatusEvent::IntrinsicSizeInBytes() -
      emboss::EventHeader::IntrinsicSizeInBytes());
  writer.status().Write(status);
  writer.command_opcode_enum().Write(opcode);
  PW_CHECK(writer.Ok());

  buf->PushBack(std::move(alloc));
  return buf;
}

void SniffOffloadManagerTest::SimulateAclActivity(
    uint16_t handle, SniffOffloadManager::Direction direction, bool valid) {
  if (!valid) {
    sniff_offload_manager().ProcessAclPacket(MakeZeroBuffer(), direction);
    SyncWithDispatcher();
    return;
  }

  auto alloc = internal_allocator().MakeUnique<std::byte[]>(
      emboss::AclDataFrameHeader::IntrinsicSizeInBytes());
  PW_CHECK(alloc != nullptr, "Test alloc failed");
  memset(alloc.get(), 0, alloc.size());

  MultiBuf::Instance buf{internal_allocator_};
  PW_CHECK(buf->TryReserveForPushBack());

  emboss::AclDataFrameHeaderWriter writer(alloc.get(), alloc.size());
  PW_CHECK(writer.IsComplete());
  writer.handle().Write(handle);
  writer.packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
  writer.broadcast_flag().Write(
      emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
  writer.data_total_length().Write(0);
  PW_CHECK(writer.Ok());

  buf->PushBack(std::move(alloc));
  sniff_offload_manager().ProcessAclPacket(std::move(buf), direction);
  SyncWithDispatcher();
}

MultiBuf::Instance SniffOffloadManagerTest::MakeZeroBuffer(size_t size) {
  MultiBuf::Instance buf{internal_allocator()};
  PW_CHECK(buf->TryReserveForPushBack());

  if (size > 0) {
    auto alloc = internal_allocator().MakeUnique<std::byte[]>(size);
    PW_CHECK(alloc != nullptr, "Test alloc failed");
    memset(alloc.get(), 0, alloc.size());
    buf->PushBack(std::move(alloc));
  }

  return buf;
}

#define VERIFY_OR_RETURN(pred)                                            \
  do {                                                                    \
    if (!(pred)) {                                                        \
      PW_LOG_INFO("%s:%d: `" #pred "` unsatisfied.", __FILE__, __LINE__); \
      return false;                                                       \
    }                                                                     \
  } while (0)

bool SniffOffloadManagerTest::CheckSniffSubratingSent(
    std::optional<uint16_t> connection_handle) {
  VERIFY_OR_RETURN(!packets_to_controller().empty());

  std::array<std::byte, emboss::SniffSubratingCommand::IntrinsicSizeInBytes()>
      scratch;
  auto span = packets_to_controller().front().buf->Get(scratch);
  emboss::SniffSubratingCommandView view(span.data(), span.size());

  VERIFY_OR_RETURN(view.Ok());
  VERIFY_OR_RETURN(view.header().opcode().Read() ==
                   emboss::OpCode::SNIFF_SUBRATING);

  if (connection_handle.has_value()) {
    VERIFY_OR_RETURN(view.connection_handle().Read() == *connection_handle);
  }

  packets_to_controller().pop_front();
  return true;
}

bool SniffOffloadManagerTest::CheckSniffModeSent(
    std::optional<uint16_t> connection_handle) {
  VERIFY_OR_RETURN(!packets_to_controller().empty());

  std::array<std::byte, emboss::SniffModeCommand::IntrinsicSizeInBytes()>
      scratch;
  auto span = packets_to_controller_.front().buf->Get(scratch);
  emboss::SniffModeCommandView view(span.data(), span.size());

  VERIFY_OR_RETURN(view.Ok());
  VERIFY_OR_RETURN(view.header().opcode().Read() == emboss::OpCode::SNIFF_MODE);

  if (connection_handle.has_value()) {
    VERIFY_OR_RETURN(view.connection_handle().Read() == *connection_handle);
  }

  packets_to_controller_.pop_front();
  return true;
}

bool SniffOffloadManagerTest::CheckSniffExitSent(
    std::optional<uint16_t> connection_handle) {
  VERIFY_OR_RETURN(!packets_to_controller().empty());

  std::array<std::byte, emboss::ExitSniffModeCommand::IntrinsicSizeInBytes()>
      scratch;
  auto span = packets_to_controller().front().buf->Get(scratch);
  emboss::ExitSniffModeCommandView view(span.data(), span.size());

  VERIFY_OR_RETURN(view.Ok());
  VERIFY_OR_RETURN(view.header().opcode().Read() ==
                   emboss::OpCode::EXIT_SNIFF_MODE);

  if (connection_handle.has_value()) {
    VERIFY_OR_RETURN(view.connection_handle().Read() == *connection_handle);
  }

  packets_to_controller_.pop_front();
  return true;
}

bool SniffOffloadManagerTest::CheckCommandCompleteEventSent(
    std::optional<CommandOpcode> opcode) {
  VERIFY_OR_RETURN(!packets_to_host().empty());

  std::array<std::byte, emboss::CommandCompleteEvent::IntrinsicSizeInBytes()>
      scratch;
  auto span = packets_to_host().front()->Get(scratch);
  emboss::CommandCompleteEventView view(span.data(), span.size());

  VERIFY_OR_RETURN(view.Ok());
  VERIFY_OR_RETURN(view.header().event_code().Read() ==
                   emboss::EventCode::COMMAND_COMPLETE);

  if (opcode.has_value()) {
    VERIFY_OR_RETURN(view.command_opcode().Read() == *opcode);
  }

  packets_to_host().pop_front();
  return true;
}

bool SniffOffloadManagerTest::CheckCommandStatusEventSent(
    std::optional<CommandOpcode> opcode,
    std::optional<emboss::StatusCode> status) {
  VERIFY_OR_RETURN(!packets_to_host().empty());

  std::array<std::byte, emboss::CommandStatusEvent::IntrinsicSizeInBytes()>
      scratch;
  auto span = packets_to_host().front()->Get(scratch);
  emboss::CommandStatusEventView view(span.data(), span.size());

  VERIFY_OR_RETURN(view.Ok());
  VERIFY_OR_RETURN(view.header().event_code().Read() ==
                   emboss::EventCode::COMMAND_STATUS);

  if (opcode.has_value()) {
    VERIFY_OR_RETURN(view.command_opcode_enum().Read() == *opcode);
  }
  if (status.has_value()) {
    VERIFY_OR_RETURN(view.status().Read() == *status);
  }

  packets_to_host().pop_front();
  return true;
}

#undef VERIFY_OR_RETURN

}  // namespace
}  // namespace pw::bluetooth::proxy::hci
