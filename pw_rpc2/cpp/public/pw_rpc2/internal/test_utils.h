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

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_assert/check.h"
#include "pw_async2/runnable_dispatcher.h"
#include "pw_async2/value_future.h"
#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_containers/inline_deque.h"
#include "pw_result/result.h"
#include "pw_rpc2/client.h"
#include "pw_rpc2/internal/connection_task.h"
#include "pw_rpc2/internal/handshake.h"
#include "pw_rpc2/internal/method_info.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_rpc2/internal/serialize.h"
#include "pw_rpc2/method_type.h"
#include "pw_status/status.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_transport/socket.h"
#include "pw_transport/transport.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2::test {

/// Exposes `ReliableDatagramListener::WrapSocket` to test code.
///
/// `ReliableDatagramSocket`'s constructor is private: handles are normally
/// minted by a `ReliableDatagramListener` or `ReliableDatagramConnector` from
/// their protected `WrapSocket` hook. Tests frequently want a handle to a bare
/// `ReliableDatagramSocketImpl` with no transport behind it, so this class
/// inherits the hook and republishes it. It is abstract and is never
/// instantiated; only its static `WrapSocket` is used.
class SocketFactory : public transport::ReliableDatagramListener {
 public:
  using transport::ReliableDatagramListener::WrapSocket;
};

/// Returns a `ReliableDatagramSocket` handle to `impl` for testing.
inline transport::ReliableDatagramSocket WrapSocket(
    transport::ReliableDatagramSocketImpl& impl) {
  return SocketFactory::WrapSocket(impl);
}

class MockConnection : public transport::ReliableDatagramSocketImpl {
 public:
  /// Maximum number of committed (written) packets retained for inspection.
  /// Committing more than this many packets is a fatal error rather than a
  /// silent eviction, since dropping packets would corrupt the indices
  /// reported by test fixtures such as `PayloadView`.
  static constexpr size_t kMaxWrittenPackets = 16;

  /// Maximum number of queued inbound packets awaiting `DoRead()`.
  static constexpr size_t kMaxReadQueueCapacity = 16;

  explicit MockConnection(pw::Allocator& allocator);
  ~MockConnection() override = default;

  async2::Poll<pw::ConstBuf> DoRead() override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this);
  std::optional<transport::WriteReservation> DoTryReserveWrite(
      size_t size) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this);
  void DoClose() override PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  void SetBlockReserveWrite(bool block) PW_LOCKS_EXCLUDED(*this);
  void UnblockReserveWrite(size_t size) PW_LOCKS_EXCLUDED(*this);
  void SetNextRead(pw::Buf buf) PW_LOCKS_EXCLUDED(*this);
  void PushNextRead(pw::Buf buf) PW_LOCKS_EXCLUDED(*this);
  void TryResolveRead() PW_LOCKS_EXCLUDED(*this);
  ConstBuf last_written_buf() const PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    if (written_packets_.empty()) {
      return ConstBuf();
    }
    return ConstBuf::Unowned(written_packets_.back());
  }
  size_t commit_count() const PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    return commit_count_;
  }
  size_t written_packet_count() const PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    return written_packets_.size();
  }
  pw::ConstByteSpan written_packet(size_t index) const
      PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    PW_CHECK(index < written_packets_.size());
    return pw::ConstByteSpan(
        written_packets_[static_cast<decltype(written_packets_)::size_type>(
            index)]);
  }
  void clear_written() PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    written_packets_.clear();
    commit_count_ = 0;
  }
  /// Acquires the socket lock, unlike the inherited `is_closed()`, which
  /// requires the caller to already hold it.
  bool is_closed() const PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    return ReliableDatagramSocketImpl::is_closed();
  }

  void lock() const PW_EXCLUSIVE_LOCK_FUNCTION() override { lock_.lock(); }
  void unlock() const PW_UNLOCK_FUNCTION() override { lock_.unlock(); }

 protected:
  bool DoCommitWrite(pw::Buf&& buffer) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this);
  void DoCancelWrite(pw::Buf&& buffer) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

 private:
  pw::Allocator& alloc_;
  bool block_reserve_write_ PW_GUARDED_BY(*this) = false;
  pw::InlineDeque<pw::Buf, kMaxReadQueueCapacity> read_queue_
      PW_GUARDED_BY(*this);
  pw::InlineDeque<pw::Buf, kMaxWrittenPackets> written_packets_
      PW_GUARDED_BY(*this);
  size_t commit_count_ PW_GUARDED_BY(*this) = 0;
  mutable sync::InterruptSpinLock lock_;
};

