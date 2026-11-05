// Copyright 2020 The Pigweed Authors
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

#include "pw_preprocessor/boolean.h"
#include "pw_preprocessor/compiler.h"
#include "pw_preprocessor/internal/arg_count_impl.h"

// Expands to a comma followed by __VA_ARGS__, if __VA_ARGS__ is non-empty.
// Otherwise, expands to nothing. If the final argument is empty, it is omitted.
// This is useful when passing __VA_ARGS__ to a variadic function or template
// parameter list, since it removes the extra comma when no arguments are
// provided. PW_COMMA_ARGS must NOT be used when invoking a macro from another
// macro.
//
// This is a more flexible, standard-compliant version of ##__VA_ARGS__. Unlike
// ##__VA_ARGS__, this can be used to eliminate an unwanted comma when
// __VA_ARGS__ expands to an empty argument because an outer macro was called
// with __VA_ARGS__ instead of ##__VA_ARGS__. Also, since PW_COMMA_ARGS drops
// the last argument if it is empty, both MY_MACRO(1, 2) and MY_MACRO(1, 2, )
// can work correctly.
//
// PW_COMMA_ARGS must NOT be used to conditionally include a comma when invoking
// a macro from another macro. PW_COMMA_ARGS only functions correctly when the
// macro expands to C or C++ code! Using it with intermediate macros can result
// in out-of-order parameters. When invoking one macro from another, simply pass
// __VA_ARGS__. Only the final macro that expands to C/C++ code should use
// PW_COMMA_ARGS.
//
// For example, the following does NOT work:
/*
     #define MY_MACRO(fmt, ...) \
         NESTED_MACRO(fmt PW_COMMA_ARGS(__VA_ARGS__))  // BAD! Do not do this!
*/
// Instead, only use PW_COMMA_ARGS when the macro expands to C/C++ code:
/*
     #define MY_MACRO(fmt, ...) \
         NESTED_MACRO(fmt, __VA_ARGS__)  // Pass __VA_ARGS__ to nested macros

     #define NESTED_MACRO(fmt, ...) \
         printf(fmt PW_COMMA_ARGS(__VA_ARGS__))  // PW_COMMA_ARGS is OK here
*/
#define PW_COMMA_ARGS(...)                                       \
  _PW_IF(PW_EMPTY_ARGS(__VA_ARGS__), _PW_EXPAND, _PW_COMMA_ARGS) \
  (PW_DROP_LAST_ARG_IF_EMPTY(__VA_ARGS__))

#define _PW_COMMA_ARGS(...) , __VA_ARGS__

// Allows calling a different function-like macros based on the number of
// arguments. For example:
//
//   #define ARG_PRINT(...)  PW_DELEGATE_BY_ARG_COUNT(_ARG_PRINT, __VA_ARGS__)
//   #define _ARG_PRINT1(a)        LOG_INFO("1 arg: %s", a)
//   #define _ARG_PRINT2(a, b)     LOG_INFO("2 args: %s, %s", a, b)
//   #define _ARG_PRINT3(a, b, c)  LOG_INFO("3 args: %s, %s, %s", a, b, c)
//
// This can the be called from C/C++ code:
//
//    ARG_PRINT("a");            // Outputs: 1 arg: a
//    ARG_PRINT("a", "b");       // Outputs: 2 args: a, b
//    ARG_PRINT("a", "b", "c");  // Outputs: 3 args: a, b, c
//
#define PW_DELEGATE_BY_ARG_COUNT(function, ...)                 \
  _PW_DELEGATE_BY_ARG_COUNT(                                    \
      _PW_PASTE2(function, PW_FUNCTION_ARG_COUNT(__VA_ARGS__)), \
      PW_DROP_LAST_ARG_IF_EMPTY(__VA_ARGS__))

#define _PW_DELEGATE_BY_ARG_COUNT(function, ...) function(__VA_ARGS__)

