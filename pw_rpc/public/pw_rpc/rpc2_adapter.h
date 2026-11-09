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

#include <cstdint>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_bytes/span.h"
#include "pw_containers/forward_list.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_containers/intrusive_queue.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/internal/config.h"
#include "pw_rpc/internal/rpc2_adapter.h"
#include "pw_rpc/server.h"
#include "pw_rpc/service.h"
#include "pw_rpc/service_id.h"
#include "pw_rpc2/service.h"
#include "pw_status/status.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/options.h"
#include "pw_thread/thread.h"

namespace pw::rpc2::internal {
class ServerConnectionTask;
class ServerTask;
}  // namespace pw::rpc2::internal

namespace pw::rpc {

// The adapter calls the pw::rpc::Server from its worker thread, from threads
// that write to adapted calls, and alongside the project's own use of the
// server, so the server must be thread safe.
static_assert(PW_RPC_USE_GLOBAL_MUTEX,
              "The pw_rpc Rpc2Adapter requires PW_RPC_USE_GLOBAL_MUTEX = 1");

/// @module{pw_rpc}

class Rpc2Adapter;
template <typename>
class Rpc2ServiceFor;

/// Policy tag for `Rpc2ServiceFor` and `Rpc2ServiceAdapter` that delivers the
/// response payload (with `kOk`) when a unary or client streaming `pw_rpc`
/// handler finishes with a non-@OK status.
///
/// Pass generated `pw_rpc` method functions (such as
/// `MyServiceImpl::ServiceInfo::MyMethod`) to opt in specific methods, or omit
/// template arguments (`DeliverNonOkResponses()`) to opt in all unary and
/// client streaming methods of the service.
///
/// @warning `pw_rpc2` has no application status codes, so `pw_rpc2` clients
/// receive `kOk` alongside the serialized response for **any** non-@OK handler
/// status, including early error returns that leave the response
/// default-initialized. Opted-in methods must encode error state in the
/// response message itself on every non-@OK return path. Only use the
/// service-wide `DeliverNonOkResponses()` form when every unary and client
/// streaming method in the service follows this contract.
template <auto... kNonOkResponseMethods>
constexpr internal::DeliverNonOkResponsesTag<kNonOkResponseMethods...>
DeliverNonOkResponses() {
  return internal::DeliverNonOkResponsesTag<kNonOkResponseMethods...>{};
}

/// Configuration for `Rpc2Adapter` and `Rpc2ServerAdapter`.
struct Rpc2AdapterConfig {
  /// Lowest channel ID the adapter assigns to `pw_rpc2` connections. Must be
  /// larger than a project's own channel IDs and must not be 0. Defaults to
  /// 100.
  ///
  /// New IDs are assigned sequentially upward from `first_channel_id`. IDs
  /// already open on the server are skipped. Closed connections' channels
  /// remain open, but are reused before allocating new IDs.
  ///
  /// ID assignments never wrap past `UINT32_MAX`, so set `first_channel_id`
  /// above your project's largest channel ID, but leave room below `UINT32_MAX`
  /// for peak concurrent `pw_rpc2` connections.
  uint32_t first_channel_id = 100;

  /// Maximum number of channels the adapter keeps open on the server at once,
  /// or 0 for no limit other than the server's free channel slots.
  uint16_t max_channels = 0;

