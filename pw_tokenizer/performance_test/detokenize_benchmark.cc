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

// Standalone C++ detokenizer benchmark paired with `detokenize_benchmark.rs`.
//
// This benchmark uses the same token database and test cases as
// `//pw_tokenizer:detokenize_perf_test`
// (`pw_tokenizer/detokenize_perf_test.cc`), but runs 1,000-iteration
// batch-timed loops with explicit input/output optimization barriers instead of
// `pw_perf_test` (which is C++-only and times each iteration individually) so
// that C++ and Rust execution times and binary sizes can be compared directly.

#define PW_LOG_MODULE_NAME "PERF_CC"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "pw_log/log.h"
#include "pw_span/span.h"
#include "pw_tokenizer/detokenize.h"
#include "pw_tokenizer/internal/decode.h"

namespace pw::tokenizer {
namespace {

uint64_t GetTimeNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

uint64_t GetTimeUs() { return GetTimeNs() / 1000; }

// Number of iterations to run each individual benchmark case per session.
constexpr uint32_t kIterations = 1000;

// Token database identical to `kDataWithArguments` in
// `pw_tokenizer/detokenize_perf_test.cc`.
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

struct BinaryTestCase {
  const char* name;
  const char* data;
  size_t size;
  const char* expected;
};

constexpr char kNoMessageData[] = "\x01\x02\x03\x04\x05\x06";
constexpr char kNoArgsData[] = "\x00\x00\x00\x00";
constexpr char kOneArgData[] = "\xAA\xAA\xAA\xAA\xfc\x01";
constexpr char kTwoArgs1Data[] = "\x0E\x0F\x00\x01\x04\x04them";
constexpr char kTwoArgs2Data[] = "\x0E\x0F\x00\x01\x80\x01\x04them";

constexpr BinaryTestCase kBinaryCases[] = {
    {"Detokenize_NoMessage", kNoMessageData, sizeof(kNoMessageData) - 1, ""},
    {"Detokenize_NoArgs", kNoArgsData, sizeof(kNoArgsData) - 1, ""},
    {"Detokenize_OneArg", kOneArgData, sizeof(kOneArgData) - 1, "~!"},
    {"Detokenize_TwoArgs1",
     kTwoArgs1Data,
     sizeof(kTwoArgs1Data) - 1,
     "Now there are 2 of them!"},
    {"Detokenize_TwoArgs2",
     kTwoArgs2Data,
     sizeof(kTwoArgs2Data) - 1,
     "Now there are 64 of them!"},
};

struct TextTestCase {
  const char* name;
  std::string_view text;
  const char* expected;
};

constexpr TextTestCase kTextCases[] = {
    {"DetokenizeText_NoMessage", "Nothing!!", "Nothing!!"},
    {"DetokenizeText_NoArgs", "$AAAAAA==", ""},
    {"DetokenizeText_OneArg", "$qqqqqvwB", "~!"},
    {"DetokenizeText_TwoArgs1",
     "$Dg8AAQQEdGhlbQ==",
     "Now there are 2 of them!"},
    {"DetokenizeText_TwoArgs2",
     "$Dg8AAYABBHRoZW0=",
     "Now there are 64 of them!"},
    {"DetokenizeText_TwoMessages",
     "What the $qqqqqvwB, $Dg8AAQQEdGhlbQ==",
     "What the ~!, Now there are 2 of them!"},
};

struct FormatStringTestCase {
  const char* name;
  const char* fmt;
  std::string_view args_str;
};

constexpr FormatStringTestCase kFormatCases[] = {
    {"FormatStringFormat_NoArgs", "Hello", ""},
    {"FormatStringFormat_OneArg", "Hello %s", "\5hello"},
    {"FormatStringFormat_TwoArgs", "The %d %s", "\6\x0amusketeer"},
    {"FormatStringFormat_FourArgs",
     "A %d B %d C %d D %d",
     std::string_view("\2\4\6\x08", 4)},
    {"FormatStringFormat_EightArgs",
     "%d %d %d %d %d %d %d %d",
     std::string_view("\2\4\6\x08\x0a\x0c\x0e\x10", 8)},
};

// Prevents the compiler from constant-propagating or hoisting `var` across
// benchmark loop iterations while keeping it in general-purpose registers.
#define DO_NOT_OPTIMIZE_INPUT(var) asm volatile("" : "+r"(var))

// Forces the compiler to materialize the result pointed to by `ptr` in memory
// on each benchmark loop iteration instead of dead-store eliminating it.
#define DO_NOT_OPTIMIZE_OUTPUT(ptr) asm volatile("" : : "r"(ptr) : "memory")

bool RunPerfSession(Detokenizer& detokenizer, uint32_t session_num) {
  PW_LOG_INFO(
      "=== Starting C++ Detokenizer Perf Test Session #%u (%u iterations/case) "
      "===",
      static_cast<unsigned>(session_num),
      static_cast<unsigned>(kIterations));

  bool all_passed = true;
  uint64_t session_start_us = GetTimeUs();

  for (const auto& tc : kBinaryCases) {
    uint64_t start_ns = GetTimeNs();
    std::string last_result;
    const std::byte* data_ptr = reinterpret_cast<const std::byte*>(tc.data);
    for (uint32_t i = 0; i < kIterations; ++i) {
      DO_NOT_OPTIMIZE_INPUT(data_ptr);
      span<const std::byte> data(data_ptr, tc.size);
      last_result = detokenizer.Detokenize(data).BestString();
      DO_NOT_OPTIMIZE_OUTPUT(last_result.data());
    }
    uint64_t elapsed_ns = GetTimeNs() - start_ns;
    uint64_t elapsed_us = elapsed_ns / 1000;
    if (last_result != tc.expected) {
      PW_LOG_ERROR("FAIL %s: expected '%s', got '%s'",
                   tc.name,
                   tc.expected,
                   last_result.c_str());
      all_passed = false;
    }
    uint64_t ns_per_iter = elapsed_ns / kIterations;
    PW_LOG_INFO("[PERF] %s: %u iters in %llu us (%llu ns/iter)",
                tc.name,
                static_cast<unsigned>(kIterations),
                static_cast<unsigned long long>(elapsed_us),
                static_cast<unsigned long long>(ns_per_iter));
  }

  for (const auto& tc : kTextCases) {
    uint64_t start_ns = GetTimeNs();
    std::string last_result;
    const char* text_ptr = tc.text.data();
    size_t text_len = tc.text.size();
    for (uint32_t i = 0; i < kIterations; ++i) {
      DO_NOT_OPTIMIZE_INPUT(text_ptr);
      std::string_view text(text_ptr, text_len);
      last_result = detokenizer.DetokenizeText(text);
      DO_NOT_OPTIMIZE_OUTPUT(last_result.data());
    }
    uint64_t elapsed_ns = GetTimeNs() - start_ns;
    uint64_t elapsed_us = elapsed_ns / 1000;
    if (last_result != tc.expected) {
      PW_LOG_ERROR("FAIL %s: expected '%s', got '%s'",
                   tc.name,
                   tc.expected,
                   last_result.c_str());
      all_passed = false;
    }
    uint64_t ns_per_iter = elapsed_ns / kIterations;
    PW_LOG_INFO("[PERF] %s: %u iters in %llu us (%llu ns/iter)",
                tc.name,
                static_cast<unsigned>(kIterations),
                static_cast<unsigned long long>(elapsed_us),
                static_cast<unsigned long long>(ns_per_iter));
  }

  uint64_t session_elapsed_us = GetTimeUs() - session_start_us;
  size_t total_cases = (sizeof(kBinaryCases) / sizeof(kBinaryCases[0])) +
                       (sizeof(kTextCases) / sizeof(kTextCases[0]));
  PW_LOG_INFO(
      "=== Session #%u Complete: %u cases (%u total detokenizations) in %llu "
      "us ===",
      static_cast<unsigned>(session_num),
      static_cast<unsigned>(total_cases),
      static_cast<unsigned>(total_cases * kIterations),
      static_cast<unsigned long long>(session_elapsed_us));

  // Also benchmark low-level FormatString::Format argument decoding without
  // token database lookup (matching //pw_tokenizer:detokenize_perf_test).
  for (const auto& tc : kFormatCases) {
    FormatString format_string(tc.fmt);
    const uint8_t* args_ptr =
        reinterpret_cast<const uint8_t*>(tc.args_str.data());
    size_t args_len = tc.args_str.size();
    uint64_t start_ns = GetTimeNs();
    for (uint32_t i = 0; i < kIterations; ++i) {
      DO_NOT_OPTIMIZE_INPUT(args_ptr);
      span<const uint8_t> args(args_ptr, args_len);
      auto result = format_string.Format(args);
      DO_NOT_OPTIMIZE_OUTPUT(&result);
    }
    uint64_t elapsed_ns = GetTimeNs() - start_ns;
    uint64_t elapsed_us = elapsed_ns / 1000;
    uint64_t ns_per_iter = elapsed_ns / kIterations;
    PW_LOG_INFO("[PERF-FMT] %s: %u iters in %llu us (%llu ns/iter)",
                tc.name,
                static_cast<unsigned>(kIterations),
                static_cast<unsigned long long>(elapsed_us),
                static_cast<unsigned long long>(ns_per_iter));
  }

  return all_passed;
}

#undef DO_NOT_OPTIMIZE_INPUT
#undef DO_NOT_OPTIMIZE_OUTPUT

}  // namespace
}  // namespace pw::tokenizer

int main() {
  PW_LOG_INFO("Initializing C++ detokenizer from binary database...");
  uint64_t init_start_us = pw::tokenizer::GetTimeUs();
  pw::tokenizer::Detokenizer detokenizer(pw::tokenizer::kDatabase);
  uint64_t init_elapsed_us = pw::tokenizer::GetTimeUs() - init_start_us;
  PW_LOG_INFO("Detokenizer initialized in %llu us",
              static_cast<unsigned long long>(init_elapsed_us));

  if (!pw::tokenizer::RunPerfSession(detokenizer, 1)) {
    return 1;
  }
  return 0;
}
