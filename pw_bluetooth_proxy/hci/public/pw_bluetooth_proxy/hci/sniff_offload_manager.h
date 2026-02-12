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

#pragma once

#include <optional>

#include "pw_allocator/allocator.h"
#include "pw_async2/channel.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/system_time_provider.h"
#include "pw_async2/time_provider.h"
#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth_proxy/hci/types.h"
#include "pw_containers/intrusive_map.h"
#include "pw_function/function.h"
#include "pw_multibuf/v2/multibuf.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"

namespace pw::bluetooth::proxy::hci {

using ::pw::multibuf::v2::MultiBuf;

/// Manage sniff offload, a feature defined in Android bluetooth extensions that
/// allows the host to offload managing the sniff state on connections to the
/// proxy.
///
/// This class is thread-safe. All external accesses will be synchronized
/// internally.
///
/// @see
/// https://source.android.com/docs/core/connect/bluetooth/hci_requirements#sniff-offload
class SniffOffloadManager final {
 public:
  /// Direction of ACL traffic, as defined in the Android sniff offload
  /// requirements.
  enum class Direction : bool {
    /// Packets passing from the controller to the host.
    kControllerToHost,
    /// Packets passing from the host to the controller.
    kHostToController,

    /// Receive-direction HCI-ACL is defined as ACL packet transmission from
    /// controller to host over HCI.
    kRx = kControllerToHost,
    /// Transmit-direction HCI-ACL is defined as ACL packet transmission from
    /// host to controller over HCI.
    kTx = kHostToController,
  };

  /// Specify what to do when processing a command/event.
  struct [[nodiscard]] HandlerAction final {
    /// Whether to intercept the packet or allow it to pass through.
    enum class Interception : bool {
      kPassthrough = false,
      kIntercept = true,
    };

    /// Whether to continue intercepting the command/event or stop.
    enum class Resumption : bool {
      kStop = false,
      kResume = true,
    };

    Interception intercept = Interception::kPassthrough;
    Resumption resume = Resumption::kResume;

    bool operator==(const HandlerAction& other) const;
  };

  /// Specify the reason an error is reported.
  enum class ErrorReason {
    /// Indicated when the manager cannot allocate space for connection state.
    kCannotAllocateConnection,
    /// Indicated when the manager cannot allocate space for a buffer for a
    /// command or event.
    kCannotAllocateBuffer,
    /// Indicated when seeing a connection complete for a connection that's
    /// already being tracked.
    kConnectionAlreadyExists,
    /// Indicated when seeing ACL traffic for an unknown ACL connection.
    kConnectionNotFound,
    /// Indicated when an invalid packet is seen.
    kInvalidPacket,
    /// Indicated when a SendCommand call returns a failure.
    kSendCommandFailed,
    /// Indicated when a SendEvent call returns a failure.
    kSendEventFailed,
    /// Indicated when a sent command responds with a failure status.
    kCommandStatusFailed,
  };

  /// Specify what to do when an error occurs.
  enum class [[nodiscard]] ErrorAction {
    // Ignore the error and carry on, if possible.
    kIgnore,
    // Disable sniff offload.
    kDisable,
  };

  /// Specify what event completes a command.
  enum class CompletionEvent {
    kCommandStatus,
    kCommandComplete,
  };

  using CommandOpcode = emboss::OpCode;
  using EventCode = emboss::EventCode;

  using SendCommandFunc = Function<Status(MultiBuf::Instance&& command_packet,
                                          CompletionEvent completion)>;
  using SendEventFunc = Function<Status(MultiBuf::Instance&& event_packet)>;
  using OnErrorFunc = Function<ErrorAction(
      ErrorReason reason, std::optional<ConnectionHandle> connection_handle)>;

  SniffOffloadManager(allocator::Allocator& allocator,
                      async2::Dispatcher& dispatcher,
                      SendCommandFunc&& send_command,
                      SendEventFunc&& send_event,
                      OnErrorFunc&& on_error = nullptr,
                      async2::TimeProvider<chrono::SystemClock>& time_provider =
                          async2::GetSystemTimeProvider());

  ~SniffOffloadManager();

  /// Process ACL packets for managing sniff state. Does not modify the packet
  /// in any way. Must provide all ACL traffic or sniff state can desync with
  /// connections.
  void ProcessAclPacket(const MultiBuf& acl_packet, Direction direction)
      PW_LOCKS_EXCLUDED(mutex_);

