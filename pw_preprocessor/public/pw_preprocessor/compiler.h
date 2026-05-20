// Copyright 2019 The Pigweed Authors
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
//
// Preprocessor macros that wrap compiler-specific features.
// This file is used by both C++ and C code.
#pragma once

// TODO: b/234877280 - compiler.h should be refactored out of pw_preprocessor as
// the scope is outside of the module. Perhaps it should be split up and placed
// under pw_compiler, e.g. pw_compiler/attributes.h & pw_compiler/builtins.h.

#include "pw_polyfill/static_assert.h"

/// @submodule{pw_preprocessor,compiler}

/// Marks a struct or class as packed.
///
/// Use packed structs with extreme caution! Packed structs are rarely needed.
/// Instead, define the struct and `static_assert` to verify that the size and
/// alignement are as expected.
///
/// Packed structs should only be used to avoid standard padding or to force
/// unaligned members when describing in-memory or wire format data structures.
/// Packed struct members should NOT be accessed directly because they may be
/// unaligned. Instead, `memcpy` the fields into variables. For example:
///
/// @code{.cpp}
///   PW_PACKED(struct) PackedStruct {
///     uint8_t a;
///     uint32_t b;
///     uint16_t c;
///   };
///
///   void UsePackedStruct(const PackedStruct& packed_struct) {
///     uint8_t a;
///     uint32_t b;
///     uint16_t c;
///     std::memcpy(&a, &packed_struct.a, sizeof(a));
///     std::memcpy(&b, &packed_struct.b, sizeof(b));
///     std::memcpy(&c, &packed_struct.c, sizeof(c));
///   }
/// @endcode
#define PW_PACKED(declaration) declaration __attribute__((packed))

/// Marks a function or object as used, ensuring code for it is generated.
#define PW_USED __attribute__((used))

/// Prevents generation of a prologue or epilogue for a function. This is
/// helpful when implementing the function in assembly.
#define PW_NO_PROLOGUE __attribute__((naked))

/// Marks that a function declaration takes a printf-style format string and
/// variadic arguments. This allows the compiler to perform check the validity
/// of the format string and arguments. This macro must only be on the function
/// declaration, not the definition.
///
/// The format_index is index of the format string parameter and parameter_index
/// is the starting index of the variadic arguments. Indices start at 1. For C++
/// class member functions, add one to the index to account for the implicit
/// this parameter.
///
/// This example shows a function where the format string is argument 2 and the
/// varargs start at argument 3.
///
/// @code{.cpp}
///   int PrintfStyleFunction(char* buffer, const char* fmt, ...)
///   PW_PRINTF_FORMAT(2,3);
///
///   int PrintfStyleFunction(char* buffer, const char* fmt, ...) { ...
///   implementation here ...  }
/// @endcode
#define PW_PRINTF_FORMAT(format_index, parameter_index) \
  __attribute__((format(_PW_PRINTF_FORMAT_TYPE, format_index, parameter_index)))

/// When compiling for host using MinGW, use gnu_printf() rather than printf()
/// to support %z format specifiers only if available (non-clang compilers).
/// @ingroup pw_preprocessor_internal
#if defined(__USE_MINGW_ANSI_STDIO) && !defined(__clang__)
#define _PW_PRINTF_FORMAT_TYPE gnu_printf
#else
#define _PW_PRINTF_FORMAT_TYPE printf
#endif  // defined(__USE_MINGW_ANSI_STDIO) && !defined(__clang__)

/// Places a variable in the specified linker section.
#ifdef __APPLE__
#define PW_PLACE_IN_SECTION(name) __attribute__((section("__DATA," name)))
#else
#define PW_PLACE_IN_SECTION(name) __attribute__((section(name)))
#endif  // __APPLE__

/// Places a variable in the specified linker section and directs the compiler
/// to keep the variable, even if it is not used. Depending on the linker
/// options, the linker may still remove this section if it is not declared in
/// the linker script and marked `KEEP`.
#ifdef __APPLE__
#define PW_KEEP_IN_SECTION(name) __attribute__((section("__DATA," name), used))
#else
#define PW_KEEP_IN_SECTION(name) __attribute__((section(name), used))
#endif  // __APPLE__

/// Indicate to the compiler that the annotated function won't return. Example:
///
/// @code{.cpp}
///   PW_NO_RETURN void HandleAssertFailure(ErrorCode error_code);
/// @endcode
#define PW_NO_RETURN __attribute__((noreturn))

