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
#include <utility>

#include "pw_assert/assert.h"
#include "pw_async2/await.h"
#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_bytes/span.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/call.h"
#include "pw_rpc2/internal/future_base.h"
#include "pw_rpc2/internal/packet.h"
#include "pw_status/status.h"
#include "pw_transport/socket.h"

namespace pw::rpc2 {

class ReserveWriteFuture;
class ServiceClient;

namespace internal {
class WriterBase;
struct CallAccess;
}  // namespace internal

/// A reserved buffer for writing an outbound RPC payload in place.
///
/// Returned by `ReserveWriteFuture` (from `Writer::ReserveWrite()` or
/// `UnaryWriter::ReserveFinish()`). Provides container-like access (`data()`,
/// `size()`, `operator[]`, iterators, and conversion to `ByteSpan`) to the
/// reserved buffer.
///
/// Write the payload into the buffer and call `Commit()` with the number of
/// bytes written to send it. Destroying the reservation without calling
/// `Commit()` (or calling `Drop()`) releases the buffer without sending
/// anything.
class WriteReservation {
 public:
  using iterator = ByteSpan::iterator;
  using const_iterator = ConstByteSpan::iterator;

  WriteReservation(const WriteReservation&) = delete;
  WriteReservation& operator=(const WriteReservation&) = delete;

  WriteReservation(WriteReservation&& other) noexcept = default;

  WriteReservation& operator=(WriteReservation&& other) noexcept {
    if (this != &other) {
      Drop();
      reservation_ = std::move(other.reservation_);
      packet_ = other.packet_;
      call_ = std::move(other.call_);
    }
    return *this;
  }

  ~WriteReservation() { Drop(); }

  /// Returns a pointer to the start of the reserved write buffer.
  std::byte* data() { return PayloadSpan().data(); }
  const std::byte* data() const { return PayloadSpan().data(); }

  /// Returns the size of the reserved write buffer in bytes.
  size_t size() const { return PayloadSpan().size(); }

  /// Accesses the byte at `index`.
  std::byte& operator[](size_t index) { return PayloadSpan()[index]; }
  const std::byte& operator[](size_t index) const {
    return PayloadSpan()[index];
  }

  /// Returns an iterator to the beginning of the reserved buffer.
  iterator begin() { return PayloadSpan().begin(); }
  const_iterator begin() const { return PayloadSpan().begin(); }
  const_iterator cbegin() const { return begin(); }

  /// Returns an iterator to the end of the reserved buffer.
  iterator end() { return PayloadSpan().end(); }
  const_iterator end() const { return PayloadSpan().end(); }
  const_iterator cend() const { return end(); }

  /// Sends the first `size_bytes` bytes of the reserved buffer.
  ///
  /// Consumes the reservation; it cannot be used or committed again.
  /// `size_bytes` must not exceed `size()`.
  ///
  /// @returns
  /// * `OK`: the message or response was queued for transmission.
  /// * `FAILED_PRECONDITION`: this reservation was already committed, dropped,
  ///   or moved from, or the call's outbound stream was already finished.
  /// * The call's completion status (such as `CANCELLED`) if the call ended
  ///   with an error while the reservation was held.
  /// * `UNAVAILABLE`: the connection closed before the write could be sent.
  [[nodiscard]] Status Commit(size_t size_bytes);

  /// Releases the reservation without sending any data.
  void Drop() {
    if (!is_active()) {
      return;
    }
    reservation_.Cancel();
    ReleaseCall(/*committed=*/false);
  }

 private:
  friend class ReserveWriteFuture;

  WriteReservation(transport::WriteReservation&& reservation,
                   internal::OutboundPacket packet,
                   IntrusivePtr<internal::Call> call = nullptr)
      : reservation_(std::move(reservation)),
        packet_(packet),
        call_(std::move(call)) {}

  bool is_active() const { return reservation_.data() != nullptr; }

  // Releases the call, telling it whether a terminal packet was committed.
  void ReleaseCall(bool committed);

  ConstByteSpan PayloadSpan() const;

  ByteSpan PayloadSpan() {
    ConstByteSpan span =
        static_cast<const WriteReservation*>(this)->PayloadSpan();
    return {const_cast<std::byte*>(span.data()), span.size()};
  }

  transport::WriteReservation reservation_;
  internal::OutboundPacket packet_;
  IntrusivePtr<internal::Call> call_;
};

/// Future returned by `Writer::ReserveWrite()` and
/// `UnaryWriter::ReserveFinish()` that resolves to a `WriteReservation`.
class ReserveWriteFuture : public internal::FutureBase {
 public:
  using value_type = Result<WriteReservation>;

  ReserveWriteFuture() = default;

  ReserveWriteFuture(const ReserveWriteFuture&) = delete;
  ReserveWriteFuture& operator=(const ReserveWriteFuture&) = delete;
  ReserveWriteFuture(ReserveWriteFuture&& other) noexcept = default;
  ReserveWriteFuture& operator=(ReserveWriteFuture&& other) noexcept {
    if (this != &other) {
      AbandonTerminalIfPending();
      internal::FutureBase::operator=(std::move(other));
      reserve_fut_ = std::move(other.reserve_fut_);
      packet_ = other.packet_;
      status_ = other.status_;
      call_ = std::move(other.call_);
    }
    return *this;
  }

  ~ReserveWriteFuture() { AbandonTerminalIfPending(); }

