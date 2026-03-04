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

#include <optional>

#include "pw_async2/context.h"
#include "pw_async2/coro.h"
#include "pw_async2/fallible_coro_task.h"
#include "pw_function/function.h"

namespace pw::async2 {

/// @submodule{pw_async2,coroutines}

/// @deprecated Use `FallibleCoroTask` instead.
class [[deprecated("Use FallibleCoroTask instead")]] CoroOrElseTask final
    : public Task {
 public:
  /// Create a new ``Task`` which runs ``coro``, invoking ``or_else`` on
  /// any non-OK status.
  CoroOrElseTask(Coro<Status>&& coro, pw::Function<void(Status)>&& or_else)
      : coro_task_(std::in_place,
                   std::move(coro),
                   [this] { or_else_(Status::Internal()); }),
        or_else_(std::move(or_else)) {}

  ~CoroOrElseTask() override { Deregister(); }

  /// *Non-atomically* sets `coro`.
  ///
  /// The task must not be `Post`ed when `coro` is changed.
  void SetCoro(Coro<Status>&& coro) {
    PW_ASSERT(!IsRegistered());
    coro_task_.emplace(std::move(coro),
                       [this] { or_else_(Status::Internal()); });
  }

  /// *Non-atomically* sets `or_else`.
  ///
  /// The task must not be `Post`ed when `or_else` is changed.
  void SetErrorHandler(pw::Function<void(Status)>&& or_else) {
    PW_ASSERT(!IsRegistered());
    or_else_ = std::move(or_else);
  }

 private:
  Poll<> DoPend(Context& cx) final { return coro_task_->Pend(cx); }

  std::optional<FallibleCoroTask<Status>> coro_task_;
  pw::Function<void(Status)> or_else_;
};

/// @endsubmodule

}  // namespace pw::async2
