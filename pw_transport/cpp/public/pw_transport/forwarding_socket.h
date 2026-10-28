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
#include "pw_transport/socket.h"

namespace pw::transport {

/// @module{pw_transport}

/// Configuration for a `ForwardingDatagramSocketPair`.
struct ForwardingDatagramSocketOptions {
  /// Default for the maximum datagram sizes. Matches the 1500-byte Ethernet
  /// MTU, so a default pair behaves like a typical network link and bounds the
  /// size of each write buffer allocation.
  static constexpr size_t kDefaultMaxDatagramSizeBytes = 1500;

  /// The maximum number of active write reservations plus unread datagrams
  /// allowed in each direction at once. Must be at least 1.
  ///
  /// When a socket has `queue_depth` active write reservations and unread
  /// datagrams queued on its peer, `TryReserveWrite` returns `std::nullopt` and
  /// `ReserveWrite` waits until the peer reads a datagram or an active
  /// reservation is committed or canceled.
  uint16_t queue_depth = 1;

  /// The largest datagram that can be written to `first()` and read from
  /// `second()`. This is `first().max_write_message_size_bytes()` and
  /// `second().max_read_message_size_bytes()`.
  size_t max_first_to_second_size_bytes = kDefaultMaxDatagramSizeBytes;

  /// The largest datagram that can be written to `second()` and read from
  /// `first()`. This is `second().max_write_message_size_bytes()` and
  /// `first().max_read_message_size_bytes()`.
  size_t max_second_to_first_size_bytes = kDefaultMaxDatagramSizeBytes;
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
/// provided `Allocator`. Copies of the `ReliableSocket` handles
/// returned by `first()` and `second()` may outlive the
/// `ForwardingDatagramSocketPair` object itself; the shared state is
/// automatically destroyed and deallocated once all handles and futures for
/// both endpoints have been released.
///
/// Closing one socket (or dropping all handles to it) prevents further writes
/// in both directions. Datagrams that were already committed before the socket
/// closed can still be read by the peer, after which the peer socket also
/// closes. Calling `Close()` on the `ForwardingDatagramSocketPair` closes both
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
class ForwardingDatagramSocketPair {
 public:
  /// Creates an empty, uninitialized pair with null `first()` and `second()`
  /// sockets.
  constexpr ForwardingDatagramSocketPair() = default;

  /// Creates a connected pair of sockets, allocating the socket endpoints,
  /// queues, and write buffers from `allocator`.
  ///
  /// @param allocator Allocator used for endpoints, queues, and write buffers.
  ///   See "Memory and locking" in the class documentation for requirements.
  /// @param options Queue depth and maximum datagram sizes. Asserts if
  ///   `options.queue_depth` is 0.
  ///
  /// Crash-asserts if `allocator` cannot allocate the initial socket pair state
  /// or queues. Use `TryCreate` for fallible creation.
  static ForwardingDatagramSocketPair Create(
      Allocator& allocator,
      const ForwardingDatagramSocketOptions& options = {});

  /// Fallibly creates a connected pair of sockets, allocating the socket
  /// endpoints, queues, and write buffers from `allocator`.
  ///
  /// @param allocator Allocator used for endpoints, queues, and write buffers.
  ///   See "Memory and locking" in the class documentation for requirements.
  /// @param options Queue depth and maximum datagram sizes. Asserts if
  ///   `options.queue_depth` is 0.
  ///
  /// @returns
  /// * @OK: The socket pair was created.
  /// * @RESOURCE_EXHAUSTED: `allocator` could not allocate the socket pair
  ///   state or queues.
  static Result<ForwardingDatagramSocketPair> TryCreate(
      Allocator& allocator,
      const ForwardingDatagramSocketOptions& options = {});

  ForwardingDatagramSocketPair(const ForwardingDatagramSocketPair&) = delete;
  ForwardingDatagramSocketPair& operator=(const ForwardingDatagramSocketPair&) =
      delete;

  ForwardingDatagramSocketPair(ForwardingDatagramSocketPair&&) noexcept =
      default;
  ForwardingDatagramSocketPair& operator=(
      ForwardingDatagramSocketPair&&) noexcept = default;

  ~ForwardingDatagramSocketPair() = default;

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
  ForwardingDatagramSocketPair(ReliableSocket&& first, ReliableSocket&& second)
      : first_(std::move(first)), second_(std::move(second)) {}

  ReliableSocket first_;
  ReliableSocket second_;
};

/// @endmodule

}  // namespace pw::transport
