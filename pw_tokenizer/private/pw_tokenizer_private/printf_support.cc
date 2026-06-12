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

#include "pw_tokenizer_private/printf_support.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

#include "pw_tokenizer/detokenize.h"

namespace pw::tokenizer::test {
namespace {

const bool kSupportsC99Printf = [] {
  char buf[16] = {};
  std::snprintf(buf, sizeof(buf), "%zu", sizeof(char));
  return buf[0] == '1' && buf[1] == '\0';
}();

const bool kSupportsFloatPrintf = [] {
  char buf[16] = {};
  std::snprintf(buf, sizeof(buf), "%.1f", 3.5f);
  return buf[0] == '3' && buf[1] == '.' && buf[2] == '5' && buf[3] == '\0';
}();

bool FormatIsSupported(std::string_view format_view) {
  if constexpr (sizeof(void*) == 8) {
    return true;  // Assume 64-bit machines support all format strings
  }
  pw::tokenizer::FormatString format_string(std::string(format_view).c_str());
  for (const auto& segment : format_string.segments()) {
    if (segment.type() == pw::tokenizer::StringSegment::kLiteral ||
        segment.type() == pw::tokenizer::StringSegment::kPercent) {
      continue;
    }
    if (!kSupportsC99Printf) {
      std::string_view text = segment.text();
      for (const char* modifier : {"hh", "ll", "j", "z", "t"}) {
        if (text.find(modifier) != std::string_view::npos) {
          return false;
        }
      }
    }
    if (!kSupportsFloatPrintf &&
        segment.type() == pw::tokenizer::StringSegment::kFloatingPoint) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool FormatIsSupported(const pw::tokenizer::Detokenizer& detok,
                       std::string_view data) {
  uint32_t token = 0;
  for (size_t i = 0; i < std::min(data.size(), sizeof(token)); ++i) {
    token |= static_cast<uint32_t>(static_cast<unsigned char>(data[i]))
             << (i * 8);
  }
  const auto entries =
      detok.DatabaseLookup(token, pw::tokenizer::kDefaultDomain);
  if (entries.empty()) {
    return true;
  }
  return FormatIsSupported(entries[0].first.text());
}

}  // namespace pw::tokenizer::test
