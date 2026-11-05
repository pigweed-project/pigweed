// Copyright 2024 The Pigweed Authors
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

// Macros for working with arguments to function-like macros.
#pragma once

#include "pw_preprocessor/arguments.h"
#include "pw_preprocessor/internal/apply_macros.h"

/// @module{pw_preprocessor}

/// Repeatedly applies the given macro for each argument provided. The given
/// macro expands to accept an index value of the current argument, the
/// forwarded argument provided, and the current argument. The forwarded
/// argument is used in every macro call. The separator expands to accept the
/// index argument as well as the forwarded argument. The last separator is
/// omitted.
///
/// The example below shows how this macro expands, based on a pre-defined
/// macro and separator. The statement `PW_APPLY(MACRO, SEP, ARG, a0, a1)`
/// expands to:
///
/// @code{.cpp}
/// MACRO(0, ARG, a0) SEP(0, ARG) MACRO(1, ARG, a1)
/// @endcode
#define PW_APPLY(macro, separator, forwarded_arg, ...)              \
  _PW_APPLY(_PW_PASTE2(_PW_AP, PW_FUNCTION_ARG_COUNT(__VA_ARGS__)), \
            macro,                                                  \
            separator,                                              \
            forwarded_arg,                                          \
            PW_DROP_LAST_ARG_IF_EMPTY(__VA_ARGS__))

/// @}

#define _PW_APPLY(function, macro, separator, forwarded_arg, ...) \
  function(macro, separator, forwarded_arg, 0, __VA_ARGS__)
