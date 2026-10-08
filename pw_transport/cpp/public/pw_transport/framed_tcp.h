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
#include <optional>
#include <string_view>

#include "pw_allocator/allocator.h"
#include "pw_async2/value_future.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_string/string.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/context.h"
#include "pw_thread/thread.h"
#include "pw_transport/socket.h"
#include "pw_transport/transport.h"

/// A reliable datagram transport over TCP for Linux and macOS hosts.
///
/// Each datagram is sent as a 4-byte, big-endian payload length followed by
/// the payload. Every socket uses two threads for blocking socket I/O, so this
/// transport is intended for host-side tools and tests.
///
/// `Close()` sends the datagrams that were committed before it was called,
/// then closes the connection. Dropping the last handle to a socket without
/// closing it discards datagrams that have not been sent.
///
/// When the peer closes the connection or the connection fails, datagrams that
/// were already received can still be read. The socket closes after the last
/// one is read. Until then, committing writes fails.
///
/// Sockets and their datagram buffers are allocated from the allocator passed
/// to `FramedTcpListener` or `FramedTcpConnector`. The allocator is used from
/// multiple threads, so it must be thread safe. It must outlive all sockets.
namespace pw::transport {

/// Options for sockets created by `FramedTcpListener` and `FramedTcpConnector`.
struct FramedTcpOptions {
  /// The largest datagram that can be sent or received, in bytes. Peers must
  /// use the same limit. Receiving a larger datagram closes the connection.
  /// Must not exceed `UINT32_MAX`.
  size_t max_message_size_bytes = 64 * 1024;

  /// The number of received datagrams to buffer before the socket stops
  /// reading from the connection. Must be at least 1.
  uint16_t read_queue_depth = 4;

  /// The number of write reservations and unsent datagrams the socket allows
  /// at once. Must be at least 1.
  uint16_t write_queue_depth = 4;
};

namespace internal {

/// A pipe that wakes a thread blocked in `poll()`.
class WakePipe {
 public:
  constexpr WakePipe() = default;
  ~WakePipe();

  WakePipe(const WakePipe&) = delete;
  WakePipe& operator=(const WakePipe&) = delete;

  /// Creates the pipe if it is not already open.
  Status Open();

  /// Makes the pipe readable. Does nothing if the pipe is not open.
  void Signal();

  /// Returns the file descriptor to poll for readability.
  int fd() const { return read_fd_; }

 private:
  int read_fd_ = -1;
  int write_fd_ = -1;
};

}  // namespace internal

/// A `ReliableDatagramListener` that accepts TCP connections.
///
/// The listener accepts IPv4 and IPv6 connections on all local interfaces. It
/// accepts connections on a background thread. If a connection arrives without
/// a pending `Accept` call, the listener holds it until `Accept` is called and
/// stops listening for additional connections until then.
///
/// On destruction, pending `Accept` futures resolve to `FAILED_PRECONDITION`.
/// Sockets that were already accepted remain open.
class FramedTcpListener final : public ReliableDatagramListener {
 public:
  explicit FramedTcpListener(Allocator& allocator,
                             const FramedTcpOptions& options = {});

  ~FramedTcpListener() override;

  FramedTcpListener(const FramedTcpListener&) = delete;
  FramedTcpListener& operator=(const FramedTcpListener&) = delete;

  /// Starts listening for connections on `port`. If `port` is 0, the system
  /// picks an available port, which can be queried via `port()`.
  ///
  /// `Listen` does not wait for a connection; it returns as soon as the
  /// listening socket is set up. Connections are delivered through `Accept`.
  /// If `Listen` fails, it may be called again, for example with another port.
  ///
  /// @returns
  /// * @OK: The listener is accepting connections.
  /// * @FAILED_PRECONDITION: The listener is already listening or has failed.
  /// * @UNAVAILABLE: The listening socket could not be set up. For example,
  ///   the port may already be in use.
  Status Listen(uint16_t port = 0) PW_LOCKS_EXCLUDED(lock_);

  /// Returns the port that the listener is bound to, or 0 if `Listen` has not
  /// succeeded.
  uint16_t port() const PW_LOCKS_EXCLUDED(lock_);

