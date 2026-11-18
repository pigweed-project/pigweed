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
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_result/result.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_transport/socket.h"
#include "pw_transport/transport.h"

namespace pw::transport {

/// @module{pw_transport}

/// Configuration for in-memory forwarding sockets (`ForwardingSocketPair` and
/// `ForwardingTransport`).
struct ForwardingSocketOptions {
  /// Default for `max_message_size_bytes`. Matches the 1500-byte Ethernet MTU,
  /// so a default socket behaves like a typical network link and bounds the
  /// size of each write buffer allocation.
  static constexpr size_t kDefaultMaxMessageSizeBytes = 1500;

  /// The maximum number of active write reservations plus unread datagrams
  /// allowed in each direction at once. Must be at least 1.
  ///
  /// When a socket has `queue_depth` active write reservations and unread
  /// datagrams queued on its peer, `TryReserveWrite` returns `std::nullopt` and
  /// `ReserveWrite` waits until the peer reads a datagram or an active
  /// reservation is committed or canceled.
  uint16_t queue_depth = 1;

  /// The largest message that can be sent or received, in bytes. Sets both
  /// `max_write_message_size_bytes()` and `max_read_message_size_bytes()` on
  /// each socket.
  size_t max_message_size_bytes = kDefaultMaxMessageSizeBytes;
};

/// A connected pair of in-memory `ReliableSocket` endpoints that
/// forward datagrams to each other.
///
/// Writes committed on `first()` are delivered as reads on `second()`, and
/// writes committed on `second()` are delivered as reads on `first()`.
/// Datagrams are forwarded without copying by transferring ownership of the
/// underlying `Buf` from the writer's reservation to the reader's queue.
///
/// The underlying socket endpoints are reference-counted and allocated from the
/// provided `Allocator`. Copies of the `ReliableSocket` handles returned by
/// `first()` and `second()` may outlive the `ForwardingSocketPair` object
/// itself; the shared state is automatically destroyed and deallocated once all
/// handles and futures for both endpoints have been released.
///
/// Closing one socket (or dropping all handles to it) prevents further writes
/// in both directions. Datagrams that were already committed before the socket
/// closed can still be read by the peer, after which the peer socket also
/// closes. Calling `Close()` on the `ForwardingSocketPair` closes both
/// endpoints immediately and discards any unread datagrams.
///
/// # Memory and locking
///
/// Each write reservation allocates its buffer from the pair's allocator. The
/// buffer is freed when the reader releases the received `ConstBuf`, or when
/// the reservation is canceled or fails to commit. Both sockets in a pair share
/// one `pw::sync::InterruptSpinLock`, and the allocator is called while that
/// lock is held, so it must not block (for example, it must not be guarded by
/// a `pw::sync::Mutex`).
///
/// If the allocator cannot provide a write buffer, `TryReserveWrite` returns
/// `std::nullopt` and `ReserveWrite` remains pending. Pending reservations are
/// only retried when write capacity is released (the peer reads a datagram, or
/// a reservation is canceled or fails to commit), not when unrelated memory is
/// freed. Size the allocator to hold `queue_depth` maximum-size datagrams in
/// each direction to avoid stalls.
class ForwardingSocketPair {
 public:
  /// Creates an empty, uninitialized pair with null `first()` and `second()`
  /// sockets.
  constexpr ForwardingSocketPair() = default;

  /// Creates a connected pair of sockets, allocating the socket endpoints,
  /// queues, and write buffers from `allocator`.
  ///
  /// @param allocator Allocator used for endpoints, queues, and write buffers.
  ///   See "Memory and locking" in the class documentation for requirements.
  /// @param options Queue depth and maximum message size. Asserts if
  ///   `options.queue_depth` is 0.
  ///
  /// Crash-asserts if `allocator` cannot allocate the initial socket pair state
  /// or queues. Use `TryCreate` for fallible creation.
  static ForwardingSocketPair Create(
      Allocator& allocator, const ForwardingSocketOptions& options = {});