// PW_MACRO_ARG_COUNT counts the number of arguments it was called with. It
// evalulates to an integer literal in the range 0 to 64. Counting more than 64
// arguments is not currently supported.
//
// PW_MACRO_ARG_COUNT is most commonly used to count __VA_ARGS__ in a variadic
// macro. For example, the following code counts the number of arguments passed
// to a logging macro:
//
/*   #define LOG_INFO(format, ...) {                                   \
         static const int kArgCount = PW_MACRO_ARG_COUNT(__VA_ARGS__); \
         SendLog(kArgCount, format, ##__VA_ARGS__);                    \
       }
*/
// The macro argument lists were generated with a Python script:
/*
COUNT = 256

for i in range(COUNT, 0, -1):
    if i % 8 == 0:
        print('\ \n', end='')
    print(f"{i:3}, ", end='')

for i in range(COUNT, 0, -1):
    if i % 8 == 0:
        print('\ \n', end='')
    print(f"a{i:03}, ", end='')
*/
// clang-format off
#define PW_MACRO_ARG_COUNT(...)               \
  _PW_MACRO_ARG_COUNT_IMPL(__VA_ARGS__,       \
      256, 255, 254, 253, 252, 251, 250, 249, \
      248, 247, 246, 245, 244, 243, 242, 241, \
      240, 239, 238, 237, 236, 235, 234, 233, \
      232, 231, 230, 229, 228, 227, 226, 225, \
      224, 223, 222, 221, 220, 219, 218, 217, \
      216, 215, 214, 213, 212, 211, 210, 209, \
      208, 207, 206, 205, 204, 203, 202, 201, \
      200, 199, 198, 197, 196, 195, 194, 193, \
      192, 191, 190, 189, 188, 187, 186, 185, \
      184, 183, 182, 181, 180, 179, 178, 177, \
      176, 175, 174, 173, 172, 171, 170, 169, \
      168, 167, 166, 165, 164, 163, 162, 161, \
      160, 159, 158, 157, 156, 155, 154, 153, \
      152, 151, 150, 149, 148, 147, 146, 145, \
      144, 143, 142, 141, 140, 139, 138, 137, \
      136, 135, 134, 133, 132, 131, 130, 129, \
      128, 127, 126, 125, 124, 123, 122, 121, \
      120, 119, 118, 117, 116, 115, 114, 113, \
      112, 111, 110, 109, 108, 107, 106, 105, \
      104, 103, 102, 101, 100,  99,  98,  97, \
       96,  95,  94,  93,  92,  91,  90,  89, \
       88,  87,  86,  85,  84,  83,  82,  81, \
       80,  79,  78,  77,  76,  75,  74,  73, \
       72,  71,  70,  69,  68,  67,  66,  65, \
       64,  63,  62,  61,  60,  59,  58,  57, \
       56,  55,  54,  53,  52,  51,  50,  49, \
       48,  47,  46,  45,  44,  43,  42,  41, \
       40,  39,  38,  37,  36,  35,  34,  33, \
       32,  31,  30,  29,  28,  27,  26,  25, \
       24,  23,  22,  21,  20,  19,  18,  17, \
       16,  15,  14,  13,  12,  11,  10,   9, \
        8,   7,   6,   5,   4,   3,   2, PW_HAS_ARGS(__VA_ARGS__))


#define _PW_MACRO_ARG_COUNT_IMPL(                   \
    a256, a255, a254, a253, a252, a251, a250, a249, \
    a248, a247, a246, a245, a244, a243, a242, a241, \
    a240, a239, a238, a237, a236, a235, a234, a233, \
    a232, a231, a230, a229, a228, a227, a226, a225, \
    a224, a223, a222, a221, a220, a219, a218, a217, \
    a216, a215, a214, a213, a212, a211, a210, a209, \
    a208, a207, a206, a205, a204, a203, a202, a201, \
    a200, a199, a198, a197, a196, a195, a194, a193, \
    a192, a191, a190, a189, a188, a187, a186, a185, \
    a184, a183, a182, a181, a180, a179, a178, a177, \
    a176, a175, a174, a173, a172, a171, a170, a169, \
    a168, a167, a166, a165, a164, a163, a162, a161, \
    a160, a159, a158, a157, a156, a155, a154, a153, \
    a152, a151, a150, a149, a148, a147, a146, a145, \
    a144, a143, a142, a141, a140, a139, a138, a137, \
    a136, a135, a134, a133, a132, a131, a130, a129, \
    a128, a127, a126, a125, a124, a123, a122, a121, \
    a120, a119, a118, a117, a116, a115, a114, a113, \
    a112, a111, a110, a109, a108, a107, a106, a105, \
    a104, a103, a102, a101, a100, a099, a098, a097, \
    a096, a095, a094, a093, a092, a091, a090, a089, \
    a088, a087, a086, a085, a084, a083, a082, a081, \
    a080, a079, a078, a077, a076, a075, a074, a073, \
    a072, a071, a070, a069, a068, a067, a066, a065, \
    a064, a063, a062, a061, a060, a059, a058, a057, \
    a056, a055, a054, a053, a052, a051, a050, a049, \
    a048, a047, a046, a045, a044, a043, a042, a041, \
    a040, a039, a038, a037, a036, a035, a034, a033, \
    a032, a031, a030, a029, a028, a027, a026, a025, \
    a024, a023, a022, a021, a020, a019, a018, a017, \
    a016, a015, a014, a013, a012, a011, a010, a009, \
    a008, a007, a006, a005, a004, a003, a002, a001, \
    count, ...)                                     \
  count

// clang-format on

