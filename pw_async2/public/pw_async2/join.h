// Copyright 2024 The Pigweed Authors
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

#include "pw_async2/dispatcher.h"
#include "pw_async2/future.h"

namespace pw::async2 {

/// @submodule{pw_async2,combinators}

template <typename... Futures>
class JoinFuture {
 public:
  using value_type = std::tuple<FutureValue<Futures>&&...>;

  constexpr JoinFuture() : outputs_{} {}

  [[nodiscard]] bool is_pendable() const { return state_.is_pendable(); }

  [[nodiscard]] bool is_complete() const { return state_.is_complete(); }

  /// Attempts to complete all of the futures, returning ``Ready``
  /// with their results if all are complete.
  Poll<value_type> Pend(Context& cx) {
    PW_ASSERT(is_pendable());
    if (!PendElements(cx, kTupleIndexSequence)) {
      return Pending();
    }

    state_.MarkComplete();
    return TakeOutputs(kTupleIndexSequence);
  }

 private:
  static_assert(sizeof...(Futures) > 0u, "Cannot join an empty set of futures");

  static constexpr auto kTupleIndexSequence =
      std::make_index_sequence<sizeof...(Futures)>();
  using TupleOfOutputRvalues = std::tuple<FutureValue<Futures>&&...>;

  template <typename... Fs>
  friend constexpr auto Join(Fs&&...);

  explicit constexpr JoinFuture(Futures&&... futures)
      : futures_(std::move(futures)...),
        outputs_(Poll<typename Futures::value_type>(Pending())...),
        state_(FutureState::kPending) {}

  /// Pends all non-completed futures at indices ``Is...`.
  ///
  /// Returns whether or not all futures have completed.
  template <size_t... Is>
  bool PendElements(Context& cx, std::index_sequence<Is...>) {
    (PendElement<Is>(cx), ...);
    return (std::get<Is>(outputs_).IsReady() && ...);
  }

  /// For future at `TupleIndex`, if it has not already returned a ``Ready``
  /// result, attempts to complete it and store the result in ``outputs_``.
  template <size_t kTupleIndex>
  void PendElement(Context& cx) {
    auto& output = std::get<kTupleIndex>(outputs_);
    if (!output.IsReady()) {
      output = std::get<kTupleIndex>(futures_).Pend(cx);
    }
  }

  /// Takes the results of all futures at indices ``Is...`.
  template <size_t... Is>
  Poll<TupleOfOutputRvalues> TakeOutputs(std::index_sequence<Is...>) {
    return Poll<TupleOfOutputRvalues>(
        std::forward_as_tuple<FutureValue<Futures>...>(TakeOutput<Is>()...));
  }

  /// Takes the result of the future at index ``kTupleIndex``.
  template <size_t kTupleIndex>
  FutureValue<
      typename std::tuple_element<kTupleIndex, std::tuple<Futures...>>::type>&&
  TakeOutput() {
    return std::move(std::get<kTupleIndex>(outputs_).value());
  }

  static_assert((Future<Futures> && ...),
                "All types in JoinFuture must be Future types");
  std::tuple<Futures...> futures_;
  std::tuple<Poll<typename Futures::value_type>...> outputs_;
  FutureState state_;
};

template <typename... Futures>
JoinFuture(Futures&&...) -> JoinFuture<Futures...>;

/// Creates a future which pends the provided futures until all of them have
/// completed.
///
/// When ready, the resulting future returns a tuple containing each future's
/// output in the order the futures were provided.
template <typename... Futures>
constexpr auto Join(Futures&&... futures) {
  static_assert((Future<Futures> && ...),
                "All arguments to Join must be Future types");
  return JoinFuture(std::forward<Futures>(futures)...);
}

/// @endsubmodule

}  // namespace pw::async2
