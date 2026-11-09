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

// clang-format off
#include "pw_rpc/internal/log_config.h" // PW_LOG_* macros must be first.

#include "pw_rpc/rpc2_adapter.h"
// clang-format on

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>

#include "pw_allocator/layout.h"
#include "pw_assert/check.h"
#include "pw_log/log.h"
#include "pw_rpc/internal/call.h"
#include "pw_rpc/internal/config.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/server_call.h"
#include "pw_rpc2/internal/server_connection_task.h"
#include "pw_rpc2/internal/server_task.h"

namespace pw::rpc {

using internal::pwpb::PacketType;
using rpc2::internal::ProtocolError;

namespace internal {

// -----------------------------------------------------------------------------
// Rpc2LegacyCall: runs on the pw_rpc2 dispatcher.
// -----------------------------------------------------------------------------

ProtocolError Rpc2LegacyCall::Invoke(rpc2::Service& service,
                                     rpc2::internal::ServerCall& call,
                                     ConstBuf&& request,
                                     size_t method_index_and_flags) {
  auto& adapted = static_cast<Rpc2ServiceAdapter&>(service);
  const uint32_t service_id = adapted.service_id();
  const rpc2::internal::Method& method =
      adapted.method(method_index_and_flags >> 1);
  const uint32_t method_id = method.id();
  const rpc2::MethodType type = method.type();
  const bool deliver_non_ok_response = (method_index_and_flags & 1u) != 0u;

  // Client streaming requests arrive through the reader, so `request` is empty.
  Rpc2QueuedPacket* packet = adapted.adapter_.NewRequest(
      call.server_connection_task(), service_id, method_id, request);
  if (packet == nullptr) {
    PW_LOG_WARN(
        "Rpc2Adapter could not allocate a request or channel for %08x/%08x; "
        "rejecting the call",
        static_cast<unsigned>(service_id),
        static_cast<unsigned>(method_id));
    return ProtocolError::kFailedToAllocateCall;
  }

  // As for native pw_rpc2 methods: a unary request is the whole request.
  if (!rpc2::HasClientStream(type)) {
    call.CloseRead();
  }
  return call.EmplaceFutureFromFactory<Rpc2LegacyCall>([&] {
    return Rpc2LegacyCall(
        adapted.adapter_, call, type, deliver_non_ok_response, *packet);
  });
}

Rpc2LegacyCall::Rpc2LegacyCall(Rpc2ServerAdapter& adapter,
                               rpc2::internal::ServerCall& call,
                               rpc2::MethodType type,
                               bool deliver_non_ok_response,
                               Rpc2QueuedPacket& request)
    : adapter_(&adapter), shared_(request, type, deliver_non_ok_response) {
  using rpc2::internal::CallAccess;
  if (rpc2::HasClientStream(type)) {
    reader_ = CallAccess::Create<rpc2::RawReader>(call.shared_call());
  }
  if (rpc2::HasServerStream(type)) {
    writer_ = CallAccess::Create<rpc2::RawWriter>(call.shared_call());
  } else {
    unary_writer_ =
        CallAccess::Create<rpc2::RawUnaryWriter>(call.shared_call());
  }

  adapter.StartCall(*this, request);
}

Rpc2LegacyCall::~Rpc2LegacyCall() {
  if (adapter_ == nullptr) {
    return;
  }
  // The write may refer to a queued packet's payload; drop it before the
  // adapter frees the packet.
  write_ = rpc2::WriteFuture<ConstBuf>();
  adapter_->EndCall(*this);
}

async2::Poll<> Rpc2LegacyCall::Pend(async2::Context& cx) {
  if (PendLegacyToRpc2(cx).IsReady() || PendRpc2ToLegacy(cx).IsReady()) {
    complete_ = true;
    return async2::Ready();
  }
  return async2::Pending();
}

async2::Poll<> Rpc2LegacyCall::PendLegacyToRpc2(async2::Context& cx) {
  while (true) {
    if (write_.is_pendable()) {
      async2::Poll<rpc2::RpcStatus> written = write_.Pend(cx);
      if (written.IsPending()) {
        return async2::Pending();
      }
      if (!written->ok()) {
        PW_LOG_DEBUG("Rpc2Adapter failed to write to %08x/%08x: %s",
                     static_cast<unsigned>(shared_.service_id),
                     static_cast<unsigned>(shared_.method_id),
                     written->str());
        FailRpc2(ProtocolError::kCancelled);
        return async2::Ready();
      }
      if (final_write_) {
        return async2::Ready();
      }
    }

    const Rpc2ServerAdapter::Event event = adapter_->NextEvent(*this, cx);
    if (event.packet == nullptr) {
      if (!event.done) {
        return async2::Pending();
      }
      // A successful server stream ends when the writer is destroyed.
      if (event.error == ProtocolError::kUnknownService) {
        PW_LOG_WARN(
            "Rpc2Adapter: service %08x is not registered with the "
            "pw::rpc::Server",
            static_cast<unsigned>(shared_.service_id));
      }
      if (event.error != ProtocolError::kNone) {
        FailRpc2(event.error);
      }
      return async2::Ready();
    }

    // The payload is stored inline in the queued packet. The adapter frees it
    // after the write completes.
    writing_ = event.packet;
    ConstBuf payload = ConstBuf::Unowned(writing_->payload());
    if (writing_->type() == PacketType::SERVER_STREAM) {
      write_ = writer_.WriteCopy(std::move(payload));
    } else {  // The RESPONSE of a unary or client streaming RPC.
      write_ = unary_writer_.FinishCopy(std::move(payload));
      final_write_ = true;
    }
  }
}

async2::Poll<> Rpc2LegacyCall::PendRpc2ToLegacy(async2::Context& cx) {
  if (!rpc2::HasClientStream(shared_.type) || client_stream_done_) {
    return async2::Pending();
  }

  while (!adapter_->InboundBlocked(*this, cx)) {
    if (!read_.is_pendable()) {
      read_ = reader_.Read();
    }
    async2::Poll<rpc2::RpcResult<ConstBuf>> message = read_.Pend(cx);
    if (message.IsPending()) {
      return async2::Pending();
    }

    PacketType type = PacketType::CLIENT_STREAM;
    ConstByteSpan payload;
    if (message->ok()) {
      payload = message->value();
    } else if (message->status().IsEndOfStream()) {
      client_stream_done_ = true;
      reader_ = rpc2::RawReader();
      if constexpr (!PW_RPC_COMPLETION_REQUEST_CALLBACK) {
        return async2::Pending();
      }
      type = PacketType::CLIENT_REQUEST_COMPLETION;
    } else {
      // The pw_rpc2 call ended. Destroying this future cancels the legacy call.
      return async2::Ready();
    }

    if (!adapter_->SendToLegacy(*this, type, payload)) {
      PW_LOG_WARN("Rpc2Adapter could not queue a request for %08x/%08x",
                  static_cast<unsigned>(shared_.service_id),
                  static_cast<unsigned>(shared_.method_id));
      FailRpc2(ProtocolError::kFailedToAllocateCallResourcesWhileRunning);
      return async2::Ready();
    }
    if (client_stream_done_) {
      return async2::Pending();
    }
  }
  return async2::Pending();
}

void Rpc2LegacyCall::FailRpc2(ProtocolError error) {
  rpc2::internal::Call& call =
      rpc2::HasServerStream(shared_.type)
          ? rpc2::internal::CallAccess::call(writer_)
          : rpc2::internal::CallAccess::call(unary_writer_);
  if (error == ProtocolError::kCancelled) {
    call.Cancel();
  } else if (!call.is_write_closed()) {
    // pw_rpc2 has no public API for ending a call with a specific error.
    call.QueueError(error);
    call.CloseWrite();
  }
}

}  // namespace internal

// -----------------------------------------------------------------------------
// Rpc2ServerAdapter
// -----------------------------------------------------------------------------

Rpc2ServerAdapter::Rpc2ServerAdapter(bool owns_server,
                                     Server& server,
                                     Allocator& allocator,
                                     const thread::Options& options,
                                     const Rpc2AdapterConfig& config)
    : server_(server),
      allocator_(allocator),
      config_(config),
      bindings_(allocator),
      owns_server_(owns_server),
      output_(*this) {
  PW_CHECK_UINT_NE(config_.first_channel_id,
                   Channel::kUnassignedChannelId,
                   "Rpc2AdapterConfig::first_channel_id must not be 0");
  PW_CHECK_UINT_GT(config_.max_queued_messages_per_call,
                   0,
                   "Rpc2AdapterConfig::max_queued_messages_per_call must be "
                   "at least 1");
  if (owns_server_) {
    PW_CHECK_OK(server_.SetDefaultChannelOutput(output_));
  } else {
    internal::RpcLockGuard rpc_lock;
    PW_CHECK(!server_.has_default_channel(),
             "An Rpc2ServerAdapter's shared pw::rpc::Server must not use "
             "SetDefaultChannelOutput()");
  }
  thread_ = Thread(options, [this] { RunThread(); });
}

Rpc2ServerAdapter::~Rpc2ServerAdapter() {
#if PW_THREAD_JOINING_ENABLED
  Stop();
#else
  PW_CRASH(
      "An Rpc2ServerAdapter cannot be destroyed without a pw_thread backend "
      "that supports joining (PW_THREAD_JOINING_ENABLED = 1)");
#endif  // PW_THREAD_JOINING_ENABLED
  std::lock_guard lock(lock_);
  PW_CHECK(
      calls_.empty(),
      "An Rpc2ServerAdapter was destroyed while pw_rpc2 calls were using it");
  // The worker drained the queue before Stop() joined it.
  PW_DCHECK(work_.empty());
}

#if PW_THREAD_JOINING_ENABLED
void Rpc2ServerAdapter::DoStop() {
  if (thread_.joinable()) {
    {
      std::lock_guard lock(lock_);
      stop_requested_ = true;
    }
    work_available_.release();
    thread_.join();  // The thread drains the queue before it exits.
  }

  // Close the adapter's channels. The server's lock is acquired before the
  // adapter's (see HandleLegacyPacket()), so close each without holding lock_.
  while (true) {
    uint32_t channel_id;
    {
      std::lock_guard lock(lock_);
      PW_CHECK(calls_.empty(),
               "Rpc2ServerAdapter::Stop() was called while pw_rpc2 calls were "
               "using it; close the pw::rpc2::Server first");
      if (bindings_.empty()) {
        return;
      }
      const bool open = bindings_.front().channel_open;
      channel_id = bindings_.front().channel_id;
      bindings_.pop_front();
      binding_count_ -= 1;
      if (!open) {
        continue;
      }
    }
    // Aborts any legacy call a handler left open on the channel.
    server_.CloseChannel(channel_id).IgnoreError();  // NOT_FOUND if closed
    if (owns_server_) {
      // An owned server has no channels, so CloseChannel() found and closed
      // the default channel, which every binding uses. Restore it so that
      // closing the next binding's channel ID aborts that binding's calls.
      server_.SetDefaultChannelOutput(output_).IgnoreError();
    }
  }
}
#endif  // PW_THREAD_JOINING_ENABLED

void Rpc2ServerAdapter::RunThread() {
  while (true) {
    QueuedPacket* queued = nullptr;
    {
      std::lock_guard lock(lock_);
      if (!work_.empty()) {
        queued = &work_.front();
        work_.pop_front();
      } else if (stop_requested_) {
        return;
      }
    }

    if (queued == nullptr) {
      work_available_.acquire();
      continue;
    }

    ProcessPacket(*queued);
  }
}

namespace {

constexpr ProtocolError ServerErrorToProtocolError(Status status) {
  if (status.IsNotFound()) {
    return ProtocolError::kUnknownService;
  }
  if (status.IsDataLoss()) {
    return ProtocolError::kInvalidRequestPayload;
  }
  if (status.IsResourceExhausted()) {
    return ProtocolError::kFailedToAllocateCallResourcesWhileRunning;
  }
  return ProtocolError::kCancelled;
}

}  // namespace

uint32_t Rpc2ServerAdapter::OpenChannel(Binding& binding) {
  uint32_t channel_id;
  {
    std::lock_guard lock(lock_);
    channel_id = binding.channel_id;
  }
  while (true) {
    const Status status = server_.OpenChannel(channel_id, output_);
    if (status.IsAlreadyExists()) {
      internal::RpcLockGuard rpc_lock;
      PW_CHECK(!server_.has_default_channel(),
               "An Rpc2ServerAdapter's shared pw::rpc::Server must not use "
               "SetDefaultChannelOutput()");
    }
    std::lock_guard lock(lock_);
    if (status.ok()) {
      binding.channel_open = true;
      return channel_id;
    }
    if (!status.IsAlreadyExists()) {
      PW_LOG_WARN("Rpc2ServerAdapter could not open pw_rpc channel %u: %s",
                  static_cast<unsigned>(channel_id),
                  status.str());
      return Channel::kUnassignedChannelId;
    }
    // Another user of the server has this ID. Move past it.
    channel_id = NextChannelIdLocked();
    if (channel_id == Channel::kUnassignedChannelId) {
      PW_LOG_WARN(
          "Rpc2ServerAdapter could not open a pw_rpc channel: channel IDs are "
          "exhausted");
      return Channel::kUnassignedChannelId;
    }
    binding.channel_id = channel_id;
  }
}

uint32_t Rpc2ServerAdapter::ChannelFor(QueuedPacket& queued) {
  Binding& binding = queued.binding();
  {
    std::lock_guard lock(lock_);
    if (!queued.is_cancel() && FindCallLocked(queued.call_id()) == nullptr) {
      FreePacketLocked(&queued);
      return Channel::kUnassignedChannelId;
    }
    // Only this thread opens channels, so if the channel is open, its ID
    // cannot change after reading it here.
    if (binding.channel_open) {
      return binding.channel_id;
    }
  }

  // A connection's first packet opens its channel. No legacy call can exist
  // on a channel that is not open, so there is nothing to cancel.
  if (!queued.is_cancel()) {
    if (const uint32_t channel_id = OpenChannel(binding);
        channel_id != Channel::kUnassignedChannelId) {
      return channel_id;
    }
  }

  std::lock_guard lock(lock_);
  if (!queued.is_cancel()) {
    if (Call::Shared* call = FindCallLocked(queued.call_id());
        call != nullptr && !call->done) {
      call->inbound_in_flight = false;
      FinishLocked(*call, ProtocolError::kFailedToAllocateCall);
      call->waker.Wake();
    }
  }
  FreePacketLocked(&queued);
  return Channel::kUnassignedChannelId;
}

void Rpc2ServerAdapter::ProcessPacket(QueuedPacket& queued) {
  const bool is_cancel = queued.is_cancel();
  internal::Packet packet;
  while (true) {
    const uint32_t channel_id = ChannelFor(queued);
    if (channel_id == Channel::kUnassignedChannelId) {
      return;  // ChannelFor() freed the packet.
    }

    // Legacy handlers run here, without the adapter's lock held, so they may
    // write to their calls (which re-enters HandleLegacyPacket()).
    packet = queued.ToPacket(channel_id);
    const Status status = server_.ProcessPacket(packet);
    if (status.ok()) {
      break;
    }

    // The server only rejects packets for channels that are not open, so the
    // channel was closed outside the adapter, which aborted its legacy calls.
    // Open a new channel for the connection and retry.
    PW_CHECK(status.IsUnavailable(), "Unexpected status %s", status.str());
    PW_LOG_WARN(
        "Rpc2ServerAdapter's pw_rpc channel %u was closed; reopening it",
        static_cast<unsigned>(channel_id));
    std::lock_guard lock(lock_);
    queued.binding().channel_open = false;
  }

  bool legacy_call_open = false;
  if (!is_cancel) {
    internal::RpcLockGuard rpc_lock;
    legacy_call_open = server_.FindCall(packet) != server_.calls_end();
  }

  bool cancel_immediately = false;
  {
    std::lock_guard lock(lock_);
    QueuedPacket* to_free = &queued;
    Call::Shared* call = is_cancel ? nullptr : FindCallLocked(queued.call_id());
    if (call != nullptr) {
      call->inbound_in_flight = false;
      if (!call->done && !legacy_call_open) {
        // The legacy call closed without sending a RESPONSE or SERVER_ERROR
        // packet, such as when decoding a CLIENT_STREAM payload fails.
        FinishLocked(*call,
                     queued.type() == PacketType::CLIENT_STREAM
                         ? ProtocolError::kInvalidRequestPayload
                         : ProtocolError::kDroppedWithoutResponse);
      } else if (queued.type() == PacketType::REQUEST && legacy_call_open) {
        if (call->done) {
          // TryFinish() failed to queue RESPONSE and left the legacy call open.
          cancel_immediately = true;
        } else {
          // Keep the REQUEST packet to cancel the legacy call if needed.
          queued.BecomeCancel();
          call->cancel = std::exchange(to_free, nullptr);
        }
      }
      call->waker.Wake();
    } else if (queued.type() == PacketType::REQUEST && legacy_call_open) {
      // The pw_rpc2 call ended while its REQUEST was being processed, and the
      // handler left the legacy call open. Cancel it before processing any
      // newer packets in work_.
      cancel_immediately = true;
    }
    FreePacketLocked(to_free);
  }

  if (cancel_immediately) {
    // UNAVAILABLE if the channel was closed externally, which already aborted
    // its calls.
    server_
        .ProcessPacket(
            internal::Packet::ClientError(packet, Status::Cancelled()))
        .IgnoreError();
  }
}

Status Rpc2ServerAdapter::HandleLegacyPacket(const internal::Packet& packet) {
  std::lock_guard lock(lock_);
  Call::Shared* call = FindCallLocked(packet.call_id());
  if (call == nullptr || call->done) {
    return OkStatus();  // The pw_rpc2 call is gone; drop the packet.
  }
  // Call IDs are unique across the adapter's channels.
  PW_DCHECK_UINT_EQ(packet.channel_id(), call->binding->channel_id);

  Status status;
  switch (packet.type()) {
    case PacketType::SERVER_STREAM:
      if (call->queued >= config_.max_queued_messages_per_call ||
          !PushToInboxLocked(*call, packet)) {
        return Status::ResourceExhausted();
      }
      break;
    case PacketType::RESPONSE:
      if (!packet.status().ok() && !call->deliver_non_ok_response) {
        PW_LOG_DEBUG("Legacy call to %08x/%08x ended with %s",
                     static_cast<unsigned>(call->service_id),
                     static_cast<unsigned>(call->method_id),
                     packet.status().str());
        FinishLocked(*call, ProtocolError::kCancelled);
      } else if (rpc2::HasServerStream(call->type) ||
                 PushToInboxLocked(*call, packet)) {
        FinishLocked(*call, ProtocolError::kNone);
      } else {
        // The legacy call is over for pw_rpc2, so end the pw_rpc2 call. Keep
        // `call->cancel` in case the handler called `TryFinish()`, which leaves
        // the legacy call open when sending fails.
        call->done = true;
        call->error = ProtocolError::kFailedToAllocateCallResourcesWhileRunning;
        status = Status::ResourceExhausted();
      }
      break;
    case PacketType::SERVER_ERROR:
      FinishLocked(*call, ServerErrorToProtocolError(packet.status()));
      break;
    case PacketType::REQUEST:
    case PacketType::CLIENT_STREAM:
    case PacketType::CLIENT_ERROR:
    case PacketType::CLIENT_REQUEST_COMPLETION:
    default:
      return Status::Internal();  // Servers only send server packet types.
  }

  call->waker.Wake();
  return status;
}

Rpc2ServerAdapter::QueuedPacket* Rpc2ServerAdapter::NewRequest(
    rpc2::internal::ServerConnectionTask& connection,
    uint32_t service_id,
    uint32_t method_id,
    ConstByteSpan payload) {
  std::lock_guard lock(lock_);
  Binding* binding = AcquireBindingLocked(connection);
  if (binding == nullptr) {
    return nullptr;
  }
  QueuedPacket* packet = NewPacketLocked(
      *binding, PacketType::REQUEST, service_id, method_id, payload);
  if (packet == nullptr) {
    binding->live_calls -= 1;
    return nullptr;
  }
  // The call is registered in StartCall(), which the pw_rpc2 dispatcher calls
  // before it can invoke another method, so the ID remains unique.
  packet->set_call_id(AllocateCallIdLocked());
  return packet;
}

void Rpc2ServerAdapter::StartCall(Call& call, QueuedPacket& request) {
  std::lock_guard lock(lock_);
  Call::Shared& shared = call.shared_;
  calls_.push_front(shared);
  shared.inbound_in_flight = true;
  EnqueueLocked(request);
}

void Rpc2ServerAdapter::EndCall(Call& call) {
  std::lock_guard lock(lock_);
  Call::Shared& shared = call.shared_;
  calls_.remove(shared);
  shared.binding->live_calls -= 1;

  while (!shared.inbox.empty()) {
    QueuedPacket& packet = shared.inbox.front();
    shared.inbox.pop_front();
    FreePacketLocked(&packet);
  }
  FreePacketLocked(std::exchange(call.writing_, nullptr));

  // If the legacy call is still open, cancel it. Any packets this call already
  // queued are dropped by ProcessPacket(), which also cancels a legacy call
  // started by a REQUEST that is currently being processed.
  if (shared.cancel != nullptr) {
    EnqueueLocked(*std::exchange(shared.cancel, nullptr));
  }
}

Rpc2ServerAdapter::Event Rpc2ServerAdapter::NextEvent(Call& call,
                                                      async2::Context& cx) {
  std::lock_guard lock(lock_);
  FreePacketLocked(std::exchange(call.writing_, nullptr));
  Call::Shared& shared = call.shared_;
  if (!shared.inbox.empty()) {
    QueuedPacket& packet = shared.inbox.front();
    shared.inbox.pop_front();
    shared.queued -= 1;
    return Event{&packet, false, ProtocolError::kNone};
  }
  if (!shared.done) {
    PW_ASYNC_STORE_WAKER(cx, shared.waker, "waiting for legacy pw_rpc packets");
  }
  return Event{nullptr, shared.done, shared.error};
}

bool Rpc2ServerAdapter::InboundBlocked(Call& call, async2::Context& cx) {
  std::lock_guard lock(lock_);
  Call::Shared& shared = call.shared_;
  if (shared.done) {
    return true;  // The legacy call is over; stop forwarding requests.
  }
  if (shared.inbound_in_flight) {
    PW_ASYNC_STORE_WAKER(cx, shared.waker, "waiting for the legacy server");
    return true;
  }
  return false;
}

bool Rpc2ServerAdapter::SendToLegacy(Call& call,
                                     PacketType type,
                                     ConstByteSpan payload) {
  std::lock_guard lock(lock_);
  Call::Shared& shared = call.shared_;
  QueuedPacket* packet = NewPacketLocked(
      *shared.binding, type, shared.service_id, shared.method_id, payload);
  if (packet == nullptr) {
    return false;
  }
  packet->set_call_id(shared.call_id);
  shared.inbound_in_flight = true;
  EnqueueLocked(*packet);
  return true;
}

Rpc2ServerAdapter::Binding* Rpc2ServerAdapter::AcquireBindingLocked(
    rpc2::internal::ServerConnectionTask& connection) {
  const rpc2::internal::ServerTask& rpc2_server = connection.server_task();
  if (rpc2_server_ == nullptr) {
    rpc2_server_ = &rpc2_server;
  }
  PW_CHECK(rpc2_server_ == &rpc2_server,
           "An Rpc2ServerAdapter may only serve one pw::rpc2::Server");

  // A connection keeps its binding, and channel ID, for its whole lifetime. If
  // a new connection reuses the ID of one that was destroyed, it takes over the
  // old connection's binding here. That is equivalent to taking over a closed
  // connection's binding below: the old connection's calls all ended before it
  // was destroyed, so their cancellations are already queued ahead of anything
  // the new connection sends.
  const rpc2::internal::ConnectionId id = connection.id();
  for (Binding& binding : bindings_) {
    if (binding.connection == id) {
      binding.live_calls += 1;
      return &binding;
    }
  }

  // Take over the lowest-numbered binding of a connection that has closed. A
  // binding with live calls belongs to an open connection, so skip those.
  Binding* closed = nullptr;
  for (Binding& binding : bindings_) {
    if (binding.live_calls == 0 &&
        (closed == nullptr || binding.channel_id < closed->channel_id) &&
        !rpc2_server.HasConnection(binding.connection)) {
      closed = &binding;
    }
  }
  if (closed != nullptr) {
    closed->connection = id;
    closed->live_calls = 1;
    return closed;
  }

  if (config_.max_channels != 0 && binding_count_ >= config_.max_channels) {
    PW_LOG_WARN(
        "Rpc2ServerAdapter is using its maximum of %u channels; rejecting a "
        "call from a new pw_rpc2 connection",
        static_cast<unsigned>(config_.max_channels));
    return nullptr;
  }

  const uint32_t channel_id = NextChannelIdLocked();
  if (channel_id == Channel::kUnassignedChannelId) {
    PW_LOG_WARN(
        "Rpc2ServerAdapter exhausted pw_rpc channel IDs; rejecting a call "
        "from a new pw_rpc2 connection");
    return nullptr;
  }
  if (!bindings_.try_emplace_front(id, channel_id, owns_server_)) {
    return nullptr;
  }
  binding_count_ += 1;
  return &bindings_.front();
}

uint32_t Rpc2ServerAdapter::NextChannelIdLocked() const {
  uint32_t next = config_.first_channel_id;
  for (const Binding& binding : bindings_) {
    if (binding.channel_id == std::numeric_limits<uint32_t>::max()) {
      return Channel::kUnassignedChannelId;
    }
    next = std::max(next, binding.channel_id + 1);
  }
  return next;
}

Rpc2ServerAdapter::QueuedPacket* Rpc2ServerAdapter::NewPacketLocked(
    Binding& binding,
    PacketType type,
    uint32_t service_id,
    uint32_t method_id,
    ConstByteSpan payload) {
  void* memory = allocator_.Allocate(allocator::Layout(
      sizeof(QueuedPacket) + payload.size(), alignof(QueuedPacket)));
  if (memory == nullptr) {
    return nullptr;
  }
  auto* packet = new (memory)
      QueuedPacket(binding, type, service_id, method_id, payload.size());
  std::copy(payload.begin(), payload.end(), packet->payload().begin());
  return packet;
}

void Rpc2ServerAdapter::FreePacketLocked(QueuedPacket* packet) {
  if (packet == nullptr) {
    return;
  }
  packet->~QueuedPacket();
  allocator_.Deallocate(packet);
}

void Rpc2ServerAdapter::EnqueueLocked(QueuedPacket& packet) {
  work_.push_back(packet);
  work_available_.release();
}

bool Rpc2ServerAdapter::PushToInboxLocked(Call::Shared& call,
                                          const internal::Packet& packet) {
  QueuedPacket* queued = NewPacketLocked(*call.binding,
                                         packet.type(),
                                         call.service_id,
                                         call.method_id,
                                         packet.payload());
  if (queued == nullptr) {
    return false;
  }
  call.inbox.push_back(*queued);
  call.queued += 1;
  return true;
}

void Rpc2ServerAdapter::FinishLocked(Call::Shared& call, ProtocolError error) {
  call.done = true;
  call.error = error;
  FreePacketLocked(std::exchange(call.cancel, nullptr));
}

Rpc2ServerAdapter::Call::Shared* Rpc2ServerAdapter::FindCallLocked(
    uint32_t call_id) {
  auto call = std::find_if(calls_.begin(), calls_.end(), [call_id](auto& c) {
    return c.call_id == call_id;
  });
  return call == calls_.end() ? nullptr : &(*call);
}

uint32_t Rpc2ServerAdapter::AllocateCallIdLocked() {
  while (true) {
    const uint32_t id = next_call_id_++;
    // Avoid the call IDs pw_rpc reserves for open calls.
    if (id == internal::kLegacyOpenCallId || id == internal::kOpenCallId) {
      continue;
    }
    if (FindCallLocked(id) == nullptr) {
      return id;
    }
  }
}

}  // namespace pw::rpc