// Argument count for using with a C/C++ function or template parameter list.
// The difference from PW_MACRO_ARG_COUNT is that the last argument is not
// counted if it is empty. This makes it easier to drop the final comma when
// expanding to C/C++ code.
#define PW_FUNCTION_ARG_COUNT(...) \
  _PW_FUNCTION_ARG_COUNT(PW_LAST_ARG(__VA_ARGS__), __VA_ARGS__)

#define _PW_FUNCTION_ARG_COUNT(last_arg, ...) \
  _PW_PASTE2(_PW_FUNCTION_ARG_COUNT_, PW_EMPTY_ARGS(last_arg))(__VA_ARGS__)

#define _PW_FUNCTION_ARG_COUNT_0 PW_MACRO_ARG_COUNT
#define _PW_FUNCTION_ARG_COUNT_1(...) \
  PW_MACRO_ARG_COUNT(PW_DROP_LAST_ARG(__VA_ARGS__))

// Evaluates to the last argument in the provided arguments.
#define PW_LAST_ARG(...) \
  _PW_PASTE2(_PW_LAST_ARG_, PW_MACRO_ARG_COUNT(__VA_ARGS__))(__VA_ARGS__)

// Evaluates to the provided arguments, excluding the final argument.
#define PW_DROP_LAST_ARG(...) \
  _PW_PASTE2(_PW_DROP_LAST_ARG_, PW_MACRO_ARG_COUNT(__VA_ARGS__))(__VA_ARGS__)

// Evaluates to the arguments, excluding the final argument if it is empty.
#define PW_DROP_LAST_ARG_IF_EMPTY(...)                                       \
  _PW_IF(                                                                    \
      PW_EMPTY_ARGS(PW_LAST_ARG(__VA_ARGS__)), PW_DROP_LAST_ARG, _PW_EXPAND) \
  (__VA_ARGS__)

// Expands to 1 if one or more arguments are provided, 0 otherwise.
#define PW_HAS_ARGS(...) PW_NOT(PW_EMPTY_ARGS(__VA_ARGS__))

#if PW_VA_OPT_SUPPORTED()

// Expands to 0 if one or more arguments are provided, 1 otherwise.
#define PW_EMPTY_ARGS(...) _PW_EMPTY_ARGS_##__VA_OPT__(0)
#define _PW_EMPTY_ARGS_ 1
#define _PW_EMPTY_ARGS_0 0

#else

// If __VA_OPT__ is not available, use a complicated fallback mechanism. This
// approach is from Jens Gustedt's blog:
//   https://gustedt.wordpress.com/2010/06/08/detect-empty-macro-arguments/
//
// Normally, with a standard-compliant C preprocessor, it's impossible to tell
// whether a variadic macro was called with no arguments or with one argument.
// A macro invoked with no arguments is actually passed one empty argument.
//
// This macro works by checking for the presence of a comma in four situations.
// These situations give the following information about __VA_ARGS__:
//
//   1. It is two or more variadic arguments.
//   2. It expands to one argument surrounded by parentheses.
//   3. It is a function-like macro that produces a comma when invoked.
//   4. It does not interfere with calling a macro when placed between it and
//      parentheses.
//
// If a comma is not present in 1, 2, 3, but is present in 4, then __VA_ARGS__
// is empty. For this case (0001), and only this case, a corresponding macro
// that expands to a comma is defined. The presence of this comma determines
// whether any arguments were passed in.
#define PW_EMPTY_ARGS(...)                                             \
  _PW_HAS_NO_ARGS(_PW_HAS_COMMA(__VA_ARGS__),                          \
                  _PW_HAS_COMMA(_PW_MAKE_COMMA_IF_CALLED __VA_ARGS__), \
                  _PW_HAS_COMMA(__VA_ARGS__()),                        \
                  _PW_HAS_COMMA(_PW_MAKE_COMMA_IF_CALLED __VA_ARGS__()))

// clang-format off
#define _PW_HAS_COMMA(...)                                           \
  _PW_MACRO_ARG_COUNT_IMPL(__VA_ARGS__,                              \
  /*  16 */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*  32 */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*  48 */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*  64 */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*     */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*     */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*     */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /* 128 */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*     */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*     */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*     */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /* 196 */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*     */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*     */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /*     */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, \
  /* 256 */          1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0)
// clang-format on

#define _PW_HAS_NO_ARGS(a1, a2, a3, a4) \
  _PW_HAS_COMMA(_PW_PASTE_RESULTS(a1, a2, a3, a4))
#define _PW_PASTE_RESULTS(a1, a2, a3, a4) _PW_HAS_COMMA_CASE_##a1##a2##a3##a4
#define _PW_HAS_COMMA_CASE_0001 ,
#define _PW_MAKE_COMMA_IF_CALLED(...) ,

#endif  // PW_VA_OPT_SUPPORTED()
