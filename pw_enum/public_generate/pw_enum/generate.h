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

// Generated pw_enum footers specialize pw::EnumTraits for each enum.
#include "pw_enum/traits.h"
#include "pw_preprocessor/apply.h"

/// @module{pw_enum}

#ifdef _PW_ENUM_GENERATING
#define PW_ENUM(enum_name, ...)
#define PW_ENUM_STATUS(status_name, ok_code, ...)
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

/// Registers a C++ enum and generates a `pw::StatusBase`-derived status class
/// wrapping it, with automatic stringification, tokenization, static factory
/// functions (e.g. `MyStatus::NotFound()`), and `Is*()` query functions (e.g.
/// `status.IsNotFound()`). `IsOk()` is not generated for codes named OK; use
/// `ok()`. A code named OK that is not the OK code is an error.
///
/// This macro must be called at the global scope (outside of any namespaces,
/// classes, or functions) in place of @ref PW_ENUM. The status class is
/// generated at the location of the `PW_ENUM_STATUS` invocation, so place it
/// at the bottom of the header, after any declarations that reference the
/// status class. Otherwise, those declarations require a forward declaration
/// of the status class (e.g. `namespace my_ns { class MyStatus; }`).
///
/// @example{pw_enum/examples/private/enum_example/i2c_status.h,pw_enum-examples-status-enum}
///
/// @param status_name The fully-qualified name of the status class to generate
///                    (e.g., `my_ns::MyStatus`).
/// @param ok_code The enumerator that represents success, including its enum
///                type (e.g., `MyStatusCode::kOk` or
///                `my_ns::MyStatusCode::kOk`). If only the enum type and
///                enumerator are given, the enum is assumed to be in the
///                namespace of `status_name`. Otherwise, the name is treated
///                as fully qualified, with or without a leading `::`. Enums
///                in other namespaces or nested in classes must be fully
///                qualified (e.g., `my_ns::MyClass::Code::kOk`). The enum type
///                is deduced from this argument.
/// @param ... The enumerator names to register (same syntax as @ref PW_ENUM).
///            Must include the OK enumerator.
#define PW_ENUM_STATUS(status_name, ok_code, ...)                            \
  static_assert(false,                                                       \
                "PW_ENUM_STATUS(" #status_name                               \
                ") must be compiled with a pw_cc_enum target!");             \
  _PW_ENUM_STATUS_MUST_BE_COMPILED_BY_pw_cc_enum(                            \
      #status_name,                                                          \
      #ok_code,                                                              \
      PW_APPLY(_PW_ENUM_STRINGIFY_ARG, _PW_ENUM_COMMA, unused, __VA_ARGS__)) \
      _PW_ENUM_MACRO_END

/// @}

#define _PW_ENUM_STRINGIFY_ARG(index, _, arg) #arg
#define _PW_ENUM_COMMA(index, _) ,

#endif  // _PW_ENUM_GENERATING

// Prevent PW_ENUM and PW_ENUM_STATUS declarations within namespaces by
// attempting to specialize this global type.
template <int>
struct _PW_ENUM_cannot_be_used_within_namespaces;

// Internal macro used in place of PW_ENUM and PW_ENUM_STATUS in generated
// pw_enum headers.
#define _PW_ENUM_GENERATED(...) \
  template <>                   \
  struct _PW_ENUM_cannot_be_used_within_namespaces<__COUNTER__>
