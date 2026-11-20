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

#include "pw_tokenizer/detokenize_from_this_program.h"

#include <cstdint>
#include <string>

#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_tokenizer/detokenize.h"
#include "pw_tokenizer/tokenize.h"
#include "pw_unit_test/framework.h"

namespace pw::tokenizer {
namespace {

TEST(DetokenizeFromThisProgram, DetokenizesSuccessfully) {
  uint32_t token1 = PW_TOKENIZE_STRING("Token one");
  uint32_t token2 = PW_TOKENIZE_STRING("Token two");

  pw::Result<Detokenizer> detok = GetDetokenizerFromThisProgram();
  PW_TEST_ASSERT_OK(detok);

  EXPECT_EQ(detok->Detokenize(ObjectAsBytes(token1)).BestString(), "Token one");
  EXPECT_EQ(detok->Detokenize(ObjectAsBytes(token2)).BestString(), "Token two");
}

// Returns the string that this program's token database has for a token in
// TEST_DOMAIN, or an empty string if it has none.
std::string DetokenizeInTestDomain(uint32_t token) {
  pw::Result<Detokenizer> detok = GetDetokenizerFromThisProgram();
  if (!detok.ok()) {
    ADD_FAILURE() << "Failed to read the token database of this program";
    return "";
  }
  return detok->Detokenize(ObjectAsBytes(token), "TEST_DOMAIN").BestString();
}

uint32_t DefineTokenAtRunTime() {
  constexpr uint32_t kToken = 0xC0FFEE01u;
  PW_TOKENIZER_DEFINE_TOKEN_ALWAYS(kToken, "TEST_DOMAIN", "At run time");
  return kToken;
}

TEST(DefineTokenAlways, DefinedAtRunTime) {
  EXPECT_EQ(DetokenizeInTestDomain(DefineTokenAtRunTime()), "At run time");
}

// The string of the entry is the name of this function, not of a function that
// the macro defines.
uint32_t TokenForNameOfThisFunction() {
  PW_TOKENIZER_DEFINE_TOKEN_ALWAYS(0xC0FFEE02u, "TEST_DOMAIN", __func__);
  return 0xC0FFEE02u;
}

TEST(DefineTokenAlways, StringIsFunctionName) {
  EXPECT_EQ(DetokenizeInTestDomain(TokenForNameOfThisFunction()),
            "TokenForNameOfThisFunction");
}

// The functions below are only called in constant expressions, so nothing in
// the binary refers to them or their entries. The tokens are in the binary, so
// the entries must still be in the token database.

constexpr uint32_t DefineTokenInConstantExpression() {
  constexpr uint32_t kToken = 0xC0FFEE03u;
  PW_TOKENIZER_DEFINE_TOKEN_ALWAYS(
      kToken, "TEST_DOMAIN", "In a constant expression");
  return kToken;
}

TEST(DefineTokenAlways, DefinedInConstantExpression) {
  constexpr uint32_t kToken = DefineTokenInConstantExpression();
  EXPECT_EQ(DetokenizeInTestDomain(kToken), "In a constant expression");
}

// Each instantiation of a template defines its own entry.
template <uint32_t kToken>
constexpr uint32_t DefineTokenInFunctionTemplate() {
  PW_TOKENIZER_DEFINE_TOKEN_ALWAYS(
      kToken, "TEST_DOMAIN", "In a function template");
  return kToken;
}

TEST(DefineTokenAlways, DefinedForEachInstantiationOfFunctionTemplate) {
  constexpr uint32_t kFirst = DefineTokenInFunctionTemplate<0xC0FFEE04u>();
  constexpr uint32_t kSecond = DefineTokenInFunctionTemplate<0xC0FFEE05u>();
  EXPECT_EQ(DetokenizeInTestDomain(kFirst), "In a function template");
  EXPECT_EQ(DetokenizeInTestDomain(kSecond), "In a function template");
}

constexpr uint32_t DefineSameTokenAgain() {
  constexpr uint32_t kToken = 0xC0FFEE03u;
  PW_TOKENIZER_DEFINE_TOKEN_ALWAYS(
      kToken, "TEST_DOMAIN", "In a constant expression");
  return kToken;
}

TEST(DefineTokenAlways, SameEntryDefinedTwice) {
  constexpr uint32_t kToken = DefineSameTokenAgain();
  EXPECT_EQ(DetokenizeInTestDomain(kToken), "In a constant expression");
}

// GCC cannot compile this use of the macro without class-type NTTPs.
#if _PW_TOKENIZER_CLASS_NTTP_SUPPORTED || defined(__clang__)

struct TokenInStaticDataMember {
  static constexpr uint32_t kToken = [] {
    PW_TOKENIZER_DEFINE_TOKEN_ALWAYS(
        0xC0FFEE06u, "TEST_DOMAIN", "In a static data member");
    return 0xC0FFEE06u;
  }();
};

TEST(DefineTokenAlways, DefinedInLambdaInStaticDataMemberInitializer) {
  EXPECT_EQ(DetokenizeInTestDomain(TokenInStaticDataMember::kToken),
            "In a static data member");
}

#endif  // _PW_TOKENIZER_CLASS_NTTP_SUPPORTED || defined(__clang__)

TEST(DefineTokenAlways, TokenThatIsNotDefinedIsNotInDatabase) {
  EXPECT_EQ(DetokenizeInTestDomain(0xC0FFEEFFu), "");
}

}  // namespace
}  // namespace pw::tokenizer
