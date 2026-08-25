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

#ifdef __cplusplus
#include <array>
#include <cstddef>

#include "pw_bytes/span.h"
#include "pw_hex_dump/hex_dump.h"
#include "pw_log/log.h"
#include "pw_log/options.h"
#endif  // __cplusplus

#include "pw_preprocessor/util.h"

#ifdef __cplusplus
namespace pw::dump {

/// @module{pw_hex_dump}

/// Helper to log human-readable hex dumps to console.
///
/// Example:
///
/// @code{.cpp}
///   std::array<const std::byte, 9> my_data = {
///     std::byte('h'),
///     std::byte('e'),
///     std::byte('l'),
///     std::byte('l'),
///     std::byte('o'),
///     std::byte(0xde),
///     std::byte(0xad),
///     std::byte(0xbe),
///     std::byte(0xef),
///   };
///
///   LogBytes(PW_LOG_LEVEL_DEBUG, my_data);
/// @endcode
///
/// @code
///   DBG  0000: 68 65 6c 6c 6f de ad be ef                       hello....
/// @endcode
///
/// To print other data types, obtain a `ConstByteSpan` view first:
///
/// @code{.cpp}
///   LogBytes(PW_LOG_LEVEL_DEBUG, pw::as_bytes(pw::span("world!")));
/// @endcode
///
/// @code
///   DBG  0000: 77 6f 72 6c 64 21 00                             world!.
/// @endcode
///
/// Use template arguments to modify the number of bytes printed per line:
///
/// @code{.cpp}
///   LogBytes<8>(PW_LOG_LEVEL_DEBUG, pw::as_bytes(pw::span("hello world!")));
/// @endcode
///
/// @code
///   DBG  0000: 68 65 6c 6c 6f 20 77 6f  hello wo
///   DBG  0008: 72 6c 64 21 00           rld!.
/// @endcode
///
///
/// @tparam     kBytesPerLine  The number of input bytes to display per line.
/// Defaults to 16.
///
/// @param[in]  log_level      The `PW_LOG_LEVEL` to log at.
///
/// @param[in]  bytes          The data to log.
template <std::size_t kBytesPerLine = 16>
inline void LogBytes(int log_level, pw::ConstByteSpan bytes) {
  // An input byte uses 3 bytes for the hex representation plus 1 for ASCII.
  // 8 bytes go to offset, padding, and string termination.
  const std::size_t kMaxLogLineLength = 8 + 4 * kBytesPerLine;

  if (kBytesPerLine == 0) {
    return;
  }

  std::array<char, kMaxLogLineLength> temp{};
  const auto flags = pw::dump::FormattedHexDumper::Flags{
      .bytes_per_line = kBytesPerLine,
      .group_every = 1,
      .show_ascii = true,
      .show_header = false,
      .prefix_mode = pw::dump::FormattedHexDumper::AddressMode::kOffset};
  pw::dump::FormattedHexDumper hex_dumper(temp, flags);
  if (hex_dumper.BeginDump(bytes).ok()) {
    while (hex_dumper.DumpLine().ok()) {
      PW_LOG(log_level,
             PW_LOG_LEVEL,
             PW_LOG_MODULE_NAME,
             PW_LOG_FLAGS,
             "%s",
             temp.data());
    }
  }
}

}  // namespace pw::dump
#else
#include <stddef.h>
#endif  // __cplusplus

PW_EXTERN_C_START

/// C wrapper for `pw::dump::LogBytes`.
void pw_dump_LogBytes(int log_level, const void* data, size_t length);

PW_EXTERN_C_END
