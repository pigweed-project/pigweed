// Copyright 2025 The Pigweed Authors
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

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_async2/poll.h"
#include "pw_async2/time_provider.h"
#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth/hci_events.emb.h"
#include "pw_bluetooth_proxy/hci/internal/type_safe_ids.h"
#include "pw_chrono/system_clock.h"
#include "pw_chrono/system_timer.h"
#include "pw_containers/dynamic_queue.h"
#include "pw_containers/intrusive_map.h"
#include "pw_function/function.h"
#include "pw_multibuf/multibuf_v2.h"
#include "pw_result/expected.h"
#include "pw_result/result.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"

namespace pw::bluetooth::proxy::hci {

struct CommandPacket final {
  MultiBuf::Instance buffer;
};

struct EventPacket final {
  MultiBuf::Instance buffer;
};

/// A failure (non-OK) status and a returned buffer, used when a call would
/// normally consume some data but should be returned on failure.
class FailureWithBuffer {
 public:
  FailureWithBuffer(pw::Status status, MultiBuf::Instance&& buffer)
      : status_(status), buffer_(std::move(buffer)) {
    // Status must not be `OK`, this type should only be used for failure cases.
    PW_ASSERT(!status.ok());
  }

  FailureWithBuffer() = delete;
  FailureWithBuffer(const FailureWithBuffer&) = delete;
  FailureWithBuffer(FailureWithBuffer&&) = default;
  FailureWithBuffer& operator=(const FailureWithBuffer&) = delete;
  FailureWithBuffer& operator=(FailureWithBuffer&&) = default;

  /// Get the status code of the failure. Will never be `OK`.
  [[nodiscard]] pw::Status status() { return status_; }

  /// Get a reference to the buffer.
  [[nodiscard]] MultiBuf& buffer() { return buffer_; }
  [[nodiscard]] const MultiBuf& buffer() const { return buffer_; }

  /// Take the buffer out of this object.
  [[nodiscard]] MultiBuf::Instance TakeBuffer() { return std::move(buffer_); }

 private:
  pw::Status status_;
  MultiBuf::Instance buffer_;
};

// The subevent code of vendor-specific debug events (event code 0xFF).
// See Core Specification v6.1, Vol 4, Part E, Section 5.4.4.
// The Core Specification does not specify subevent codes, but they are used
// by Android vendor events. The subevent code is the first event parameter.
// See
// https://source.android.com/docs/core/connect/bluetooth/hci_requirements#hci-event-vendor-specific.
// This is an enum class for strong typing.
enum class VendorDebugSubEventCode : uint8_t {};

// The Opcode to intercept for Command Complete events (event code 0x0E).
//
// See Core Specification v6.1, Vol 4, Part E, Section 7.7.14.
class CommandCompleteOpcode {
  using Self = CommandCompleteOpcode;

 public:
  pw::bluetooth::emboss::OpCode code;

  bool operator<(Self right) const { return code < right.code; }
  bool operator>(Self right) const { return code > right.code; }
  bool operator<=(Self right) const { return code <= right.code; }
  bool operator>=(Self right) const { return code >= right.code; }
  bool operator==(Self right) const { return code == right.code; }
  bool operator!=(Self right) const { return code != right.code; }
};

// The Opcode to intercept for Command Status events (event code 0x0F).
//
// See Core Specification v6.1, Vol 4, Part E, Section 7.7.15.
class CommandStatusOpcode {
  using Self = CommandStatusOpcode;

 public:
  pw::bluetooth::emboss::OpCode code;

  bool operator<(Self right) const { return code < right.code; }
  bool operator>(Self right) const { return code > right.code; }
  bool operator<=(Self right) const { return code <= right.code; }
  bool operator>=(Self right) const { return code >= right.code; }
  bool operator==(Self right) const { return code == right.code; }
  bool operator!=(Self right) const { return code != right.code; }
};

class CommandMultiplexer final {
 public:
  static constexpr chrono::SystemClock::duration kDefaultCommandTimeout =
      std::chrono::seconds(30);

