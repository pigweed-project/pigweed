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

#include "pw_bluetooth_proxy/hci/command_multiplexer.h"

#include "lib/stdcompat/utility.h"
#include "pw_assert/check.h"
#include "pw_bluetooth/hci_events.emb.h"
#include "pw_bluetooth/hci_h4.emb.h"
#include "pw_log/log.h"
#include "pw_multibuf/multibuf_v2.h"
#include "pw_sync/lock_annotations.h"

namespace pw::bluetooth::proxy::hci {
namespace {

constexpr std::array<std::byte, 1> kH4EventHeader{
    std::byte(emboss::H4PacketType::EVENT),
};

}

class CommandMultiplexer::InterceptorStateWrapper
    : public InterceptorMap::Pair {
 public:
  virtual ~InterceptorStateWrapper() = default;
  virtual std::variant<EventInterceptorWrapper*, CommandInterceptorWrapper*>
  Downcast() = 0;

 protected:
  InterceptorStateWrapper(InterceptorId::ValueType interceptor_id)
      : InterceptorMap::Pair(interceptor_id) {}
};

class CommandMultiplexer::EventInterceptorState
    : public EventInterceptorMap::Pair {
 public:
  EventInterceptorState(EventCodeVariant key, EventHandler handler)
      : EventInterceptorMap::Pair(key), handler_(std::move(handler)) {}
  EventHandler& handler() { return handler_; }

 private:
  EventHandler handler_;
};

class CommandMultiplexer::EventInterceptorWrapper
    : public InterceptorStateWrapper {
 public:
  EventInterceptorWrapper(InterceptorId::ValueType interceptor_id,
                          EventCodeVariant key,
                          EventHandler&& handler)
      : InterceptorStateWrapper(interceptor_id),
        wrapped_(key, std::move(handler)) {}

  std::variant<EventInterceptorWrapper*, CommandInterceptorWrapper*> Downcast()
      override {
    return this;
  }

  EventInterceptorState& state() { return wrapped_; }

 private:
  EventInterceptorState wrapped_;
};

class CommandMultiplexer::CommandInterceptorState
    : public CommandInterceptorMap::Pair {
 public:
  CommandInterceptorState(pw::bluetooth::emboss::OpCode op_code,
                          CommandHandler&& handler)
      : CommandInterceptorMap::Pair(op_code), handler_(std::move(handler)) {}
  CommandHandler& handler() { return handler_; }

 private:
  CommandHandler handler_;
};

class CommandMultiplexer::CommandInterceptorWrapper
    : public InterceptorStateWrapper {
 public:
  CommandInterceptorWrapper(InterceptorId::ValueType interceptor_id,
                            pw::bluetooth::emboss::OpCode op_code,
                            CommandHandler&& handler)
      : InterceptorStateWrapper(interceptor_id),
        wrapped_(op_code, std::move(handler)) {}

  CommandInterceptorState& state() { return wrapped_; }

  std::variant<EventInterceptorWrapper*, CommandInterceptorWrapper*> Downcast()
      override {
    return this;
  }

 private:
  CommandInterceptorState wrapped_;
};

CommandMultiplexer::CommandMultiplexer(
    Allocator& allocator,
    Function<void(MultiBuf::Instance&& h4_packet)>&& send_to_host_fn,
    Function<void(MultiBuf::Instance&& h4_packet)>&& send_to_controller_fn,
    [[maybe_unused]] async2::TimeProvider<chrono::SystemClock>& time_provider,
    [[maybe_unused]] chrono::SystemClock::duration command_timeout)
    : allocator_(allocator),
      send_to_host_fn_(std::move(send_to_host_fn)),
      send_to_controller_fn_(std::move(send_to_controller_fn)) {}

CommandMultiplexer::CommandMultiplexer(
    Allocator& allocator,
    Function<void(MultiBuf::Instance&& h4_packet)>&& send_to_host_fn,
    Function<void(MultiBuf::Instance&& h4_packet)>&& send_to_controller_fn,
    [[maybe_unused]] Function<void()> timeout_fn,
    [[maybe_unused]] chrono::SystemClock::duration command_timeout)
    : allocator_(allocator),
      send_to_host_fn_(std::move(send_to_host_fn)),
      send_to_controller_fn_(std::move(send_to_controller_fn)) {}

Result<async2::Poll<>> CommandMultiplexer::PendCommandTimeout(
    [[maybe_unused]] async2::Context& cx) {
  // Not implemented.
  return Status::Unimplemented();
}

void CommandMultiplexer::HandleH4FromHost(MultiBuf::Instance&& h4_packet) {
  MultiBuf::Instance buf = std::move(h4_packet);
  if (buf->empty()) {
    PW_LOG_WARN("Ignoring empty H4 packet from host.");
    return;
  }

  // Only intercept command packets from host.
  if (*buf->begin() != std::byte(emboss::H4PacketType::COMMAND)) {
    send_to_controller_fn_(std::move(buf));
    return;
  }

  std::array<std::byte,
             emboss::CommandHeaderView::IntrinsicSizeInBytes().Read()>
      header_buf;
  auto bytes = buf->Get(header_buf, /*offset=*/sizeof(emboss::H4PacketType));

  emboss::CommandHeaderView view(
      static_cast<const uint8_t*>(static_cast<const void*>(bytes.data())),
      bytes.size());
  if (!view.Ok()) {
    // View is malformed, don't intercept.
    SendToControllerOrQueue(std::move(buf));
    return;
  }

  std::lock_guard lock(mutex_);
  auto iter = command_interceptors_.find(view.opcode().Read());
  if (iter == command_interceptors_.end()) {
    // No registered interceptors.
    SendToControllerOrQueue(std::move(buf));
    return;
  }

  auto& handler = iter->handler();
  if (!handler) {
    // Interceptor doesn't have a handler.
    SendToControllerOrQueue(std::move(buf));
    return;
  }

  CommandInterceptorReturn result = handler(CommandPacket{std::move(buf)});

  static_assert(std::variant_size_v<decltype(result.action)> == 2,
                "Unhandled variant members.");
  if (auto* action = std::get_if<RemoveThisInterceptor>(&result.action)) {
    if (!action->id.is_valid()) {
      // Ignoring RemoveThisInterceptor action for invalid ID.
      return;
    }
    auto interceptors_iter = interceptors_.find(action->id.value());

    // Should be impossible to fail this check from type safety, failing
    // would require forging or reusing an ID.
    PW_CHECK(interceptors_iter != interceptors_.end());

    // Ensure the action didn't provide a different interceptor's ID.
    auto downcast = interceptors_iter->Downcast();
    PW_CHECK(std::holds_alternative<CommandInterceptorWrapper*>(downcast));
    PW_CHECK(&std::get<CommandInterceptorWrapper*>(downcast)->state() ==
             &*iter);

    RemoveInterceptor(interceptors_iter);
  }

  // Only other action is continue.

  if (result.command.has_value()) {
    SendToControllerOrQueue(std::move(result.command->buffer));
  }
}

void CommandMultiplexer::HandleH4FromController(
    MultiBuf::Instance&& h4_packet) {
  MultiBuf::Instance buf = std::move(h4_packet);
  if (buf->empty()) {
    PW_LOG_WARN("Ignoring empty H4 packet from controller.");
    return;
  }

  // Only intercept event packets from controller.
  if (*buf->begin() != std::byte(emboss::H4PacketType::EVENT)) {
    send_to_host_fn_(std::move(buf));
    return;
  }

  std::array<std::byte, kMaxEventHeaderSize> header_buf;
  auto bytes = buf->Get(header_buf, /*offset=*/sizeof(emboss::H4PacketType));

  emboss::EventHeaderView view(
      static_cast<const uint8_t*>(static_cast<const void*>(bytes.data())),
      bytes.size());
  if (!view.Ok()) {
    // View is malformed, don't intercept.
    send_to_host_fn_(std::move(buf));
    return;
  }

  std::lock_guard lock(mutex_);

  auto event_code = EventCodeValue(view.event_code().Read());
  auto iter = FindEventInterceptor(event_code, bytes);

  if (iter == event_interceptors_.end()) {
    send_to_host_fn_(std::move(buf));
    return;
  }

  auto& handler = iter->handler();
  if (!handler) {
    // Interceptor doesn't have a handler.
    send_to_host_fn_(std::move(buf));
    return;
  }

  EventInterceptorReturn result = handler(EventPacket{std::move(buf)});

  static_assert(std::variant_size_v<decltype(result.action)> == 2,
                "Unhandled variant members.");
  if (auto* action = std::get_if<RemoveThisInterceptor>(&result.action)) {
    if (!action->id.is_valid()) {
      // Ignoring RemoveThisInterceptor action for invalid ID.
      return;
    }
    auto interceptors_iter = interceptors_.find(action->id.value());

    // Should be impossible to fail this check from type safety, failing
    // would require forging or reusing an ID.
    PW_CHECK(interceptors_iter != interceptors_.end());

    // Ensure the action didn't provide a different interceptor's ID.
    auto downcast = interceptors_iter->Downcast();
    PW_CHECK(std::holds_alternative<EventInterceptorWrapper*>(downcast));
    PW_CHECK(&std::get<EventInterceptorWrapper*>(downcast)->state() == &*iter);

    RemoveInterceptor(interceptors_iter);
  } else {
    // Only other valid action is to continue, so no need to do anything.
  }

  if (result.event.has_value()) {
    send_to_host_fn_(std::move(result.event->buffer));
  }
}

expected<void, FailureWithBuffer> CommandMultiplexer::SendCommand(
    CommandPacket&& command,
    EventHandler&& event_handler,
    [[maybe_unused]] EventCodeVariant complete_event_code,
    [[maybe_unused]] pw::span<pw::bluetooth::emboss::OpCode> exclusions) {
  EventHandler _(std::move(event_handler));
  // Not implemented.
  return unexpected(
      FailureWithBuffer{Status::Unimplemented(), std::move(command.buffer)});
}

expected<void, FailureWithBuffer> CommandMultiplexer::SendEvent(
    EventPacket&& event) {
  if (event.buffer->empty()) {
    return unexpected(
        FailureWithBuffer{Status::InvalidArgument(), std::move(event.buffer)});
  }

  if (!event.buffer->TryReserveForInsert(event.buffer->begin())) {
    return unexpected(
        FailureWithBuffer{Status::Unavailable(), std::move(event.buffer)});
  }
  event.buffer->Insert(event.buffer->begin(), kH4EventHeader);

  // No buffering of events is needed.
  send_to_host_fn_(std::move(event.buffer));
  return {};
}

Result<EventInterceptor> CommandMultiplexer::RegisterEventInterceptor(
    EventCodeVariant event_code, EventHandler&& handler) {
  static_assert(std::variant_size_v<EventCodeVariant> == 5,
                "Event code may need special casing.");
  if (auto* event = std::get_if<emboss::EventCode>(&event_code)) {
    switch (cpp23::to_underlying(*event)) {
      case cpp23::to_underlying(emboss::EventCode::LE_META_EVENT):
      case cpp23::to_underlying(emboss::EventCode::VENDOR_DEBUG):
      case cpp23::to_underlying(emboss::EventCode::COMMAND_STATUS):
      case cpp23::to_underlying(emboss::EventCode::COMMAND_COMPLETE):
        return Status::InvalidArgument();
      default:
        break;
    }
  }

  std::lock_guard lock(mutex_);
  if (event_interceptors_.find(event_code) != event_interceptors_.end()) {
    return Status::AlreadyExists();
  }

  std::optional<InterceptorId> id = AllocateInterceptorId();
  if (!id.has_value() || !id->is_valid()) {
    // Exhausted ID space.
    return Status::Unavailable();
  }

  auto* interceptor = allocator_.New<EventInterceptorWrapper>(
      id->value(), event_code, std::move(handler));
  if (!interceptor) {
    // Exhausted allocator space.
    return Status::Unavailable();
  }

  interceptors_.insert(*interceptor);
  event_interceptors_.insert(interceptor->state());

  return EventInterceptor(*this, std::move(*id));
}

Result<CommandInterceptor> CommandMultiplexer::RegisterCommandInterceptor(
    pw::bluetooth::emboss::OpCode op_code, CommandHandler&& handler) {
  std::lock_guard lock(mutex_);
  if (command_interceptors_.find(op_code) != command_interceptors_.end()) {
    return Status::AlreadyExists();
  }

  std::optional<InterceptorId> id = AllocateInterceptorId();
  if (!id.has_value() || !id->is_valid()) {
    // Exhausted ID space.
    return Status::Unavailable();
  }

  auto* interceptor = allocator_.New<CommandInterceptorWrapper>(
      id->value(), op_code, std::move(handler));
  if (!interceptor) {
    // Exhausted allocator space.
    return Status::Unavailable();
  }

  interceptors_.insert(*interceptor);
  command_interceptors_.insert(interceptor->state());

  return CommandInterceptor(*this, std::move(*id));
}

void CommandMultiplexer::UnregisterInterceptor(InterceptorId id) {
  PW_CHECK(id.is_valid(), "Attempt to unregister invalid ID.");
  std::lock_guard lock(mutex_);
  auto iter = interceptors_.find(id.value());
  // Should be impossible to fail this check from type safety, failing would
  // require forging or reusing an ID.
  PW_CHECK(iter != interceptors_.end());

  RemoveInterceptor(iter);
}

std::optional<CommandMultiplexer::InterceptorId>
CommandMultiplexer::AllocateInterceptorId() {
  // Ignoring lock safety, the capability doesn't pass through MintId, but this
  // function is annotated with PW_EXCLUSIVE_LOCKS_REQUIRED(mutex_), so we
  // should always hold mutex_ here.
  return id_mint_.MintId(
      [&](InterceptorId::ValueType candidate) PW_NO_LOCK_SAFETY_ANALYSIS {
        return interceptors_.find(candidate) != interceptors_.end();
      });
}

CommandMultiplexer::EventInterceptorMap::iterator
CommandMultiplexer::FindEventInterceptor(EventCodeValue event,
                                         ConstByteSpan span) {
  static_assert(std::variant_size_v<EventCodeVariant> == 5,
                "Event code variant may need special casing.");
  switch (event) {
    case cpp23::to_underlying(emboss::EventCode::COMMAND_COMPLETE):
      return FindCommandComplete(span);
    case cpp23::to_underlying(emboss::EventCode::COMMAND_STATUS):
      return FindCommandStatus(span);
    case cpp23::to_underlying(emboss::EventCode::LE_META_EVENT):
      return FindLeMetaEvent(span);
    case cpp23::to_underlying(emboss::EventCode::VENDOR_DEBUG):
      return FindVendorDebug(span);
    default:
      return event_interceptors_.find(emboss::EventCode{event});
  }
}

CommandMultiplexer::EventInterceptorMap::iterator
CommandMultiplexer::FindCommandComplete(ConstByteSpan span) {
  emboss::CommandCompleteEventView view(
      static_cast<const uint8_t*>(static_cast<const void*>(span.data())),
      span.size());

  if (!view.Ok()) {
    // View is malformed, don't intercept.
    return event_interceptors_.end();
  }

  auto subevent_code = view.command_opcode().Read();
  return event_interceptors_.find(CommandCompleteOpcode{subevent_code});
}

CommandMultiplexer::EventInterceptorMap::iterator
CommandMultiplexer::FindCommandStatus(ConstByteSpan span) {
  emboss::CommandStatusEventView view(
      static_cast<const uint8_t*>(static_cast<const void*>(span.data())),
      span.size());

  if (!view.Ok()) {
    // View is malformed, don't intercept.
    return event_interceptors_.end();
  }

  auto subevent_code = view.command_opcode_enum().Read();
  return event_interceptors_.find(CommandStatusOpcode{subevent_code});
}

CommandMultiplexer::EventInterceptorMap::iterator
CommandMultiplexer::FindLeMetaEvent(ConstByteSpan span) {
  emboss::LEMetaEventView view(
      static_cast<const uint8_t*>(static_cast<const void*>(span.data())),
      span.size());

  if (!view.Ok()) {
    return event_interceptors_.end();
  }

  auto subevent_code = view.subevent_code_enum().Read();
  return event_interceptors_.find(subevent_code);
}

CommandMultiplexer::EventInterceptorMap::iterator
CommandMultiplexer::FindVendorDebug(ConstByteSpan span) {
  emboss::VendorDebugEventView view(
      static_cast<const uint8_t*>(static_cast<const void*>(span.data())),
      span.size());

  if (!view.Ok()) {
    // View is malformed, don't intercept.
    return event_interceptors_.end();
  }

  auto subevent_code = view.subevent_code().Read();
  return event_interceptors_.find(VendorDebugSubEventCode{subevent_code});
}

void CommandMultiplexer::RemoveInterceptor(InterceptorMap::iterator iterator) {
  auto downcast = iterator->Downcast();
  static_assert(std::variant_size_v<decltype(downcast)> == 2,
                "Unhandled variant members.");
  if (auto** event = std::get_if<EventInterceptorWrapper*>(&downcast)) {
    event_interceptors_.erase((*event)->state().key());
  } else if (auto** cmd = std::get_if<CommandInterceptorWrapper*>(&downcast)) {
    command_interceptors_.erase((*cmd)->state().key());
  } else {
    PW_UNREACHABLE;
  }

  auto& interceptor = *iterator;
  interceptors_.erase(iterator);
  allocator_.Delete(&interceptor);
}

void CommandMultiplexer::SendToControllerOrQueue(MultiBuf::Instance&& buf) {
  // Queuing not yet supported.
  send_to_controller_fn_(std::move(buf));
}

}  // namespace pw::bluetooth::proxy::hci