// Safe RAII factory helper that allocates MockConnection on a Pigweed Allocator
// and returns both the transport::ReliableDatagramSocket handle and raw pointer
// for testing.
inline std::pair<transport::ReliableDatagramSocket, MockConnection*>
MakeMockConnection(pw::Allocator& allocator) {
  auto* raw = allocator.New<MockConnection>(allocator);
  return {WrapSocket(*raw), raw};
}

/// Queues the `kSynAck` a server sends to accept a client's `kSyn`.
void PushSynAck(pw::Allocator& allocator, MockConnection& connection);

/// Connects a `Client` over `connection` with `Client::Connect()`, answering
/// the handshake as a server would. `raw_connection` must be the
/// `MockConnection` behind `connection`.
///
/// The client writes two handshake packets (`kSyn` and `kAck`) before
/// returning, so the first packet a call writes is the third one recorded. Use
/// `MakeMockPeer()` instead when a test does not care about the handshake.
Client ConnectMockClient(async2::RunnableDispatcher& dispatcher,
                         pw::Allocator& allocator,
                         transport::ReliableDatagramSocket connection,
                         MockConnection& raw_connection);

/// A connection that hosts calls but serves nothing.
///
/// `ConnectionTask` is abstract, and its two concrete subclasses each bring a
/// role with them: a server connection needs a `Server` to resolve service
/// IDs against, and a client connection owns call-ID allocation and the
/// thread-safe `Close()`. A fixture that only needs somewhere for a `Call` to
/// live wants neither, so this supplies the missing `DoPend()` and nothing
/// else.
///
/// Packets still flow: posting this task drains the egress control queue and
/// delivers inbound messages to the calls registered on it. Only request
/// dispatch is absent --- an inbound `kStart` packet is rejected with
/// `UNIMPLEMENTED`, because there is no service to route it to.
class TestConnectionTask final : public internal::ConnectionTask {
 public:
  TestConnectionTask(
      internal::EstablishedConnection established_connection,
      pw::Allocator& allocator,
      internal::EndpointRole role = internal::EndpointRole::kServer)
      : ConnectionTask(std::move(established_connection), allocator, role) {}

 private:
  async2::Poll<> DoPend(async2::Context& cx) override {
    if (is_closed()) {
      return async2::Ready();
    }

    StoreWaker(cx);

    bool progressed = false;
    for (int i = 0; i < kMaxPacketsPerPoll; ++i) {
      progressed = PendPacket(cx);
      if (!progressed || is_closed()) {
        break;
      }
    }

    if (is_closed()) {
      return async2::Ready();
    }
    if (progressed) {
      cx.ReEnqueue();
    }
    return async2::Pending();
  }
};

/// Decodes a payload recorded by `MockPeer` into a message.
///
/// The mirror of `PayloadView`: raw request and stream-message bytes go in,
/// the type the method declares comes out.
///
/// `pw::ConstBuf` (what raw methods declare) is returned as an unowned view of
/// `payload`, so it stays valid exactly as long as the span does. Any other
/// type is deserialized through its `SerializerFor` specialization, whose
/// generated `*_serde.h` header the caller must include.
template <typename Msg>
pw::Result<Msg> PayloadAs(ConstByteSpan payload) {
  static_assert(!std::is_same_v<Msg, ConstByteSpan>,
                "Decode raw payloads as pw::ConstBuf; ConstByteSpan has no "
                "SerializerFor specialization.");

  if constexpr (std::is_same_v<Msg, ConstBuf>) {
    return ConstBuf::Unowned(payload);
  } else {
    return internal::Deserialize<Msg>(payload);
  }
}

