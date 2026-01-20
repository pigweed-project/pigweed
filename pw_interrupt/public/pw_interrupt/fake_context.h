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

#include "pw_function/function.h"
#include "pw_interrupt/context.h"

namespace pw::interrupt::fake {

/// @module{pw_interrupt}

/// @brief Sets a callback which can be used by a test to set the return
/// value of the `InInterruptContext()` when using the fake backend.
///
/// @param callback Callback to invoke to get the return value.
void SetInInterruptContextCallback(Function<bool()>&& callback);

///);

}  // namespace pw::interrupt::fake
