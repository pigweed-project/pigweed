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

#include "pw_tokenizer/tokenize.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_string/string.h"
#include "pw_tokenizer/hash.h"
#include "pw_tokenizer_private/tokenize_test.h"
#include "pw_unit_test/framework.h"
#include "pw_varint/varint.h"

namespace {

// Example of problematic tokenization in headers and a possible fix.
#define TOKENIZED_LOG(...)                                               \
  do {                                                                   \
    char buffer[32];                                                     \
    size_t size = sizeof(buffer);                                        \
    PW_TOKENIZE_TO_BUFFER_DOMAIN("example", buffer, &size, __VA_ARGS__); \
  } while (0)

#define MACRO_DEFINED_AT_COMMAND_LINE "CORE"

void DoImportantWork() {}

// DOCSTAG: [pw_tokenizer-header-example-1]
// header file
inline void FunctionInHeader() {
  // This log statement varies depending on the translation unit!
  TOKENIZED_LOG("Initializing subsystem: " MACRO_DEFINED_AT_COMMAND_LINE);
  DoImportantWork();
  // ...
}
// DOCSTAG: [pw_tokenizer-header-example-1]

// DOCSTAG: [pw_tokenizer-header-example-3]
// source file
void LogInitializingSubsystem() {
  TOKENIZED_LOG("Initializing subsystem: " MACRO_DEFINED_AT_COMMAND_LINE);
}
// DOCSTAG: [pw_tokenizer-header-example-3]

// DOCSTAG: [pw_tokenizer-header-example-2]
// header file
inline void UpdatedFunctionInHeader() {
  LogInitializingSubsystem();  // Log from the .cc file
  DoImportantWork();
  // ...
}
// DOCSTAG: [pw_tokenizer-header-example-2]

TEST(TokenizationExamples, CallTheFunctions) {
  FunctionInHeader();
  UpdatedFunctionInHeader();
}

// Constructs an array with the hashed string followed by the provided bytes.
template <uint8_t... kData, size_t kSize>
constexpr auto ExpectedData(
    const char (&format)[kSize],
    uint32_t token_mask = std::numeric_limits<uint32_t>::max()) {
  const uint32_t value = pw::tokenizer::Hash(format) & token_mask;
  return std::array<uint8_t, sizeof(uint32_t) + sizeof...(kData)>{
      static_cast<uint8_t>(value & 0xff),
      static_cast<uint8_t>(value >> 8 & 0xff),
      static_cast<uint8_t>(value >> 16 & 0xff),
      static_cast<uint8_t>(value >> 24 & 0xff),
      kData...};
}

TEST(TokenizeString, EmptyString_IsZero) {
  constexpr pw_tokenizer_Token token = PW_TOKENIZE_STRING("");
  EXPECT_EQ(0u, token);
}

TEST(TokenizeString, String_MatchesHash) {
  constexpr uint32_t token = PW_TOKENIZE_STRING("[:-)");
  EXPECT_EQ(pw::tokenizer::Hash("[:-)"), token);
}

TEST(TokenizeString, String_MatchesHashExpr) {
  EXPECT_EQ(pw::tokenizer::Hash("[:-)"), PW_TOKENIZE_STRING_EXPR("[:-)"));
}

TEST(TokenizeString, ExpressionWithStringVariable) {
  constexpr char kTestString[] = "test";
  EXPECT_EQ(pw::tokenizer::Hash(kTestString),
            PW_TOKENIZE_STRING_EXPR(kTestString));
  EXPECT_EQ(pw::tokenizer::Hash(kTestString),
            PW_TOKENIZE_STRING_DOMAIN_EXPR("TEST_DOMAIN", kTestString));
  EXPECT_EQ(
      pw::tokenizer::Hash(kTestString) & 0xAAAAAAAA,
      PW_TOKENIZE_STRING_MASK_EXPR("TEST_DOMAIN", 0xAAAAAAAA, kTestString));
}

constexpr uint32_t kGlobalToken = PW_TOKENIZE_STRING(">:-[]");

TEST(TokenizeString, GlobalVariable_MatchesHash) {
  EXPECT_EQ(pw::tokenizer::Hash(">:-[]"), kGlobalToken);
}

struct TokenizedWithinClass {
  static constexpr uint32_t kThisToken = PW_TOKENIZE_STRING("???");
};

static_assert(pw::tokenizer::Hash("???") == TokenizedWithinClass::kThisToken);

TEST(TokenizeString, ClassMember_MatchesHash) {
  EXPECT_EQ(pw::tokenizer::Hash("???"), TokenizedWithinClass().kThisToken);
}

TEST(TokenizeString, WithinNonCapturingLambda) {
  uint32_t non_capturing_lambda = [] {
    return PW_TOKENIZE_STRING("Lambda!");
  }();

  EXPECT_EQ(pw::tokenizer::Hash("Lambda!"), non_capturing_lambda);
}

TEST(TokenizeString, WithinCapturingLambda) {
  bool executed_lambda = false;
  uint32_t capturing_lambda = [&executed_lambda] {
    if (executed_lambda) {
      return PW_TOKENIZE_STRING("Never should be returned!");
    }
    executed_lambda = true;
    return PW_TOKENIZE_STRING("Capturing lambda!");
  }();

  ASSERT_TRUE(executed_lambda);
  EXPECT_EQ(pw::tokenizer::Hash("Capturing lambda!"), capturing_lambda);
}

TEST(TokenizeString, Mask) {
  [[maybe_unused]] constexpr uint32_t token = PW_TOKENIZE_STRING("(O_o)");
  [[maybe_unused]] constexpr uint32_t masked_1 =
      PW_TOKENIZE_STRING_MASK("domain", 0xAAAAAAAA, "(O_o)");
  [[maybe_unused]] constexpr uint32_t masked_2 =
      PW_TOKENIZE_STRING_MASK("domain", 0x55555555, "(O_o)");
  [[maybe_unused]] constexpr uint32_t masked_3 =
      PW_TOKENIZE_STRING_MASK("domain", 0xFFFF0000, "(O_o)");

  static_assert(token != masked_1 && token != masked_2 && token != masked_3);
  static_assert(masked_1 != masked_2 && masked_2 != masked_3);
  static_assert((token & 0xAAAAAAAA) == masked_1);
  static_assert((token & 0x55555555) == masked_2);
  static_assert((token & 0xFFFF0000) == masked_3);
}

TEST(TokenizeString, MaskExpr) {
  uint32_t token = PW_TOKENIZE_STRING("(O_o)");
  uint32_t masked_1 =
      PW_TOKENIZE_STRING_MASK_EXPR("domain", 0xAAAAAAAA, "(O_o)");
  uint32_t masked_2 =
      PW_TOKENIZE_STRING_MASK_EXPR("domain", 0x55555555, "(O_o)");
  uint32_t masked_3 =
      PW_TOKENIZE_STRING_MASK_EXPR("domain", 0xFFFF0000, "(O_o)");

  EXPECT_TRUE(token != masked_1 && token != masked_2 && token != masked_3);
  EXPECT_TRUE(masked_1 != masked_2 && masked_2 != masked_3);
  EXPECT_TRUE((token & 0xAAAAAAAA) == masked_1);
  EXPECT_TRUE((token & 0x55555555) == masked_2);
  EXPECT_TRUE((token & 0xFFFF0000) == masked_3);
}

// Use a function with a shorter name to test tokenizing __func__ and
// __PRETTY_FUNCTION__.
//
// WARNING: This function might cause errors for compilers other than GCC and
// clang. It relies on two GCC/clang extensions:
//
//   1 - The __PRETTY_FUNCTION__ C++ function name variable.
//   2 - __func__ as a static constexpr array instead of static const. See
//       https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66639 for background.
//
void TestName() {
  constexpr uint32_t function_hash = PW_TOKENIZE_STRING(__func__);
  EXPECT_EQ(pw::tokenizer::Hash(__func__), function_hash);

  // Check the non-standard __PRETTY_FUNCTION__ name.
  constexpr uint32_t pretty_function = PW_TOKENIZE_STRING(__PRETTY_FUNCTION__);
  EXPECT_EQ(pw::tokenizer::Hash(__PRETTY_FUNCTION__), pretty_function);
}

TEST(TokenizeString, FunctionName) { TestName(); }

TEST(TokenizeString, Array) {
  constexpr char array[] = "won-won-won-wonderful";

  const uint32_t array_hash = PW_TOKENIZE_STRING(array);
  EXPECT_EQ(pw::tokenizer::Hash(array), array_hash);
}

TEST(TokenizeString, NullInString) {
  // Use PW_TOKENIZER_STRING_TOKEN to avoid emitting strings with NUL into the
  // ELF file. The CSV database format does not support NUL.
  constexpr char nulls[32] = {};
  static_assert(pw::tokenizer::Hash(nulls) == PW_TOKENIZER_STRING_TOKEN(nulls));
  static_assert(PW_TOKENIZER_STRING_TOKEN(nulls) != 0u);

  static_assert(PW_TOKENIZER_STRING_TOKEN("\0") == pw::tokenizer::Hash("\0"));
  static_assert(PW_TOKENIZER_STRING_TOKEN("\0") != pw::tokenizer::Hash(""));

  static_assert(PW_TOKENIZER_STRING_TOKEN("abc\0def") ==
                pw::tokenizer::Hash("abc\0def"));

  static_assert(pw::tokenizer::Hash("abc\0def") !=
                pw::tokenizer::Hash("abc\0def\0"));
}

// Verify that we can tokenize multiple strings from one source line.
#define THREE_FOR_ONE(first, second, third)             \
  [[maybe_unused]] constexpr uint32_t token_1 =         \
      PW_TOKENIZE_STRING_DOMAIN("TEST_DOMAIN", first);  \
  [[maybe_unused]] constexpr uint32_t token_2 =         \
      PW_TOKENIZE_STRING_DOMAIN("TEST_DOMAIN", second); \
  [[maybe_unused]] constexpr uint32_t token_3 =         \
      PW_TOKENIZE_STRING_DOMAIN("TEST_DOMAIN", third);

TEST(TokenizeString, MultipleTokenizationsInOneMacroExpansion) {
  // This verifies that we can safely tokenize multiple times in a single macro
  // expansion. This can be useful when for example a name and description are
  // both tokenized after being passed into a macro.
  //
  // This test only verifies that this compiles correctly; it does not test
  // that the tokenizations make it to the final token database.
  THREE_FOR_ONE("hello", "yes", "something");
}

// Verify that we can tokenize multiple strings from one source line.
#define THREE_FOR_ONE_EXPR(first, second, third)             \
  [[maybe_unused]] uint32_t token_1 =                        \
      PW_TOKENIZE_STRING_DOMAIN_EXPR("TEST_DOMAIN", first);  \
  [[maybe_unused]] uint32_t token_2 =                        \
      PW_TOKENIZE_STRING_DOMAIN_EXPR("TEST_DOMAIN", second); \
  [[maybe_unused]] uint32_t token_3 =                        \
      PW_TOKENIZE_STRING_DOMAIN_EXPR("TEST_DOMAIN", third);

TEST(TokenizeString, MultipleTokenizationsInOneMacroExpansionExpr) {
  // This verifies that we can safely tokenize multiple times in a single macro
  // expansion. This can be useful when for example a name and description are
  // both tokenized after being passed into a macro.
  //
  // This test only verifies that this compiles correctly; it does not test
  // that the tokenizations make it to the final token database.
  THREE_FOR_ONE_EXPR("hello", "yes", "something");
}

class TokenizeToBuffer : public ::testing::Test {
 public:
  TokenizeToBuffer() : buffer_{} {}

