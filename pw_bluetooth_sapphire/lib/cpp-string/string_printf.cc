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

#include <cpp-string/string_printf.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

namespace bt_lib_cpp_string {
namespace {

void StringVAppendfHelper(std::string* dest, const char* format, va_list ap) {
  // Size of the small stack buffer to use. This should be kept in sync
  // with the numbers in StringPrintfTest.
  constexpr size_t kStackBufferSize = 1024u;

  char stack_buf[kStackBufferSize];
  // |result| is the number of characters that would have been written if
  // kStackBufferSize were sufficiently large, not counting the terminating null
  // character. |vsnprintf()| always null-terminates!
  int result = vsnprintf(stack_buf, kStackBufferSize, format, ap);
  if (result < 0) {
    // As far as I can tell, we'd only get |EOVERFLOW| if the result is so large
    // that it can't be represented by an |int| (in which case retrying would be
    // futile), so Chromium's implementation is wrong.
    return;
  }

  // Only append what fit into our stack buffer.
  // Strings that are too long will be truncated.
  size_t actual_len_excluding_null = static_cast<size_t>(result);
  if (actual_len_excluding_null > kStackBufferSize - 1) {
    actual_len_excluding_null = kStackBufferSize - 1;
  }
  dest->append(stack_buf, actual_len_excluding_null);
}

}  // namespace

std::string StringPrintf(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  std::string rv;
  StringVAppendf(&rv, format, ap);
  va_end(ap);
  return rv;
}

std::string StringVPrintf(const char* format, va_list ap) {
  std::string rv;
  StringVAppendf(&rv, format, ap);
  return rv;
}

void StringAppendf(std::string* dest, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  StringVAppendf(dest, format, ap);
  va_end(ap);
}

void StringVAppendf(std::string* dest, const char* format, va_list ap) {
  int old_errno = errno;
  StringVAppendfHelper(dest, format, ap);
  errno = old_errno;
}

}  // namespace bt_lib_cpp_string
