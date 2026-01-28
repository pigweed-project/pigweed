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

#include "pw_tokenizer/detokenize.h"

#include <string>
#include <string_view>

#include "pw_stream/memory_stream.h"
#include "pw_tokenizer/base64.h"
#include "pw_tokenizer/example_binary_with_tokenized_strings.h"
#include "pw_unit_test/framework.h"

namespace pw::tokenizer {
namespace {

using namespace std::literals::string_view_literals;

// Use a shorter name for the error string macro.
#define ERR PW_TOKENIZER_ARG_DECODING_ERROR

using Case = std::pair<std::string_view, std::string_view>;

template <typename... Args>
auto TestCases(Args... args) {
  return std::array<Case, sizeof...(Args)>{args...};
}

// Binary format token database with 8 entries.
constexpr char kTestDatabase[] =
    "TOKENS\0\0"
    "\x08\x00\x00\x00"  // Number of tokens in this database.
    "\0\0\0\0"
    "\x01\x00\x00\x00----"
    "\x05\x00\x00\x00----"
    "\xd7\x00\x00\x00----"
    "\xeb\x00\x00\x00----"
    "\xFF\x00\x00\x00----"
    "\xFF\xEE\xEE\xDD----"
    "\xEE\xEE\xEE\xEE----"
    "\x9D\xA7\x97\xF8----"
    "One\0"
    "TWO\0"
    "d7 encodes as 16==\0"
    "$64==+\0"  // recursively decodes to itself with a + after it
    "333\0"
    "FOUR\0"
    "$AQAAAA==\0"
    "■msg♦This is $AQAAAA== message■module♦■file♦file.txt";

constexpr const char kCsvDefaultDomain[] =
    "1,2001-01-01,,Hello World!\n"
    "2,,,\n"
    "3,,, Goodbye!\n";

constexpr const char kCsvDifferentDomains[] =
    "1,          , d o m a i n 1,Hello\n"
    "2,          , dom  ain2,\n"
    "3,          ,\t\t\tdomain   3,World!\n";

constexpr const char kCsvBadDates[] =
    "1,01-01-2001, D1, Hello\n"
    "2,          , D2, \n"
    "3,          , D3, Goodbye!\n";

constexpr const char kCsvBadToken[] =
    ",2001-01-01, D1, Hello\n"
    "2,          , D2, \n"
    "3,          , D3, Goodbye!\n";

constexpr const char kCsvBadFormat[] =
    "1,2001-01-01, D1, Hello\n"
    "2,, \n"
    "3,          , D3, Goodbye!\n";

TEST(DetokenizeFromCsv, DefaultDomain) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(kCsvDefaultDomain);
  PW_TEST_ASSERT_OK(detok_csv);
  EXPECT_EQ(detok_csv->Detokenize("\1\0\0\0"sv).BestString(), "Hello World!");
}

TEST(DetokenizeFromCsv, DifferentDomains_IgnoreWhitespace) {
  pw::Result<Detokenizer> detok_csv =
      Detokenizer::FromCsv(kCsvDifferentDomains);
  PW_TEST_ASSERT_OK(detok_csv);
  auto it = detok_csv->database().begin();
  EXPECT_EQ(it->first, "domain3");
  it++;
  EXPECT_EQ(it->first, "domain2");
  it++;
  EXPECT_EQ(it->first, "domain1");
}

TEST(DetokenizeFromCsv, CountDomains) {
  pw::Result<Detokenizer> detok_csv1 = Detokenizer::FromCsv(kCsvDefaultDomain);
  pw::Result<Detokenizer> detok_csv2 =
      Detokenizer::FromCsv(kCsvDifferentDomains);
  PW_TEST_ASSERT_OK(detok_csv1);
  PW_TEST_ASSERT_OK(detok_csv2);
  EXPECT_EQ(detok_csv1->database().size(), 1u);
  EXPECT_EQ(detok_csv2->database().size(), 3u);
}

TEST(DetokenizeFromCsv, BadCsv_Date) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(kCsvBadDates);
  EXPECT_FALSE(detok_csv.ok());
}

TEST(DetokenizeFromCsv, BadCsv_Token) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(kCsvBadToken);
  EXPECT_FALSE(detok_csv.ok());
}