  using InterceptorId = Identifier<uint16_t>;

  // InterceptorAction: Continue intercepting this.
  struct ContinueIntercepting {};
  // InterceptorAction: Remove this interceptor, this invalidates the
  // InterceptorId, so it must be supplied in the response.
  struct RemoveThisInterceptor {
    InterceptorId id;
  };
  // This return value enables handler functions to safely and conveniently
  // remove the interceptor from within the function.
  using InterceptorAction =
      std::variant<ContinueIntercepting, RemoveThisInterceptor>;

  // The return type of CommandHandler.
  struct CommandInterceptorReturn {
    // If a command is returned it will be sent to the controller. Return
    // nullopt to prevent the command from being propagated.
    std::optional<CommandPacket> command = std::nullopt;
    InterceptorAction action = ContinueIntercepting{};
  };

  using CommandHandler =
      pw::Function<CommandInterceptorReturn(CommandPacket&& command_packet)>;

  // The return type of EventHandler.
  struct EventInterceptorReturn {
    // If an event is returned it will be sent to the host. Return nullopt to
    // prevent the event from being propagated.
    std::optional<EventPacket> event = std::nullopt;
    InterceptorAction action = ContinueIntercepting{};
  };

  using EventHandler =
      pw::Function<EventInterceptorReturn(EventPacket&& event_packet)>;

  using EventCodeVariant = std::variant<pw::bluetooth::emboss::EventCode,
                                        pw::bluetooth::emboss::LeSubEventCode,
                                        VendorDebugSubEventCode,
                                        CommandCompleteOpcode,
                                        CommandStatusOpcode>;

  class EventInterceptor;
  class CommandInterceptor;

  // Common interceptor implementation.
  class Interceptor {
   public:
    Interceptor() = delete;
    Interceptor(const Interceptor&) = delete;
    Interceptor(Interceptor&& other) = default;
    Interceptor& operator=(const Interceptor&) = delete;
    Interceptor& operator=(Interceptor&&) = default;

    ~Interceptor() {
      if (multiplexer_ && id_.is_valid()) {
        multiplexer_->UnregisterInterceptor(std::move(id_));
      }
    }

    InterceptorId& id() { return id_; }
    const InterceptorId& id() const { return id_; }

   private:
    friend class EventInterceptor;
    friend class CommandInterceptor;

    explicit Interceptor(CommandMultiplexer& multiplexer, InterceptorId&& id)
        : multiplexer_(&multiplexer), id_(std::move(id)) {}

    CommandMultiplexer* multiplexer_;
    InterceptorId id_;
  };

  // Destroying the EventInterceptor object will unregister the interceptor.
  // Must not outlive the CommandMultiplexer that created it. Do not destroy an
  // interceptor in an interception callback, instead return a
  // `RemoveThisInterceptor` value with the interceptor's ID to remove it. An
  // interceptor which has had its ID moved from can be destroyed at any time
  // with no effect.
  class EventInterceptor final : public Interceptor {
   private:
    friend class CommandMultiplexer;
    explicit EventInterceptor(CommandMultiplexer& multiplexer,
                              InterceptorId&& id)
        : Interceptor(multiplexer, std::move(id)) {}
  };

  // Destroying the CommandInterceptor object will unregister the interceptor.
  // Must not outlive the CommandMultiplexer that created it. Do not destroy an
  // interceptor in an interception callback, instead return a
  // `RemoveThisInterceptor` value with the interceptor's ID to remove it. An
  // interceptor which has had its ID moved from can be destroyed at any time
  // with no effect.
  class CommandInterceptor final : public Interceptor {
   private:
    friend class CommandMultiplexer;
    explicit CommandInterceptor(CommandMultiplexer& multiplexer,
                                InterceptorId&& id)
        : Interceptor(multiplexer, std::move(id)) {}
  };

