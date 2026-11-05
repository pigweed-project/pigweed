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

// Include these headers as C++ code, in case assert.h is included within an
// extern "C" block.
#ifdef __cplusplus
extern "C++" {
#endif  // __cplusplus

#include "pw_assert/assert.h"
#include "pw_preprocessor/util.h"

#ifdef __cplusplus
}  // extern "C++"
#endif  // __cplusplus

// Provide static_assert() for C11 and C17. static_assert is a keyword in C23.
#if (defined(__USE_ISOC11) || defined(__STDC_VERSION__) &&         \
                                  (__STDC_VERSION__ >= 201112L) && \
                                  (__STDC_VERSION__ < 202311L)) && \
    !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif  // C11 or newer

// Provide assert()
#undef assert
#if defined(NDEBUG)  // Required by ANSI C standard.
#define assert(condition) ((void)0)
#else
#define assert(condition) PW_ASSERT(condition)
#endif  // defined(NDEBUG)
