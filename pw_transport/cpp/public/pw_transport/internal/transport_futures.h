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

#include "pw_async2/context.h"
#include "pw_async2/poll.h"
#include "pw_async2/value_future.h"
#include "pw_result/result.h"
#include "pw_transport/socket.h"

namespace pw::transport::internal {

template <typename FutureType>
class TransportFutureProvider;

template <typename FutureType>
class TransportFutureListProvider;

/// Common future implementation for `Listener` and `Connector` futures.
template <typename Derived, typename SocketType>
class TransportFuture {
 public:
  static_assert(std::is_same_v<SocketType, Socket> ||
                    std::is_same_v<SocketType, ReliableSocket>,
                "SocketType must be Socket or ReliableSocket");

  using value_type = Result<SocketType>;

  TransportFuture(const TransportFuture&) = delete;
  TransportFuture& operator=(const TransportFuture&) = delete;

  /// Creates a future that is immediately resolved with `result`.
  static Derived Resolved(value_type&& result) {
    return Wrap(
        async2::ValueFuture<Result<Socket>>::Resolved(std::move(result)));
  }

  /// Creates a future that is immediately resolved with `result`.
  static Derived Resolved(const value_type& result) {
    return Wrap(async2::ValueFuture<Result<Socket>>::Resolved(result));
  }

  /// Cancels the future.
  void Cancel() { future_.Cancel(); }

  /// Returns `true` if the future can be pended.
  [[nodiscard]] bool is_pendable() const { return future_.is_pendable(); }

  /// Returns `true` if the future has completed.
  [[nodiscard]] bool is_complete() const { return future_.is_complete(); }

  /// Polls the future, returning the socket once the connection completes.
  async2::Poll<value_type> Pend(async2::Context& cx) {
    if constexpr (std::is_same_v<SocketType, ReliableSocket>) {
      return PendReliableSocket(future_, cx);
    } else {
      return future_.Pend(cx);
    }
  }

 protected:
  constexpr TransportFuture() = default;

  TransportFuture(TransportFuture&&) noexcept = default;
  TransportFuture& operator=(TransportFuture&&) noexcept = default;

  ~TransportFuture() = default;

  explicit TransportFuture(
      async2::ValueFuture<Result<Socket>>&& future) noexcept
      : future_(std::move(future)) {}

  async2::ValueFuture<Result<Socket>>&& TakeFuture() && noexcept {
    return std::move(future_);
  }

 private:
  friend class TransportFutureProvider<Derived>;
  friend class TransportFutureListProvider<Derived>;

  static Derived Wrap(async2::ValueFuture<Result<Socket>>&& future) {
    return Derived(std::move(future));
  }

  async2::ValueFuture<Result<Socket>> future_;
};

/// One-to-one provider for `AcceptFuture` or `ConnectFuture`.
///
/// Wraps an `async2::ValueProvider`. At most one future may be pending at a
/// time. The provider must resolve its future, if any, before it is destroyed.
template <typename FutureType>
class TransportFutureProvider {
 public:
  using ResultType = typename FutureType::value_type;

  constexpr TransportFutureProvider() = default;

  TransportFutureProvider(TransportFutureProvider&&) noexcept = default;
  TransportFutureProvider& operator=(TransportFutureProvider&&) noexcept =
      default;

  TransportFutureProvider(const TransportFutureProvider&) = delete;
  TransportFutureProvider& operator=(const TransportFutureProvider&) = delete;

  ~TransportFutureProvider() = default;

  /// Returns a future that completes when `Resolve` is called.
  ///
  /// Asserts if a previously vended future is still pending.
  FutureType Get() { return FutureType::Wrap(provider_.Get()); }

  /// Returns a future if none is currently pending, or `std::nullopt`.
  std::optional<FutureType> TryGet() {
    std::optional<async2::ValueFuture<Result<Socket>>> future =
        provider_.TryGet();
    if (!future.has_value()) {
      return std::nullopt;
    }
    return FutureType::Wrap(std::move(*future));
  }

  /// Returns `true` if a vended future is still pending.
  [[nodiscard]] bool has_future() const { return provider_.has_future(); }

  /// Resolves the pending future, if any, with `result`.
  void Resolve(ResultType&& result) { provider_.Resolve(std::move(result)); }

  /// Resolves the pending future, if any, with `result`.
  void Resolve(const ResultType& result) { provider_.Resolve(result); }

 private:
  async2::ValueProvider<Result<Socket>> provider_;
};

/// Multi-consumer provider that manages a list of pending `AcceptFuture` or
/// `ConnectFuture` instances.
///
/// Wraps an `async2::ValueListProvider`. Any number of futures may be pending
/// at once. The provider must resolve all of its futures before it is
/// destroyed.
template <typename FutureType>
class TransportFutureListProvider {
 public:
  using ResultType = typename FutureType::value_type;

  constexpr TransportFutureListProvider() = default;

  TransportFutureListProvider(TransportFutureListProvider&&) noexcept = default;
  TransportFutureListProvider& operator=(
      TransportFutureListProvider&&) noexcept = default;

  TransportFutureListProvider(const TransportFutureListProvider&) = delete;
  TransportFutureListProvider& operator=(const TransportFutureListProvider&) =
      delete;

  ~TransportFutureListProvider() = default;

  /// Vends a future and registers it in the pending list.
  FutureType Get() { return FutureType::Wrap(provider_.Get()); }

  /// Returns `true` if the list contains no pending futures.
  [[nodiscard]] bool empty() const { return provider_.empty(); }

  /// Returns the number of pending futures.
  [[nodiscard]] size_t size() const { return provider_.size(); }

  /// Resolves the oldest pending future in the list, if any.
  ///
  /// If no future is pending, `result` is left unmodified and `false` is
  /// returned.
  bool ResolveFirst(ResultType&& result) {
    return provider_.ResolveFirstMatching(
        [&result](auto&) { return std::optional(std::move(result)); });
  }

  /// Resolves the oldest pending future in the list, if any.
  ///
  /// Returns `false` if no future is pending.
  bool ResolveFirst(const ResultType& result) {
    return provider_.ResolveFirstMatching(
        [&result](auto&) { return std::optional(result); });
  }

  /// Resolves all pending futures in the list with `result`.
  void ResolveAll(const ResultType& result) {
    provider_.ResolveAll([&result](auto&) { return result; });
  }

 private:
  async2::ValueListProvider<Result<Socket>> provider_;
};

}  // namespace pw::transport::internal
