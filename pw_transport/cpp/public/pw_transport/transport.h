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

#include <utility>

#include "pw_async2/future.h"
#include "pw_async2/value_future.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_transport/internal/transport_futures.h"
#include "pw_transport/socket.h"

namespace pw::transport {

/// @module{pw_transport}

/// The listening side of a datagram-oriented transport.
///
/// A `Listener` accepts inbound connections initiated by remote peers,
/// ensuring that they are properly established, then provides a handle
/// which is used to read to or write from that socket.
///
/// Any `ReliableListener` is also a `Listener` and can be passed wherever a
/// `Listener` is expected.
///
/// The `Listener` interface does not specify any addressing or ports, as
/// those details are protocol-dependent. A concrete `Listener` instance
/// should be constructed with and internally store any such state if
/// required.
///
/// Implementations override `DoAccept()` and return a `Listener::AcceptFuture`.
///
/// If a `Listener` is destructed, it must resolve any pending
/// `AcceptFuture` objects with a `FAILED_PRECONDITION`.
class Listener {
 public:
  /// The future returned by `Accept`, resolving to a `Socket`
  /// handle on success.
  class [[nodiscard]] AcceptFuture final
      : public internal::TransportFuture<AcceptFuture, Socket> {
   public:
    constexpr AcceptFuture() = default;

   private:
    friend class internal::TransportFuture<AcceptFuture, Socket>;
    friend class ReliableListener;

    explicit AcceptFuture(async2::ValueFuture<Result<Socket>>&& future) noexcept
        : TransportFuture(std::move(future)) {}
  };

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
  AcceptFuture Accept() { return DoAccept(); }

 protected:
  /// One-to-one provider that vends `AcceptFuture` instances.
  using AcceptProvider = internal::TransportFutureProvider<AcceptFuture>;

  /// Multi-consumer provider that vends a list of pending `AcceptFuture`
  /// instances.
  using AcceptListProvider =
      internal::TransportFutureListProvider<AcceptFuture>;

  /// Creates a `Socket` handle from an implementation.
  static Socket WrapSocket(SocketImpl& impl) { return Socket(impl); }

 private:
  /// Implements accepting an inbound connection from a peer.
  virtual AcceptFuture DoAccept() = 0;
};

static_assert(async2::Future<Listener::AcceptFuture>);

/// The listening side of a reliable, datagram-oriented transport.
///
/// A `ReliableListener` accepts inbound connections initiated by remote
/// peers, ensuring that they are properly established, then provides a handle
/// which is used to read to or write from that socket.
///
/// `ReliableListener` derives from `Listener`, so any `ReliableListener` can be
/// implicitly converted to and used as a `Listener`.
///
/// The `ReliableListener` interface does not specify any addressing or
/// ports, as those details are protocol-dependent. A concrete
/// `ReliableListener` instance should be constructed with and
/// internally store any such state if required.
///
/// Implementations override `DoAcceptReliable()` and return a
/// `ReliableListener::AcceptFuture`, typically using `AcceptProvider` or
/// `AcceptListProvider`.
///
/// If a `ReliableListener` is destructed, it must resolve any pending
/// `AcceptFuture` objects with a `FAILED_PRECONDITION`.
class ReliableListener : public Listener {
 public:
  /// The future returned by `Accept`, resolving to a `ReliableSocket`
  /// handle on success.
  class [[nodiscard]] AcceptFuture final
      : public internal::TransportFuture<AcceptFuture, ReliableSocket> {
   public:
    constexpr AcceptFuture() = default;

    /// Implicitly converts to `Listener::AcceptFuture`.
    operator Listener::AcceptFuture() && noexcept {
      return Listener::AcceptFuture(std::move(*this).TakeFuture());
    }

   private:
    friend class internal::TransportFuture<AcceptFuture, ReliableSocket>;

    explicit AcceptFuture(async2::ValueFuture<Result<Socket>>&& future) noexcept
        : TransportFuture(std::move(future)) {}
  };

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
  AcceptFuture Accept() { return DoAcceptReliable(); }

 protected:
  /// One-to-one provider that vends `AcceptFuture` instances.
  using AcceptProvider = internal::TransportFutureProvider<AcceptFuture>;

  /// Multi-consumer provider that vends a list of pending `AcceptFuture`
  /// instances.
  using AcceptListProvider =
      internal::TransportFutureListProvider<AcceptFuture>;

  /// Creates a `ReliableSocket` handle from an implementation.
  static ReliableSocket WrapSocket(ReliableSocketImpl& impl) {
    return ReliableSocket(impl);
  }

 private:
  Listener::AcceptFuture DoAccept() final { return DoAcceptReliable(); }