TEST(DetokenizeFromCsv, BadCsv_Format) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(kCsvBadFormat);
  // Will give warning but continue as expected:
  // WRN  Skipped 1 of 3 lines because they did not have 4 columns as expected.
  EXPECT_TRUE(detok_csv.ok());
}

TEST(DetokenizeFromCsv, WithExplicitDomain) {
  pw::Result<Detokenizer> detok_csv =
      Detokenizer::FromCsv(kCsvDifferentDomains);
  PW_TEST_ASSERT_OK(detok_csv);
  EXPECT_EQ(detok_csv->Detokenize("\1\0\0\0"sv, "domain1").BestString(),
            "Hello");
  EXPECT_EQ(detok_csv->Detokenize("\2\0\0\0"sv, "domain2").BestString(), "");
  EXPECT_EQ(detok_csv->Detokenize("\3\0\0\0"sv, "domain3").BestString(),
            "World!");
}

TEST(DetokenizeFromCsv, DomainIgnoresWhitespace) {
  pw::Result<Detokenizer> detok_csv =
      Detokenizer::FromCsv(kCsvDifferentDomains);
  PW_TEST_ASSERT_OK(detok_csv);
  EXPECT_EQ(detok_csv->Detokenize("\1\0\0\0"sv, "dom\n  ain1").BestString(),
            "Hello");
  EXPECT_EQ(detok_csv->Detokenize("\2\0\0\0"sv, " domain2").BestString(), "");
  EXPECT_EQ(
      detok_csv->Detokenize("\3\0\0\0"sv, " d\toma\r\vin  3\n").BestString(),
      "World!");
}

TEST(DetokenizeFromCsv, DuplicateEntriesAreIgnored) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(
      "1,,,Hello World!\n"
      "2,,,another entry\n"
      "1,,,Hello World!\n"
      "3,,,Goodbye!\n");
  PW_TEST_ASSERT_OK(detok_csv);

  EXPECT_EQ(detok_csv->database().at("").at(1).size(), 1u);
}

TEST(DetokenizeFromCsv, DuplicateEntriesUsesNewestRemovalDate) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(
      "1,2001-01-01,,Hello World!\n"
      "2,,,another entry\n"
      "1,2000-01-01,,Hello World!\n"
      "1,2002-01-01,,Hello World!\n"
      "3,,,Goodbye!\n");
  PW_TEST_ASSERT_OK(detok_csv);

  ASSERT_EQ(detok_csv->database().at("").at(1).size(), 1u);
  const uint32_t date = detok_csv->database().at("").at(1).front().second;
  EXPECT_EQ((date & 0xFFFF0000) >> 16, 2002u);
  EXPECT_EQ((date & 0x0000FF00) >> 8, 1u);
  EXPECT_EQ((date & 0x000000FF) >> 0, 1u);
}

class Detokenize : public ::testing::Test {
 protected:
  Detokenize() : detok_(TokenDatabase::Create<kTestDatabase>()) {}
  Detokenizer detok_;
};

TEST_F(Detokenize, NoFormatting) {
  EXPECT_EQ(detok_.Detokenize("\1\0\0\0"sv).BestString(), "One");
  EXPECT_EQ(detok_.Detokenize("\5\0\0\0"sv).BestString(), "TWO");
  EXPECT_EQ(detok_.Detokenize("\xff\x00\x00\x00"sv).BestString(), "333");
  EXPECT_EQ(detok_.Detokenize("\xff\xee\xee\xdd"sv).BestString(), "FOUR");
}

TEST_F(Detokenize, BestString_MissingToken_IsEmpty) {
  EXPECT_FALSE(detok_.Detokenize("").ok());
  EXPECT_TRUE(detok_.Detokenize("", 0u).BestString().empty());
}

TEST_F(Detokenize, BestString_ShorterToken_ZeroExtended) {
  EXPECT_EQ(detok_.Detokenize("\x42", 1u).token(), 0x42u);
  EXPECT_EQ(detok_.Detokenize("\1\0"sv).token(), 0x1u);
  EXPECT_EQ(detok_.Detokenize("\1\0\3"sv).token(), 0x030001u);
  EXPECT_EQ(detok_.Detokenize("\0\0\0"sv).token(), 0x0u);
}