/// Prevents the compiler from inlining a fuction.
#define PW_NO_INLINE __attribute__((noinline))

/// Indicate to the compiler that the given section of code will not be reached.
/// Example:
///
/// @code{.cpp}
///   int main() {
///     InitializeBoard();
///     vendor_StartScheduler();  // Note: vendor forgot noreturn attribute.
///     PW_UNREACHABLE;
///   }
/// @endcode
#define PW_UNREACHABLE __builtin_unreachable()

/// Indicate to a sanitizer compiler runtime to skip the named check in the
/// associated function.
/// Example:
///
/// @code{.cpp}
///   uint32_t djb2(const void* buf, size_t len)
///       PW_NO_SANITIZE("unsigned-integer-overflow") {
///     uint32_t hash = 5381;
///     const uint8_t* u8 = static_cast<const uint8_t*>(buf);
///     for (size_t i = 0; i < len; ++i) {
///       hash = (hash * 33) + u8[i]; /* hash * 33 + c */
///     }
///     return hash;
///   }
/// @endcode
#ifdef __clang__
#define PW_NO_SANITIZE(check) __attribute__((no_sanitize(check)))
#else
#define PW_NO_SANITIZE(check)
#endif  // __clang__

/// Wrapper around `__has_attribute`, which is defined by GCC 5+ and Clang and
/// evaluates to a non zero constant integer if the attribute is supported or 0
/// if not.
#ifdef __has_attribute
#define PW_HAVE_ATTRIBUTE(x) __has_attribute(x)
#else
#define PW_HAVE_ATTRIBUTE(x) 0
#endif  // __has_attribute

/// A function-like feature checking macro that accepts C++11 style attributes.
/// It is a wrapper around `__has_cpp_attribute`, which was introduced in the <a
/// href="https://en.cppreference.com/w/cpp/feature_test">C++20 standard</a>. It
/// is supported by compilers even if C++20 is not in use. Evaluates to a
/// non-zero constant integer if the C++ attribute is supported or 0 if not.
///
/// This is a copy of `ABSL_HAVE_CPP_ATTRIBUTE`.
#if defined(__cplusplus) && defined(__has_cpp_attribute)
#define PW_HAVE_CPP_ATTRIBUTE(x) __has_cpp_attribute(x)
#else
#define PW_HAVE_CPP_ATTRIBUTE(x) 0
#endif  // defined(__cplusplus) && defined(__has_cpp_attribute)

/// @ingroup pw_preprocessor_internal
#define _PW_REQUIRE_SEMICOLON \
  static_assert(1, "This macro must be terminated with a semicolon")

/// Starts a new group of @c_macro{PW_MODIFY_DIAGNOSTIC} statements. A
/// @c_macro{PW_MODIFY_DIAGNOSTICS_POP} statement must follow.
#define PW_MODIFY_DIAGNOSTICS_PUSH() \
  _Pragma("GCC diagnostic push") _PW_REQUIRE_SEMICOLON

/// @c_macro{PW_MODIFY_DIAGNOSTIC} statements since the most recent
/// @c_macro{PW_MODIFY_DIAGNOSTICS_PUSH} no longer apply after this statement.
#define PW_MODIFY_DIAGNOSTICS_POP() \
  _Pragma("GCC diagnostic pop") _PW_REQUIRE_SEMICOLON

/// Changes how a diagnostic (warning or error) is handled. Most commonly used
/// to disable warnings. ``PW_MODIFY_DIAGNOSTIC`` should be used between
/// @c_macro{PW_MODIFY_DIAGNOSTICS_PUSH} and @c_macro{PW_MODIFY_DIAGNOSTICS_POP}
/// statements to avoid applying the modifications too broadly.
///
/// ``kind`` may be ``warning``, ``error``, or ``ignored``.
#define PW_MODIFY_DIAGNOSTIC(kind, option) \
  PW_PRAGMA(GCC diagnostic kind option) _PW_REQUIRE_SEMICOLON

/// Applies ``PW_MODIFY_DIAGNOSTIC`` only for GCC. This is useful for warnings
/// that aren't supported by or don't need to be changed in other compilers.
#ifdef __clang__
#define PW_MODIFY_DIAGNOSTIC_GCC(kind, option) _PW_REQUIRE_SEMICOLON
#else
#define PW_MODIFY_DIAGNOSTIC_GCC(kind, option) \
  PW_MODIFY_DIAGNOSTIC(kind, option)