  /// Creates a `CommandMultiplexer` that will process HCI command and event
  /// packets.
  /// @param[in] allocator - The allocator to use for buffers and internal data
  /// structures.
  /// @param[in] send_to_host_fn - Callback that will be called when proxy wants
  /// to send H4 HCI event packets towards the host. All unhandled events will
  /// be forwarded to the host, so this is also the default event handler.
  /// @param[in] send_to_controller_fn - Callback that will be called when
  /// proxy wants to send H4 HCI command packets towards the controller.
  /// @param[in] time_provider - The TimeProvider to use for command timeouts.
  /// @param[in] command_timeout - The duration to wait for a command to
  /// timeout.
  CommandMultiplexer(
      Allocator& allocator,
      Function<void(MultiBuf::Instance&& h4_packet)>&& send_to_host_fn,
      Function<void(MultiBuf::Instance&& h4_packet)>&& send_to_controller_fn,
      async2::TimeProvider<chrono::SystemClock>& time_provider,
      chrono::SystemClock::duration command_timeout = kDefaultCommandTimeout);

  /// This constructor will use chrono::SystemTimer for command timeouts.
  /// Prefer to use the other constructor with async2::TimeProvider if possible.
  /// @param[in] timeout_fn - Function to call when a command times out. This
  /// may be called from an interrupt, so it must be IRQ safe, minimal, and non-
  /// blocking. If you need a more complex timeout handler, use the async2
  /// alternative.
  /// @param[in] command_timeout - The duration to wait for a command to
  /// timeout.
  CommandMultiplexer(
      Allocator& allocator,
      Function<void(MultiBuf::Instance&& h4_packet)>&& send_to_host_fn,
      Function<void(MultiBuf::Instance&& h4_packet)>&& send_to_controller_fn,
      Function<void()> timeout_fn,
      chrono::SystemClock::duration command_timeout = kDefaultCommandTimeout);

  ~CommandMultiplexer();

  /// Asynchronously poll/pend for a command timeout. Only 1 client/waker is
  /// supported at a time.
  ///
  /// @returns A Result<async2::Poll> where
  /// * Ok(Pending): Timeout pending
  /// * Ok(Ready): Timeout has expired.
  /// * Unimplemented: This instance was not configured for async2
  ///   timeout handling.
  Result<async2::Poll<>> PendCommandTimeout(async2::Context& cx);

  /// Called by container to ask CommandMultiplexer to handle a H4 packet
  /// sent from the host side towards the controller side.
  /// CommandMultiplexer will in turn call the `send_to_controller_fn`
  /// provided during construction to pass the packet on to the controller. Some
  /// packets may be modified, added, or removed.
  void HandleH4FromHost(MultiBuf::Instance&& h4_packet);

  /// Called by container to ask CommandMultiplexer to handle a H4 packet
  /// sent from the controller side towards the host side.
  /// CommandMultiplexer will in turn call the `send_to_host_fn` provided
  /// during construction to pass the packet on to the host. Some packets may be
  /// modified, added, or removed.
  void HandleH4FromController(MultiBuf::Instance&& h4_packet);

  /// This method will bypass `RegisterCommandInterceptor` interceptors.
  /// @param[in] command - The command buffer to send.
  /// @param[in] event_handler - The function to call with all events related to
  /// the command transaction (e.g. Command_Complete, Command_Status,
  /// Remote_Name_Request_Complete). If the command results in a Command_Status
  /// event, it will be sent to this callback before the event with
  /// `complete_event_code` is sent, so the event code must be checked if
  /// multiple events are expected. If the Command_Complete or Command_Status
  /// events report an error, no further events will be sent.
  /// @param[in] complete_event_code - The event code of the event that will
  /// complete the command procedure with the controller. This is
  /// Command_Complete for most commands.
  /// @param[in] exclusions - Opcodes for commands that this command cannot run
  /// concurrently with.
  /// @returns A pw::expected<void, FailureWithBuffer> where either:
  /// * has_value(): Command was successfully queued.
  /// * error().status():
  ///   * @UNAVAILABLE: The resources (e.g. memory) required to queue this
  ///     command are not available.
  ///   * @INVALID_ARGUMENT: Command buffer is invalid.
  pw::expected<void, FailureWithBuffer> SendCommand(
      CommandPacket&& command,
      EventHandler&& event_handler,
      EventCodeVariant complete_event_code =
          pw::bluetooth::emboss::EventCode::COMMAND_COMPLETE,
      pw::span<pw::bluetooth::emboss::OpCode> exclusions = {});