/// Lazy, zero-allocation container view over captured response packets.
///
/// Deserializes proto messages on demand when `operator[]`, `back()`, or
/// iterators are accessed.
template <typename Response>
class PayloadView {
 public:
  class Iterator {
   public:
    using value_type = Response;
    using pointer = const Response*;
    using reference = Response;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    Iterator() = default;
    Iterator(const MockConnection* conn, uint32_t call_id, size_t packet_index)
        : conn_(conn), call_id_(call_id), packet_index_(packet_index) {
      AdvanceToValid();
    }

    Response operator*() const {
      PW_CHECK(conn_ != nullptr);
      auto decode_res = internal::InboundPacket::Decode(
          ConstBuf::Unowned(conn_->written_packet(packet_index_)));
      if (!decode_res.ok()) {
        ADD_FAILURE() << "PayloadView failed to decode written packet "
                      << packet_index_;
        return Response{};
      }
      if constexpr (std::is_same_v<Response, ConstByteSpan>) {
        return decode_res->payload();
      } else {
        auto deser_res = PayloadAs<Response>(decode_res->payload());
        if (!deser_res.ok()) {
          ADD_FAILURE() << "PayloadView failed to deserialize the payload of "
                           "written packet "
                        << packet_index_;
          return Response{};
        }
        return std::move(*deser_res);
      }
    }

    Iterator& operator++() {
      if (conn_ != nullptr && packet_index_ < conn_->written_packet_count()) {
        packet_index_++;
        AdvanceToValid();
      }
      return *this;
    }

    Iterator operator++(int) {
      Iterator copy = *this;
      ++(*this);
      return copy;
    }

    bool operator==(const Iterator& other) const {
      return conn_ == other.conn_ && call_id_ == other.call_id_ &&
             packet_index_ == other.packet_index_;
    }

    bool operator!=(const Iterator& other) const { return !(*this == other); }

   private:
    void AdvanceToValid() {
      if (conn_ == nullptr) {
        return;
      }
      while (packet_index_ < conn_->written_packet_count()) {
        auto pkt = internal::InboundPacket::Decode(
            ConstBuf::Unowned(conn_->written_packet(packet_index_)));
        if (pkt.ok() && pkt->call_id() == call_id_ && pkt->type().is_server() &&
            pkt->type().has_payload()) {
          break;
        }
        packet_index_++;
      }
    }

    const MockConnection* conn_ = nullptr;
    uint32_t call_id_ = 0;
    size_t packet_index_ = 0;
  };

  PayloadView(const MockConnection& conn, uint32_t call_id)
      : conn_(&conn), call_id_(call_id) {}

  Iterator begin() const { return Iterator(conn_, call_id_, 0); }
  Iterator end() const {
    return Iterator(conn_, call_id_, conn_->written_packet_count());
  }

  size_t size() const {
    size_t count = 0;
    for (auto it = begin(); it != end(); ++it) {
      count++;
    }
    return count;
  }

  bool empty() const { return size() == 0; }

  Response operator[](size_t index) const {
    auto it = begin();
    for (size_t i = 0; i < index; ++i) {
      if (it == end()) {
        ADD_FAILURE() << "PayloadView index " << index << " is out of range ("
                      << i << " responses available)";
        return Response{};
      }
      ++it;
    }
    if (it == end()) {
      ADD_FAILURE() << "PayloadView index " << index << " is out of range ("
                    << index << " responses available)";
      return Response{};
    }
    return *it;
  }

  Response back() const {
    const size_t count = size();
    if (count == 0) {
      ADD_FAILURE() << "PayloadView is empty; the RPC produced no responses";
      return Response{};
    }
    return operator[](count - 1);
  }