 protected:
  uint8_t buffer_[64];
};

TEST_F(TokenizeToBuffer, Integer64) {
  size_t message_size = 14;
  PW_TOKENIZE_TO_BUFFER(
      buffer_,
      &message_size,
      "%" PRIu64,
      static_cast<uint64_t>(0x55555555'55555555ull));  // 0xAAAAAAAA'AAAAAAAA

  // Pattern becomes 10101010'11010101'10101010 ...
  constexpr std::array<uint8_t, 14> expected =
      ExpectedData<0xAA, 0xD5, 0xAA, 0xD5, 0xAA, 0xD5, 0xAA, 0xD5, 0xAA, 0x01>(
          "%" PRIu64);
  ASSERT_EQ(expected.size(), message_size);
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

TEST_F(TokenizeToBuffer, Integer64Overflow) {
  size_t message_size;

  for (size_t size = 4; size < 20; ++size) {
    message_size = size;

    PW_TOKENIZE_TO_BUFFER(
        buffer_,
        &message_size,
        "%" PRIx64,
        static_cast<uint64_t>(std::numeric_limits<int64_t>::min()));

    if (size < 14) {
      constexpr std::array<uint8_t, 4> empty = ExpectedData("%" PRIx64);
      ASSERT_EQ(sizeof(uint32_t), message_size);
      EXPECT_EQ(std::memcmp(empty.data(), &buffer_, empty.size()), 0);

      // Make sure nothing was written past the end of the buffer.
      EXPECT_TRUE(std::all_of(&buffer_[size], std::end(buffer_), [](uint8_t v) {
        return v == '\0';
      }));
    } else {
      constexpr std::array<uint8_t, 14> expected =
          ExpectedData<0xff,
                       0xff,
                       0xff,
                       0xff,
                       0xff,
                       0xff,
                       0xff,
                       0xff,
                       0xff,
                       0x01>("%" PRIx64);
      ASSERT_EQ(expected.size(), message_size);
      EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
    }
  }
}

TEST_F(TokenizeToBuffer, IntegerNegative) {
  size_t message_size = 9;
  PW_TOKENIZE_TO_BUFFER(
      buffer_, &message_size, "%" PRId32, std::numeric_limits<int32_t>::min());

  // 0x8000'0000 -zig-zag-> 0xff'ff'ff'ff'0f
  constexpr std::array<uint8_t, 9> expected =
      ExpectedData<0xff, 0xff, 0xff, 0xff, 0x0f>("%" PRId32);
  ASSERT_EQ(expected.size(), message_size);
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

TEST_F(TokenizeToBuffer, IntegerMin) {
  size_t message_size = 9;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "%d", -1);

  constexpr std::array<uint8_t, 5> expected = ExpectedData<0x01>("%d");
  ASSERT_EQ(expected.size(), message_size);
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

TEST_F(TokenizeToBuffer, IntegerDoesntFit) {
  size_t message_size = 8;
  PW_TOKENIZE_TO_BUFFER(
      buffer_, &message_size, "%" PRId32, std::numeric_limits<int32_t>::min());

  constexpr std::array<uint8_t, 4> expected = ExpectedData<>("%" PRId32);
  ASSERT_EQ(expected.size(), message_size);
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

TEST_F(TokenizeToBuffer, String) {
  size_t message_size = sizeof(buffer_);

  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "The answer is: %s", "5432!");
  constexpr std::array<uint8_t, 10> expected =
      ExpectedData<5, '5', '4', '3', '2', '!'>("The answer is: %s");

  ASSERT_EQ(expected.size(), message_size);
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

TEST_F(TokenizeToBuffer, String_BufferTooSmall_TruncatesAndSetsTopStatusBit) {
  size_t message_size = 8;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "The answer is: %s", "5432!");

  constexpr std::array<uint8_t, 8> truncated_1 =
      ExpectedData<0x83, '5', '4', '3'>("The answer is: %s");

  ASSERT_EQ(truncated_1.size(), message_size);
  EXPECT_EQ(std::memcmp(truncated_1.data(), buffer_, truncated_1.size()), 0);
}

TEST_F(TokenizeToBuffer, String_TwoBytesLeft_TruncatesToOneCharacter) {
  size_t message_size = 6;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "The answer is: %s", "5432!");

  constexpr std::array<uint8_t, 6> truncated_2 =
      ExpectedData<0x81, '5'>("The answer is: %s");

  ASSERT_EQ(truncated_2.size(), message_size);
  EXPECT_EQ(std::memcmp(truncated_2.data(), buffer_, truncated_2.size()), 0);
}

TEST_F(TokenizeToBuffer, String_OneByteLeft_OnlyWritesTruncatedStatusByte) {
  size_t message_size = 5;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "The answer is: %s", "5432!");

  std::array<uint8_t, 5> result = ExpectedData<0x80>("The answer is: %s");
  ASSERT_EQ(result.size(), message_size);
  EXPECT_EQ(std::memcmp(result.data(), buffer_, result.size()), 0);
}

TEST_F(TokenizeToBuffer, EmptyString_OneByteLeft_EncodesCorrectly) {
  size_t message_size = 5;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "The answer is: %s", "");

  std::array<uint8_t, 5> result = ExpectedData<0>("The answer is: %s");
  ASSERT_EQ(result.size(), message_size);
  EXPECT_EQ(std::memcmp(result.data(), buffer_, result.size()), 0);
}

TEST_F(TokenizeToBuffer, String_ZeroBytesLeft_WritesNothing) {
  size_t message_size = 4;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "The answer is: %s", "5432!");

  constexpr std::array<uint8_t, 4> empty = ExpectedData<>("The answer is: %s");
  ASSERT_EQ(empty.size(), message_size);
  EXPECT_EQ(std::memcmp(empty.data(), buffer_, empty.size()), 0);
}

TEST_F(TokenizeToBuffer, Array) {
  static constexpr char array[] = "1234";
  size_t message_size = 4;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, array);

