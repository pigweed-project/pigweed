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
#include <mutex>
#include <utility>

#include "pw_assert/check.h"
#include "pw_containers/inline_deque.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_transport/socket.h"
#include "pw_transport/transport.h"

namespace pw::transport {
namespace internal {

template <typename FutureType,
          typename ListProviderType,
          size_t kMaxQueuedResults>
class MockEndpoint {
 public:
  using ResultType = typename FutureType::value_type;

  constexpr MockEndpoint() = default;

  ~MockEndpoint() {
    std::lock_guard lock(lock_);
    provider_.ResolveAll(Status::FailedPrecondition());
    // Leave `queued_results_` to be destroyed after `lock_` is released so
    // unconsumed sockets are not destroyed while holding `lock_`.
  }

  MockEndpoint(const MockEndpoint&) = delete;
  MockEndpoint& operator=(const MockEndpoint&) = delete;
  MockEndpoint(MockEndpoint&&) = delete;
  MockEndpoint& operator=(MockEndpoint&&) = delete;

  FutureType NextFuture() PW_LOCKS_EXCLUDED(lock_) {
    std::lock_guard lock(lock_);
    ++call_count_;
    if (!queued_results_.empty()) {
      ResultType result = std::move(queued_results_.front());
      queued_results_.pop_front();
      return FutureType::Resolved(std::move(result));
    }
    return provider_.Get();
  }

  void Resolve(ResultType&& result) PW_LOCKS_EXCLUDED(lock_) {
    std::lock_guard lock(lock_);
    if (provider_.TryResolveFirst(result)) {
      return;
    }
    PW_CHECK(!queued_results_.full(),
             "Mock transport result queue is full (%zu results); call "
             "Accept()/Connect() to consume queued results or increase "
             "kMaxQueuedResults.",
             kMaxQueuedResults);
    queued_results_.push_back(std::move(result));
  }

  [[nodiscard]] bool has_pending() const PW_LOCKS_EXCLUDED(lock_) {
    std::lock_guard lock(lock_);
    return !provider_.empty();
  }

  [[nodiscard]] size_t pending_count() const PW_LOCKS_EXCLUDED(lock_) {
    std::lock_guard lock(lock_);
    return provider_.size();
  }

  [[nodiscard]] size_t call_count() const PW_LOCKS_EXCLUDED(lock_) {
    std::lock_guard lock(lock_);
    return call_count_;
  }

 private:
  mutable sync::InterruptSpinLock lock_;
  ListProviderType provider_ PW_GUARDED_BY(lock_);
  InlineDeque<ResultType, kMaxQueuedResults> queued_results_
      PW_GUARDED_BY(lock_);
  size_t call_count_ PW_GUARDED_BY(lock_) = 0;
};

template <typename ListenerType, size_t kMaxQueuedResults>
class MockListenerBase;

template <size_t kMaxQueuedResults>
class MockListenerBase<Listener, kMaxQueuedResults> : public Listener {
 protected:
  using SocketType = Socket;
  MockEndpoint<AcceptFuture, AcceptListProvider, kMaxQueuedResults> endpoint_;

 private:
  AcceptFuture DoAccept() final { return endpoint_.NextFuture(); }
};

template <size_t kMaxQueuedResults>
class MockListenerBase<ReliableListener, kMaxQueuedResults>
    : public ReliableListener {
 protected:
  using SocketType = ReliableSocket;
  MockEndpoint<AcceptFuture, AcceptListProvider, kMaxQueuedResults> endpoint_;

 private:
  AcceptFuture DoAcceptReliable() final { return endpoint_.NextFuture(); }
};

template <typename ConnectorType, size_t kMaxQueuedResults>
class MockConnectorBase;

template <size_t kMaxQueuedResults>
class MockConnectorBase<Connector, kMaxQueuedResults> : public Connector {
 protected:
  using SocketType = Socket;
  MockEndpoint<ConnectFuture, ConnectListProvider, kMaxQueuedResults> endpoint_;

