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
#pragma once

// A private implementation of PW_STRINGIFY to avoid depending on
// pw_preprocessor (which is for C and C++ code).
#define __PW_MUST_PLACE_STRINGIFY(x) #x
#define _PW_MUST_PLACE_STRINGIFY(x) __PW_MUST_PLACE_STRINGIFY(x)

/// @module{pw_build}

// clang-format off
/// @cond PRIVATE
#define ___PW_MUST_PLACE(isection, start_sym, end_sym)                                       \
    start_sym = .;                                                                           \
    isection                                                                                 \
    end_sym = .;                                                                             \
    ASSERT(start_sym != end_sym,                                                             \
           "Error: PW_MUST_PLACE did not find required input section(s) matching pattern:"); \
    ASSERT(start_sym != end_sym, #isection);                                                 \
    ASSERT(start_sym != end_sym, "at file, line:");                                          \
    ASSERT(start_sym != end_sym, __FILE__);                                                  \
    ASSERT(start_sym != end_sym, _PW_MUST_PLACE_STRINGIFY(__LINE__));                        \
    ASSERT(start_sym != end_sym, "")

#define __PW_MUST_PLACE(isection, sym_prefix, unique)   \
    ___PW_MUST_PLACE(isection, sym_prefix ## start_ ## unique, sym_prefix ## end_ ## unique)

#define _PW_MUST_PLACE(isection, sym_prefix, unique)    \
    __PW_MUST_PLACE(isection, sym_prefix, unique)
/// @endcond

/// PW_MUST_PLACE is a macro intended for use in linker scripts to ensure inputs
/// are non-zero sized.
///
/// Say you want to place a specific object file into a particular section. You
/// can reference it by file path like:
///
/// @code
/// SECTIONS
/// {
///   .special_code
///   {
///     */src/path/libspecial_code.a:*.o
///   }
/// }
/// @endcode
///
/// This works but is fragile as it will silently break if the filename or path
/// changes. Use PW_MUST_PLACE to get a linker assertion if the input is empty.
///
/// @code
/// #include "pw_build/must_place.ld.h"
///
/// SECTIONS
/// {
///   .special_code
///   {
///     PW_MUST_PLACE(*/src/path/libspecial_code.a:*.o)
///   }
/// }
/// @endcode
///
/// If the wildcard match fails PW_MUST_PLACE will generate an error telling you
/// which input had no symbols.
///
/// @code
///   Error: No symbols found in pattern below
///   */src/path/libspecial_code.a:*.o
/// @endcode
///
/// This could be because you had a typo, the path changed, or the symbols were
/// dropped due to linker section garbage collection.In the latter case,
/// you can choose to add ``KEEP()`` around your input to prevent garbage
/// collection.
#define PW_MUST_PLACE(isection)  \
    _PW_MUST_PLACE(isection, __section_place_, __COUNTER__)

// clang-format off
/// @cond PRIVATE
#define ___PW_MUST_PLACE_SIZE(isection, isize, start_sym, end_sym)                    \
    start_sym = .;                                                                    \
    isection                                                                          \
    end_sym = .;                                                                      \
    ASSERT(end_sym - start_sym == isize ,                                             \
           "Error: PW_MUST_PLACE_SIZE found input section(s) with unexpected size:"); \
    ASSERT(end_sym - start_sym == isize, #isection);                                  \
    ASSERT(end_sym - start_sym == isize, "at file, line:");                           \
    ASSERT(end_sym - start_sym == isize, __FILE__);                                   \
    ASSERT(end_sym - start_sym == isize, _PW_MUST_PLACE_STRINGIFY(__LINE__));         \
    ASSERT(end_sym - start_sym == isize, "")

#define __PW_MUST_PLACE_SIZE(isection, isize, sym_prefix, unique)   \
    ___PW_MUST_PLACE_SIZE(isection, isize, sym_prefix ## start_ ## unique, sym_prefix ## end_ ## unique)

#define _PW_MUST_PLACE_SIZE(isection, isize, sym_prefix, unique)    \
    __PW_MUST_PLACE_SIZE(isection, isize, sym_prefix, unique)
/// @endcond

/// PW_MUST_PLACE_SIZE is a macro intended for use in linker scripts to ensure
/// inputs are an expected size.
///
/// This is helpful for shared memory placements between multiple cores.
///
/// Imagine the following code is consumed while linking for 2 separate cores:
///
/// @code
/// SECTIONS
/// {
///   .shared_memory
///   {
///     */src/path/libspecial_code.a:shared_memory.o(.bss.shared_memory)
///   }
/// }
/// @endcode
///
/// This works but is fragile as it will silently break if the size of
/// .bss.shared_memory changes on a single core. This may occur if compiler or
/// other parameters are changed on one core and not the other.
///
/// @code
/// #include "pw_build/must_place.ld.h"
///
/// SECTIONS
/// {
///   .special_code
///   {
///     PW_MUST_PLACE_SIZE(*/src/path/libspecial_code.a:shared_memory.o(.bss.shared_memory), 16)
///   }
/// }
/// @endcode
///
/// If the wildcard match results in a different size from what was specified
/// PW_MUST_PLACE_SIZE will generate an error.
///
/// @code
///   Error: Pattern did not match expected size
///   */src/path/libspecial_code.a:shared_memory.o(.bss.shared_memory)
/// @endcode
///
/// This could be because you had a typo, the path changed, the symbols changed
/// in size (either because the symbol was changed or compilation settings were
/// changed), symbols were dropped due to linker section garbage collection. In
/// the latter case, you can choose to add ``KEEP()`` around your input to
/// prevent garbage collection.
#define PW_MUST_PLACE_SIZE(isection, isize)  \
    _PW_MUST_PLACE_SIZE(isection, isize, __section_place_, __COUNTER__)

// clang-format off
/// @cond PRIVATE
#define ___PW_MUST_NOT_PLACE(isection, start_sym, end_sym)                                  \
    start_sym = .;                                                                          \
    isection                                                                                \
    end_sym = .;                                                                            \
    ASSERT(start_sym == end_sym,                                                            \
           "Error: PW_MUST_NOT_PLACE found unexpected input section(s) matching pattern:"); \
    ASSERT(start_sym == end_sym, #isection);                                                \
    ASSERT(start_sym == end_sym, "at file, line:");                                         \
    ASSERT(start_sym == end_sym, __FILE__);                                                 \
    ASSERT(start_sym == end_sym, _PW_MUST_PLACE_STRINGIFY(__LINE__));                       \
    ASSERT(start_sym == end_sym, "")

#define __PW_MUST_NOT_PLACE(isection, sym_prefix, unique)   \
    ___PW_MUST_NOT_PLACE(isection, sym_prefix ## start_ ## unique, sym_prefix ## end_ ## unique)

#define _PW_MUST_NOT_PLACE(isection, sym_prefix, unique)    \
    __PW_MUST_NOT_PLACE(isection, sym_prefix, unique)
/// @endcond

/// PW_MUST_NOT_PLACE is a macro intended for use in linker scripts to ensure inputs
/// are _not_ present. It does the opposite of PW_MUST_PLACE.
///
/// This can be used to assert that no data members are added to an object file where
/// there should be none. This is useful to ensure the safety of code that must run
/// before data or bss init.
///
/// @code
/// SECTIONS
/// {
///   .zero_init_ram_early_init
///   {
///      PW_MUST_NOT_PLACE(*/libearly_memory_init.a:*.o(.bss*));
///   }
/// }
/// @endcode
///
/// When adding a new variable to a library marked with this macro it is expected to
/// change to PW_MUST_PLACE
#define PW_MUST_NOT_PLACE(isection)  \
    _PW_MUST_NOT_PLACE(isection, __section_not_place_, __COUNTER__)

/// @endmodule