TEST_F(Detokenize, BestString_UnknownToken_IsEmpty) {
  EXPECT_FALSE(detok_.Detokenize("\0\0\0\0"sv).ok());
  EXPECT_TRUE(detok_.Detokenize("\0\0\0\0"sv).BestString().empty());
  EXPECT_TRUE(detok_.Detokenize("\2\0\0\0"sv).BestString().empty());
  EXPECT_TRUE(detok_.Detokenize("\x10\x32\x54\x76\x99"sv).BestString().empty());
  EXPECT_TRUE(detok_.Detokenize("\x98\xba\xdc\xfe"sv).BestString().empty());
}

TEST_F(Detokenize, BestStringWithErrors_MissingToken_ErrorMessage) {
  EXPECT_FALSE(detok_.Detokenize("").ok());
  EXPECT_EQ(detok_.Detokenize("", 0u).BestStringWithErrors(),
            ERR("missing token"));
}

TEST_F(Detokenize, BestStringWithErrors_ShorterTokenMatchesStrings) {
  EXPECT_EQ(detok_.Detokenize("\1", 1u).BestStringWithErrors(), "One");
  EXPECT_EQ(detok_.Detokenize("\1\0"sv).BestStringWithErrors(), "One");
  EXPECT_EQ(detok_.Detokenize("\1\0\0"sv).BestStringWithErrors(), "One");
}

TEST_F(Detokenize, BestStringWithErrors_UnknownToken_ErrorMessage) {
  ASSERT_FALSE(detok_.Detokenize("\0\0\0\0"sv).ok());
  EXPECT_EQ(detok_.Detokenize("\0"sv).BestStringWithErrors(),
            ERR("unknown token 00000000"));
  EXPECT_EQ(detok_.Detokenize("\0\0\0"sv).BestStringWithErrors(),
            ERR("unknown token 00000000"));
  EXPECT_EQ(detok_.Detokenize("\0\0\0\0"sv).BestStringWithErrors(),
            ERR("unknown token 00000000"));
  EXPECT_EQ(detok_.Detokenize("\2\0\0\0"sv).BestStringWithErrors(),
            ERR("unknown token 00000002"));
  EXPECT_EQ(detok_.Detokenize("\x10\x32\x54\x76\x99"sv).BestStringWithErrors(),
            ERR("unknown token 76543210"));
  EXPECT_EQ(detok_.Detokenize("\x98\xba\xdc\xfe"sv).BestStringWithErrors(),
            ERR("unknown token fedcba98"));
}

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

constexpr TokenDatabase kWithArgs = TokenDatabase::Create<kDataWithArguments>();
class DetokenizeWithArgs : public ::testing::Test {
 protected:
  DetokenizeWithArgs() : detok_(kWithArgs) {}

  Detokenizer detok_;
};

TEST_F(DetokenizeWithArgs, NoMatches) {
  EXPECT_TRUE(detok_.Detokenize("\x23\xab\xc9\x87"sv).matches().empty());
}

TEST_F(DetokenizeWithArgs, SingleMatch) {
  EXPECT_EQ(detok_.Detokenize("\x00\x00\x00\x00"sv).matches().size(), 1u);
}

TEST_F(DetokenizeWithArgs, Empty) {
  EXPECT_EQ(detok_.Detokenize("\x00\x00\x00\x00"sv).BestString(), "");
}

constexpr const char kCsvCollisons[] =
    "1,, D1,crocodile!\n"
    "1,, D1,alligator!\n"
    "2,, D2,See ya later ${D1}#00000001\n";

TEST_F(Detokenize, FromCsvFile_BadCsv_Collisons) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(kCsvCollisons);
  // Will give warning but continue as expected:
  // WRN  Collision with token 1 in domain D1.
  EXPECT_EQ(detok_csv->RecursiveDetokenize("\2\0\0\0"sv, "D2").BestString(),
            "See ya later ${D1}#00000001");
}

constexpr const char kCsvNestedHashedArg[] =
    "1,,,This is a ${}#00000002\n"
    "2,,,nested argument!\n"
    "3,,,Hello\n";