#endif  // __clang__

/// Applies ``PW_MODIFY_DIAGNOSTIC`` only for Clang. This is useful for warnings
/// that aren't supported by or don't need to be changed in other compilers.
#ifdef __clang__
#define PW_MODIFY_DIAGNOSTIC_CLANG(kind, option) \
  PW_MODIFY_DIAGNOSTIC(kind, option)
#else
#define PW_MODIFY_DIAGNOSTIC_CLANG(kind, option) _PW_REQUIRE_SEMICOLON
#endif  // __clang__

/// Expands to a `_Pragma` with the contents as a string. `_Pragma` must take a
/// single string literal; this can be used to construct a `_Pragma` argument.
#define PW_PRAGMA(contents) _Pragma(#contents)

/// Marks a function or object as weak, allowing the definition to be overriden.
///
/// This can be useful when supporting third-party SDKs which may conditionally
/// compile in code, for example:
///
/// @code{.cpp}
///   PW_WEAK void SysTick_Handler(void) {
///     // Default interrupt handler that might be overriden.
///   }
/// @endcode
#define PW_WEAK __attribute__((weak))

/// Marks a weak function as an alias to another, allowing the definition to
/// be given a default and overriden.
///
/// This can be useful when supporting third-party SDKs which may conditionally
/// compile in code, for example:
///
/// @code{.cpp}
///   // Driver handler replaced with default unless overridden.
///   void USART_DriverHandler(void) PW_ALIAS(DefaultDriverHandler);
/// @endcode
#define PW_ALIAS(aliased_to) __attribute__((weak, alias(#aliased_to)))

/// `PW_ATTRIBUTE_LIFETIME_BOUND` indicates that a resource owned by a function
/// parameter or implicit object parameter is retained by the return value of
/// the annotated function (or, for a parameter of a constructor, in the value
/// of the constructed object). This attribute causes warnings to be produced if
/// a temporary object does not live long enough.
///
/// When applied to a reference parameter, the referenced object is assumed to
/// be retained by the return value of the function. When applied to a
/// non-reference parameter (for example, a pointer or a class type), all
/// temporaries referenced by the parameter are assumed to be retained by the
/// return value of the function.
///
/// See also the upstream documentation:
/// https://clang.llvm.org/docs/AttributeReference.html#lifetimebound
///
/// This is a copy of `ABSL_ATTRIBUTE_LIFETIME_BOUND`.
#if PW_HAVE_CPP_ATTRIBUTE(clang::lifetimebound)
#define PW_ATTRIBUTE_LIFETIME_BOUND [[clang::lifetimebound]]
#elif PW_HAVE_ATTRIBUTE(lifetimebound)
#define PW_ATTRIBUTE_LIFETIME_BOUND __attribute__((lifetimebound))
#else
#define PW_ATTRIBUTE_LIFETIME_BOUND
#endif  // PW_ATTRIBUTE_LIFETIME_BOUND

/// Evaluates to 1 if `__VA_OPT__` is supported, regardless of the C or C++
/// standard in use.
#if (defined(__clang_major__) && __clang_major__ < 9) || \
    (defined(__GNUC__) && __GNUC__ < 12)
#define PW_VA_OPT_SUPPORTED() 0  // Don't bother checking on old compilers.
#else
#define PW_VA_OPT_SUPPORTED() _PW_VA_OPT_SUPPORTED()

#define _PW_VA_OPT_SUPPORTED(...) _PW_VA_OPT_SUPPORTED_##__VA_OPT__()
#define _PW_VA_OPT_SUPPORTED_ 1
#define _PW_VA_OPT_SUPPORTED___VA_OPT__() 0

#endif  // __clang_major__ < 9 || __GNUC__ < 12

/// `PW_NO_UNIQUE_ADDRESS` indicates that an object can have no unique
/// address. This is usually used to force empty member types to occupy
/// 0 bytes instead of 1 byte to have a unique address.
#if PW_HAVE_CPP_ATTRIBUTE(no_unique_address)
#define PW_NO_UNIQUE_ADDRESS [[no_unique_address]]
#else
#define PW_NO_UNIQUE_ADDRESS
#endif  // PW_NO_UNIQUE_ADDRESS

/// @endsubmodule
