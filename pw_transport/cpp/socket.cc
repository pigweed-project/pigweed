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

#include "pw_transport/socket.h"

#include <memory>
#include <utility>

#include "pw_assert/assert.h"

namespace pw::transport {

namespace internal {

BaseSocketFuture::BaseSocketFuture(DatagramSocketImpl& socket)
    : core_(async2::FutureState::kPending) {
  std::lock_guard lock(socket);
  if (socket.is_open()) {
    socket_ = pw::IntrusivePtr<DatagramSocketImpl>(&socket);
  }
}

BaseSocketFuture::BaseSocketFuture(BaseSocketFuture&& other) noexcept {
  MoveFrom(other);
}

BaseSocketFuture& BaseSocketFuture::operator=(
    BaseSocketFuture&& other) noexcept {
  if (this != &other) {
    RemoveFromSocket();
    MoveFrom(other);
  }
  return *this;
}

void BaseSocketFuture::MoveFrom(BaseSocketFuture& other)
    PW_NO_LOCK_SAFETY_ANALYSIS {
  if (other.socket_ == nullptr) {
    core_ = std::move(other.core_);
  } else {
    std::lock_guard lock(*other.socket_);
    core_ = std::move(other.core_);
    socket_ = std::move(other.socket_);
  }
}

void BaseSocketFuture::RemoveFromSocket() {
  if (socket_ != nullptr) {
    {
      std::lock_guard lock(*socket_);
      core_.Unlist();
    }
    socket_ = nullptr;
  }
}

}  // namespace internal

ReadFuture::ReadFuture(DatagramSocketImpl& socket) : BaseSocketFuture(socket) {}

async2::Poll<pw::ConstBuf> ReadFuture::Pend(async2::Context& cx) {
  DatagramSocketImpl* sock = socket();
  if (sock == nullptr) {
    PW_DASSERT(is_pendable());
    MarkCompletedWhenSocketUnbound();
    return async2::Ready(std::move(resolved_value_));
  }

  IntrusivePtr<DatagramSocketImpl> completed_socket;
  std::lock_guard lock(*sock);
  PW_DASSERT(is_pendable());

  async2::Poll<pw::ConstBuf> result = sock->DoRead();
  if (result.IsReady()) {
    completed_socket = Finish();
    return result;
  }

  if (sock->is_closed()) {
    completed_socket = Finish();
    return async2::Ready(pw::ConstBuf());
  }

  PW_ASYNC_STORE_WAKER(cx, core().waker(), "DatagramSocket::Read");
  if (!core().in_list()) {
    PW_ASSERT(sock->read_futures_.empty());
    sock->add_read_future(*this);
  }
  return async2::Pending();
}

ReserveWriteFuture::ReserveWriteFuture(ReserveWriteFuture&& other) noexcept
    : BaseSocketFuture(async2::FutureState::kPending), min_size_bytes_(0) {
  MoveFrom(other);
}

ReserveWriteFuture& ReserveWriteFuture::operator=(
    ReserveWriteFuture&& other) noexcept {
  if (this != &other) {
    RemoveFromSocket();
    // Cancelling a reservation locks its socket. Cancel before moving to
    // prevent deadlocks if both sockets share the same lock.
    resolved_value_.reset();
    MoveFrom(other);
  }
  return *this;
}

void ReserveWriteFuture::MoveFrom(ReserveWriteFuture& other)
    PW_NO_LOCK_SAFETY_ANALYSIS {
  if (other.socket() == nullptr) {
    core() = std::move(other.core());
    min_size_bytes_ = std::exchange(other.min_size_bytes_, 0);
    resolved_value_ = std::move(other.resolved_value_);
  } else {
    std::lock_guard lock(*other.socket());
    core() = std::move(other.core());
    MoveSocket(other);
    min_size_bytes_ = std::exchange(other.min_size_bytes_, 0);
    resolved_value_ = std::move(other.resolved_value_);
  }
}

void ReserveWriteFuture::Resolve(
    std::optional<WriteReservation>&& reservation) {
  resolved_value_ = std::move(reservation);
  core().WakeAndMarkReady();
}

async2::Poll<std::optional<WriteReservation>> ReserveWriteFuture::Pend(
    async2::Context& cx) {
  DatagramSocketImpl* sock = socket();
  if (sock == nullptr) {
    PW_DASSERT(is_pendable());
    MarkCompletedWhenSocketUnbound();
    return async2::Ready(std::move(resolved_value_));
  }

  IntrusivePtr<DatagramSocketImpl> completed_socket;
  std::lock_guard lock(*sock);
  PW_DASSERT(is_pendable());

  if (core().is_ready()) {
    completed_socket = Finish();
    return async2::Ready(std::move(resolved_value_));
  }

  if (sock->is_closed()) {
    completed_socket = Finish();
    return async2::Ready(std::optional<WriteReservation>(std::nullopt));
  }

  PW_ASYNC_STORE_WAKER(cx, core().waker(), "DatagramSocket::ReserveWrite");
  if (!core().in_list()) {
    sock->add_write_future(*this);
  }
  sock->ResolveOneWriter();
  if (core().is_ready()) {
    completed_socket = Finish();
    return async2::Ready(std::move(resolved_value_));
  }

  return async2::Pending();
}

CloseFuture::CloseFuture(DatagramSocketImpl& socket)
    : BaseSocketFuture(async2::FutureState::kPending) {
  if (socket.is_closed()) {
    return;
  }
  StoreSocket(socket);
  socket.add_closed_future(*this);
}

async2::Poll<> CloseFuture::Pend(async2::Context& cx) {
  DatagramSocketImpl* sock = socket();
  if (sock == nullptr) {
    PW_DASSERT(is_pendable());
    MarkCompletedWhenSocketUnbound();
    return async2::Ready();
  }

  IntrusivePtr<DatagramSocketImpl> completed_socket;
  std::lock_guard lock(*sock);
  PW_DASSERT(is_pendable());

  if (sock->is_closed()) {
    completed_socket = Finish();
    return async2::Ready();
  }

  PW_ASYNC_STORE_WAKER(cx, core().waker(), "DatagramSocket::Closed");
  return async2::Pending();
}

WriteReservation& WriteReservation::operator=(
    WriteReservation&& other) noexcept {
  if (this != &other) {
    Cancel();
    socket_ = std::move(other.socket_);
    buffer_ = std::move(other.buffer_);
  }
  return *this;
}

bool WriteReservation::Commit(size_t size_bytes) {
  PW_ASSERT(socket_ != nullptr);
  PW_ASSERT(size_bytes <= buffer_.size());
  buffer_ = pw::Truncate(std::move(buffer_), size_bytes);
  bool success = socket_.impl_->CommitWrite(std::move(buffer_));
  socket_.Reset();
  buffer_.reset();
  return success;
}

void WriteReservation::Cancel() {
  if (socket_) {
    socket_.impl_->CancelWrite(std::move(buffer_));
    socket_.Reset();
  }
  buffer_.reset();
}

void DatagramSocketImpl::pw_recycle() {
  pw::Allocator& alloc = allocator_;
  std::destroy_at(this);
  alloc.Deallocate(this);
}

void DatagramSocketImpl::MarkClosed() {
  if (state_ != State::kClosed) {
    state_ = State::kClosed;
    WakeReader();
    write_futures_.ResolveAllWith(
        [](ReserveWriteFuture& future)
            PW_NO_LOCK_SAFETY_ANALYSIS { future.Resolve(std::nullopt); });
    closed_futures_.ResolveAllWith(
        [](internal::BaseSocketFuture& future)
            PW_NO_LOCK_SAFETY_ANALYSIS { future.Wake(); });
  }
}

CloseFuture DatagramSocketImpl::Close() {
  std::lock_guard lock(*this);
  if (state_ == State::kOpen) {
    state_ = State::kClosing;
    DoClose();
  }
  return CloseFuture(*this);
}

CloseFuture DatagramSocketImpl::WhenClosed() {
  std::lock_guard lock(*this);
  return CloseFuture(*this);
}

ReserveWriteFuture DatagramSocketImpl::ReserveWrite(size_t min_size_bytes) {
  PW_ASSERT(min_size_bytes <= max_write_message_size_bytes());
  return ReserveWriteFuture(*this, min_size_bytes);
}

std::optional<WriteReservation> DatagramSocketImpl::TryReserveWrite(
    size_t min_size_bytes) {
  PW_ASSERT(min_size_bytes <= max_write_message_size_bytes());
  std::lock_guard lock(*this);
  if (!is_open()) {
    return std::nullopt;
  }
  if (!write_futures_.empty()) {
    ResolveOneWriter();
    return std::nullopt;
  }
  return DoTryReserveWrite(min_size_bytes);
}

bool DatagramSocketImpl::ResolveOneWriter() PW_NO_LOCK_SAFETY_ANALYSIS {
  std::optional<WriteReservation> reservation;
  ReserveWriteFuture* future = write_futures_.Remove(
      [&](ReserveWriteFuture& f) PW_NO_LOCK_SAFETY_ANALYSIS {
        reservation = DoTryReserveWrite(f.min_size_bytes());
        return reservation.has_value();
      });
  if (future != nullptr) {
    future->Resolve(std::move(*reservation));
    return true;
  }
  return false;
}

bool DatagramSocketImpl::CommitWrite(pw::Buf buffer) {
  PW_ASSERT(buffer.size() <= max_write_message_size_bytes());
  std::lock_guard lock(*this);
  if (is_closed()) {
    return false;
  }
  return DoCommitWrite(std::move(buffer));
}

void DatagramSocketImpl::CancelWrite(pw::Buf buffer) {
  std::lock_guard lock(*this);
  DoCancelWrite(std::move(buffer));
  WakeOneWriter();
}

void DatagramSocketImpl::AdoptLocked(WriteReservation& reservation,
                                     size_t prefix_trim,
                                     size_t suffix_trim) {
  reservation.socket_ = DatagramSocket(*this);
  reservation.buffer_ =
      pw::Slice(std::move(reservation.buffer_),
                prefix_trim,
                reservation.buffer_.size() - (prefix_trim + suffix_trim));
}

}  // namespace pw::transport