  /// Waits for a peer to connect. `Accept` may be called before `Listen`,
  /// in which case the future waits until the listener starts and a peer
  /// connects.
  ///
  /// @returns
  /// * @OK: A connection was accepted.
  /// * @FAILED_PRECONDITION: The listener failed or was destroyed.
  /// * @RESOURCE_EXHAUSTED: The connection could not be accepted because
  ///   memory or file descriptors ran out.
  AcceptFuture Accept() override PW_LOCKS_EXCLUDED(lock_);

 private:
  enum class State : uint8_t {
    kIdle,
    kListening,
    kStopped,
  };

  void AcceptLoop() PW_LOCKS_EXCLUDED(lock_);
  bool WaitForUnclaimedSlot() PW_LOCKS_EXCLUDED(lock_);
  void Deliver(Result<ReliableDatagramSocket>&& result)
      PW_LOCKS_EXCLUDED(lock_);
  void Stop() PW_LOCKS_EXCLUDED(lock_);

  Allocator& allocator_;
  const FramedTcpOptions options_;

  // Written by Listen() before the accept thread starts and closed after it
  // exits, so the accept thread can use it without locking.
  int listen_fd_ = -1;
  internal::WakePipe wake_pipe_;

  mutable sync::Mutex lock_;
  State state_ PW_GUARDED_BY(lock_) = State::kIdle;
  uint16_t port_ PW_GUARDED_BY(lock_) = 0;

  // A connection that was accepted when no Accept() call was waiting.
  std::optional<Result<ReliableDatagramSocket>> unclaimed_ PW_GUARDED_BY(lock_);

  async2::ValueListProvider<Result<ReliableDatagramSocket>> accept_provider_;

  // Released when `unclaimed_` is claimed or the listener stops.
  sync::ThreadNotification unclaimed_slot_available_;

  DefaultThreadContext thread_context_;
  Thread thread_;
};

/// A `ReliableDatagramConnector` that opens TCP connections to a peer.
///
/// Connections are opened on a background thread. Multiple connections can be
/// open at once.
///
/// Destruction cancels any connection in progress, resolving pending `Connect`
/// futures with `FAILED_PRECONDITION`. Sockets that were already connected
/// remain open.
class FramedTcpConnector final : public ReliableDatagramConnector {
 public:
  /// The maximum length of the `host` string.
  static constexpr size_t kMaxHostLength = 253;

  /// Creates a connector for the peer at `host` and `port`. `host` may be a
  /// hostname or an IPv4 or IPv6 address, and must not be longer than
  /// `kMaxHostLength`.
  FramedTcpConnector(Allocator& allocator,
                     std::string_view host,
                     uint16_t port,
                     const FramedTcpOptions& options = {});

  ~FramedTcpConnector() override;

  FramedTcpConnector(const FramedTcpConnector&) = delete;
  FramedTcpConnector& operator=(const FramedTcpConnector&) = delete;

  /// Connects to the peer.
  ///
  /// @returns
  /// * @OK: The connection was established.
  /// * @UNAVAILABLE: The host could not be resolved or did not accept the
  ///   connection.
  /// * @FAILED_PRECONDITION: The connector was destroyed.
  /// * @RESOURCE_EXHAUSTED: The connection could not be opened because memory
  ///   or file descriptors ran out.
  ConnectFuture Connect() override PW_LOCKS_EXCLUDED(lock_);

 private:
  void ConnectLoop() PW_LOCKS_EXCLUDED(lock_);
  Result<ReliableDatagramSocket> ConnectToPeer();

  Allocator& allocator_;
  const FramedTcpOptions options_;
  const InlineString<kMaxHostLength> host_;
  const uint16_t port_;

  // Opened by the first Connect() call before the connect thread starts.
  internal::WakePipe wake_pipe_;

  sync::Mutex lock_;
  bool stopping_ PW_GUARDED_BY(lock_) = false;

  async2::ValueListProvider<Result<ReliableDatagramSocket>> connect_provider_;

  // Released when Connect() is called or the connector is destroyed.
  sync::ThreadNotification connect_requested_;

  DefaultThreadContext thread_context_;
  Thread thread_ PW_GUARDED_BY(lock_);
};

}  // namespace pw::transport