  /// Maximum outgoing stream messages to buffer per call before writes fail
  /// (must not be 0). Defaults to 8 so handlers can write a short burst without
  /// waiting while bounding the memory a stalled call can hold.
  uint16_t max_queued_messages_per_call = 8;
};

/// Adapts an existing `pw::rpc::Server` to serve `pw_rpc` services on a
/// `pw::rpc2::Server`.
///
/// The adapter injects calls from `pw_rpc2` clients into an existing
/// `pw::rpc::Server`, so one service instance can serve `pw_rpc` and `pw_rpc2`
/// clients at the same time. (If you do not have an existing `pw::rpc::Server`,
/// use `Rpc2Adapter` instead.) Wrap each service with an `Rpc2ServiceAdapter`
/// (or construct and own it with `Rpc2ServiceFor`) and register the wrapper
/// with a `pw::rpc2::Server`. `pw_rpc2` clients generated from the same
/// `.proto` file can call the service without changes.
///
/// Each `pw_rpc2` connection appears to `pw_rpc` services as its own channel.
/// The adapter opens channels on the server as connections need them, in any
/// free channel slot, assigning IDs upward from
/// `Rpc2AdapterConfig::first_channel_id` and skipping IDs already in use. A
/// connection keeps its channel ID for its whole lifetime, across any number of
/// calls. After a connection closes, its channel ID may be reused by a later
/// connection. Do not close the adapter's channels on a shared server: doing so
/// aborts their `pw_rpc` calls without ending the `pw_rpc2` calls. The adapter
/// opens a new channel for the connection's next packet. A shared server must
/// not have a default channel output (`SetDefaultChannelOutput()`), which
/// prevents opening channels.
///
/// Method handlers and stream callbacks for `pw_rpc2` clients run on a worker
/// thread started by the adapter, so they may block without stalling the
/// `pw_rpc2` dispatcher. Handlers for the server's other channels run wherever
/// the project processes those packets, so a shared service may be invoked from
/// both threads concurrently.
///
/// Differences from running on a `pw::rpc::Server` alone:
///
/// - **Status codes**: `pw_rpc2` does not have application status codes. By
///   default, finishing a call with a non-@OK status ends the `pw_rpc2` call
///   with `kCancelled` and discards any response payload. Pass
///   `DeliverNonOkResponses()` to `Rpc2ServiceFor` or `Rpc2ServiceAdapter` for
///   unary and client streaming methods that encode failure in the response
///   message alongside a non-@OK status.
/// - **Flow control**: `pw_rpc` has no flow control. The adapter buffers up to
///   `max_queued_messages_per_call` outgoing stream messages per call; writes
///   beyond that limit or when the allocator is exhausted fail.
/// - **Client stream completion**: Client and bidirectional streaming handlers
///   that need to know when the client closes its stream must enable
///   `PW_RPC_COMPLETION_REQUEST_CALLBACK` and call
///   `set_on_completion_requested`.
///
/// The adapter requires `PW_RPC_USE_GLOBAL_MUTEX`.
///
/// @example{pw_rpc/examples/use_rpc_services_in_rpc2.cc,pw_rpc-examples-rpc2-server-adapter}
class Rpc2ServerAdapter {
 public:
  /// Creates an adapter for `server` and starts the worker thread that runs
  /// `pw_rpc` handlers for `pw_rpc2` clients.
  ///
  /// @param[in] server The server that adapted services are registered with.
  ///   It needs a free channel slot (or `PW_RPC_DYNAMIC_ALLOCATION`) for each
  ///   concurrent `pw_rpc2` connection. Must outlive the adapter.
  /// @param[in] allocator Buffers in-flight packets and holds per-connection
  ///   state. Accessed under the adapter's lock, so it need not be thread-safe
  ///   unless shared outside the adapter.
  /// @param[in] options Options for the worker thread. Anything `options`
  ///   references (such as a stack or thread context) must outlive the adapter.
  /// @param[in] config Channel assignment and buffering limits.
  ///
  /// @pre `server` must not use `SetDefaultChannelOutput()`, and
  ///   `config.first_channel_id` and `config.max_queued_messages_per_call` must
  ///   not be 0.
  Rpc2ServerAdapter(Server& server,
                    Allocator& allocator,
                    const thread::Options& options,
                    const Rpc2AdapterConfig& config = Rpc2AdapterConfig())
      : Rpc2ServerAdapter(
            /*owns_server=*/false, server, allocator, options, config) {}

  /// Calls `Stop()` and frees any remaining queued packets.
  ///
  /// Without `PW_THREAD_JOINING_ENABLED`, the worker thread cannot be stopped,
  /// so destroying the adapter crashes. Adapters on such backends must live
  /// for the rest of the program.
  ///
  /// @pre All `Rpc2ServiceAdapter`s using this adapter must have been
  ///   destroyed, and the `pw::rpc2::Server` closed.
  ~Rpc2ServerAdapter();

