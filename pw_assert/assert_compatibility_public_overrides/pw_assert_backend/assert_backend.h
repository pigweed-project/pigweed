// Copyright 2022 The Pigweed Authors
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

// IWYU pragma: private, include "pw_assert/assert.h"

#include "pw_preprocessor/compiler.h"  // IWYU pragma: export
#include "pw_preprocessor/util.h"      // IWYU pragma: export

PW_EXTERN_C_START

/// @module{pw_assert}

/// Handles a low-level crash from `pw_assert/assert.h`. In this crash handler,
/// there is no access to line, file, expression, or other rich assert
/// information. Must not return.
PW_NO_RETURN void pw_assert_HandleFailure(void);

PW_EXTERN_C_END

/// Handles a low-level crash from `pw_assert/assert.h`. Backends must ensure
/// their implementation is safe for usage in headers, constexpr contexts, and
/// templates. This macro should expand to an expression that does not return.
#define PW_ASSERT_HANDLE_FAILURE(condition_string) pw_assert_HandleFailure()

/// @endmodule
