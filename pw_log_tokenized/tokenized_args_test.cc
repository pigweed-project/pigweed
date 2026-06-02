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
#include "pw_log/tokenized_args.h"

#include <cinttypes>
#include <cstring>

#include "pw_log_tokenized/log_tokenized.h"
#include "pw_tokenizer/enum.h"
#include "pw_tokenizer/hash.h"
#include "pw_tokenizer/tokenize.h"
#include "pw_unit_test/framework.h"

namespace this_is_a_test {
namespace {

enum Thing { kAlpha, kBravo, kCharlie };

// Tokenize the enum! Adding a new entry above but not here is a compiler error.
PW_TOKENIZE_ENUM(::this_is_a_test::Thing, kAlpha, kBravo, kCharlie);

enum Thing2 { kDelta, kEcho, kFoxtrot };

// Tokenize the enum with custom strings! Adding a new entry above but not here
// is a compiler error.
PW_TOKENIZE_ENUM_CUSTOM(::this_is_a_test::Thing2,
                        (kDelta, "DELTA"),
                        (kEcho, "ECHO"),
                        (kFoxtrot, "FOXTROT"));

// pw_log backends that use pw_tokenizer and want to support nested tokenization
// define this file under their public_overrides/ directory to activate the
// PW_LOG_TOKEN aliases. If this file does not exist in the log backend,
// arguments behave as basic strings (const char*).
#if __has_include("pw_log_backend/log_backend_uses_pw_tokenizer.h")

TEST(TokenizedArgs, EmptyString_TokenizingBackend) {
  constexpr PW_LOG_TOKEN_TYPE token = PW_LOG_TOKEN("");
  EXPECT_EQ(0u, token);
}

TEST(TokenizedArgs, ExprMatchesStringExpr_TokenizingBackend) {
  EXPECT_EQ(::pw::tokenizer::Hash("[:-)"), PW_LOG_TOKEN_EXPR("[:-)"));
}

TEST(TokenizedArgs, LogTokenFmt_TokenizingBackend) {
  constexpr const char* nested_token = PW_LOG_TOKEN_FMT();
  EXPECT_STREQ("$#%08" PRIx32, nested_token);
}

TEST(TokenizedArgs, LogTokenEnumFmt_TokenizingBackend) {
  constexpr char nested_token[] = PW_LOG_ENUM_FMT(::this_is_a_test::Thing);
  EXPECT_STREQ("${::this_is_a_test::Thing}#%08" PRIx32, nested_token);
}

TEST(TokenizedArgs, LogTokenOrString_TokenizingBackend) {
  EXPECT_EQ(static_cast<uint32_t>(kAlpha), PW_LOG_ENUM(kAlpha));
}

TEST(TokenizedArgs, LogTokenEnumFmt2_TokenizingBackend) {
  constexpr char nested_token[] = PW_LOG_ENUM_FMT(::this_is_a_test::Thing2);
  EXPECT_STREQ("${::this_is_a_test::Thing2}#%08" PRIx32, nested_token);
}

TEST(TokenizedArgs, LogTokenOrString2_TokenizingBackend) {
  EXPECT_EQ(static_cast<uint32_t>(kDelta), PW_LOG_ENUM(kDelta));
}

TEST(TokenizedArgs, NestedTokenFmt1_TokenizingBackend) {
  constexpr char nested_token[] = PW_LOG_NESTED_TOKEN_FMT();
  EXPECT_STREQ("${$#%08" PRIx32 "}#%08" PRIx32, nested_token);
}

TEST(TokenizedArgs, NestedTokenFmt2_TokenizingBackend) {
  constexpr char nested_token[] = PW_LOG_NESTED_TOKEN_FMT("enum_domain");
  EXPECT_STREQ("${${enum_domain}#%08" PRIx32 "}#%08" PRIx32, nested_token);
}

#else

TEST(TokenizedArgs, EmptyString_NonTokenizingBackend) {
  constexpr PW_LOG_TOKEN_TYPE token = PW_LOG_TOKEN("");
  EXPECT_STREQ("", token);
}

TEST(TokenizedArgs, ExprMatchesStringExpr_NonTokenizingBackend) {
  constexpr PW_LOG_TOKEN_TYPE token1 = PW_LOG_TOKEN("[:-)");
  constexpr PW_LOG_TOKEN_TYPE token2 = PW_LOG_TOKEN_EXPR("[:-)");
  EXPECT_STREQ(token1, token2);
}

TEST(TokenizedArgs, LogTokenFmt_NonTokenizingBackend) {
  constexpr PW_LOG_TOKEN_TYPE token = PW_LOG_TOKEN_FMT();
  EXPECT_STREQ("%s", token);
}

TEST(TokenizedArgs, LogTokenEnumFmt_NonTokenizingBackend) {
  constexpr PW_LOG_TOKEN_TYPE nested_token =
      PW_LOG_ENUM_FMT(::this_is_a_test::Thing);
  EXPECT_STREQ("%s", nested_token);
}

TEST(TokenizedArgs, LogTokenOrString_NonTokenizingBackend) {
  constexpr PW_LOG_TOKEN_TYPE nested_token = PW_LOG_ENUM(kAlpha);
  EXPECT_STREQ("kAlpha", nested_token);
}

TEST(TokenizedArgs, LogTokenEnumFmt2_NonTokenizingBackend) {
  constexpr PW_LOG_TOKEN_TYPE nested_token =
      PW_LOG_ENUM_FMT(::this_is_a_test::Thing2);
  EXPECT_STREQ("%s", nested_token);
}

TEST(TokenizedArgs, LogTokenOrString2_NonTokenizingBackend) {
  constexpr PW_LOG_TOKEN_TYPE nested_token = PW_LOG_ENUM(kDelta);
  EXPECT_STREQ("DELTA", nested_token);
}

TEST(TokenizedArgs, NestedTokenFmt1_NonTokenizingBackend) {
  constexpr char nested_token[] = PW_LOG_NESTED_TOKEN_FMT();
  EXPECT_STREQ("%s::%s", nested_token);
}

TEST(TokenizedArgs, NestedTokenFmt2_NonTokenizingBackend) {
  constexpr char nested_token[] =
      PW_LOG_NESTED_TOKEN_FMT(::this_is_a_test::Thing);
  EXPECT_STREQ("%s::%s", nested_token);
}

#endif  //__has_include("log_backend/log_backend_uses_pw_tokenizer.h")

}  // namespace
}  // namespace this_is_a_test