  /// Process a command packet. Forward all
  ///   * `ANDROID_Write_Sniff_Offload_Enable`,
  ///   * `ANDROID_Write_Sniff_Offload_Parameters`,
  ///   * `HCI_Sniff_Mode`, and
  ///   * `HCI_Exit_Sniff_Mode`
  /// events. All others will do nothing and return `{.resume = kStop}`.
  HandlerAction ProcessCommand(const MultiBuf& command_packet,
                               CommandOpcode opcode) PW_LOCKS_EXCLUDED(mutex_);

  /// Indicate that a previously sent command has completed.
  ///
  /// @return
  ///   * @OK                   - Processing completed successfully.
  ///   * @INVALID_ARGUMENT     - Event packet is invalid.
  ///   * @NOT_FOUND            - Associated connection does not exist.
  ///   * @FAILED_PRECONDITION  - No active command with that opcode (for the
  ///                             associated connection)
  Status ProcessCommandStatus(const MultiBuf& event_packet)
      PW_LOCKS_EXCLUDED(mutex_);

  /// Process an event packet. Forward all
  ///   * `HCI_Mode_Change`,
  ///   * `HCI_Connection_Complete`,
  ///   * `HCI_Disconnection_Complete`, and
  ///   * `HCI_Sniff_Subrating`
  /// events. All others will do nothing and return `{.resume = kStop}`.
  HandlerAction ProcessEvent(const MultiBuf& event_packet, EventCode event_code)
      PW_LOCKS_EXCLUDED(mutex_);

 private:
  // Finite state machine managing the sniff state of a single connection.
  class ConnectionFsm;

  // Wrappers for injected functions.
  void SendCommand(MultiBuf::Instance&& command_packet,
                   CompletionEvent completion,
                   std::optional<ConnectionHandle> connection_handle =
                       std::nullopt) PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void SendEvent(MultiBuf::Instance&& event_packet,
                 std::optional<ConnectionHandle> connection_handle =
                     std::nullopt) PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void OnError(ErrorReason reason,
               std::optional<ConnectionHandle> connection_handle = std::nullopt)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Command handlers.
  HandlerAction ProcessWriteSniffOffloadEnable(const MultiBuf& command_packet)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  HandlerAction ProcessWriteSniffOffloadParameters(
      const MultiBuf& command_packet) PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  HandlerAction ProcessSniffMode(const MultiBuf& command_packet)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  HandlerAction ProcessExitSniffMode(const MultiBuf& command_packet)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Event handlers.
  HandlerAction ProcessModeChange(const MultiBuf& event_packet)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  HandlerAction ProcessConnectionComplete(const MultiBuf& event_packet)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  HandlerAction ProcessDisconnectionComplete(const MultiBuf& event_packet)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  HandlerAction ProcessSniffSubrating(const MultiBuf& event_packet)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Internal types.
  struct Disabled final {};
  struct Enabled final {
    uint16_t subrating_max_latency;
    uint16_t subrating_min_remote_timeout;
    uint16_t subrating_min_local_timeout;
  };
  using ConnectionMap = pw::IntrusiveMap<uint16_t, ConnectionFsm>;

  enum class CommandStatus {
    kSuccess,
    kBadArguments,
    kUnknownConnection,
  };

  struct ConnectionFinishedClosing final {
    ConnectionHandle handle;
    ConnectionFsm& fsm;
  };

  // Internal actions and helpers.
  void SendCommandComplete(uint16_t opcode) PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void SendCommandStatus(uint16_t opcode, CommandStatus status)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void DoDisable() PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void DoEnable(Enabled&& enabled) PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Result<MultiBuf::Instance> AllocateBuffer(ConstByteSpan span)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Utility members.
  sync::Mutex mutex_;
  Allocator& allocator_ PW_GUARDED_BY(mutex_);

  // Async members.
  async2::Dispatcher& dispatcher_;
  async2::TimeProvider<chrono::SystemClock>& time_provider_;

  // Injected functions.
  const SendCommandFunc send_command_;
  const SendEventFunc send_event_;
  const OnErrorFunc on_error_;

  // Internal state.
  std::variant<Disabled, Enabled> state_ PW_GUARDED_BY(mutex_) = Disabled{};
  ConnectionMap connections_ PW_GUARDED_BY(mutex_);

  bool suppress_mode_change_event_ PW_GUARDED_BY(mutex_) = false;
  bool suppress_sniff_subrating_event_ PW_GUARDED_BY(mutex_) = false;
};

}  // namespace pw::bluetooth::proxy::hci