TEST_F(Detokenize, FromCsvFile_NestedHashedArg) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(kCsvNestedHashedArg);
  PW_TEST_ASSERT_OK(detok_csv);
  constexpr const char* expected = "This is a nested argument!";
  EXPECT_EQ(detok_csv->RecursiveDetokenize("\1\0\0\0"sv).BestString(),
            expected);
}

constexpr const char kCsvNestedBase64Arg[] =
    "1,,,base64 argument\n"
    "2,,,This is a $AQAAAA==\n";

TEST_F(Detokenize, FromCsvFile_NestedBase64Arg) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(kCsvNestedBase64Arg);
  PW_TEST_ASSERT_OK(detok_csv);
  EXPECT_EQ(detok_csv->RecursiveDetokenize("\2\0\0\0"sv).BestString(),
            "This is a base64 argument");
}

constexpr const char kCsvDeeplyNestedArg[] =
    "1,,,$10#0000000005\n"
    "2,,,This is a $#00000004\n"
    "3,,,deeply nested argument.\n"
    "4,,,$AQAAAA==\n"
    "5,,,$AwAAAA==\n";

TEST_F(Detokenize, FromCsvFile_DeeplyNestedArg) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(kCsvDeeplyNestedArg);
  PW_TEST_ASSERT_OK(detok_csv);
  EXPECT_EQ(detok_csv->RecursiveDetokenize("\2\0\0\0"sv).BestString(),
            "This is a deeply nested argument.");
}

constexpr const char kCsvNestedTokenOneDomain[] =
    "1,, D1,Hello ${D1}#00000002\n"
    "2,, D1,World!\n"
    "3,, D1, Today is a great day.\n";

TEST_F(Detokenize, FromCsvFile_NestedTokenOneDomain) {
  pw::Result<Detokenizer> detok_csv =
      Detokenizer::FromCsv(kCsvNestedTokenOneDomain);
  // Check the number of domains
  ASSERT_EQ(detok_csv->database().size(), 1u);

  // Check the number of entries in each domain
  for (const auto& [domain, inner_map] : detok_csv->database()) {
    size_t total_entries = 0;
    for (const auto& [token, entries] : inner_map) {
      total_entries += entries.size();
    }
    EXPECT_EQ(total_entries, 3u);  // Expect 6 entries in each domain
  }
  PW_TEST_ASSERT_OK(detok_csv);
  EXPECT_EQ(detok_csv->RecursiveDetokenize("\1\0\0\0"sv, "D1").BestString(),
            "Hello World!");
}

constexpr const char kCsvMultipleNestedTokens[] =
    "1,, D1,nested token 1\n"
    "2,, D1,This is ${D1}10#0000000001 and this is ${D2}16#00000003\n"
    "3,, D2,nested token 2.\n";

TEST_F(Detokenize, FromCsvFile_MultipleNestedTokens) {
  pw::Result<Detokenizer> detok_csv =
      Detokenizer::FromCsv(kCsvMultipleNestedTokens);
  PW_TEST_ASSERT_OK(detok_csv);
  EXPECT_EQ(detok_csv->RecursiveDetokenize("\2\0\0\0"sv, "D1").BestString(),
            "This is nested token 1 and this is nested token 2.");
}

constexpr const char kCsvDoubleNested[] =
    "1,,D1,${$#00000004}#00000002\n"
    "4,,,D2\n"
    "2,,D2,You found me!\n";

TEST_F(Detokenize, FromCsvFile_DoubleNestedArg) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(kCsvDoubleNested);
  PW_TEST_ASSERT_OK(detok_csv);
  EXPECT_EQ(detok_csv->RecursiveDetokenize("\1\0\0\0"sv, "D1").BestString(),
            "You found me!");
}

constexpr const char kCsvDoubleNested_HexOnlyValues[] =
    "1,,D1,${$16#000000FF}16#000000AB\n"
    "FF,,,D2\n"
    "AB,,D2,Hidden Base16 Tokens!\n";