  Rpc2ServerAdapter(const Rpc2ServerAdapter&) = delete;
  Rpc2ServerAdapter& operator=(const Rpc2ServerAdapter&) = delete;

  /// Stops the worker thread after processing any packets already queued (such
  /// as cancellations from closing the `pw::rpc2::Server`), then closes the
  /// channels the adapter opened on the server. Idempotent.
  ///
  /// Close the `pw::rpc2::Server` and call `Stop()` before destroying adapted
  /// services, since handlers may run until `Stop()` returns.
  ///
  /// Fails to compile without `PW_THREAD_JOINING_ENABLED`.
  ///
  /// @pre The `pw::rpc2::Server` must be closed. Must not be called from the
  ///   adapter's worker thread.
  template <int&... kExplicitGuard,
            bool kJoiningEnabled = PW_THREAD_JOINING_ENABLED>
  void Stop() PW_LOCKS_EXCLUDED(lock_) {
    static_assert(
        kJoiningEnabled,
        "Rpc2ServerAdapter::Stop() requires a pw_thread backend that supports "
        "joining (PW_THREAD_JOINING_ENABLED = 1)");
    DoStop();
  }

 private:
  friend class internal::Rpc2LegacyCall;
  friend class Rpc2Adapter;
  template <typename>
  friend class Rpc2ServiceFor;

  using QueuedPacket = internal::Rpc2QueuedPacket;
  using Binding = internal::Rpc2ChannelBinding;
  using Call = internal::Rpc2LegacyCall;

  Rpc2ServerAdapter(bool owns_server,
                    Server& server,
                    Allocator& allocator,
                    const thread::Options& options,
                    const Rpc2AdapterConfig& config);

  // Implements Stop(). Only defined if PW_THREAD_JOINING_ENABLED.
  void DoStop() PW_LOCKS_EXCLUDED(lock_);

  // Receives the packets the server sends on the adapter's channels.
  class Output final : public ChannelOutput {
   public:
    constexpr explicit Output(Rpc2ServerAdapter& adapter)
        : ChannelOutput("pw::rpc::Rpc2ServerAdapter"), adapter_(adapter) {}

    bool SupportsSendPacket() const override { return true; }

    Status SendPacket(const Packet& packet)
        PW_RPC_CHANNEL_OUTPUT_SEND_LOCK_REQUIREMENT override {
      return adapter_.HandleLegacyPacket(packet);
    }

    // Never called, since SupportsSendPacket() returns true.
    Status Send(span<const std::byte>)
        PW_RPC_CHANNEL_OUTPUT_SEND_LOCK_REQUIREMENT override {
      return Status::Unimplemented();
    }

   private:
    Rpc2ServerAdapter& adapter_;
  };

  // What NextEvent() returns: the next packet the legacy server sent for a
  // call, or, if there is none, whether and how the legacy call ended.
  struct Event {
    QueuedPacket* packet;
    bool done;
    rpc2::internal::ProtocolError error;
  };

  // Runs on the adapter's thread.
  void RunThread() PW_LOCKS_EXCLUDED(lock_);

  // Passes a queued packet to the legacy server and frees or repurposes it.
  void ProcessPacket(QueuedPacket& queued) PW_LOCKS_EXCLUDED(lock_);

  // Returns the ID of the channel for `queued`'s connection, opening the
  // channel if needed. If `queued`'s call already ended, or the channel cannot
  // be opened (ending the call), frees `queued` and returns
  // `Channel::kUnassignedChannelId`. Called on the adapter's thread.
  uint32_t ChannelFor(QueuedPacket& queued) PW_LOCKS_EXCLUDED(lock_);

  // Opens the binding's channel on the server, moving it to a different ID if
  // its ID is taken. Returns the channel's ID, or
  // `Channel::kUnassignedChannelId` if no channel could be opened. Called on
  // the adapter's thread.
  uint32_t OpenChannel(Binding& binding) PW_LOCKS_EXCLUDED(lock_);

  // Handles a packet from the legacy server. Called on any thread.
  Status HandleLegacyPacket(const internal::Packet& packet)
      PW_LOCKS_EXCLUDED(lock_);