  constexpr std::array<uint8_t, 4> result = ExpectedData<>("1234");
  ASSERT_EQ(result.size(), message_size);
  EXPECT_EQ(std::memcmp(result.data(), buffer_, result.size()), 0);
}

TEST_F(TokenizeToBuffer, NullptrString_EncodesNull) {
  char* string = nullptr;
  size_t message_size = 9;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "The answer is: %s", string);

  std::array<uint8_t, 9> result =
      ExpectedData<4, 'N', 'U', 'L', 'L'>("The answer is: %s");
  ASSERT_EQ(result.size(), message_size);
  EXPECT_EQ(std::memcmp(result.data(), buffer_, result.size()), 0);
}

TEST_F(TokenizeToBuffer, NullptrString_BufferTooSmall_EncodesTruncatedNull) {
  char* string = nullptr;
  size_t message_size = 6;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "The answer is: %s", string);

  std::array<uint8_t, 6> result = ExpectedData<0x81, 'N'>("The answer is: %s");
  ASSERT_EQ(result.size(), message_size);
  EXPECT_EQ(std::memcmp(result.data(), buffer_, result.size()), 0);
}

TEST_F(TokenizeToBuffer, Domain_String) {
  size_t message_size = sizeof(buffer_);

  PW_TOKENIZE_TO_BUFFER_DOMAIN(
      "TEST_DOMAIN", buffer_, &message_size, "The answer was: %s", "5432!");
  constexpr std::array<uint8_t, 10> expected =
      ExpectedData<5, '5', '4', '3', '2', '!'>("The answer was: %s");

  ASSERT_EQ(expected.size(), message_size);
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

TEST_F(TokenizeToBuffer, Mask) {
  size_t message_size = sizeof(buffer_);

  PW_TOKENIZE_TO_BUFFER_MASK("TEST_DOMAIN",
                             0x0000FFFF,
                             buffer_,
                             &message_size,
                             "The answer was: %s",
                             "5432!");
  constexpr std::array<uint8_t, 10> expected =
      ExpectedData<5, '5', '4', '3', '2', '!'>("The answer was: %s",
                                               0x0000FFFF);

  ASSERT_EQ(expected.size(), message_size);
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

TEST_F(TokenizeToBuffer, TruncateArgs) {
  // Args that can't fit are dropped completely
  size_t message_size = 6;
  PW_TOKENIZE_TO_BUFFER(buffer_,
                        &message_size,
                        "%u %d",
                        static_cast<uint8_t>(0b0010'1010u),
                        0xffffff);

  constexpr std::array<uint8_t, 5> expected =
      ExpectedData<0b0101'0100u>("%u %d");
  ASSERT_EQ(expected.size(), message_size);
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

TEST_F(TokenizeToBuffer, NoRoomForToken) {
  // Nothing is written if there isn't room for the token.
  std::memset(buffer_, '$', sizeof(buffer_));
  auto is_untouched = [](uint8_t v) { return v == '$'; };

  size_t message_size = 3;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "The answer: \"%s\"", "5432!");
  EXPECT_EQ(0u, message_size);
  EXPECT_TRUE(std::all_of(buffer_, std::end(buffer_), is_untouched));

  message_size = 2;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "Jello, world!");
  EXPECT_EQ(0u, message_size);
  EXPECT_TRUE(std::all_of(buffer_, std::end(buffer_), is_untouched));

  message_size = 1;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "Jello!");
  EXPECT_EQ(0u, message_size);
  EXPECT_TRUE(std::all_of(buffer_, std::end(buffer_), is_untouched));

  message_size = 0;
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "Jello?");
  EXPECT_EQ(0u, message_size);
  EXPECT_TRUE(std::all_of(buffer_, std::end(buffer_), is_untouched));
}

