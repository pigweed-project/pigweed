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

#include <functional>
#include <type_traits>

#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_function/function.h"

namespace pw::async2 {
namespace internal {

template <typename FutureType>
using CallbackType =
    std::conditional_t<std::is_void_v<typename FutureType::value_type>,
                       Function<void()>,
                       Function<void(FutureValue<FutureType>)>>;

}  // namespace internal

/// @submodule{pw_async2,tasks}

/// A `Task` which pends a future and invokes a provided callback
/// with its output when it returns `Ready`.
///
/// A `CallbackTask` terminates after the underlying future returns
/// `Ready` and can be cleaned up afterwards.
template <typename FutureType,
          typename Func = internal::CallbackType<FutureType>>
class CallbackTask final : public Task {
 public:
  using value_type = typename FutureType::value_type;

  static_assert(Future<FutureType>,
                "CallbackTask can only be used with Future types");

  /// Creates a new `CallbackTask` which will run the `future` to completion
  /// and then invoke the `callback` with the result.
  constexpr CallbackTask(Func&& callback, FutureType&& future)
      : Task(PW_ASYNC_TASK_NAME("CallbackTask")),
        future_(std::move(future)),
        callback_(std::move(callback)) {}

  ~CallbackTask() override { Deregister(); }

  /// Creates a new `CallbackTask` which will run the `future` to completion
  /// and then invoke the `callback` with the result.
  ///
  /// This function constructs the `FutureType` in-place from `future_args`.
  template <typename Callback, typename... Args>
  static constexpr auto Emplace(Callback&& callback, Args&&... future_args) {
    static_assert(sizeof...(Args) >= 1u,
                  "Cannot default construct a Future with Emplace");
    return CallbackTask<FutureType, Callback>(
        std::forward<Callback>(callback), std::forward<Args>(future_args)...);
  }

 private:
  template <typename, typename>
  friend class CallbackTask;

  template <typename... Args>
  constexpr CallbackTask(Func&& callback, Args&&... future_args)
      : future_(std::forward<Args>(future_args)...),
        callback_(std::move(callback)) {}

  Poll<> DoPend(Context& cx) final {
    Poll<value_type> poll = future_.Pend(cx);
    if (poll.IsPending()) {
      return Pending();
    }

    if constexpr (std::is_void_v<value_type>) {
      callback_();
    } else {
      callback_(std::move(*poll));
    }

    return Ready();
  }

  FutureType future_;
  Func callback_;
};

template <typename FutureType, typename Func>
CallbackTask(Func&&, FutureType&&) -> CallbackTask<FutureType, Func>;

// TODO: b/458069794 - Add StreamCallbackTask.

/// @endsubmodule

}  // namespace pw::async2