TEST_F(Detokenize, FromCsvFile_DoubleNestedArg_HexOnlyValues) {
  pw::Result<Detokenizer> detok_csv =
      Detokenizer::FromCsv(kCsvDoubleNested_HexOnlyValues);
  PW_TEST_ASSERT_OK(detok_csv);
  EXPECT_EQ(detok_csv->RecursiveDetokenize("\1\0\0\0"sv, "D1").BestString(),
            "Hidden Base16 Tokens!");
}

constexpr const char kCsvDoubleNested_DecOnlyValues[] =
    "1,,D1,${$10#0987654321}10#1234567890\n"
    "3ADE68B1,,,D2\n"
    "499602D2,,D2,Hidden Base10 Tokens!\n";

TEST_F(Detokenize, FromCsvFile_DoubleNestedArg_DecOnlyValues) {
  pw::Result<Detokenizer> detok_csv =
      Detokenizer::FromCsv(kCsvDoubleNested_DecOnlyValues);
  PW_TEST_ASSERT_OK(detok_csv);
  EXPECT_EQ(detok_csv->RecursiveDetokenize("\1\0\0\0"sv, "D1").BestString(),
            "Hidden Base10 Tokens!");
}

constexpr const char kCsvEmptyExpansion[] =
    "1,,D1,This should expand to nothing: $#00000002\n"
    "2,,,\n"
    "2,,D3,Hello World!\n";

TEST_F(Detokenize, FromCsvFile_NestedTokenEmptyExpansion) {
  pw::Result<Detokenizer> detok_csv = Detokenizer::FromCsv(kCsvEmptyExpansion);
  EXPECT_EQ(detok_csv->RecursiveDetokenize("\1\0\0\0"sv, "D1").BestString(),
            "This should expand to nothing: ");
}

constexpr const char kCsvInfiniteRecursion[] =
    "1,,,$#00000002\n"
    "2,,,$#00000001\n"
    "3,,,Hello World!\n"
    "10,,,All good here!\n";

TEST_F(Detokenize, FromCsvFile_NestedTokenInfiniteRecursion) {
  pw::Result<Detokenizer> detok_csv =
      Detokenizer::FromCsv(kCsvInfiniteRecursion);
  EXPECT_EQ(detok_csv->RecursiveDetokenize("\1\0\0\0"sv).BestString(),
            "$#00000002");
}

TEST_F(Detokenize, DetokenizeText_InvalidBase) {
  pw::Result<Detokenizer> detok_csv =
      Detokenizer::FromCsv(kCsvInfiniteRecursion);
  EXPECT_EQ(detok_csv->DetokenizeText("$17#abcd12345"sv), "$17#abcd12345");
  EXPECT_EQ(detok_csv->DetokenizeText("$016#abcdefgh"sv), "$016#abcdefgh");
  EXPECT_EQ(detok_csv->DetokenizeText("$100#00000100"), "$100#00000100");
}

TEST_F(Detokenize, DetokenizeText_ValidTokenAndBase) {
  pw::Result<Detokenizer> detok_csv1 =
      Detokenizer::FromCsv(kCsvInfiniteRecursion);
  EXPECT_EQ(detok_csv1->DetokenizeText("$#00000010"sv), "All good here!");

  pw::Result<Detokenizer> detok_csv2 =
      Detokenizer::FromCsv(kCsvMultipleNestedTokens);
  EXPECT_EQ(detok_csv2->DetokenizeText("${D1}10#0000000002"sv),
            "This is nested token 1 and this is nested token 2.");
}

TEST_F(Detokenize, DetokenizeText_InvalidLengthTokens) {
  pw::Result<Detokenizer> detok_csv =
      Detokenizer::FromCsv(kCsvInfiniteRecursion);
  EXPECT_EQ(detok_csv->DetokenizeText("$#0010"sv), "$#0010");
  EXPECT_EQ(detok_csv->DetokenizeText("$16#1234567890"sv), "$16#1234567890");
  EXPECT_EQ(detok_csv->DetokenizeText("$10#0000010010"), "$10#0000010010");
}

TEST_F(DetokenizeWithArgs, ExtraDataError) {
  auto error = detok_.Detokenize("\x00\x00\x00\x00MORE data"sv);
  EXPECT_FALSE(error.ok());
  EXPECT_EQ("", error.BestString());
}

