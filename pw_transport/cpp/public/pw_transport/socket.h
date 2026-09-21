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
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_assert/assert.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_buf/buf.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"
#include "pw_intrusive_ptr/recyclable.h"
#include "pw_intrusive_ptr/ref_counted.h"
#include "pw_memory/container_of.h"
#include "pw_sync/lock_annotations.h"

namespace pw::transport {

/// @module{pw_transport}

class ReliableDatagramSocket;
class ReliableDatagramSocketImpl;
class ReliableDatagramListener;
class ReliableDatagramConnector;
class WriteReservation;
class ReadFuture;
class ReserveWriteFuture;
class CloseFuture;

namespace internal {

/// Base class for socket-bound futures.
class BaseSocketFuture {
 public:
  constexpr BaseSocketFuture() = default;

  BaseSocketFuture(const BaseSocketFuture&) = delete;
  BaseSocketFuture& operator=(const BaseSocketFuture&) = delete;

  BaseSocketFuture(BaseSocketFuture&& other) noexcept
      PW_LOCKS_EXCLUDED(*socket_, *other.socket_);
  BaseSocketFuture& operator=(BaseSocketFuture&& other) noexcept
      PW_LOCKS_EXCLUDED(*socket_, *other.socket_);

  [[nodiscard]] bool is_pendable() const PW_NO_LOCK_SAFETY_ANALYSIS {
    return core_.is_pendable();
  }
  [[nodiscard]] bool is_complete() const PW_NO_LOCK_SAFETY_ANALYSIS {
    return core_.is_complete();
  }

  void Wake() PW_EXCLUSIVE_LOCKS_REQUIRED(*socket_) { core_.Wake(); }

 protected:
  explicit BaseSocketFuture(ReliableDatagramSocketImpl& socket)
      PW_LOCKS_EXCLUDED(socket);

  explicit constexpr BaseSocketFuture(async2::FutureState::Pending)
      : core_(async2::FutureState::kPending) {}

  // Unlists this future and removes a reference from its socket.
  void RemoveFromSocket() PW_LOCKS_EXCLUDED(*socket_);

  IntrusivePtr<ReliableDatagramSocketImpl> Finish()
      PW_EXCLUSIVE_LOCKS_REQUIRED(*socket_) {
    core_.Unlist();
    core_.MarkComplete();
    return std::move(socket_);
  }

  /// Marks a future that is not bound to a socket (e.g. an immediately resolved
  /// future where `socket_ == nullptr`) as complete. Because the socket doubles
  /// as the lock provider, without one, lock safety analysis has to be disabled
  /// to access `core_`.
  void MarkCompletedWhenSocketUnbound() PW_NO_LOCK_SAFETY_ANALYSIS {
    PW_DASSERT(socket_ == nullptr);
    core_.MarkComplete();
  }

  void StoreSocket(ReliableDatagramSocketImpl& socket)
      PW_EXCLUSIVE_LOCKS_REQUIRED(socket) {
    socket_ = IntrusivePtr<ReliableDatagramSocketImpl>(&socket);
  }

  void ClearSocket() { socket_ = nullptr; }
  void MoveSocket(BaseSocketFuture& other) {
    socket_ = std::move(other.socket_);
  }

  ReliableDatagramSocketImpl* socket() PW_LOCK_RETURNED(socket_.get()) {
    return socket_.get();
  }
  const ReliableDatagramSocketImpl* socket() const
      PW_LOCK_RETURNED(socket_.get()) {
    return socket_.get();
  }

  async2::FutureCore& core() PW_EXCLUSIVE_LOCKS_REQUIRED(*socket_) {
    return core_;
  }

 private:
  friend class ::pw::transport::ReliableDatagramSocketImpl;
  friend class ::pw::transport::ReserveWriteFuture;
  friend class ::pw::transport::CloseFuture;

  void MoveFrom(BaseSocketFuture& other)
      PW_LOCKS_EXCLUDED(*socket_, *other.socket_);

  IntrusivePtr<ReliableDatagramSocketImpl> socket_;
  async2::FutureCore core_ PW_GUARDED_BY(*socket_);