  // Functions for Rpc2LegacyCall, which calls them on the pw_rpc2 dispatcher.
  QueuedPacket* NewRequest(rpc2::internal::ServerConnectionTask& connection,
                           uint32_t service_id,
                           uint32_t method_id,
                           ConstByteSpan payload) PW_LOCKS_EXCLUDED(lock_);
  void StartCall(Call& call, QueuedPacket& request) PW_LOCKS_EXCLUDED(lock_);
  void EndCall(Call& call) PW_LOCKS_EXCLUDED(lock_);
  Event NextEvent(Call& call, async2::Context& cx) PW_LOCKS_EXCLUDED(lock_);
  bool InboundBlocked(Call& call, async2::Context& cx) PW_LOCKS_EXCLUDED(lock_);
  bool SendToLegacy(Call& call,
                    internal::pwpb::PacketType type,
                    ConstByteSpan payload) PW_LOCKS_EXCLUDED(lock_);

  // Returns the binding for `connection`, taking over a closed connection's
  // binding or creating a new one if needed. Returns null if no binding is
  // available. Called on the pw_rpc2 dispatcher.
  Binding* AcquireBindingLocked(
      rpc2::internal::ServerConnectionTask& connection)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);
  // Returns the channel ID for a new binding, or for one whose ID is taken on
  // the server: one past the highest ID any binding uses, or
  // Channel::kUnassignedChannelId if IDs are exhausted. Bindings are only
  // removed by Stop(), and closed connections' bindings are reused, so the
  // only gaps below that ID are IDs held by other users of the server.
  uint32_t NextChannelIdLocked() const PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  QueuedPacket* NewPacketLocked(Binding& binding,
                                internal::pwpb::PacketType type,
                                uint32_t service_id,
                                uint32_t method_id,
                                ConstByteSpan payload)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void FreePacketLocked(QueuedPacket* packet)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void EnqueueLocked(QueuedPacket& packet) PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);
  bool PushToInboxLocked(Call::Shared& call, const internal::Packet& packet)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void FinishLocked(Call::Shared& call, rpc2::internal::ProtocolError error)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);
  Call::Shared* FindCallLocked(uint32_t call_id)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);
  uint32_t AllocateCallIdLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  Server& server_;
  Allocator& allocator_;
  const Rpc2AdapterConfig config_;

  sync::Mutex lock_;
  IntrusiveQueue<QueuedPacket> work_ PW_GUARDED_BY(lock_);
  IntrusiveForwardList<Call::Shared> calls_ PW_GUARDED_BY(lock_);
  ForwardList<Binding> bindings_ PW_GUARDED_BY(lock_);
  // The pw_rpc2 server the adapted services are registered with, learned from
  // the first call. An adapter serves only one pw_rpc2 server.
  const rpc2::internal::ServerTask* rpc2_server_ PW_GUARDED_BY(lock_) = nullptr;

  uint32_t next_call_id_ PW_GUARDED_BY(lock_) = 1;
  uint16_t binding_count_ PW_GUARDED_BY(lock_) = 0;
  bool stop_requested_ PW_GUARDED_BY(lock_) = false;
  const bool owns_server_;  // True for an Rpc2Adapter

  sync::ThreadNotification work_available_;

  Output output_;
  Thread thread_;
};

namespace internal {

// Holds the Server of an Rpc2Adapter in a base class, so that it is
// constructed before, and destroyed after, the Rpc2ServerAdapter base that
// uses it. Only accessed when constructing Rpc2Adapter; afterwards, the
// adapter uses the server through Rpc2ServerAdapter::server_.
class Rpc2OwnedServer {
 protected:
  _PW_RPC_CONSTEXPR Rpc2OwnedServer() : owned_server_(span<Channel>()) {}

  Server owned_server_;
};

}  // namespace internal