 private:
  const MockConnection* conn_;
  uint32_t call_id_;
};

/// A mocked downstream dependency: the `Client` that code under test calls
/// through, plus the remote endpoint that answers those calls.
///
/// Hand the client to the code under test with `client()`, then drive the
/// far end with `ExpectInvocation()`:
///
/// @code{.cpp}
///   auto peer = test::MakeMockPeer(dispatcher, allocator);
///   service.SetClient(EchoService::Client(peer.client()));
///   ...
///   peer.ExpectInvocation<EchoService::Echo>().Finish(EchoResponse{});
/// @endcode
///
/// Nothing is really on the other side of the connection: packets the client
/// writes are recorded, and packets it reads are injected here. The handshake
/// is skipped, so the client emits no `kSyn`/`kAck` and the first recorded
/// packet is the first real request.
///
/// Every send runs the dispatcher until it stalls, so by the time one returns
/// the client (and anything awaiting it) has already observed the packet.
///
/// Each `MockPeer` models one downstream dependency; use one per dependency.
class MockPeer {
 public:
  /// A downstream call observed by the peer, already checked to target
  /// `MethodInfo`. Obtained from `ExpectInvocation()`.
  ///
  /// @warning A handle decodes lazily from the recorded packet, so it must not
  /// outlive a `MockConnection::clear_written()`.
  template <typename MethodInfo>
  class Invocation {
   public:
    uint32_t call_id() const { return packet_.call_id(); }

    /// Completes the call successfully, carrying `response`.
    void Finish(const typename MethodInfo::Response& response) {
      static_assert(MethodInfo::kType == MethodType::kUnary ||
                        MethodInfo::kType == MethodType::kClientStreaming,
                    "Finish(response) completes a call that returns a single "
                    "message.");
      peer_->InjectResponse(call_id(), response);
    }

    /// Decodes the request payload the client sent.
    pw::Result<typename MethodInfo::Request> request() const {
      return PayloadAs<typename MethodInfo::Request>(packet_.payload());
    }

   private:
    friend class MockPeer;

    Invocation(MockPeer& peer, internal::InboundPacket&& packet)
        : peer_(&peer), packet_(std::move(packet)) {}

    MockPeer* peer_;
    internal::InboundPacket packet_;
  };

  MockPeer(Client client,
           MockConnection& connection,
           async2::RunnableDispatcher& dispatcher,
           pw::Allocator& allocator)
      : client_(std::move(client)),
        connection_(&connection),
        dispatcher_(&dispatcher),
        allocator_(&allocator) {}

  ~MockPeer() {
    // Close explicitly: code under test may still hold copies of the client.
    ControlFuture closed = client_.Close();
  }

  MockPeer(const MockPeer&) = delete;
  MockPeer& operator=(const MockPeer&) = delete;

  Client& client() { return client_; }

  /// Claims the single outstanding downstream call and binds it to
  /// `MethodInfo`.
  ///
  /// Runs the dispatcher first, then checks that exactly one unclaimed packet
  /// was written and that it starts a call routed to `MethodInfo`. Each packet
  /// is claimed once, so successive `ExpectInvocation()`s walk successive
  /// calls.
  template <typename MethodInfo>
  Invocation<MethodInfo> ExpectInvocation() {
    dispatcher_->RunUntilStalled();
    const size_t unclaimed = unclaimed_packet_count();
    PW_CHECK(unclaimed == 1u,
             "MockPeer::ExpectInvocation() expected exactly 1 outstanding "
             "call to service %u method %u, but %zu packets are pending.",
             static_cast<unsigned>(MethodInfo::kServiceId),
             static_cast<unsigned>(MethodInfo::kMethodId),
             unclaimed);
    internal::InboundPacket packet = DecodeWrittenPacket(claimed_++);
    PW_CHECK(packet.type().is_start(),
             "MockPeer expected a call to service %u method %u, but the "
             "client wrote a packet of type 0x%02x.",
             static_cast<unsigned>(MethodInfo::kServiceId),
             static_cast<unsigned>(MethodInfo::kMethodId),
             static_cast<unsigned>(packet.type().bits()));
    PW_CHECK(packet.service_id() == MethodInfo::kServiceId &&
                 packet.method_id() == MethodInfo::kMethodId,
             "MockPeer expected a call to service %u method %u, but the "
             "client called service %u method %u.",
             static_cast<unsigned>(MethodInfo::kServiceId),
             static_cast<unsigned>(MethodInfo::kMethodId),
             static_cast<unsigned>(packet.service_id()),
             static_cast<unsigned>(packet.method_id()));
    return Invocation<MethodInfo>(*this, std::move(packet));
  }