  /// Polls for the write reservation.
  ///
  /// @returns
  /// * `OK` with a `WriteReservation` once buffer space is available.
  /// * `FAILED_PRECONDITION` if the writer is already closed.
  /// * `RESOURCE_EXHAUSTED` if the requested size exceeds the maximum write
  ///   size.
  /// * The call's completion status (such as `CANCELLED`) if the call ended
  ///   with an error.
  /// * `UNAVAILABLE` if the connection is closed.
  [[nodiscard]] async2::Poll<Result<WriteReservation>> Pend(
      async2::Context& cx) {
    PW_ASSERT(is_pendable());

    if (!status_.ok()) {
      mark_complete();
      return async2::Ready(Result<WriteReservation>(status_));
    }

    PW_AWAIT(auto write_res, reserve_fut_, cx);
    mark_complete();

    if (!write_res.has_value()) {
      AbandonTerminalIfPending();
      // TODO: hepler@ - The transport only resolves a reservation to
      // `std::nullopt` when the socket has closed, so this should be
      // `UNAVAILABLE`, matching `SendControlPacketFuture` and the documented
      // `WriteFuture` behavior.
      return async2::Ready(
          Result<WriteReservation>(Status::ResourceExhausted()));
    }

    return async2::Ready(Result<WriteReservation>(
        WriteReservation(std::move(*write_res), packet_, std::move(call_))));
  }

 private:
  friend class internal::WriterBase;
  friend class ServiceClient;
  friend struct internal::CallAccess;

  // Constructs a future that resolves immediately to `status`, which must not
  // be `OK`.
  [[nodiscard]] static ReserveWriteFuture Failed(Status status) {
    return ReserveWriteFuture(status);
  }

  [[nodiscard]] static ReserveWriteFuture Message(
      Result<transport::ReserveWriteFuture> reserve_fut,
      internal::EndpointRole sender,
      uint32_t call_id,
      IntrusivePtr<internal::Call> call = nullptr) {
    return Create(std::move(reserve_fut),
                  internal::OutboundPacket::Message(sender, call_id),
                  std::move(call));
  }

  // Reserves the packet that starts a unary or server-streaming call with its
  // only request message.
  [[nodiscard]] static ReserveWriteFuture StartUnary(
      Result<transport::ReserveWriteFuture> reserve_fut,
      uint32_t call_id,
      uint32_t service_id,
      uint32_t method_id,
      IntrusivePtr<internal::Call> call = nullptr) {
    return Create(
        std::move(reserve_fut),
        internal::OutboundPacket::StartUnary(call_id, service_id, method_id),
        std::move(call));
  }

  // Reserves the packet that starts a client-streaming or bidirectional-
  // streaming call. It carries no message, and leaves the client's stream
  // open.
  [[nodiscard]] static ReserveWriteFuture StartStream(
      Result<transport::ReserveWriteFuture> reserve_fut,
      uint32_t call_id,
      uint32_t service_id,
      uint32_t method_id,
      IntrusivePtr<internal::Call> call = nullptr) {
    return Create(
        std::move(reserve_fut),
        internal::OutboundPacket::StartStream(call_id, service_id, method_id),
        std::move(call));
  }

  [[nodiscard]] static ReserveWriteFuture Response(
      Result<transport::ReserveWriteFuture> reserve_fut,
      uint32_t call_id,
      IntrusivePtr<internal::Call> call = nullptr) {
    return Create(std::move(reserve_fut),
                  internal::OutboundPacket::Response(call_id),
                  std::move(call));
  }

  // Reserves the packet that finishes `sender`'s stream normally. See
  // `internal::OutboundPacket::Finish()`.
  [[nodiscard]] static ReserveWriteFuture Finish(
      Result<transport::ReserveWriteFuture> reserve_fut,
      internal::EndpointRole sender,
      uint32_t call_id,
      IntrusivePtr<internal::Call> call = nullptr) {
    return Create(std::move(reserve_fut),
                  internal::OutboundPacket::Finish(sender, call_id),
                  std::move(call));
  }

  static ReserveWriteFuture Create(
      Result<transport::ReserveWriteFuture> reserve_fut,
      internal::OutboundPacket packet,
      IntrusivePtr<internal::Call> call = nullptr) {
    if (!reserve_fut.ok()) {
      return ReserveWriteFuture(reserve_fut.status());
    }
    if (call != nullptr && packet.closes_stream()) {
      call->BeginTerminalWrite();
    }
    return ReserveWriteFuture(std::move(*reserve_fut), packet, std::move(call));
  }

  explicit ReserveWriteFuture(Status status)
      : internal::FutureBase(async2::FutureState::kPending), status_(status) {
    PW_ASSERT(!status.ok());
  }

  ReserveWriteFuture(transport::ReserveWriteFuture reserve_fut,
                     internal::OutboundPacket packet,
                     IntrusivePtr<internal::Call> call)
      : internal::FutureBase(async2::FutureState::kPending),
        reserve_fut_(std::move(reserve_fut)),
        packet_(packet),
        call_(std::move(call)) {}

  void AbandonTerminalIfPending() {
    if (call_ != nullptr && packet_.closes_stream()) {
      call_->AbandonTerminalWrite();
    }
    call_ = nullptr;
  }

  transport::ReserveWriteFuture reserve_fut_;
  internal::OutboundPacket packet_;
  // `OK` while this future represents a real reservation attempt; otherwise
  // the reason the call could not write, resolved on the first `Pend`.
  Status status_;
  IntrusivePtr<internal::Call> call_;
};

static_assert(async2::Future<ReserveWriteFuture>);

}  // namespace pw::rpc2