TEST_F(TokenizeToBuffer, CharArray) {
  size_t message_size = sizeof(buffer_);
  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, __func__);
  constexpr auto expected = ExpectedData(__func__);
  ASSERT_EQ(expected.size(), message_size);
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

TEST_F(TokenizeToBuffer, C_StringShortFloat) {
  size_t size = sizeof(buffer_);
  pw_tokenizer_ToBufferTest_StringShortFloat(buffer_, &size);
  constexpr std::array<uint8_t, 11> expected =  // clang-format off
      ExpectedData<1, '1',                 // string '1'
                   3,                      // -2 (zig-zag encoded)
                   0x00, 0x00, 0x40, 0x40  // 3.0 in floating point
                   >(TEST_FORMAT_STRING_SHORT_FLOAT);
  ASSERT_EQ(expected.size(), size);  // clang-format on
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

TEST_F(TokenizeToBuffer, C_SequentialZigZag) {
  size_t size = sizeof(buffer_);
  pw_tokenizer_ToBufferTest_SequentialZigZag(buffer_, &size);
  constexpr std::array<uint8_t, 18> expected =
      ExpectedData<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13>(
          TEST_FORMAT_SEQUENTIAL_ZIG_ZAG);

  ASSERT_EQ(expected.size(), size);
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

TEST_F(TokenizeToBuffer, C_Overflow) {
  std::memset(buffer_, '$', sizeof(buffer_));

  {
    size_t size = 7;
    pw_tokenizer_ToBufferTest_Requires8(buffer_, &size);
    constexpr std::array<uint8_t, 7> expected =
        ExpectedData<2, 'h', 'i'>(TEST_FORMAT_REQUIRES_8);
    ASSERT_EQ(expected.size(), size);
    EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
    EXPECT_EQ(buffer_[7], '$');
  }

  {
    size_t size = 8;
    pw_tokenizer_ToBufferTest_Requires8(buffer_, &size);
    constexpr std::array<uint8_t, 8> expected =
        ExpectedData<2, 'h', 'i', 13>(TEST_FORMAT_REQUIRES_8);
    ASSERT_EQ(expected.size(), size);
    EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
    EXPECT_EQ(buffer_[8], '$');
  }
}

// TODO: b/408040194 - Remove this when supported.
[[maybe_unused]] void TokenizeForbidPercentDotStarS() {
#if PW_NC_TEST(TokenizeForbidPercentDotStarS)
  PW_NC_EXPECT("The %.*s specifier is not supported.");

  PW_TOKENIZE_FORMAT_STRING("mydomain", 0, "Hello %.*s", 6, "friend");
#endif  // PW_NC_TEST
}

TEST_F(TokenizeToBuffer, LengthDelimitedString) {
  size_t message_size = sizeof(buffer_);

  // DOCSTAG[pw_tokenizer-length-delimited-example]
  // `greeting` is the first 5 characters only ("Hello")
  std::string_view greeting("Hello world", 5);
  PW_TOKENIZE_TO_BUFFER(buffer_,
                        &message_size,
                        "%s, how are you doing?",
                        pw::InlineString<5>(greeting).c_str());
  // Detokenizes to "Hello, how are you doing?"
  // DOCSTAG[pw_tokenizer-length-delimited-example]

  EXPECT_EQ(message_size, sizeof(uint32_t) + 1 + greeting.size());
  EXPECT_STREQ(reinterpret_cast<const char*>(&buffer_[5]), "Hello");
}

#define MACRO_THAT_CALLS_ANOTHER_MACRO(action) ANOTHER_MACRO(action)

#define ANOTHER_MACRO(action) action

TEST_F(TokenizeToBuffer, AsArgumentToAnotherMacro) {
  size_t message_size = sizeof(buffer_);
  MACRO_THAT_CALLS_ANOTHER_MACRO(
      PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, __func__));
  constexpr auto expected = ExpectedData(__func__);
  ASSERT_EQ(expected.size(), message_size);
  EXPECT_EQ(std::memcmp(expected.data(), buffer_, expected.size()), 0);
}

#undef MACRO_THAT_CALLS_ANOTHER_MACRO
#undef ANOTHER_MACRO

// Hijack an internal macro to capture the tokenizer domain.
#undef PW_TOKENIZER_DEFINE_TOKEN
#define PW_TOKENIZER_DEFINE_TOKEN(token, domain, string) \
  tokenizer_domain = domain;                             \
  string_literal = string

TEST_F(TokenizeToBuffer, Domain_Default) {
  const char* tokenizer_domain = nullptr;
  const char* string_literal = nullptr;

  size_t message_size = sizeof(buffer_);

  PW_TOKENIZE_TO_BUFFER(buffer_, &message_size, "The answer is: %s", "5432!");

  EXPECT_STREQ(tokenizer_domain, PW_TOKENIZER_DEFAULT_DOMAIN);
  EXPECT_STREQ(string_literal, "The answer is: %s");
}

TEST_F(TokenizeToBuffer, Domain_Specified) {
  const char* tokenizer_domain = nullptr;
  const char* string_literal = nullptr;

  size_t message_size = sizeof(buffer_);

  PW_TOKENIZE_TO_BUFFER_DOMAIN(
      "._.", buffer_, &message_size, "The answer is: %s", "5432!");

  EXPECT_STREQ(tokenizer_domain, "._.");
  EXPECT_STREQ(string_literal, "The answer is: %s");
}

#undef PW_TOKENIZER_DEFINE_TOKEN
#define PW_TOKENIZER_DEFINE_TOKEN(token, domain, string)                   \
  static_assert(false,                                                     \
                "The internal PW_TOKENIZER_DEFINE_TOKEN was "              \
                "repurposed earlier in this test! The macro or any macro " \
                "that calls it cannot be used here!")

}  // namespace