  /// The number of packets written by the client that no `ExpectInvocation()`
  /// has claimed yet.
  ///
  /// Does not claim them. Use for negative assertions, such as checking that a
  /// service short-circuited instead of calling downstream.
  size_t unclaimed_packet_count() const {
    return connection_->written_packet_count() - claimed_;
  }

  MockConnection& connection() { return *connection_; }

 private:
  template <typename MethodInfo>
  friend class Invocation;

  /// Injects a response completing `call_id` successfully, carrying `payload`.
  ///
  /// `payload` may be a `pw::ConstBuf` (what raw methods declare, sent
  /// verbatim) or any message with a `SerializerFor` specialization, which is
  /// serialized first.
  template <typename Msg>
  void InjectResponse(uint32_t call_id, const Msg& payload) {
    pw::Buf owned;
    ConstByteSpan span;
    EncodePayload(payload, owned, span);
    InjectResponseBytes(call_id, span);
  }

  /// Decodes the `index`th packet the client wrote, which must be a valid RPC
  /// packet.
  internal::InboundPacket DecodeWrittenPacket(size_t index) const;

  void InjectResponseBytes(uint32_t call_id, ConstByteSpan payload);
  void Inject(pw::Result<pw::Buf>&& packet);

  /// Renders `msg` as bytes. `owned` keeps any allocation alive for as long as
  /// `out` is used.
  template <typename Msg>
  void EncodePayload(const Msg& msg, pw::Buf& owned, ConstByteSpan& out) {
    static_assert(
        !std::is_same_v<Msg, ConstByteSpan>,
        "Pass raw payloads as pw::ConstBuf, e.g. ConstBuf::Unowned(bytes). "
        "Raw methods declare Request/Response as pw::ConstBuf, and only "
        "ConstBuf has a SerializerFor specialization; ConstByteSpan has none.");

    if constexpr (std::is_same_v<Msg, ConstBuf>) {
      out = ConstByteSpan(msg.data(), msg.size());
      return;
    } else {
      const size_t size = internal::MaxEncodedSize(msg);
      if (size == 0) {
        out = ConstByteSpan();
        return;
      }
      owned = pw::Buf::TryAllocate(*allocator_, size);
      PW_CHECK(!owned.empty(),
               "MockPeer could not allocate %zu bytes for a message payload; "
               "its allocator is likely exhausted.",
               size);
      auto ser_res = internal::Serialize(msg, owned);
      PW_CHECK(ser_res.ok(),
               "MockPeer could not serialize a message into its %zu byte "
               "buffer (%s).",
               size,
               ser_res.status().str());
      out = ConstByteSpan(owned.data(), ser_res.size());
    }
  }

  Client client_;
  MockConnection* connection_;
  async2::RunnableDispatcher* dispatcher_;
  pw::Allocator* allocator_;
  /// How many written packets `ExpectInvocation()` has consumed.
  size_t claimed_ = 0;
};

