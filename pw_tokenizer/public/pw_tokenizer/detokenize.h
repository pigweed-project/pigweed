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

// This file provides the Detokenizer class, which is used to decode tokenized
// strings.  To use a Detokenizer, load a binary format token database into
// memory, construct a TokenDatabase, and pass it to a Detokenizer:
//
//   std::vector data = ReadFile("my_tokenized_strings.db");
//   Detokenizer detok(TokenDatabase::Create(data));
//
//   DetokenizedString result = detok.Detokenize(my_data);
//   std::cout << result.BestString() << '\n';
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_stream/stream.h"
#include "pw_tokenizer/internal/decode.h"
#include "pw_tokenizer/token_database.h"
#include "pw_tokenizer/tokenize.h"

namespace pw::tokenizer {

/// @submodule{pw_tokenizer,detokenize}

class Detokenizer;

/// Token database entry.
using TokenizedStringEntry = std::pair<FormatString, uint32_t /*date removed*/>;
using DomainTokenEntriesMap = std::unordered_map<
    std::string,
    std::unordered_map<uint32_t, std::vector<TokenizedStringEntry>>>;

/// A string that has been detokenized. This class tracks all possible results
/// if there are token collisions.
class DetokenizedString {
 public:
  DetokenizedString(const Detokenizer& detokenizer,
                    bool recursion,
                    uint32_t token,
                    const span<const TokenizedStringEntry>& entries,
                    const span<const std::byte>& arguments);

  DetokenizedString() : has_token_(false), ok_(false) {}

  /// True the message decoded successfully and unambiguously.
  bool ok() const { return ok_; }

  /// Returns the strings that matched the token, with the best matches first.
  const std::vector<DecodedFormatString>& matches() const { return matches_; }

  const uint32_t& token() const { return token_; }

  /// Returns the detokenized string or an empty string if there were no
  /// matches. If there are multiple possible results, the `DetokenizedString`
  /// returns the first match.
  const std::string& BestString() const { return best_string_; }

  /// Returns the best match, with error messages inserted for arguments that
  /// failed to parse.
  std::string BestStringWithErrors() const;

 private:
  uint32_t token_;
  std::string best_string_;
  bool has_token_;
  bool ok_;
  std::vector<DecodedFormatString> matches_;
};

/// Decodes and detokenizes from a token database. This class builds a hash
/// table of tokens to give `O(1)` token lookups.
class Detokenizer {
 public:
  /// Constructs a detokenizer from a `TokenDatabase`. The `TokenDatabase` is
  /// not referenced by the `Detokenizer` after construction; its memory can be
  /// freed.
  explicit Detokenizer(const TokenDatabase& database);

  /// Constructs a detokenizer by directly passing the parsed database.
  explicit Detokenizer(DomainTokenEntriesMap&& database)
      : database_(std::move(database)) {}

  /// Constructs a detokenizer from the `.pw_tokenizer.entries` section of an
  /// ELF binary.
  static Result<Detokenizer> FromElfSection(span<const std::byte> elf_section);

  /// Overload of `FromElfSection` for a `uint8_t` span.
  static Result<Detokenizer> FromElfSection(span<const uint8_t> elf_section) {
    return FromElfSection(as_bytes(elf_section));
  }

  /// Constructs a detokenizer from the `.pw_tokenizer.entries` section of an
  /// ELF binary.
  static Result<Detokenizer> FromElfFile(stream::SeekableReader& stream);

  /// Constructs a detokenizer from a CSV database.
  static Result<Detokenizer> FromCsv(std::string_view csv);

  /// Decodes and detokenizes the binary encoded message. Returns a
  /// `DetokenizedString` that stores all possible detokenized string results.
  DetokenizedString Detokenize(const span<const std::byte>& encoded,
                               std::string_view domain = kDefaultDomain) const {
    return Detokenize(encoded, domain, false);
  }

  /// Overload of `Detokenize` for `span<const uint8_t>`.
  DetokenizedString Detokenize(const span<const uint8_t>& encoded,
                               std::string_view domain = kDefaultDomain) const {
    return Detokenize(as_bytes(encoded), domain);
  }