 public:
  using List = async2::FutureList<&BaseSocketFuture::core_>;
};

}  // namespace internal

/// A handle to an established, reliable, datagram-oriented transport socket.
///
/// `ReliableDatagramSocket` is the interface for reading and writing datagrams
/// over some underlying transport. All reads and writes operate using `Buf`
/// objects, which represent views into some memory region specific to the
/// socket. This makes zero-copy reads and writes possible, if the
/// implementation chooses to support it.
///
/// Datagrams are delivered reliably and in order. If a packet cannot be
/// delivered in order, the socket must close.
///
/// The underlying socket begins established and open, but may close at any
/// time. Once closed, all operations on the socket will report failures.
/// A `ReliableDatagramSocket` cannot be reopened. A new one must be provisioned
/// from a `ReliableDatagramListener` or a `ReliableDatagramConnector`.
///
/// A `ReliableDatagramSocket` is cheaply copyable and always safe to access,
/// ensuring that the underlying socket state is valid as long as a handle is
/// held. Users should drop their handles once no longer needed to ensure prompt
/// resource reclamation.
class ReliableDatagramSocket {
 public:
  constexpr ReliableDatagramSocket() : impl_(nullptr) {}
  constexpr ReliableDatagramSocket(std::nullptr_t) : impl_(nullptr) {}

  ReliableDatagramSocket(const ReliableDatagramSocket& other) = default;
  ReliableDatagramSocket(ReliableDatagramSocket&& other) noexcept = default;
  ReliableDatagramSocket& operator=(const ReliableDatagramSocket& other) =
      default;
  ReliableDatagramSocket& operator=(ReliableDatagramSocket&& other) noexcept =
      default;

  ~ReliableDatagramSocket() = default;

  explicit operator bool() const { return impl_ != nullptr; }

  bool operator==(const ReliableDatagramSocket& other) const {
    return impl_ == other.impl_;
  }
  bool operator!=(const ReliableDatagramSocket& other) const {
    return impl_ != other.impl_;
  }

  /// The maximum size in bytes that can be requested in a `ReserveWrite`.
  size_t max_write_message_size_bytes() const;

  /// The maximum size in bytes of a single datagram received from the peer.
  size_t max_read_message_size_bytes() const;

  /// Receives a single datagram from the peer, resolving to a `ConstBuf` view
  /// of its data.
  /// Resolves to a null `ConstBuf` when the socket is closed.
  ///
  /// Only one `Read` operation may be pended at a time. Pending a second
  /// `ReadFuture` while one is active will assert.
  ReadFuture Read() const;

  /// Requests to write a datagram of size at least `min_size_bytes`, waiting
  /// for space to become available.
  ///
  /// Resolves to a `std::optional<WriteReservation>` that allows writing to
  /// that buffer, or `std::nullopt` if the socket has closed.
  ///
  /// Multiple reservations or reservation requests can be outstanding at once.
  /// If multiple pending requests exist, the order in which they are resolved
  /// is not specified.
  ///
  /// Requesting larger than `max_write_message_size_bytes` will assert.
  ReserveWriteFuture ReserveWrite(size_t min_size_bytes) const;

  /// Synchronously attempts to reserve space for writing a datagram of at least
  /// `min_size_bytes`. Returns `std::nullopt` if space is not immediately
  /// available or if the socket is closed.
  [[nodiscard]] std::optional<WriteReservation> TryReserveWrite(
      size_t min_size_bytes) const;

  /// Terminally closes the socket, resolving once it has been fully torn
  /// down. If the socket is already closed, this is a no-op.
  CloseFuture Close() const;

  /// Returns a future that resolves when the socket is terminally closed
  /// or disconnected.
  CloseFuture WhenClosed() const;

 private:
  friend class ReliableDatagramSocketImpl;
  friend class ReliableDatagramListener;
  friend class ReliableDatagramConnector;
  friend class WriteReservation;

  explicit ReliableDatagramSocket(ReliableDatagramSocketImpl& impl)
      : impl_(&impl) {}

  /// Resets the handle, decrementing the reference count.
  void Reset() { impl_ = nullptr; }

  IntrusivePtr<ReliableDatagramSocketImpl> impl_;
};

/// A handle representing ownership of a buffer reservation for writing a
/// packet, providing container-like access to the underlying buffer.
///
/// A socket may close between the time a `WriteReservation` is provisioned
/// and `Commit` is called. If this occurs, `Commit` will return `false`, and
/// no packet will be queued.
///
/// Destructing a `WriteReservation` releases the underlying buffer back to
/// the transport.
class WriteReservation {
 public:
  using iterator = Buf::iterator;
  using const_iterator = Buf::const_iterator;

  WriteReservation(const WriteReservation&) = delete;
  WriteReservation& operator=(const WriteReservation&) = delete;