/// Creates a `MockPeer` on a fresh `MockConnection`, skipping the handshake.
///
/// `dispatcher` must be the same dispatcher that drives the code under test,
/// so that a single `RunUntilStalled()` advances both sides.
///
/// @warning Declare `allocator` before `dispatcher`. `~Dispatcher` destroys
/// still-posted tasks, which live in `allocator`.
MockPeer MakeMockPeer(async2::RunnableDispatcher& dispatcher,
                      pw::Allocator& allocator);

class PairedConnection : public transport::ReliableDatagramSocketImpl {
 public:
  explicit PairedConnection(pw::Allocator& allocator);
  ~PairedConnection() override;

  async2::Poll<pw::ConstBuf> DoRead() override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this);
  std::optional<transport::WriteReservation> DoTryReserveWrite(
      size_t size) override PW_EXCLUSIVE_LOCKS_REQUIRED(*this);
  void DoClose() override PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  void SetPeer(PairedConnection* peer) PW_LOCKS_EXCLUDED(*this);
  void ClearPeer() PW_LOCKS_EXCLUDED(*this);
  PairedConnection* peer() const PW_LOCKS_EXCLUDED(*this);

  void SetBlockReserveWrite(bool block) PW_LOCKS_EXCLUDED(*this);
  void UnblockReserveWrite(size_t size) PW_LOCKS_EXCLUDED(*this);
  void TryResolveRead() PW_LOCKS_EXCLUDED(*this);
  void ReceivePacket(pw::ConstBuf packet) PW_LOCKS_EXCLUDED(*this);
  void SimulateDisconnect(Status status = Status::Cancelled(),
                          bool disconnect_peer = false)
      PW_LOCKS_EXCLUDED(*this);

  ConstBuf last_written_buf() const PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    return ConstBuf::Unowned(last_written_buf_);
  }
  size_t commit_count() const PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    return commit_count_;
  }
  /// Acquires the socket lock, unlike the inherited `is_closed()`, which
  /// requires the caller to already hold it.
  bool is_closed() const PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    return ReliableDatagramSocketImpl::is_closed();
  }

  void lock() const PW_EXCLUSIVE_LOCK_FUNCTION() override { lock_.lock(); }
  void unlock() const PW_UNLOCK_FUNCTION() override { lock_.unlock(); }

 protected:
  bool DoCommitWrite(pw::Buf&& payload) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this);
  void DoCancelWrite(pw::Buf&& payload) override
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

 private:
  pw::Allocator& alloc_;
  PairedConnection* peer_ PW_GUARDED_BY(*this) = nullptr;
  bool block_reserve_write_ PW_GUARDED_BY(*this) = false;

  static constexpr size_t kReadQueueCapacity = 64;
  pw::InlineDeque<pw::ConstBuf, kReadQueueCapacity> read_queue_
      PW_GUARDED_BY(*this);
  pw::Buf last_written_buf_ PW_GUARDED_BY(*this);
  size_t commit_count_ PW_GUARDED_BY(*this) = 0;
  mutable sync::InterruptSpinLock lock_;
};

struct PairedConnections {
  transport::ReliableDatagramSocket first;
  transport::ReliableDatagramSocket second;
  PairedConnection* first_raw = nullptr;
  PairedConnection* second_raw = nullptr;

  transport::ReliableDatagramSocket& client() { return first; }
  const transport::ReliableDatagramSocket& client() const { return first; }
  transport::ReliableDatagramSocket& server() { return second; }
  const transport::ReliableDatagramSocket& server() const { return second; }

  PairedConnection* client_raw() const { return first_raw; }
  PairedConnection* server_raw() const { return second_raw; }
};

inline PairedConnections MakePairedConnections(pw::Allocator& allocator) {
  auto* raw_first = allocator.New<PairedConnection>(allocator);
  auto* raw_second = allocator.New<PairedConnection>(allocator);
  PW_CHECK(raw_first != nullptr && raw_second != nullptr);
  raw_first->SetPeer(raw_second);
  raw_second->SetPeer(raw_first);
  return PairedConnections{
      .first = WrapSocket(*raw_first),
      .second = WrapSocket(*raw_second),
      .first_raw = raw_first,
      .second_raw = raw_second,
  };
}

