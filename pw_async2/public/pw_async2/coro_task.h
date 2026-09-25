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

#include <type_traits>

#include "pw_async2/coro.h"
#include "pw_async2/future_task.h"

namespace pw::async2 {

/// @submodule{pw_async2,coroutines}

/// A `Task` that delegates to a provided `Coro<T>`.
///
/// `CoroTask` is an alias of `FutureTask<Coro<T>, policy>`.
///
/// @deprecated Use `FutureTask` instead.
template <typename T = void,
          ReturnValuePolicy policy = std::is_void_v<T>
                                         ? ReturnValuePolicy::kDiscard
                                         : ReturnValuePolicy::kKeep>
using CoroTask [[deprecated("Use FutureTask instead")]] =
    FutureTask<Coro<T>, policy>;

/// @endsubmodule

}  // namespace pw::async2