 private:
  ConnectFuture DoConnect() final { return endpoint_.NextFuture(); }
};

template <size_t kMaxQueuedResults>
class MockConnectorBase<ReliableConnector, kMaxQueuedResults>
    : public ReliableConnector {
 protected:
  using SocketType = ReliableSocket;
  MockEndpoint<ConnectFuture, ConnectListProvider, kMaxQueuedResults> endpoint_;

 private:
  ConnectFuture DoConnectReliable() final { return endpoint_.NextFuture(); }
};

}  // namespace internal

namespace test {

/// @submodule{pw_transport,testing}

/// Manually controlled transport listener for unit tests.
///
/// Tests feed inbound sockets or error statuses via `ResolveAccept()`. If an
/// `Accept()` future is already waiting, `ResolveAccept()` resolves the oldest
/// pending future immediately; otherwise the result is queued (up to
/// `kMaxQueuedResults`) and returned by the next `Accept()` call.
///
/// All methods are thread-safe. On destruction, all pending `Accept()` futures
/// resolve to @FAILED_PRECONDITION.
///
/// @tparam ListenerType Transport listener interface to mock
///   (`ReliableListener` or `Listener`).
/// @tparam kMaxQueuedResultsValue Maximum number of `ResolveAccept()` results
///   buffered before `Accept()` is called.
template <typename ListenerType, size_t kMaxQueuedResultsValue = 4>
class BasicMockListener
    : public internal::MockListenerBase<ListenerType, kMaxQueuedResultsValue> {
 private:
  using Base = internal::MockListenerBase<ListenerType, kMaxQueuedResultsValue>;

 public:
  /// Socket handle type yielded by `Accept()` (`ReliableSocket` or `Socket`).
  using SocketType = typename Base::SocketType;

  /// Maximum number of `ResolveAccept()` results buffered before `Accept()` is
  /// called.
  static constexpr size_t kMaxQueuedResults = kMaxQueuedResultsValue;

  /// Creates a mock listener with no pending or queued `Accept()` results.
  constexpr BasicMockListener() = default;

  /// Resolves all still-pending `Accept()` futures with @FAILED_PRECONDITION
  /// and releases any unconsumed queued sockets.
  ~BasicMockListener() override = default;

  BasicMockListener(const BasicMockListener&) = delete;
  BasicMockListener& operator=(const BasicMockListener&) = delete;
  BasicMockListener(BasicMockListener&&) = delete;
  BasicMockListener& operator=(BasicMockListener&&) = delete;

  /// Resolves the oldest pending `Accept()` future with `result`, or queues
  /// `result` in FIFO order for the next `Accept()` call if none is pending.
  ///
  /// @param[in] result Socket or error status to deliver to `Accept()`.
  /// @pre Either at least one `Accept()` future is pending or fewer than
  ///   `kMaxQueuedResults` results are already queued (crash-asserts
  ///   otherwise).
  void ResolveAccept(Result<SocketType> result) {
    this->endpoint_.Resolve(std::move(result));
  }

  /// Checks whether any `Accept()` future is awaiting `ResolveAccept()`.
  ///
  /// @returns `true` if at least one `Accept()` future is currently pending.
  [[nodiscard]] bool has_pending_accept() const {
    return this->endpoint_.has_pending();
  }

  /// Counts `Accept()` futures currently awaiting `ResolveAccept()`.
  ///
  /// @returns The number of pending `Accept()` futures.
  [[nodiscard]] size_t pending_accept_count() const {
    return this->endpoint_.pending_count();
  }

  /// Counts total `Accept()` invocations on this listener.
  ///
  /// @returns The total number of times `Accept()` has been called.
  [[nodiscard]] size_t accept_count() const {
    return this->endpoint_.call_count();
  }
};

/// Manually controlled unreliable `Listener` for unit tests.
using MockListener = BasicMockListener<Listener>;

/// Manually controlled `ReliableListener` for unit tests.
using MockReliableListener = BasicMockListener<ReliableListener>;

/// Manually controlled transport connector for unit tests.
///
/// Tests feed outbound sockets or error statuses via `ResolveConnect()`. If a
/// `Connect()` future is already waiting, `ResolveConnect()` resolves the
/// oldest pending future immediately; otherwise the result is queued (up to
/// `kMaxQueuedResults`) and returned by the next `Connect()` call.
///
/// All methods are thread-safe. On destruction, all pending `Connect()` futures
/// resolve to @FAILED_PRECONDITION.
///
/// @tparam ConnectorType Transport connector interface to mock
///   (`ReliableConnector` or `Connector`).
/// @tparam kMaxQueuedResultsValue Maximum number of `ResolveConnect()` results
///   buffered before `Connect()` is called.
template <typename ConnectorType, size_t kMaxQueuedResultsValue = 4>
class BasicMockConnector
    : public internal::MockConnectorBase<ConnectorType,
                                         kMaxQueuedResultsValue> {
 private:
  using Base =
      internal::MockConnectorBase<ConnectorType, kMaxQueuedResultsValue>;

 public:
  /// Socket handle type yielded by `Connect()` (`ReliableSocket` or `Socket`).
  using SocketType = typename Base::SocketType;

  /// Maximum number of `ResolveConnect()` results buffered before `Connect()`
  /// is called.
  static constexpr size_t kMaxQueuedResults = kMaxQueuedResultsValue;

  /// Creates a mock connector with no pending or queued `Connect()` results.
  constexpr BasicMockConnector() = default;

  /// Resolves all still-pending `Connect()` futures with @FAILED_PRECONDITION
  /// and releases any unconsumed queued sockets.
  ~BasicMockConnector() override = default;

  BasicMockConnector(const BasicMockConnector&) = delete;
  BasicMockConnector& operator=(const BasicMockConnector&) = delete;
  BasicMockConnector(BasicMockConnector&&) = delete;
  BasicMockConnector& operator=(BasicMockConnector&&) = delete;

  /// Resolves the oldest pending `Connect()` future with `result`, or queues
  /// `result` in FIFO order for the next `Connect()` call if none is pending.
  ///
  /// @param[in] result Socket or error status to deliver to `Connect()`.
  /// @pre Either at least one `Connect()` future is pending or fewer than
  ///   `kMaxQueuedResults` results are already queued (crash-asserts
  ///   otherwise).
  void ResolveConnect(Result<SocketType> result) {
    this->endpoint_.Resolve(std::move(result));
  }

  /// Checks whether any `Connect()` future is awaiting `ResolveConnect()`.
  ///
  /// @returns `true` if at least one `Connect()` future is currently pending.
  [[nodiscard]] bool has_pending_connect() const {
    return this->endpoint_.has_pending();
  }

  /// Counts `Connect()` futures currently awaiting `ResolveConnect()`.
  ///
  /// @returns The number of pending `Connect()` futures.
  [[nodiscard]] size_t pending_connect_count() const {
    return this->endpoint_.pending_count();
  }

  /// Counts total `Connect()` invocations on this connector.
  ///
  /// @returns The total number of times `Connect()` has been called.
  [[nodiscard]] size_t connect_count() const {
    return this->endpoint_.call_count();
  }
};

/// Manually controlled unreliable `Connector` for unit tests.
using MockConnector = BasicMockConnector<Connector>;

/// Manually controlled `ReliableConnector` for unit tests.
using MockReliableConnector = BasicMockConnector<ReliableConnector>;

/// @endsubmodule

}  // namespace test
}  // namespace pw::transport