inline PairedConnections MakePairedConnection(pw::Allocator& allocator) {
  return MakePairedConnections(allocator);
}

/// Bidirectional mock usable both as a server listener and a client connector.
class MockTransport : public transport::ReliableDatagramListener,
                      public transport::ReliableDatagramConnector {
 public:
  MockTransport() = default;
  ~MockTransport() override {
    if (accept_provider_.has_future()) {
      accept_provider_.Resolve(Status::Cancelled());
    }
    if (connect_provider_.has_future()) {
      connect_provider_.Resolve(Status::Cancelled());
    }
  }

  transport::ReliableDatagramConnector::ConnectFuture Connect() override {
    if (pending_connect_result_.has_value()) {
      auto res = std::move(*pending_connect_result_);
      pending_connect_result_.reset();
      return transport::ReliableDatagramConnector::ConnectFuture::Resolved(
          std::move(res));
    }
    return connect_provider_.Get();
  }
  transport::ReliableDatagramListener::AcceptFuture Accept() override {
    if (pending_accept_result_.has_value()) {
      auto res = std::move(*pending_accept_result_);
      pending_accept_result_.reset();
      return transport::ReliableDatagramListener::AcceptFuture::Resolved(
          std::move(res));
    }
    return accept_provider_.Get();
  }

  void ResolveConnect(Result<transport::ReliableDatagramSocket> res) {
    if (connect_provider_.has_future()) {
      connect_provider_.Resolve(std::move(res));
    } else {
      pending_connect_result_ = std::move(res);
    }
  }
  void ResolveAccept(Result<transport::ReliableDatagramSocket> res) {
    if (accept_provider_.has_future()) {
      accept_provider_.Resolve(std::move(res));
    } else {
      pending_accept_result_ = std::move(res);
    }
  }

  bool has_pending_connect() const {
    return connect_provider_.has_future() ||
           pending_connect_result_.has_value();
  }
  bool has_pending_accept() const {
    return accept_provider_.has_future() || pending_accept_result_.has_value();
  }

 private:
  async2::ValueProvider<Result<transport::ReliableDatagramSocket>>
      connect_provider_;
  async2::ValueProvider<Result<transport::ReliableDatagramSocket>>
      accept_provider_;
  std::optional<Result<transport::ReliableDatagramSocket>>
      pending_connect_result_;
  std::optional<Result<transport::ReliableDatagramSocket>>
      pending_accept_result_;
};

/// Closes `client` and runs `dispatcher` until the connection is torn down.
/// Use this instead of `Client::CloseBlocking()` on the thread that runs the
/// dispatcher.
inline void CloseClient(Client& client,
                        async2::RunnableDispatcher& dispatcher) {
  ControlFuture closed = client.Close();
  dispatcher.RunUntilStalled();
}

/// Holds a `Client` and, when the scope exits, closes it and runs the
/// dispatcher, so the connection task is torn down before the dispatcher and
/// allocator. Declare it after the dispatcher. Converts to `Client&`, so it can
/// be passed straight to a service client.
class ScopedClient {
 public:
  ScopedClient(Client client, async2::RunnableDispatcher& dispatcher)
      : client_(std::move(client)), dispatcher_(&dispatcher) {}

  ~ScopedClient() { CloseClient(client_, *dispatcher_); }

  ScopedClient(const ScopedClient&) = delete;
  ScopedClient& operator=(const ScopedClient&) = delete;

  operator Client&() & {  // NOLINT(google-explicit-constructor)
    return client_;
  }
  operator const Client&() const& {  // NOLINT(google-explicit-constructor)
    return client_;
  }
  operator const Client&() && = delete;

 private:
  Client client_;
  async2::RunnableDispatcher* dispatcher_;
};

}  // namespace pw::rpc2::test
