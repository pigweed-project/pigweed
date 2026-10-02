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

namespace pw::rpc2::internal {

/// Reusable base class for futures that delegates state tracking (`kEmpty`,
/// `kPending`, `kComplete`) to `pw::async2::FutureState`.
class FutureBase {
 public:
  FutureBase(const FutureBase&) = delete;
  FutureBase& operator=(const FutureBase&) = delete;

  [[nodiscard]] constexpr bool is_pendable() const {
    return state_.is_pendable();
  }

  [[nodiscard]] constexpr bool is_complete() const {
    return state_.is_complete();
  }

 protected:
  constexpr FutureBase() = default;

  explicit constexpr FutureBase(async2::FutureState::Pending)
      : state_(async2::FutureState::kPending) {}

  explicit constexpr FutureBase(async2::FutureState state)
      : state_(std::move(state)) {}

  constexpr FutureBase(FutureBase&&) noexcept = default;
  constexpr FutureBase& operator=(FutureBase&&) noexcept = default;

  ~FutureBase() = default;

  constexpr void mark_pending() {
    state_ = async2::FutureState(async2::FutureState::kPending);
  }

  void mark_complete() { state_.MarkComplete(); }

 private:
  async2::FutureState state_;
};

}  // namespace pw::rpc2::internal
