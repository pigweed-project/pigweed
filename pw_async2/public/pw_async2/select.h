// Copyright 2025 The Pigweed Authors
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

#include <algorithm>
#include <tuple>
#include <utility>
#include <variant>

#include "pw_async2/dispatcher.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_containers/optional_tuple.h"

namespace pw::async2 {

/// @submodule{pw_async2,combinators}

template <typename... Futures>
class SelectFuture {
 public:
  static_assert(sizeof...(Futures) > 0,
                "Cannot select over an empty set of futures");

  using value_type = OptionalTuple<typename Futures::value_type...>;

  constexpr SelectFuture() = default;

  explicit SelectFuture(Futures&&... futures)
      : futures_(std::move(futures)...), state_(FutureState::kPending) {}

  Poll<value_type> Pend(Context& cx) {
    value_type tuple;
    PendAll(cx, kTupleIndexSequence, tuple);
    if (!tuple.empty()) {
      state_.MarkComplete();
      futures_ = std::tuple<Futures...>();
      return tuple;
    }
    return Pending();
  }

  [[nodiscard]] constexpr bool is_pendable() const {
    return state_.is_pendable();
  }
  [[nodiscard]] constexpr bool is_complete() const {
    return state_.is_complete();
  }

 private:
  static constexpr auto kTupleIndexSequence =
      std::make_index_sequence<sizeof...(Futures)>();

  enum State : uint8_t {
    kDefaultConstructed,
    kPendable,
    kComplete,
  };

  /// Pends every sub-future, storing the results of each that is ready in the
  /// provided result tuple.
  template <size_t... Is>
  void PendAll(Context& cx, std::index_sequence<Is...>, value_type& result) {
    (PendFuture<Is>(cx, result), ...);
  }

  template <size_t kTupleIndex>
  void PendFuture(Context& cx, value_type& result) {
    auto& future = std::get<kTupleIndex>(futures_);
    if (future.is_complete()) {
      return;
    }

    auto poll = future.Pend(cx);
    if (poll.IsReady()) {
      result.template emplace<kTupleIndex>(std::move(*poll));
    }
  }

  std::tuple<Futures...> futures_;
  FutureState state_;
};

template <typename... Futures>
SelectFuture(Futures&&...) -> SelectFuture<Futures...>;

/// Runs each of the provided futures, resolving once the first has completed.
///
/// As additional futures may have completed during the time between the first
/// completion and the task running, `Select` returns a `Future<OptionalTuple>`
/// which stores the results of all the sub-futures which managed to complete.
template <typename... Futures>
SelectFuture<Futures...> Select(Futures&&... futures) {
  static_assert((Future<Futures> && ...),
                "All arguments to Select must be Future types");
  return SelectFuture(std::forward<Futures>(futures)...);
}

/// @endsubmodule

}  // namespace pw::async2