  /// Overload of `Detokenize` for `std::string_view`.
  DetokenizedString Detokenize(std::string_view encoded,
                               std::string_view domain = kDefaultDomain) const {
    return Detokenize(encoded.data(), encoded.size(), domain);
  }

  /// Overload of `Detokenize` for a pointer and length.
  DetokenizedString Detokenize(const void* encoded,
                               size_t size_bytes,
                               std::string_view domain = kDefaultDomain) const {
    return Detokenize(span(static_cast<const std::byte*>(encoded), size_bytes),
                      domain);
  }

  /// Decodes and detokenizes the binary encoded message. Returns a
  /// `DetokenizedString` that stores all possible detokenized string results.
  DetokenizedString RecursiveDetokenize(
      const span<const std::byte>& encoded,
      std::string_view domain = kDefaultDomain) const {
    return Detokenize(encoded, domain, true);
  }

  /// Overload of `Detokenize` for `span<const uint8_t>`.
  DetokenizedString RecursiveDetokenize(
      const span<const uint8_t>& encoded,
      std::string_view domain = kDefaultDomain) const {
    return RecursiveDetokenize(as_bytes(encoded), domain);
  }

  /// Overload of `Detokenize` for `std::string_view`.
  DetokenizedString RecursiveDetokenize(
      std::string_view encoded,
      std::string_view domain = kDefaultDomain) const {
    return RecursiveDetokenize(encoded.data(), encoded.size(), domain);
  }

  /// Overload of `Detokenize` for a pointer and length.
  DetokenizedString RecursiveDetokenize(
      const void* encoded,
      size_t size_bytes,
      std::string_view domain = kDefaultDomain) const {
    return RecursiveDetokenize(
        span(static_cast<const std::byte*>(encoded), size_bytes), domain);
  }

  /// Decodes and detokenizes a Base64-encoded message. Returns a
  /// `DetokenizedString` that stores all possible detokenized string results.
  DetokenizedString DetokenizeBase64Message(std::string_view text) const;

  /// Decodes and detokenizes nested tokenized messages in a string.
  ///
  /// This function currently only supports Base64 nested tokenized messages.
  /// Support for hexadecimal-encoded string literals will be added.
  ///
  /// @param[in] text Text potentially containing tokenized messages.
  ///
  /// @returns The original string with nested tokenized messages decoded in
  ///     context. Messages that fail to decode are left as-is.
  std::string DetokenizeText(std::string_view text) const {
    return DetokenizeTextRecursive(text, kMaxDecodePasses);
  }

  /// Decodes data that may or may not be tokenized, such as proto fields marked
  /// as optionally tokenized.
  ///
  /// This function currently only supports Base64 nested tokenized messages.
  /// Support for hexadecimal-encoded string literals will be added.
  ///
  /// This function currently assumes when data is not tokenized it is printable
  /// ASCII. Otherwise, the returned string will be base64-encoded.
  ///
  /// @param[in] optionally_tokenized_data Data optionally tokenized.
  ///
  /// @returns The decoded text if successfully detokenized or if the data is
  /// printable, otherwise returns the data base64-encoded.
  std::string DecodeOptionallyTokenizedData(
      span<const std::byte> optionally_tokenized_data) const;

  const DomainTokenEntriesMap& database() const { return database_; }

  span<const TokenizedStringEntry> DatabaseLookup(
      Token token, std::string_view domain) const;

 private:
  // 4 passes supports detokenizing two layers of nested messages with tokenized
  // domains (e.g. ${${bar}#ab12cd34}#00000012), without allowing a hypothetical
  // detokenization cycle to continue for too long.
  static constexpr unsigned kMaxDecodePasses = 4;

  std::string DetokenizeTextRecursive(std::string_view text,
                                      unsigned max_passes) const;

  /// Decodes and detokenizes the binary encoded message. Returns a
  /// `DetokenizedString` that stores all possible detokenized string results.
  DetokenizedString Detokenize(const span<const std::byte>& encoded,
                               std::string_view domain,
                               bool recursion) const;

  DomainTokenEntriesMap database_;
};

/// @}

}  // namespace pw::tokenizer
