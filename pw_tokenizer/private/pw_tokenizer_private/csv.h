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

#include <optional>
#include <string>
#include <vector>

namespace pw::tokenizer {
namespace internal {

class CsvParser {
 public:
  CsvParser() : state_(kNewEntry), line_(1u) {}

  [[nodiscard]] std::optional<std::vector<std::string>> ParseCharacter(
      char ch) {
    return ParseCharacterOrEof(ch);
  }

  [[nodiscard]] std::optional<std::vector<std::string>> Flush() {
    return ParseCharacterOrEof(kEndOfFile);
  }

 private:
  static constexpr int kEndOfFile = -1;

  std::optional<std::vector<std::string>> FinishLine();

  std::optional<std::vector<std::string>> ParseCharacterOrEof(int val);

  enum {
    kNewEntry,
    kUnquotedEntry,
    kQuotedEntry,
    kQuotedEntryQuote,
    kError,
  } state_;

  std::vector<std::string> line_;
};

}  // namespace internal

/// Parses a CSV file, calling the provided function for each line.
///
/// Errors are logged and the involved lines are skipped.
template <typename Function>
void ParseCsv(std::string_view csv, Function handle_line) {
  internal::CsvParser parser;
  for (char ch : csv) {
    if (auto line = parser.ParseCharacter(ch); line.has_value()) {
      handle_line(std::move(*line));
    }
  }
  auto line = parser.Flush();
  if (line.has_value()) {
    handle_line(std::move(*line));
  }
}

/// Parses a CSV file. Returns the results as a single nested std::vector of
/// std::string.
[[nodiscard]] inline std::vector<std::vector<std::string>> ParseCsv(
    std::string_view csv) {
  std::vector<std::vector<std::string>> result;
  ParseCsv(csv, [&result](std::vector<std::string>&& line) {
    result.push_back(std::move(line));
  });
  return result;
}

}  // namespace pw::tokenizer
