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
#include <type_traits>

#include "pw_tokenizer/enum.h"
#include "pw_tokenizer/hash.h"
#include "pw_tokenizer/tokenize.h"
#include "pw_unit_test/constexpr.h"
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

enum class ScopedThing { kItem1, kItem2 };

PW_TOKENIZE_ENUM(::this_is_a_test::ScopedThing, kItem1, kItem2);

}  // namespace
}  // namespace this_is_a_test

template <>
[[maybe_unused]] constexpr uint32_t
pw::tokenizer::PwEnumDomainToken<::this_is_a_test::ScopedThing>() {
  return 99999u;
}

namespace this_is_a_test {
namespace {

// This test depends on :tokenized_backend_indicator, as the pw_log_tokenized
// backends do, so pw_log/tokenized_args.h provides its tokenizing macros. The
// non-tokenizing macros are tested in pw_log.

TEST(TokenizedArgs, EmptyString_TokenizingBackend) {
  constexpr PW_LOG_TOKEN_TYPE token = PW_LOG_TOKEN("");
  EXPECT_EQ(0u, token);
}

TEST(TokenizedArgs, ExprMatchesStringExpr_TokenizingBackend) {
  EXPECT_EQ(::pw::tokenizer::Hash("[:-)"), PW_LOG_TOKEN_EXPR("[:-)"));
}

PW_CONSTEXPR_TEST(TokenizedArgs, DefaultToken_TokenizingBackend, {
  static_assert(std::is_same_v<pw::log::Token, PW_LOG_TOKEN_TYPE>);
  static_assert(std::is_same_v<pw::log::Token, pw_tokenizer_Token>);
  PW_TEST_EXPECT_EQ(0u, PW_LOG_TOKEN_DEFAULT_VALUE);
  PW_TEST_EXPECT_EQ(0u, pw::log::kDefaultToken);
});

PW_CONSTEXPR_TEST(TokenizedArgs, LogTokenFmt_TokenizingBackend, {
  constexpr const char* nested_token = PW_LOG_TOKEN_FMT();
  PW_TEST_EXPECT_STREQ("$#%08" PRIx32, nested_token);
});

PW_CONSTEXPR_TEST(TokenizedArgs, LogTokenEnumFmt_TokenizingBackend, {
  constexpr char nested_token[] = PW_LOG_ENUM_FMT(::this_is_a_test::Thing);
  PW_TEST_EXPECT_STREQ("${::this_is_a_test::Thing}#%08" PRIx32, nested_token);
});

PW_CONSTEXPR_TEST(TokenizedArgs, LogTokenOrString_TokenizingBackend, {
  PW_TEST_EXPECT_EQ(static_cast<uint32_t>(kAlpha), PW_LOG_ENUM(kAlpha));
});

PW_CONSTEXPR_TEST(TokenizedArgs, LogTokenEnumFmt2_TokenizingBackend, {
  constexpr char nested_token[] = PW_LOG_ENUM_FMT(::this_is_a_test::Thing2);
  PW_TEST_EXPECT_STREQ("${::this_is_a_test::Thing2}#%08" PRIx32, nested_token);
});

PW_CONSTEXPR_TEST(TokenizedArgs, LogTokenOrString2_TokenizingBackend, {
  PW_TEST_EXPECT_EQ(static_cast<uint32_t>(kDelta), PW_LOG_ENUM(kDelta));
});

PW_CONSTEXPR_TEST(TokenizedArgs, NestedTokenFmt1_TokenizingBackend, {
  constexpr char nested_token[] = PW_LOG_NESTED_TOKEN_FMT();
  PW_TEST_EXPECT_STREQ("${$#%08" PRIx32 "}#%08" PRIx32, nested_token);
});

PW_CONSTEXPR_TEST(TokenizedArgs, NestedTokenFmt2_TokenizingBackend, {
  constexpr char nested_token[] = PW_LOG_NESTED_TOKEN_FMT("enum_domain");
  PW_TEST_EXPECT_STREQ("${${enum_domain}#%08" PRIx32 "}#%08" PRIx32,
                       nested_token);
});

PW_CONSTEXPR_TEST(TokenizedArgs, LogEnumVersionedFmt_TokenizingBackend, {
  constexpr const char* nested_token = PW_LOG_ENUM_VERSIONED_FMT();
  PW_TEST_EXPECT_STREQ(PW_LOG_NESTED_TOKEN_FMT("enum_domain"), nested_token);
  constexpr const char* custom_token = PW_LOG_ENUM_VERSIONED_FMT("my_domain");
  PW_TEST_EXPECT_STREQ(PW_LOG_NESTED_TOKEN_FMT("my_domain"), custom_token);
});

PW_CONSTEXPR_TEST(
    TokenizedArgs, LogEnumVersionedCustomDomain_TokenizingBackend, {
      struct Captured {
        uint32_t domain;
        uint32_t value;
      };
      auto capture = [](uint32_t domain, uint32_t value) {
        return Captured{domain, value};
      };
      constexpr Captured result =
          capture(PW_LOG_ENUM_VERSIONED(::this_is_a_test::ScopedThing::kItem1));
      PW_TEST_EXPECT_EQ(99999u, result.domain);
      PW_TEST_EXPECT_EQ(
          static_cast<uint32_t>(::this_is_a_test::ScopedThing::kItem1),
          result.value);
    });

}  // namespace
}  // namespace this_is_a_test
