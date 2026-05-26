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

#include "pw_preprocessor/apply.h"

/// @module{pw_enum}

#ifdef _PW_ENUM_GENERATING
#define PW_ENUM(enum_name, ...)
#else

/// Registers a C++ enum for automatic stringification and tokenization.
///
/// This macro must be called at the global scope (outside of any namespaces,
/// classes, or functions).
///
/// @param enum_name The fully-qualified C++ enum name (e.g., `my_ns::MyEnum`).
/// @param ... The enumerator names to register. By default, Google-style
///            `kCamelCase` names are converted to `UPPER_SNAKE_CASE` without
///            the `k` prefix. A custom display name can be specified with
///            `kEnum = "custom"`. If multiple enumerators share the same value
///            (aliases), they are grouped and their names are joined (e.g.,
///            `"A|B"`).
#define PW_ENUM(enum_name, ...)                                              \
  static_assert(false,                                                       \
                "PW_ENUM(" #enum_name                                        \
                ") must be compiled with a pw_cc_enum target!");             \
  _PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum(                                   \
      #enum_name,                                                            \
      PW_APPLY(_PW_ENUM_STRINGIFY_ARG, _PW_ENUM_COMMA, unused, __VA_ARGS__)) \
      _PW_ENUM_MACRO_END

/// @}

#define _PW_ENUM_STRINGIFY_ARG(index, _, arg) #arg
#define _PW_ENUM_COMMA(index, _) ,

#endif  // _PW_ENUM_GENERATING

// Prevent PW_ENUM declarations within namespaces by attempting to specialize
// this global type.
template <typename T>
struct _PW_ENUM_cannot_be_used_within_namespaces;

// Internal macro used in place of PW_ENUM in generated pw_enum headers.
#define _PW_ENUM_GENERATED(enum_name, ...) \
  template <>                              \
  struct _PW_ENUM_cannot_be_used_within_namespaces<enum_name>