  WriteReservation(WriteReservation&& other) noexcept = default;
  WriteReservation& operator=(WriteReservation&& other) noexcept;

  ~WriteReservation() { Cancel(); }

  /// Returns a pointer to the start of the reserved write buffer.
  std::byte* data() { return buffer_.data(); }
  const std::byte* data() const { return buffer_.data(); }

  /// Returns the size of the reserved write buffer in bytes.
  size_t size() const { return buffer_.size(); }

  /// Accesses the byte at the specified index.
  std::byte& operator[](size_t index) { return buffer_[index]; }
  const std::byte& operator[](size_t index) const { return buffer_[index]; }

  /// Returns an iterator to the beginning of the reserved buffer.
  iterator begin() { return buffer_.begin(); }
  const_iterator begin() const { return buffer_.begin(); }
  const_iterator cbegin() const { return buffer_.cbegin(); }

  /// Returns an iterator to the end of the reserved buffer.
  iterator end() { return buffer_.end(); }
  const_iterator end() const { return buffer_.end(); }
  const_iterator cend() const { return buffer_.cend(); }

  /// Commits the written data in the reserved buffer, queueing it for
  /// transmission. Truncates the buffer to `size_bytes`. Returns true on
  /// success, or false if the socket has closed or transmission fails.
  /// Once committed, the reservation is consumed and cannot be reused or
  /// canceled.
  ///
  /// `size_bytes` must not exceed the reservation's size, or it will assert.
  [[nodiscard]] bool Commit(size_t size_bytes);

  /// Explicitly cancels the reservation, releasing the buffer back to the
  /// socket without transmitting any data.
  void Cancel();

 private:
  friend class ReliableDatagramSocketImpl;

  ReliableDatagramSocket socket_;
  Buf buffer_;

  WriteReservation(ReliableDatagramSocket socket, Buf&& buffer)
      : socket_(std::move(socket)), buffer_(std::move(buffer)) {}
};

/// Future representing receiving a single datagram from a
/// `ReliableDatagramSocket`.
class [[nodiscard]] ReadFuture final : public internal::BaseSocketFuture {
 public:
  using value_type = ConstBuf;

  constexpr ReadFuture() = default;

  ReadFuture(ReadFuture&& other) noexcept = default;
  ReadFuture& operator=(ReadFuture&& other) noexcept = default;

  ~ReadFuture() PW_LOCKS_EXCLUDED(*this->socket()) { RemoveFromSocket(); }

  async2::Poll<ConstBuf> Pend(async2::Context& cx)
      PW_LOCKS_EXCLUDED(*this->socket());

 private:
  friend class ReliableDatagramSocket;
  friend class ReliableDatagramSocketImpl;

  static ReadFuture Resolved(ConstBuf&& buffer) {
    return ReadFuture(std::move(buffer));
  }

  explicit ReadFuture(ReliableDatagramSocketImpl& socket);

  explicit ReadFuture(ConstBuf&& buffer)
      : BaseSocketFuture(async2::FutureState::kPending),
        resolved_value_(std::move(buffer)) {}

  ConstBuf resolved_value_;
};

static_assert(async2::Future<ReadFuture>);

/// Future representing reserving write space for a datagram in a
/// `ReliableDatagramSocket`.
class [[nodiscard]] ReserveWriteFuture final
    : public internal::BaseSocketFuture {
 public:
  using value_type = std::optional<WriteReservation>;

  constexpr ReserveWriteFuture() : min_size_bytes_(0) {}

  ReserveWriteFuture(ReserveWriteFuture&& other) noexcept;
  ReserveWriteFuture& operator=(ReserveWriteFuture&& other) noexcept;

  ~ReserveWriteFuture() PW_LOCKS_EXCLUDED(*this->socket()) {
    RemoveFromSocket();
  }

  [[nodiscard]] size_t min_size_bytes() const { return min_size_bytes_; }

  async2::Poll<std::optional<WriteReservation>> Pend(async2::Context& cx)
      PW_LOCKS_EXCLUDED(*this->socket());

 private:
  friend class ReliableDatagramSocket;
  friend class ReliableDatagramSocketImpl;
  friend class internal::BaseSocketFuture;

  static ReserveWriteFuture Resolved(
      std::optional<WriteReservation>&& reservation) {
    return ReserveWriteFuture(std::move(reservation));
  }

  void MoveFrom(ReserveWriteFuture& other);

  static ReserveWriteFuture* FromCore(async2::FutureCore* core) {
    return static_cast<ReserveWriteFuture*>(
        ContainerOf<&BaseSocketFuture::core_>(core));
  }
  static async2::FutureCore& ToCore(ReserveWriteFuture& future)
      PW_NO_LOCK_SAFETY_ANALYSIS {
    return future.core();
  }

  ReserveWriteFuture(ReliableDatagramSocketImpl& socket, size_t min_size_bytes)
      : BaseSocketFuture(socket), min_size_bytes_(min_size_bytes) {}

  explicit ReserveWriteFuture(std::optional<WriteReservation>&& reservation)
      : BaseSocketFuture(async2::FutureState::kPending),
        min_size_bytes_(0),
        resolved_value_(std::move(reservation)) {}

  void Resolve(std::optional<WriteReservation>&& reservation)
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this->socket());

