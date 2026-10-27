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

#include "pw_async2/value_future.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_transport/socket.h"

namespace pw::transport {

/// @module{pw_transport}

/// The listening side of a datagram-oriented transport.
///
/// A `Listener` accepts inbound connections initiated by remote peers,
/// ensuring that they are properly established, then provides a handle
/// which is used to read to or write from that socket.
///
/// The `Listener` interface does not specify any addressing or ports, as
/// those details are protocol-dependent. A concrete `Listener` instance
/// should be constructed with and internally store any such state if
/// required.
///
/// If a `Listener` is destructed, it must resolve any pending
/// `AcceptFuture` objects with a `FAILED_PRECONDITION`.
class Listener {
 public:
  /// The future returned by `Accept`, resolving to a `Socket`
  /// handle on success.
  using AcceptFuture = async2::ValueFuture<Result<Socket>>;

  virtual ~Listener() = default;

  /// Listens for a new connection from a peer, returning it once established.
  /// `Accept` may be called repeatedly to receive additional connections.
  ///
  /// `Accept` must be safe to call from multiple tasks at once, even if the
  /// underlying implementation runs its operations and resolves its futures
  /// serially. If multiple calls to `Accept` are made, the order in which
  /// their futures are resolved is implementation-specific and should not be
  /// relied upon.
  ///
  /// @returns
  /// * @OK: The connection was established and is ready for use.
  /// * @FAILED_PRECONDITION: The transport is permanently down.
  /// * @RESOURCE_EXHAUSTED: The socket object could not be allocated.
  virtual AcceptFuture Accept() = 0;

 protected:
  /// Creates a `Socket` handle from an implementation.
  static Socket WrapSocket(SocketImpl& impl) { return Socket(impl); }
};

/// The listening side of a reliable, datagram-oriented transport.
///
/// A `ReliableListener` accepts inbound connections initiated by remote
/// peers, ensuring that they are properly established, then provides a handle
/// which is used to read to or write from that socket.
///
/// The `ReliableListener` interface does not specify any addressing or
/// ports, as those details are protocol-dependent. A concrete
/// `ReliableListener` instance should be constructed with and
/// internally store any such state if required.
///
/// If a `ReliableListener` is destructed, it must resolve any pending
/// `AcceptFuture` objects with a `FAILED_PRECONDITION`.
class ReliableListener {
 public:
  /// The future returned by `Accept`, resolving to a `ReliableSocket`
  /// handle on success.
  using AcceptFuture = async2::ValueFuture<Result<ReliableSocket>>;

  virtual ~ReliableListener() = default;

  /// Listens for a new connection from a peer, returning it once established.
  /// `Accept` may be called repeatedly to receive additional connections.
  ///
  /// `Accept` must be safe to call from multiple tasks at once, even if the
  /// underlying implementation runs its operations and resolves its futures
  /// serially. If multiple calls to `Accept` are made, the order in which
  /// their futures are resolved is implementation-specific and should not be
  /// relied upon.
  ///
  /// @returns
  /// * @OK: The connection was established and is ready for use.
  /// * @FAILED_PRECONDITION: The transport is permanently down.
  /// * @RESOURCE_EXHAUSTED: The socket object could not be allocated.
  virtual AcceptFuture Accept() = 0;

 protected:
  /// Creates a `ReliableSocket` handle from an implementation.
  static ReliableSocket WrapSocket(ReliableSocketImpl& impl) {
    return ReliableSocket(impl);
  }
};

/// @deprecated Use `pw::transport::ReliableListener` instead.
using ReliableDatagramListener
    [[deprecated("Use pw::transport::ReliableListener instead")]] =
        ReliableListener;

/// The establishing side of a datagram-oriented transport.
///
/// A connector opens outbound connections to a remote peer, performing any
/// necessary protocol-level configuration and establishment, then provides
/// a handle which is used to read to or write from that socket.
///
/// The `Connector` interface does not specify any addressing or ports, as
/// those details are protocol-dependent. A concrete `Connector` instance
/// should be constructed with and internally store any such state if
/// required.
///
/// If a `Connector` is destructed, it must resolve any pending
/// `ConnectFuture` objects with a `FAILED_PRECONDITION`.
class Connector {
 public:
  /// The future returned by `Connect`, resolving to a `Socket`
  /// handle on success.
  using ConnectFuture = async2::ValueFuture<Result<Socket>>;

  virtual ~Connector() = default;

  /// Opens a connection to the peer, returning it once established.
  ///
  /// It is up to the implementation to decide whether concurrent connections to
  /// the peer are allowed. If not supported, subsequent calls to `Connect` when
  /// a connection is active should return `ALREADY_EXISTS`.
  /// It must always be possible to call `Connect` again when no connections are
  /// active and the transport is still alive.
  ///
  /// @returns
  /// * @OK: The connection was established and is ready for use.
  /// * @UNAVAILABLE: The peer could not be reached.
  /// * @FAILED_PRECONDITION: The transport is permanently down.
  /// * @ALREADY_EXISTS: A connection is already active and the transport
  ///     does not support multiple simultaneous connections.
  /// * @RESOURCE_EXHAUSTED: The socket object could not be allocated.
  virtual ConnectFuture Connect() = 0;

 protected:
  /// Creates a `Socket` handle from an implementation.
  static Socket WrapSocket(SocketImpl& impl) { return Socket(impl); }
};

/// The establishing side of a reliable, datagram-oriented transport.
///
/// A connector opens outbound connections to a remote peer, performing any
/// necessary protocol-level configuration and establishment, then provides
/// a handle which is used to read to or write from that socket.
///
/// The `ReliableConnector` interface does not specify any addressing or
/// ports, as those details are protocol-dependent. A concrete
/// `ReliableConnector` instance should be constructed with and
/// internally store any such state if required.
///
/// If a `ReliableConnector` is destructed, it must resolve any pending
/// `ConnectFuture` objects with a `FAILED_PRECONDITION`.
class ReliableConnector {
 public:
  /// The future returned by `Connect`, resolving to a `ReliableSocket`
  /// handle on success.
  using ConnectFuture = async2::ValueFuture<Result<ReliableSocket>>;

  virtual ~ReliableConnector() = default;

  /// Opens a connection to the peer, returning it once established.
  ///
  /// It is up to the implementation to decide whether concurrent connections to
  /// the peer are allowed. If not supported, subsequent calls to `Connect` when
  /// a connection is active should return `ALREADY_EXISTS`.
  /// It must always be possible to call `Connect` again when no connections are
  /// active and the transport is still alive.
  ///
  /// @returns
  /// * @OK: The connection was established and is ready for use.
  /// * @UNAVAILABLE: The peer could not be reached.
  /// * @FAILED_PRECONDITION: The transport is permanently down.
  /// * @ALREADY_EXISTS: A connection is already active and the transport
  ///     does not support multiple simultaneous connections.
  /// * @RESOURCE_EXHAUSTED: The socket object could not be allocated.
  virtual ConnectFuture Connect() = 0;

 protected:
  /// Creates a `ReliableSocket` handle from an implementation.
  static ReliableSocket WrapSocket(ReliableSocketImpl& impl) {
    return ReliableSocket(impl);
  }
};

/// @deprecated Use `pw::transport::ReliableConnector` instead.
using ReliableDatagramConnector
    [[deprecated("Use pw::transport::ReliableConnector instead")]] =
        ReliableConnector;

/// @endmodule

}  // namespace pw::transport