  /// Send an HCI event towards the host. This method will bypass event
  /// interceptors registered with `RegisterEventInterceptor`.
  ///
  /// The Num_HCI_Command_Packets field in Command Status and Command
  /// Complete packets may be overwritten by this class for flow control
  /// purposes.
  ///
  /// @param[in] event - The event packet to send to the host.
  ///
  /// @returns A pw::expected<void, FailureWithBuffer> where either:
  /// * has_value(): Event was successfully sent.
  /// * error().status():
  ///   * @UNAVAILABLE: The resources (e.g. memory) required to send this event
  ///     are not available.
  ///   * @INVALID_ARGUMENT: Event buffer is invalid.
  pw::expected<void, FailureWithBuffer> SendEvent(EventPacket&& event);

  /// Configure a handler function for intercepting an event received from the
  /// controller. Only one interceptor per event code is allowed, with the
  /// exception of Command Complete and Command Status, which must have one
  /// interceptor per command opcode.
  ///
  /// @param[in] event_code - The event code of the event to intercept.
  ///
  /// @param[in] handler - The event handler function to call with matching
  /// events.
  ///
  /// @returns an EventInterceptor handle on success. On failure, returns:
  /// * @ALREADY_EXISTS: An interceptor is already registered for the event
  ///   code.
  /// * @INVALID_ARGUMENTS: An event code of CommandComplete or CommandStatus
  ///   was used, to intercept these, use a specific OpCode.
  Result<EventInterceptor> RegisterEventInterceptor(EventCodeVariant event_code,
                                                    EventHandler&& handler);

  /// Configure a handler function for intercepting a command sent from the
  /// host. Only one interceptor per opcode is allowed.
  ///
  /// @param[in] op_code - The op code of the command to be intercepted.
  ///
  /// @param[in] handler - The function to call with the intercepted command.
  ///
  /// @returns a CommandInterceptor handle on success. On failure, returns:
  /// * @ALREADY_EXISTS: An interceptor is already registered for the opcode.
  Result<CommandInterceptor> RegisterCommandInterceptor(
      pw::bluetooth::emboss::OpCode op_code, CommandHandler&& handler);

 private:
  static_assert(std::variant_size_v<EventCodeVariant> == 5,
                "Event code may not be handled in max header size.");
  static constexpr size_t kMaxEventHeaderSize =
      std::max({emboss::EventHeader::IntrinsicSizeInBytes(),
                emboss::VendorDebugEvent::IntrinsicSizeInBytes(),
                emboss::CommandCompleteEvent::IntrinsicSizeInBytes(),
                emboss::CommandStatusEvent::IntrinsicSizeInBytes()});
  using EventCodeValue = std::underlying_type_t<emboss::EventCode>;

  void UnregisterInterceptor(InterceptorId id);
  std::optional<InterceptorId> AllocateInterceptorId()
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Because we use intrusive maps, we need to have a small type hierarchy,
  // wrapping the actual state inside a separate type to ensure the types are
  // inheritance-independent for the maps. Fortunately we can keep all of this
  // private, and the implementation details in the .cc file so the complexity
  // doesn't leak into client code.
  //
  // Once `DynamicMap` is supported, most of the complexity here goes away.
  class InterceptorStateWrapper;
  class EventInterceptorState;
  class EventInterceptorWrapper;
  class CommandInterceptorState;
  class CommandInterceptorWrapper;