  size_t min_size_bytes_;
  std::optional<WriteReservation> resolved_value_;

 public:
  using List = async2::CustomFutureList<FromCore, ToCore>;
};

static_assert(async2::Future<ReserveWriteFuture>);

/// Future representing waiting for a `ReliableDatagramSocket` to close.
class CloseFuture final : public internal::BaseSocketFuture {
 public:
  using value_type = void;

  constexpr CloseFuture() = default;

  CloseFuture(CloseFuture&& other) noexcept = default;
  CloseFuture& operator=(CloseFuture&& other) noexcept = default;

  ~CloseFuture() PW_LOCKS_EXCLUDED(*this->socket()) { RemoveFromSocket(); }

  async2::Poll<> Pend(async2::Context& cx) PW_LOCKS_EXCLUDED(*this->socket());

 private:
  friend class ReliableDatagramSocket;
  friend class ReliableDatagramSocketImpl;

  static CloseFuture Resolved() {
    return CloseFuture(async2::FutureState::kPending);
  }

  explicit constexpr CloseFuture(async2::FutureState::Pending)
      : BaseSocketFuture(async2::FutureState::kPending) {}

  explicit CloseFuture(ReliableDatagramSocketImpl& socket)
      PW_EXCLUSIVE_LOCKS_REQUIRED(socket);
};

static_assert(async2::Future<CloseFuture>);

