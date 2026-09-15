// Copyright 2025 The Pigweed Authors
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

#include <array>
#include <string_view>

#include "pw_assert/check.h"
#include "pw_bytes/array.h"
#include "pw_perf_test/perf_test.h"
#include "pw_span/span.h"
#include "pw_tokenizer/detokenize.h"
#include "pw_tokenizer/internal/decode.h"

namespace pw::tokenizer {
namespace {

constexpr char kDataWithArguments[] =
    "TOKENS\0\0"
    "\x09\x00\x00\x00"
    "\0\0\0\0"
    "\x00\x00\x00\x00----"
    "\x0A\x0B\x0C\x0D----"
    "\x0E\x0F\x00\x01----"
    "\xAA\xAA\xAA\xAA----"
    "\xBB\xBB\xBB\xBB----"
    "\xCC\xCC\xCC\xCC----"
    "\xDD\xDD\xDD\xDD----"
    "\xEE\xEE\xEE\xEE----"
    "\xFF\xFF\xFF\xFF----"
    "\0"
    "Use the %s, %s.\0"
    "Now there are %d of %s!\0"
    "%c!\0"    // AA
    "%hhu!\0"  // BB
    "%hu!\0"   // CC
    "%u!\0"    // DD
    "%lu!\0"   // EE
    "%llu!";   // FF
constexpr TokenDatabase kDatabase = TokenDatabase::Create<kDataWithArguments>();

void Detokenize(perf_test::State& state,
                span<const std::byte> data,
                std::string_view expected) {
  Detokenizer detokenizer(kDatabase);

  std::string result = detokenizer.Detokenize(data).BestString();

  while (state.KeepRunning()) {
    result = detokenizer.Detokenize(data).BestString();
  }

  PW_CHECK(result == expected);
}

PW_PERF_TEST(Detokenize_NoMessage,
             Detokenize,
             bytes::Array<1, 2, 3, 4, 5, 6>(),
             "");

PW_PERF_TEST(Detokenize_NoArgs, Detokenize, bytes::Array<0, 0, 0, 0>(), "");

PW_PERF_TEST(Detokenize_OneArg,
             Detokenize,
             bytes::String("\xAA\xAA\xAA\xAA\xfc\x01"),
             "~!");

PW_PERF_TEST(Detokenize_TwoArgs1,
             Detokenize,
             bytes::String("\x0E\x0F\x00\x01\x04\x04them"),
             "Now there are 2 of them!");

PW_PERF_TEST(Detokenize_TwoArgs2,
             Detokenize,
             bytes::String("\x0E\x0F\x00\x01\x80\x01\x04them"),
             "Now there are 64 of them!");

void DetokenizeText(perf_test::State& state,
                    std::string_view text,
                    std::string_view expected) {
  Detokenizer detokenizer(kDatabase);

  std::string result = detokenizer.DetokenizeText(text);

  while (state.KeepRunning()) {
    result = detokenizer.DetokenizeText(text);
  }

  PW_CHECK(result == expected);
}

PW_PERF_TEST(DetokenizeText_NoMessage,
             DetokenizeText,
             "Nothing!!",
             "Nothing!!");

PW_PERF_TEST(DetokenizeText_NoArgs, DetokenizeText, "$AAAAAA==", "");

PW_PERF_TEST(DetokenizeText_OneArg, DetokenizeText, "$qqqqqvwB", "~!");

PW_PERF_TEST(DetokenizeText_TwoArgs1,
             DetokenizeText,
             "$Dg8AAQQEdGhlbQ==",
             "Now there are 2 of them!");

PW_PERF_TEST(DetokenizeText_TwoArgs2,
             DetokenizeText,
             "$Dg8AAYABBHRoZW0=",
             "Now there are 64 of them!");

PW_PERF_TEST(DetokenizeText_TwoMessages,
             DetokenizeText,
             "What the $qqqqqvwB, $Dg8AAQQEdGhlbQ==",
             "What the ~!, Now there are 2 of them!");

void FormatStringFormat(perf_test::State& state,
                        const char* fmt,
                        std::string_view args_str) {
  FormatString format_string(fmt);
  span<const uint8_t> args(reinterpret_cast<const uint8_t*>(args_str.data()),
                           args_str.size());

  while (state.KeepRunning()) {
    volatile auto result = format_string.Format(args);
  }
}

PW_PERF_TEST(FormatStringFormat_NoArgs, FormatStringFormat, "Hello", "");
PW_PERF_TEST(FormatStringFormat_OneArg,
             FormatStringFormat,
             "Hello %s",
             "\5hello");
PW_PERF_TEST(FormatStringFormat_TwoArgs,
             FormatStringFormat,
             "The %d %s",
             "\6\x0amusketeer");
PW_PERF_TEST(FormatStringFormat_FourArgs,
             FormatStringFormat,
             "A %d B %d C %d D %d",
             "\2\4\6\x08");
PW_PERF_TEST(FormatStringFormat_EightArgs,
             FormatStringFormat,
             "%d %d %d %d %d %d %d %d",
             "\2\4\6\x08\x0a\x0c\x0e\x10");

}  // namespace
}  // namespace pw::tokenizer