  using InterceptorMap =
      pw::IntrusiveMap<InterceptorId::ValueType, InterceptorStateWrapper>;
  using EventInterceptorMap =
      pw::IntrusiveMap<EventCodeVariant, EventInterceptorState>;
  using CommandInterceptorMap =
      pw::IntrusiveMap<pw::bluetooth::emboss::OpCode, CommandInterceptorState>;

  EventInterceptorMap::iterator FindEventInterceptor(EventCodeValue event,
                                                     ConstByteSpan span)
      PW_EXCLUSIVE_LOCKS_REQUIRED(event_interceptors_mutex_);
  EventInterceptorMap::iterator FindCommandComplete(ConstByteSpan span)
      PW_EXCLUSIVE_LOCKS_REQUIRED(event_interceptors_mutex_);
  EventInterceptorMap::iterator FindCommandStatus(ConstByteSpan span)
      PW_EXCLUSIVE_LOCKS_REQUIRED(event_interceptors_mutex_);
  EventInterceptorMap::iterator FindLeMetaEvent(ConstByteSpan span)
      PW_EXCLUSIVE_LOCKS_REQUIRED(event_interceptors_mutex_);
  EventInterceptorMap::iterator FindVendorDebug(ConstByteSpan span)
      PW_EXCLUSIVE_LOCKS_REQUIRED(event_interceptors_mutex_);

  void RemoveCommandInterceptor(InterceptorId id)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_, command_interceptors_mutex_);
  void RemoveEventInterceptor(InterceptorId id)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_, event_interceptors_mutex_);
  void DeleteInterceptor(InterceptorMap::iterator iterator)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void SendToHost(MultiBuf::Instance&& buf) PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void SendToControllerOrQueue(MultiBuf::Instance&& buf)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  uint8_t UpdateNumHciCommandPackets(uint8_t num_hci_command_packets)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  uint8_t TryReserveQueueSpace(uint8_t requested)
      PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void ProcessQueue() PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Internal data for queued commands.
  struct QueuedCommandState {
    CommandPacket packet;
  };
  using CommandQueue = pw::DynamicDeque<QueuedCommandState>;
  using QueueSize = CommandQueue::size_type;

  // Space reserved in the command queue, clamped to the range of a `uint8_t`.
  uint8_t reserved_queue_space() PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  Allocator& allocator_;
  pw::sync::Mutex mutex_;
  pw::sync::Mutex event_interceptors_mutex_ PW_ACQUIRED_BEFORE(mutex_);
  pw::sync::Mutex command_interceptors_mutex_
      PW_ACQUIRED_BEFORE(mutex_, event_interceptors_mutex_);

  // Owning map, use DynamicMap if/when available, this would also let us unwrap
  // the "Wrapper" classes above and use state directly.
  InterceptorMap interceptors_ PW_GUARDED_BY(mutex_);
  EventInterceptorMap event_interceptors_
      PW_GUARDED_BY(event_interceptors_mutex_);
  CommandInterceptorMap command_interceptors_
      PW_GUARDED_BY(command_interceptors_mutex_);
  IdentifierMint<InterceptorId::ValueType> id_mint_ PW_GUARDED_BY(mutex_);

  // Outgoing command queue.
  CommandQueue command_queue_ PW_GUARDED_BY(mutex_);
  size_t command_credits_ PW_GUARDED_BY(mutex_) = 1u;

  Function<void(MultiBuf::Instance&& h4_packet)> send_to_host_fn_;
  Function<void(MultiBuf::Instance&& h4_packet)> send_to_controller_fn_;
};

using EventInterceptor = CommandMultiplexer::EventInterceptor;
using CommandInterceptor = CommandMultiplexer::CommandInterceptor;

}  // namespace pw::bluetooth::proxy::hci
