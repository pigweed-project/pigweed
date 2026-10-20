// Copyright 2021 The Pigweed Authors
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

/// @module{pw_assert}

/// Controls whether `DCHECK` and `DASSERT` are enabled.
///
/// This defaults to being disabled if `NDEBUG` is defined, else it is enabled
/// by default.
#if !defined(PW_ASSERT_ENABLE_DEBUG)
#if defined(NDEBUG)
// Release mode; remove all DCHECK*() and DASSERT() asserts.
#define PW_ASSERT_ENABLE_DEBUG 0
#else
// Debug mode; keep all DCHECK*() and DASSERT() asserts.
#define PW_ASSERT_ENABLE_DEBUG 1
#endif  // defined (NDEBUG)
#endif  // !defined(PW_ASSERT_ENABLE_DEBUG)

/// Controls whether the evaluated values of a `CHECK` statement are captured as
/// arguments to the final string. Disabling this will reduce code size at
/// `CHECK` callsites, but slightly reduces debuggability.
///
/// This defaults to enabled.
#if !defined(PW_ASSERT_CAPTURE_VALUES)
#define PW_ASSERT_CAPTURE_VALUES 1
#endif  // !defined(PW_ASSERT_CAPTURE_VALUES)

// Modes available for how to end an assert failure for pw_assert_basic.
#define PW_ASSERT_BASIC_ACTION_ABORT 100
#define PW_ASSERT_BASIC_ACTION_EXIT 101
#define PW_ASSERT_BASIC_ACTION_LOOP 102

#ifdef PW_ASSERT_BASIC_ABORT
#error PW_ASSERT_BASIC_ABORT is deprecated! Use PW_ASSERT_BASIC_ACTION instead.
#endif  // PW_ASSERT_BASIC_ABORT

/// Set to one of the following to define how `pw_assert_basic` should act
/// after an assert failure:
/// - `PW_ASSERT_BASIC_ACTION_ABORT`: Call `std::abort()`
/// - `PW_ASSERT_BASIC_ACTION_EXIT`: Call `std::_Exit(-1)`
/// - `PW_ASSERT_BASIC_ACTION_LOOP`: Loop forever
#ifndef PW_ASSERT_BASIC_ACTION
#define PW_ASSERT_BASIC_ACTION PW_ASSERT_BASIC_ACTION_ABORT
#endif  // PW_ASSERT_BASIC_ACTION

/// Whether to show the CRASH ASCII art banner.
#ifndef PW_ASSERT_BASIC_SHOW_BANNER
#define PW_ASSERT_BASIC_SHOW_BANNER 1
#endif  // PW_ASSERT_BASIC_SHOW_BANNER

/// Whether to use ANSI colors.
#ifndef PW_ASSERT_BASIC_USE_COLORS
#define PW_ASSERT_BASIC_USE_COLORS 1
#endif  // PW_ASSERT_BASIC_USE_COLORS

/// @endmodule