/// Implementation base class for the transport operations on a socket.
///
/// Implementations are required to call the following hooks on socket
/// lifecycle events:
///
/// - `WakeReader()` when data arrives from the peer.
/// - `WakeOneWriter()` or `WakeAllWriters()` when space becomes available
///   for writing.
/// - `MarkClosed()` when the socket is closed by any means.
///
/// The `DoCancelWrite` operation has a default no-op implementation.
/// Implementations should override it if supported.
///
/// Implementations must handle the closed socket state across read, write,
/// reserve, and close operations.
///
/// # Thread safety and locking
///
/// Subclasses provide synchronization by implementing `lock()` and `unlock()`.
/// Implementations of the protected virtual methods (`DoRead`, `DoCommitWrite`,
/// `DoCancelWrite`, `DoClose`, `DoTryReserveWrite`) are called while the
/// socket lock is held. Subclasses must NOT call public methods on
/// `ReliableDatagramSocketImpl` (such as `CommitWrite`, `CancelWrite`,
/// `TryReserveWrite`, `Close`, or `Adopt`) from within these callbacks, as
/// doing so will attempt to re-acquire the lock and cause a deadlock when using
/// non-recursive locks.
class PW_LOCKABLE("pw::transport::ReliableDatagramSocketImpl")
    ReliableDatagramSocketImpl : public RefCounted<ReliableDatagramSocketImpl>,
                                 public Recyclable<ReliableDatagramSocketImpl> {
 public:
  enum class State : uint8_t {
    kOpen,
    kClosing,
    kClosed,
  };

  virtual ~ReliableDatagramSocketImpl() { MarkClosed(); }

  // Acquires the socket's lock.
  virtual void lock() const PW_EXCLUSIVE_LOCK_FUNCTION() = 0;

  // Releases the socket's lock.
  virtual void unlock() const PW_UNLOCK_FUNCTION() = 0;

  [[nodiscard]] State state() const PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return state_;
  }

  [[nodiscard]] bool is_open() const PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return state_ == State::kOpen;
  }

  [[nodiscard]] bool is_closed() const PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return state_ == State::kClosed;
  }

  [[nodiscard]] bool is_closing() const PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return state_ == State::kClosing;
  }

  size_t max_write_message_size_bytes() const {
    return max_write_message_size_bytes_;
  }
  size_t max_read_message_size_bytes() const {
    return max_read_message_size_bytes_;
  }

  /// Receives a single datagram from the peer.
  /// Resolves to a null `ConstBuf` if the socket is closed.
  ReadFuture Read() PW_LOCKS_EXCLUDED(*this) { return ReadFuture(*this); }

  /// Reserves write space for a datagram of at least `min_size_bytes`.
  /// If `min_size_bytes` is zero, returns an immediately resolved reservation
  /// with an empty `Buf` if the socket is open, or `std::nullopt` if closed.
  ReserveWriteFuture ReserveWrite(size_t min_size_bytes)
      PW_LOCKS_EXCLUDED(*this);

  /// Synchronously attempts to reserve write space for a datagram of at least
  /// `min_size_bytes`. If `min_size_bytes` is zero, returns an empty
  /// reservation immediately.
  [[nodiscard]] std::optional<WriteReservation> TryReserveWrite(
      size_t min_size_bytes) PW_LOCKS_EXCLUDED(*this);

  /// Closes the socket, resolving once teardown is complete.
  CloseFuture Close() PW_LOCKS_EXCLUDED(*this);

  /// Resolves when the socket is closed.
  CloseFuture WhenClosed() PW_LOCKS_EXCLUDED(*this);

  /// Commits `buffer` for transmission. Returns false if the socket is
  /// closed or transmission fails.
  ///
  /// Takes ownership of `buffer` by value to ensure that the buffer is released
  /// upon return even if transmission fails or the socket is closed.
  [[nodiscard]] bool CommitWrite(Buf buffer) PW_LOCKS_EXCLUDED(*this);

  /// Cancels a write reservation. Releases empty buffers immediately without
  /// invoking driver cancellation.
  ///
  /// Takes ownership of `buffer` by value to ensure that the buffer is
  /// released.
  void CancelWrite(Buf buffer) PW_LOCKS_EXCLUDED(*this);

 protected:
  /// Base constructor for an allocated, ref-counted socket.
  /// The socket is automatically destroyed and freed once no active
  /// `ReliableDatagramSocket` handles or futures reference it.
  constexpr ReliableDatagramSocketImpl(Allocator& allocator,
                                       size_t max_write_message_size_bytes,
                                       size_t max_read_message_size_bytes)
      : allocator_(allocator),
        max_write_message_size_bytes_(max_write_message_size_bytes),
        max_read_message_size_bytes_(max_read_message_size_bytes) {}

  constexpr ReliableDatagramSocketImpl(Allocator& allocator,
                                       size_t max_message_size_bytes)
      : ReliableDatagramSocketImpl(
            allocator, max_message_size_bytes, max_message_size_bytes) {}

  /// Marks the socket as closed, resolving any pending `Close()` or
  /// `WhenClosed()` futures.
  void MarkClosed() PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  /// Wakes any pending `ReadFuture`.
  void WakeReader()
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) PW_NO_LOCK_SAFETY_ANALYSIS {
    if (!read_futures_.empty()) {
      read_futures_.front().Wake();
    }
  }

  /// Wakes one pending `ReserveWriteFuture`.
  /// Should be called whenever space becomes available for writing.
  void WakeOneWriter() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    ResolveOneWriter();
  }

  /// Wakes all pending `ReserveWriteFuture`s.
  /// Should be called when space becomes available for writing.
  void WakeAllWriters() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    while (ResolveOneWriter()) {
    }
  }

  /// Factory method for subclasses to create reservations.
  WriteReservation CreateReservation(Buf&& buffer)
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    return WriteReservation(ReliableDatagramSocket(*this), std::move(buffer));
  }

  /// Associates an existing `WriteReservation` with this socket, slicing
  /// the buffer to reserve space for protocol headers and footers.
  void Adopt(WriteReservation& reservation,
             size_t prefix_trim = 0,
             size_t suffix_trim = 0) PW_LOCKS_EXCLUDED(*this) {
    std::lock_guard lock(*this);
    AdoptLocked(reservation, prefix_trim, suffix_trim);
  }

  void AdoptLocked(WriteReservation& reservation,
                   size_t prefix_trim = 0,
                   size_t suffix_trim = 0) PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

 private:
  friend class ReliableDatagramSocket;
  friend class WriteReservation;
  friend class Recyclable<ReliableDatagramSocketImpl>;
  friend class ReadFuture;
  friend class ReserveWriteFuture;
  friend class CloseFuture;
  friend class internal::BaseSocketFuture;

  /// Implements reading a single datagram from the peer.
  /// Called from ReadFuture::Pend with `*this` locked.
  virtual async2::Poll<ConstBuf> DoRead()
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) = 0;

  /// Implements synchronously reserving write space of at least
  /// `min_size_bytes`. Called with `*this` locked.
  virtual std::optional<WriteReservation> DoTryReserveWrite(
      size_t min_size_bytes) PW_EXCLUSIVE_LOCKS_REQUIRED(*this) = 0;

  /// Implements closing the socket. Called with `*this` locked.
  ///
  /// Implementers are required to call `MarkClosed` once teardown is completed.
  ///
  /// WARNING: Because DoClose() is invoked with `*this` locked, implementations
  /// that interact with peer sockets or lower layers must take care to
  /// avoid cross-socket AB-BA deadlocks if both sockets close
  /// concurrently from different threads.
  virtual void DoClose() PW_EXCLUSIVE_LOCKS_REQUIRED(*this) = 0;

  /// Commits `buffer` for transmission. Called with `*this` locked.
  ///
  /// WARNING: Because DoCommitWrite() is invoked with `*this` locked,
  /// forwarding data to other sockets (e.g. in layered transports, bridges,
  /// or paired peer sockets) creates an AB-BA deadlock hazard if the target
  /// socket attempts to transmit back or synchronize in reverse order.
  ///
  /// `buffer` is passed by rvalue reference to minimize moves while allowing
  /// the driver to take ownership (e.g. by moving it into a hardware
  /// transmission queue). If the driver does not consume `buffer`,
  /// `CommitWrite` guarantees that it will be destroyed and released upon
  /// return.
  virtual bool DoCommitWrite(Buf&& buffer)
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) = 0;

  /// Cancels a write reservation. Called with `*this` locked.
  ///
  /// `buffer` is passed by rvalue reference to minimize moves. If not consumed,
  /// `CancelWrite` will destroy and release it upon return.
  virtual void DoCancelWrite(Buf&& /*buffer*/)
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {}

  void pw_recycle();

  bool ResolveOneWriter() PW_EXCLUSIVE_LOCKS_REQUIRED(*this);

  void add_read_future(internal::BaseSocketFuture& future)
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    read_futures_.Push(future);
  }

  void add_write_future(ReserveWriteFuture& future)
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    write_futures_.Push(future);
  }

  void add_closed_future(internal::BaseSocketFuture& future)
      PW_EXCLUSIVE_LOCKS_REQUIRED(*this) {
    closed_futures_.Push(future);
  }

  Allocator& allocator_;
  size_t max_write_message_size_bytes_ = 0;
  size_t max_read_message_size_bytes_ = 0;
  State state_ PW_GUARDED_BY(*this) = State::kOpen;
  internal::BaseSocketFuture::List read_futures_ PW_GUARDED_BY(*this);
  ReserveWriteFuture::List write_futures_ PW_GUARDED_BY(*this);
  internal::BaseSocketFuture::List closed_futures_ PW_GUARDED_BY(*this);
};

