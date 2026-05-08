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

#include <__verbose_abort>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "pw_assert/check.h"
#include "pw_string/format.h"
#include "pw_string/string.h"

// Older LLVM distributions don't define this.
#ifndef _LIBCPP_VERBOSE_ABORT_NOEXCEPT
#define _LIBCPP_VERBOSE_ABORT_NOEXCEPT _NOEXCEPT
#endif  // _LIBCPP_VERBOSE_ABORT_NOEXCEPT

// Inline functions in libc++ headers call this.
[[noreturn]] void std::__libcpp_verbose_abort(const char* format, ...)
    _LIBCPP_VERBOSE_ABORT_NOEXCEPT {
  pw::InlineString<128> buffer;
  va_list list;
  va_start(list, format);

  if (pw::string::FormatVaList(buffer, format, list).IsResourceExhausted()) {
    buffer[buffer.size() - 3] = '.';
    buffer[buffer.size() - 2] = '.';
    buffer[buffer.size() - 1] = '.';
  }
  va_end(list);

  PW_CRASH("libc++ abort: %s", buffer.c_str());
}