/// Runs `pw_rpc` services on a `pw::rpc2::Server` without an existing
/// `pw::rpc::Server`.
///
/// Use this when adapted services only need to serve `pw_rpc2` clients, and use
/// `Rpc2ServerAdapter` when adapted services also serve `pw_rpc` clients on an
/// existing `pw::rpc::Server`. Register services with this adapter using
/// `Rpc2ServiceFor`.
///
/// Like `Rpc2ServerAdapter`, it must never be destroyed if the `pw_thread`
/// backend does not support joining.
///
/// @example{pw_rpc/examples/use_rpc_services_in_rpc2.cc,pw_rpc-examples-rpc2-adapter}
class Rpc2Adapter final :
    /// @cond
    private internal::Rpc2OwnedServer,
    /// @endcond
    public Rpc2ServerAdapter {
 public:
  /// Creates an adapter and starts the worker thread that runs `pw_rpc`
  /// handlers for `pw_rpc2` clients.
  ///
  /// @param[in] allocator Buffers in-flight packets and holds per-connection
  ///   state. Accessed under the adapter's lock, so it need not be thread-safe
  ///   unless shared outside the adapter.
  /// @param[in] options Options for the worker thread. Anything `options`
  ///   references (such as a stack or thread context) must outlive the adapter.
  /// @param[in] config Channel assignment and buffering limits.
  Rpc2Adapter(Allocator& allocator,
              const thread::Options& options,
              const Rpc2AdapterConfig& config = Rpc2AdapterConfig())
      : Rpc2ServerAdapter(/*owns_server=*/true,
                          this->owned_server_,
                          allocator,
                          options,
                          config) {}
};

/// Wraps a `pw_rpc` service as a `pw::rpc2::Service`.
///
/// `pw_rpc2` calls are forwarded to the adapter's `pw::rpc::Server`, which
/// dispatches them to whichever service with this service's ID is registered
/// there. The wrapper does not register or reference `service`. To construct,
/// own, and register the service in one object, use `Rpc2ServiceFor`.
///
/// If no service with this ID is registered with the adapter's server, calls
/// fail with `pw::rpc2::RpcStatus::Unimplemented()`.
class Rpc2ServiceAdapter : public rpc2::Service {
 public:
  /// Wraps `service` for registration with a `pw::rpc2::Server`.
  ///
  /// @param[in] adapter The adapter that runs `service`'s handlers. Must
  ///   outlive this object.
  /// @param[in] service The `pw_rpc` service implementation to serve. Register
  ///   it with the adapter's `pw::rpc::Server`.
  template <typename LegacyService>
  Rpc2ServiceAdapter(Rpc2ServerAdapter& adapter, LegacyService& service)
      : rpc2::Service(internal::AdaptedServiceId(service),
                      internal::kAdaptedMethods<LegacyService>),
        adapter_(adapter) {}

  /// Wraps `service` for registration with a `pw::rpc2::Server`, delivering the
  /// response payload (with `kOk`) for selected unary or client streaming
  /// methods even when their `pw_rpc` handler finishes with a non-@OK status.
  ///
  /// See `DeliverNonOkResponses()` for usage and caveats regarding early error
  /// returns.
  ///
  /// @param[in] adapter The adapter that runs `service`'s handlers. Must
  ///   outlive this object.
  /// @param[in] policy `DeliverNonOkResponses<Methods...>()` for specific
  ///   methods, or `DeliverNonOkResponses()` for all unary and client streaming
  ///   methods of `service`.
  /// @param[in] service The `pw_rpc` service implementation to serve. Register
  ///   it with the adapter's `pw::rpc::Server`.
  template <auto... kNonOkResponseMethods, typename LegacyService>
  Rpc2ServiceAdapter(Rpc2ServerAdapter& adapter,
                     [[maybe_unused]] internal::DeliverNonOkResponsesTag<
                         kNonOkResponseMethods...> policy,
                     LegacyService& service)
      : rpc2::Service(internal::AdaptedServiceId(service),
                      internal::kAdaptedMethodsWithNonOkResponses<
                          LegacyService,
                          kNonOkResponseMethods...>),
        adapter_(adapter) {}

  Rpc2ServiceAdapter(const Rpc2ServiceAdapter&) = delete;
  Rpc2ServiceAdapter& operator=(const Rpc2ServiceAdapter&) = delete;

 protected:
  Rpc2ServerAdapter& adapter() const { return adapter_; }