inline size_t ReliableDatagramSocket::max_write_message_size_bytes() const {
  return impl_ != nullptr ? impl_->max_write_message_size_bytes() : 0;
}

inline size_t ReliableDatagramSocket::max_read_message_size_bytes() const {
  return impl_ != nullptr ? impl_->max_read_message_size_bytes() : 0;
}

inline ReadFuture ReliableDatagramSocket::Read() const {
  return impl_ != nullptr ? impl_->Read() : ReadFuture::Resolved(ConstBuf());
}

inline ReserveWriteFuture ReliableDatagramSocket::ReserveWrite(
    size_t min_size_bytes) const {
  return impl_ != nullptr ? impl_->ReserveWrite(min_size_bytes)
                          : ReserveWriteFuture::Resolved(std::nullopt);
}

inline std::optional<WriteReservation> ReliableDatagramSocket::TryReserveWrite(
    size_t min_size_bytes) const {
  return impl_ != nullptr ? impl_->TryReserveWrite(min_size_bytes)
                          : std::nullopt;
}

inline CloseFuture ReliableDatagramSocket::Close() const {
  return impl_ != nullptr ? impl_->Close() : CloseFuture::Resolved();
}

inline CloseFuture ReliableDatagramSocket::WhenClosed() const {
  return impl_ != nullptr ? impl_->WhenClosed() : CloseFuture::Resolved();
}

/// @endmodule

}  // namespace pw::transport
