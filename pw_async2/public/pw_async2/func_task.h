// Copyright 2023 The Pigweed Authors
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
#include "pw_function/function.h"

namespace pw::async2 {

/// @submodule{pw_async2,tasks}

/// A `Task` that delegates to a provided function `func`.
///
/// The provided `func` may be any callable (function, lambda, or similar) which
/// accepts a `Context&` and returns a `Poll<>`.
///
/// The resulting `Task` implements `Pend` by invoking `func`.
template <typename Func = Function<Poll<>(Context&)> >
class FuncTask final : public Task {
 public:
  /// Creates a new `Task` that delegates `Pend` to `func`.
  explicit constexpr FuncTask(Func&& func) : func_(std::forward<Func>(func)) {}

  FuncTask(const FuncTask&) = delete;
  FuncTask& operator=(const FuncTask&) = delete;

  FuncTask(FuncTask&&) = delete;
  FuncTask& operator=(FuncTask&&) = delete;

  ~FuncTask() override { Deregister(); }

 private:
  Poll<> DoPend(Context& cx) final { return func_(cx); }

  Func func_;
};

template <typename Func>
FuncTask(Func&&) -> FuncTask<Func>;

/// @endsubmodule

}  // namespace pw::async2