  /// Fallibly creates a connected pair of sockets, allocating the socket
  /// endpoints, queues, and write buffers from `allocator`.
  ///
  /// @param allocator Allocator used for endpoints, queues, and write buffers.
  ///   See "Memory and locking" in the class documentation for requirements.
  /// @param options Queue depth and maximum message size. Asserts if
  ///   `options.queue_depth` is 0.
  ///
  /// @returns
  /// * @OK: The socket pair was created.
  /// * @RESOURCE_EXHAUSTED: `allocator` could not allocate the socket pair
  ///   state or queues.
  static Result<ForwardingSocketPair> TryCreate(
      Allocator& allocator, const ForwardingSocketOptions& options = {});

  ForwardingSocketPair(const ForwardingSocketPair&) = delete;
  ForwardingSocketPair& operator=(const ForwardingSocketPair&) = delete;

  ForwardingSocketPair(ForwardingSocketPair&&) noexcept = default;
  ForwardingSocketPair& operator=(ForwardingSocketPair&&) noexcept = default;

  ~ForwardingSocketPair() = default;

  /// Returns the first socket in the pair.
  const ReliableSocket& first() const { return first_; }

  /// Returns the second socket in the pair.
  const ReliableSocket& second() const { return second_; }

  /// Terminally closes both sockets in the pair immediately, discarding any
  /// unread datagrams in both directions.
  void Close() {
    static_cast<void>(first_.Close());
    static_cast<void>(second_.Close());
  }

 private:
  ForwardingSocketPair(ReliableSocket&& first, ReliableSocket&& second)
      : first_(std::move(first)), second_(std::move(second)) {}

  ReliableSocket first_;
  ReliableSocket second_;
};

/// An in-memory `ReliableListener` and `ReliableConnector` that pairs outbound
/// `Connect()` calls with inbound `Accept()` calls.
///
/// Each matched `Connect()` and `Accept()` creates a connected pair of
/// in-memory `ReliableSocket`s allocated from `allocator` that forward
/// datagrams to each other without copying.
///
/// `Connect()` and `Accept()` may be called in either order and from multiple
/// tasks concurrently. Unmatched calls remain pending in FIFO order until a
/// peer call arrives or the future is canceled; no sockets are allocated until
/// both sides are present. If `allocator` cannot allocate the sockets, pending
/// `Connect()` futures resolve to @RESOURCE_EXHAUSTED while the listener
/// remains active for subsequent connections.
///
/// On destruction, any still-pending `Accept()` and `Connect()` futures resolve
/// to @FAILED_PRECONDITION. Sockets from already-established connections remain
/// open; `allocator` must outlive them and must not block (both sockets in a
/// connection share an `InterruptSpinLock`, and `allocator` is called while
/// that lock is held).
class ForwardingTransport final : public ReliableListener,
                                  public ReliableConnector {
 public:
  /// Creates an in-memory transport that allocates connected sockets and their
  /// buffers from `allocator` using `options`.
  ///
  /// @param[in,out] allocator Allocator used for socket endpoints, queues, and
  ///   write buffers.
  /// @param[in] options Queue depth and maximum message size for each
  ///   established connection.
  ///
  /// @pre `options.queue_depth > 0` (crash-asserts otherwise).
  /// @pre `allocator` must outlive this `ForwardingTransport` and all sockets
  ///   produced by it, and must not block.
  explicit ForwardingTransport(Allocator& allocator,
                               const ForwardingSocketOptions& options = {});

  /// Resolves any pending `Accept()` and `Connect()` futures with
  /// @FAILED_PRECONDITION. Sockets from already-established connections remain
  /// open.
  ~ForwardingTransport() override;

  ForwardingTransport(const ForwardingTransport&) = delete;
  ForwardingTransport& operator=(const ForwardingTransport&) = delete;
  ForwardingTransport(ForwardingTransport&&) = delete;
  ForwardingTransport& operator=(ForwardingTransport&&) = delete;

 private:
  ReliableListener::AcceptFuture DoAcceptReliable() override
      PW_LOCKS_EXCLUDED(lock_);
  ReliableConnector::ConnectFuture DoConnectReliable() override
      PW_LOCKS_EXCLUDED(lock_);

  Allocator& allocator_;
  const ForwardingSocketOptions options_;
  mutable sync::InterruptSpinLock lock_;
  AcceptListProvider accept_provider_ PW_GUARDED_BY(lock_);
  ConnectListProvider connect_provider_ PW_GUARDED_BY(lock_);
};

/// @endmodule

}  // namespace pw::transport