TEST_F(DetokenizeWithArgs, MissingArgumentError) {
  auto error = detok_.Detokenize("\x0A\x0B\x0C\x0D\5force"sv);
  EXPECT_FALSE(error.ok());
  EXPECT_EQ(error.BestString(), "Use the force, %s.");
  EXPECT_EQ(error.BestStringWithErrors(),
            "Use the force, " ERR("%s MISSING") ".");
}

TEST_F(DetokenizeWithArgs, DecodingError) {
  auto error = detok_.Detokenize("\x0E\x0F\x00\x01\xFF"sv);
  EXPECT_FALSE(error.ok());
  EXPECT_EQ(error.BestString(), "Now there are %d of %s!");
  EXPECT_EQ(error.BestStringWithErrors(),
            "Now there are " ERR("%d ERROR") " of " ERR("%s SKIPPED") "!");
}

constexpr char kDataWithCollisions[] =
    "TOKENS\0\0"
    "\x0F\x00\x00\x00"
    "\0\0\0\0"
    "\x00\x00\x00\x00\xff\xff\xff\xff"  // 1
    "\x00\x00\x00\x00\x01\x02\x03\x04"  // 2
    "\x00\x00\x00\x00\xff\xff\xff\xff"  // 3
    "\x00\x00\x00\x00\xff\xff\xff\xff"  // 4
    "\x00\x00\x00\x00\xff\xff\xff\xff"  // 5
    "\x00\x00\x00\x00\xff\xff\xff\xff"  // 6
    "\x00\x00\x00\x00\xff\xff\xff\xff"  // 7
    "\xAA\xAA\xAA\xAA\x00\x00\x00\x00"  // 8
    "\xAA\xAA\xAA\xAA\xff\xff\xff\xff"  // 9
    "\xBB\xBB\xBB\xBB\xff\xff\xff\xff"  // A
    "\xBB\xBB\xBB\xBB\xff\xff\xff\xff"  // B
    "\xCC\xCC\xCC\xCC\xff\xff\xff\xff"  // C
    "\xCC\xCC\xCC\xCC\xff\xff\xff\xff"  // D
    "\xDD\xDD\xDD\xDD\xff\xff\xff\xff"  // E
    "\xDD\xDD\xDD\xDD\xff\xff\xff\xff"  // F
    // String table
    "This string is present\0"   // 1
    "This string is removed\0"   // 2
    "One arg %d\0"               // 3
    "One arg %s\0"               // 4
    "Two args %s %u\0"           // 5
    "Two args %s %s %% %% %%\0"  // 6
    "Four args %d %d %d %d\0"    // 7
    "This one is removed\0"      // 8
    "This one is present\0"      // 9
    "Two ints %d %d\0"           // A
    "Three ints %d %d %d\0"      // B
    "Three strings %s %s %s\0"   // C
    "Two strings %s %s\0"        // D
    "Three %s %s %s\0"           // E
    "Five %d %d %d %d %s\0";     // F

constexpr TokenDatabase kWithCollisions =
    TokenDatabase::Create<kDataWithCollisions>();

class DetokenizeWithCollisions : public ::testing::Test {
 protected:
  DetokenizeWithCollisions() : detok_(kWithCollisions) {}

  Detokenizer detok_;
};

TEST_F(DetokenizeWithCollisions, Collision_OkIfExactlyOneSuccess) {
  auto result = detok_.Detokenize(
      "\0\0\0\0\x07"
      "1234567"sv);
  ASSERT_EQ(result.matches().size(), 7u);
  ASSERT_EQ(std::count_if(result.matches().begin(),
                          result.matches().end(),
                          [](const auto& item) { return item.ok(); }),
            1);

  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.BestString(), "One arg 1234567");
}

TEST_F(DetokenizeWithCollisions, Collision_NotOkIfMultipleSuccessfulDecodes) {
  auto result = detok_.Detokenize("\0\0\0\0"sv);
  ASSERT_EQ(result.matches().size(), 7u);
  ASSERT_EQ(std::count_if(result.matches().begin(),
                          result.matches().end(),
                          [](const auto& item) { return item.ok(); }),
            2);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.BestString(), "This string is present");
}

