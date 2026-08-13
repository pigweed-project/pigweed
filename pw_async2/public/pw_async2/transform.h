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

//         __      ___   ___ _  _ ___ _  _  ___
//         \ \    / /_\ | _ \ \| |_ _| \| |/ __|
//          \ \/\/ / _ \|   / .` || || .` | (_ |
//           \_/\_/_/ \_\_|_\_|\_|___|_|\_|\___|
//  _____  _____ ___ ___ ___ __  __ ___ _  _ _____ _   _
// | __\ \/ / _ \ __| _ \_ _|  \/  | __| \| |_   _/_\ | |
// | _| >  <|  _/ _||   /| || |\/| | _|| .` | | |/ _ \| |__
// |___/_/\_\_| |___|_|_\___|_|  |_|___|_|\_| |_/_/ \_\____|
//
// These interfaces are in an early, experimental state. The APIs are in flux
// and may change without notice. Additionally, the code size is not optimized
// and may be larger than expected.

#include <optional>
#include <variant>

#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/try.h"

namespace pw::async2::experimental {
namespace internal {

template <typename Func>
struct MapOp {
  Func func;
};

template <typename Func>
struct ThenOp {
  Func func;
};

}  // namespace internal

template <typename Func>
constexpr internal::MapOp<Func> Map(Func&& f) {
  return internal::MapOp<Func>{std::forward<Func>(f)};
}

template <typename InnerFuture, typename Func>
class MapFuture {
 public:
  using value_type =
      std::invoke_result_t<Func, typename InnerFuture::value_type>;

  MapFuture() = default;
  MapFuture(MapFuture&&) = default;
  MapFuture& operator=(MapFuture&& other) {
    inner_ = std::move(other.inner_);
    func_.reset();
    if (other.func_) {
      func_.emplace(std::move(*other.func_));
    }
    return *this;
  }

  MapFuture(InnerFuture&& inner, Func&& func)
      : inner_(std::move(inner)), func_(std::forward<Func>(func)) {}

  Poll<value_type> Pend(Context& cx) {
    PW_TRY_READY_ASSIGN(auto result, inner_.Pend(cx));
    return Poll<value_type>((*func_)(std::move(result)));
  }

  bool is_pendable() const { return inner_.is_pendable(); }
  bool is_complete() const { return inner_.is_complete(); }

 private:
  InnerFuture inner_;
  std::optional<Func> func_;
};

template <typename Func>
constexpr internal::ThenOp<Func> Then(Func&& f) {
  return internal::ThenOp<Func>{std::forward<Func>(f)};
}

template <typename FirstFuture, typename Func>
class ThenFuture {
 public:
  using SecondFuture =
      std::invoke_result_t<Func, typename FirstFuture::value_type>;
  using value_type = typename SecondFuture::value_type;

  ThenFuture() = default;
  ThenFuture(ThenFuture&&) = default;
  ThenFuture& operator=(ThenFuture&& other) {
    func_.reset();
    if (other.func_) {
      func_.emplace(std::move(*other.func_));
    }
    state_ = std::move(other.state_);
    return *this;
  }

  ThenFuture(FirstFuture&& first, Func&& func)
      : func_(std::move(func)),
        state_(std::in_place_index<0>, std::move(first)) {}

  Poll<value_type> Pend(Context& cx) {
    if (state_.index() == 0) {
      auto& first = std::get<0>(state_);
      PW_TRY_READY_ASSIGN(auto result, first.Pend(cx));

      state_.template emplace<1>((*func_)(std::move(result)));
    }

    if (state_.index() == 1) {
      auto& second = std::get<1>(state_);
      PW_TRY_READY_ASSIGN(auto result, second.Pend(cx));
      state_.template emplace<2>();
      return result;
    }

    return Pending();
  }

  bool is_pendable() const { return state_.index() != 2; }
  bool is_complete() const { return state_.index() == 2; }

 private:
  std::optional<Func> func_;
  std::variant<FirstFuture, SecondFuture, std::monostate> state_;
};

namespace internal {

template <typename InnerFuture,
          typename Func,
          typename = std::enable_if_t<Future<std::decay_t<InnerFuture>>>>
auto operator|(InnerFuture&& future, MapOp<Func>&& op) {
  return MapFuture<std::decay_t<InnerFuture>, std::decay_t<Func>>(
      std::forward<InnerFuture>(future), std::move(op.func));
}

template <typename FirstFuture,
          typename Func,
          typename = std::enable_if_t<Future<std::decay_t<FirstFuture>>>>
auto operator|(FirstFuture&& future, ThenOp<Func>&& op) {
  return ThenFuture<std::decay_t<FirstFuture>, std::decay_t<Func>>(
      std::forward<FirstFuture>(future), std::move(op.func));
}

}  // namespace internal
}  // namespace pw::async2::experimental