  /// Implements accepting a reliable inbound connection from a peer.
  virtual AcceptFuture DoAcceptReliable() = 0;
};

static_assert(async2::Future<ReliableListener::AcceptFuture>);

/// The establishing side of a datagram-oriented transport.
///
/// A connector opens outbound connections to a remote peer, performing any
/// necessary protocol-level configuration and establishment, then provides
/// a handle which is used to read to or write from that socket.
///
/// Any `ReliableConnector` is also a `Connector` and can be passed wherever a
/// `Connector` is expected.
///
/// The `Connector` interface does not specify any addressing or ports, as
/// those details are protocol-dependent. A concrete `Connector` instance
/// should be constructed with and internally store any such state if
/// required.
///
/// Implementations override `DoConnect()` and return a
/// `Connector::ConnectFuture`.
///
/// If a `Connector` is destructed, it must resolve any pending
/// `ConnectFuture` objects with a `FAILED_PRECONDITION`.
class Connector {
 public:
  /// The future returned by `Connect`, resolving to a `Socket`
  /// handle on success.
  class [[nodiscard]] ConnectFuture final
      : public internal::TransportFuture<ConnectFuture, Socket> {
   public:
    constexpr ConnectFuture() = default;

   private:
    friend class internal::TransportFuture<ConnectFuture, Socket>;
    friend class ReliableConnector;

    explicit ConnectFuture(
        async2::ValueFuture<Result<Socket>>&& future) noexcept
        : TransportFuture(std::move(future)) {}
  };

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
  ConnectFuture Connect() { return DoConnect(); }

 protected:
  /// One-to-one provider that vends `ConnectFuture` instances.
  using ConnectProvider = internal::TransportFutureProvider<ConnectFuture>;

  /// Multi-consumer provider that vends a list of pending `ConnectFuture`
  /// instances.
  using ConnectListProvider =
      internal::TransportFutureListProvider<ConnectFuture>;

  /// Creates a `Socket` handle from an implementation.
  static Socket WrapSocket(SocketImpl& impl) { return Socket(impl); }

 private:
  /// Implements opening an outbound connection to the peer.
  virtual ConnectFuture DoConnect() = 0;
};

static_assert(async2::Future<Connector::ConnectFuture>);

/// The establishing side of a reliable, datagram-oriented transport.
///
/// A connector opens outbound connections to a remote peer, performing any
/// necessary protocol-level configuration and establishment, then provides
/// a handle which is used to read to or write from that socket.
///
/// `ReliableConnector` derives from `Connector`, so any `ReliableConnector` can
/// be implicitly converted to and used as a `Connector`.
///
/// The `ReliableConnector` interface does not specify any addressing or
/// ports, as those details are protocol-dependent. A concrete
/// `ReliableConnector` instance should be constructed with and
/// internally store any such state if required.
///
/// Implementations override `DoConnectReliable()` and return a
/// `ReliableConnector::ConnectFuture`, typically using `ConnectProvider` or
/// `ConnectListProvider`.
///
/// If a `ReliableConnector` is destructed, it must resolve any pending
/// `ConnectFuture` objects with a `FAILED_PRECONDITION`.
class ReliableConnector : public Connector {
 public:
  /// The future returned by `Connect`, resolving to a `ReliableSocket`
  /// handle on success.
  class [[nodiscard]] ConnectFuture final
      : public internal::TransportFuture<ConnectFuture, ReliableSocket> {
   public:
    constexpr ConnectFuture() = default;

    /// Implicitly converts to `Connector::ConnectFuture`.
    operator Connector::ConnectFuture() && noexcept {
      return Connector::ConnectFuture(std::move(*this).TakeFuture());
    }

   private:
    friend class internal::TransportFuture<ConnectFuture, ReliableSocket>;

    explicit ConnectFuture(
        async2::ValueFuture<Result<Socket>>&& future) noexcept
        : TransportFuture(std::move(future)) {}
  };

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
  ConnectFuture Connect() { return DoConnectReliable(); }

 protected:
  /// One-to-one provider that vends `ConnectFuture` instances.
  using ConnectProvider = internal::TransportFutureProvider<ConnectFuture>;

  /// Multi-consumer provider that vends a list of pending `ConnectFuture`
  /// instances.
  using ConnectListProvider =
      internal::TransportFutureListProvider<ConnectFuture>;

  /// Creates a `ReliableSocket` handle from an implementation.
  static ReliableSocket WrapSocket(ReliableSocketImpl& impl) {
    return ReliableSocket(impl);
  }

 private:
  Connector::ConnectFuture DoConnect() final { return DoConnectReliable(); }

  /// Implements opening a reliable outbound connection to the peer.
  virtual ConnectFuture DoConnectReliable() = 0;
};

static_assert(async2::Future<ReliableConnector::ConnectFuture>);

/// @endmodule

}  // namespace pw::transport