TEST_F(DetokenizeWithCollisions, Collision_PreferMostDecodedArgs) {
  auto result = detok_.Detokenize("\xDD\xDD\xDD\xDD\x01\x02\x01\x04\x05"sv);
  EXPECT_EQ((std::string_view)result.matches()[0].value(), "Five -1 1 -1 2 %s");
  EXPECT_EQ((std::string_view)result.matches()[1].value(), "Three \2 \4 %s"sv);
}

TEST_F(DetokenizeWithCollisions, Collision_TracksAllMatches) {
  auto result = detok_.Detokenize("\0\0\0\0"sv);
  EXPECT_EQ(result.matches().size(), 7u);
}

class DetokenizeFromElfSection : public ::testing::Test {
 protected:
  // Offset and size of the .pw_tokenizer.entries section in bytes.
  static constexpr uint32_t kDatabaseOffset = 0x00000174;
  static constexpr size_t kDatabaseSize = 0x000004C2;

  // Parse the test ELF and crash if parsing fails.
  DetokenizeFromElfSection()
      : detok_(Detokenizer::FromElfSection(
                   span(::test::ns::kElfSection)
                       .subspan(kDatabaseOffset, kDatabaseSize))
                   .value()) {}

  Detokenizer detok_;
};

TEST_F(DetokenizeFromElfSection, ReadsContentsCorrectly) {
  // Create a detokenizer from an ELF file with only the pw_tokenizer sections.
  // See py/detokenize_test.py.

  // Two domains exist in the ELF file.
  // The token 881436a0="The answer is: %s" is in two domains.
  EXPECT_EQ(detok_.database().size(), 2u);
  EXPECT_EQ(detok_.database().count(""), 1u);
  EXPECT_EQ(detok_.database().count(""), 1u);

  EXPECT_EQ(detok_.database().at("").size(), 22u);
  EXPECT_EQ(detok_.database().at("TEST_DOMAIN").size(), 5u);
}

TEST_F(DetokenizeFromElfSection, DetokenizesSuccessfully) {
  EXPECT_EQ(detok_.Detokenize("\0").BestString(), "");
  EXPECT_EQ(detok_.Detokenize("\x81\x17\x63\x31\x2").BestString(), "1");
  EXPECT_EQ(detok_.Detokenize("\xd6\x8c\x66\x2e").BestString(),
            "Jello, world!");
}

TEST_F(DetokenizeFromElfSection, DuplicateEntry) {
  // This entry is present several times in the ELF, but should only appear once
  // in the detokenizer's database.
  static constexpr uint32_t kToken = 0x881436a0;
  static constexpr uint8_t kTokenArray[] = {(kToken >> 0) & 0xFF,
                                            (kToken >> 8) & 0x36,
                                            (kToken >> 16) & 0xFF,
                                            (kToken >> 24) & 0xFF,
                                            0x03,
                                            '?',
                                            '?',
                                            '?'};

  EXPECT_EQ(detok_.database().at("").at(kToken).size(), 1u) << "Deduplicates";

  auto result = detok_.Detokenize(kTokenArray);
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.BestString(), "The answer is: ???");
}

TEST(DetokenizeFromElfFile, ReadsDatabaseAndDetokenizesSuccessfully) {
  stream::MemoryReader stream(::test::ns::kElfSection);
  pw::Result<Detokenizer> detok = Detokenizer::FromElfFile(stream);
  PW_TEST_ASSERT_OK(detok);

  EXPECT_EQ(detok->database().size(), 2u);
  EXPECT_EQ(detok->database().count(""), 1u);
  EXPECT_EQ(detok->database().count(""), 1u);

  EXPECT_EQ(detok->database().at("").size(), 22u);
  EXPECT_EQ(detok->database().at("TEST_DOMAIN").size(), 5u);

  EXPECT_EQ(detok->Detokenize("\0").BestString(), "");
  EXPECT_EQ(detok->Detokenize("\x81\x17\x63\x31\x2").BestString(), "1");
  EXPECT_EQ(detok->Detokenize("\xd6\x8c\x66\x2e").BestString(),
            "Jello, world!");
}

}  // namespace
}  // namespace pw::tokenizer