 private:
  friend class internal::Rpc2LegacyCall;

  uint32_t service_id() const { return rpc2::Service::service_id(); }

  const rpc2::internal::Method& method(size_t index) const {
    return rpc2::Service::methods()[index];
  }

  Rpc2ServerAdapter& adapter_;
};

namespace internal {

// Holds the service of an Rpc2ServiceFor in a base class, so that it is
// constructed before, and destroyed after, the Rpc2ServiceAdapter base that
// refers to it.
template <typename LegacyService>
class Rpc2OwnedService {
 protected:
  template <typename... Args>
  explicit Rpc2OwnedService(Args&&... args)
      : owned_service_(std::forward<Args>(args)...) {}

  LegacyService owned_service_;
};

}  // namespace internal

/// Constructs and owns a `pw_rpc` service as a `pw::rpc2::Service`, and
/// registers it with the adapter's `pw::rpc::Server`.
///
/// By default, finishing a `pw_rpc` call with a non-@OK status ends the
/// `pw_rpc2` call with `kCancelled`. Pass `DeliverNonOkResponses()` as the
/// second constructor argument to deliver response payloads for unary or client
/// streaming methods that finish with a non-@OK status.
template <typename LegacyService>
class Rpc2ServiceFor final :
    /// @cond
    private internal::Rpc2OwnedService<LegacyService>,
    /// @endcond
    public Rpc2ServiceAdapter {
 public:
  /// Constructs the wrapped `LegacyService` from `args` and registers it with
  /// the adapter's `pw::rpc::Server`.
  ///
  /// @param[in] adapter The adapter that runs the service's handlers. Must
  ///   outlive this object.
  /// @param[in] args Arguments forwarded to `LegacyService`'s constructor.
  template <typename... Args,
            typename = std::enable_if_t<
                !internal::kStartsWithDeliverNonOkTag<Args...>>>
  explicit Rpc2ServiceFor(Rpc2ServerAdapter& adapter, Args&&... args)
      : internal::Rpc2OwnedService<LegacyService>(std::forward<Args>(args)...),
        Rpc2ServiceAdapter(adapter, this->owned_service_) {
    adapter.server_.RegisterService(this->owned_service_);
  }

  /// Constructs the wrapped `LegacyService` from `args`, registers it with the
  /// adapter's `pw::rpc::Server`, and delivers the response payload (with
  /// `kOk`) for selected unary or client streaming methods even when their
  /// `pw_rpc` handler finishes with a non-@OK status.
  ///
  /// See `DeliverNonOkResponses()` for usage and caveats regarding early error
  /// returns.
  ///
  /// @param[in] adapter The adapter that runs the service's handlers. Must
  ///   outlive this object.
  /// @param[in] policy `DeliverNonOkResponses<Methods...>()` for specific
  ///   methods, or `DeliverNonOkResponses()` for all unary and client streaming
  ///   methods of `LegacyService`.
  /// @param[in] args Arguments forwarded to `LegacyService`'s constructor.
  template <auto... kNonOkResponseMethods, typename... Args>
  Rpc2ServiceFor(
      Rpc2ServerAdapter& adapter,
      internal::DeliverNonOkResponsesTag<kNonOkResponseMethods...> policy,
      Args&&... args)
      : internal::Rpc2OwnedService<LegacyService>(std::forward<Args>(args)...),
        Rpc2ServiceAdapter(adapter, policy, this->owned_service_) {
    adapter.server_.RegisterService(this->owned_service_);
  }

  /// Unregisters the service from the adapter's `pw::rpc::Server`.
  ///
  /// @pre The `pw::rpc2::Server` this service was registered with must be
  ///   closed, and `Rpc2ServerAdapter::Stop()` must have been called.
  ~Rpc2ServiceFor() {
    adapter().server_.UnregisterService(this->owned_service_);
  }

  /// Returns the wrapped `pw_rpc` service.
  LegacyService& service() { return this->owned_service_; }
  /// @copydoc service()
  const LegacyService& service() const { return this->owned_service_; }
};

/// @endmodule

}  // namespace pw::rpc
