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
#include <cstdio>
#include <string>
#include <type_traits>

#include "pw_log/status.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_base.h"
#include "pw_status/status_with_size.h"
#include "pw_tokenizer/detokenize.h"
#include "pw_tokenizer/enum.h"
#include "pw_tokenizer/hash.h"
#include "pw_tokenizer/tokenize.h"
#include "pw_unit_test/constexpr.h"
#include "pw_unit_test/framework.h"

namespace this_is_a_test {

enum class I2cCode : uint8_t {
  kOk = 0,
  kNack = 1,
  kTimeout = 2,
};

constexpr const char* PwEnumToString(I2cCode code) {
  switch (code) {
    case I2cCode::kOk:
      return "OK";
    case I2cCode::kNack:
      return "NACK";
    case I2cCode::kTimeout:
      return "TIMEOUT";
  }
  return "UNKNOWN";
}

enum class Link : bool {
  kDown = false,
  kUp = true,
};

constexpr const char* PwEnumToString(Link link) {
  return link == Link::kUp ? "UP" : "DOWN";
}

}  // namespace this_is_a_test

_PW_TOKENIZE_ENUM_DOMAIN(::this_is_a_test::I2cCode,
                         "::this_is_a_test::I2cCode",
                         (kOk, "OK"),
                         (kNack, "NACK"),
                         (kTimeout, "TIMEOUT"));

_PW_TOKENIZE_ENUM_DOMAIN(::this_is_a_test::Link,
                         "::this_is_a_test::Link",
                         (kDown, "DOWN"),
                         (kUp, "UP"));

namespace this_is_a_test {

#define I2C_STATUS_FMT PW_LOG_STATUS_DOMAIN_FMT("::this_is_a_test::I2cCode")

using I2cStatus = pw::StatusOf<I2cCode::kOk>;

// A status whose OK code is not zero.
#define LINK_STATUS_FMT PW_LOG_STATUS_DOMAIN_FMT("::this_is_a_test::Link")

using LinkStatus = pw::StatusOf<Link::kUp>;

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

PW_CONSTEXPR_TEST(TokenizedArgs, LogEnumFromStatusAndCarriers_Tokenizing, {
  PW_TEST_EXPECT_EQ(static_cast<uint32_t>(PW_STATUS_NOT_FOUND),
                    PW_LOG_ENUM(pw::Status::NotFound()));
  PW_TEST_EXPECT_EQ(static_cast<uint32_t>(PW_STATUS_RESOURCE_EXHAUSTED),
                    PW_LOG_ENUM(pw::StatusWithSize::ResourceExhausted(16)));
  PW_TEST_EXPECT_EQ(static_cast<uint32_t>(PW_STATUS_UNAVAILABLE),
                    PW_LOG_ENUM(pw::Result<int>(pw::Status::Unavailable())));

  constexpr I2cStatus kNack(I2cCode::kNack);
  PW_TEST_EXPECT_EQ(static_cast<uint32_t>(I2cCode::kNack), PW_LOG_ENUM(kNack));
  PW_TEST_EXPECT_EQ(static_cast<uint32_t>(I2cCode::kNack),
                    PW_LOG_ENUM(pw::Result<int, I2cStatus>(kNack)));

  struct VersionedArgs {
    uint32_t domain;
    uint32_t value;
  };
  auto capture = [](uint32_t domain, uint32_t value) {
    return VersionedArgs{domain, value};
  };
  constexpr VersionedArgs versioned = capture(PW_LOG_ENUM_VERSIONED(kNack));
  PW_TEST_EXPECT_EQ(pw::tokenizer::Hash("::this_is_a_test::I2cCode"),
                    versioned.domain);
  PW_TEST_EXPECT_EQ(static_cast<uint32_t>(I2cCode::kNack), versioned.value);

  constexpr VersionedArgs sws_versioned =
      capture(PW_LOG_ENUM_VERSIONED(pw::StatusWithSize::DataLoss(4)));
  PW_TEST_EXPECT_EQ(pw::tokenizer::Hash(PW_STATUS_DOMAIN),
                    sws_versioned.domain);
  PW_TEST_EXPECT_EQ(static_cast<uint32_t>(PW_STATUS_DATA_LOSS),
                    sws_versioned.value);
});

// A status is logged as one argument: the token of its code.
TEST(TokenizedArgs, LogStatusIsItsCode_TokenizingBackend) {
  EXPECT_STREQ(I2C_STATUS_FMT, "${::this_is_a_test::I2cCode}#%08" PRIx32);
  EXPECT_STREQ(LINK_STATUS_FMT, "${::this_is_a_test::Link}#%08" PRIx32);
  EXPECT_STREQ(PW_STATUS_FMT, "${::pw::Status}#%08" PRIx32);

  const I2cStatus nack(I2cCode::kNack);
  const pw::Result<int, LinkStatus> link_up(0);
  EXPECT_EQ(PW_LOG_STATUS(nack), 1u);
  EXPECT_EQ(PW_LOG_STATUS(LinkStatus(Link::kDown)), 0u);
  EXPECT_EQ(PW_LOG_STATUS(link_up), 1u);
  EXPECT_EQ(PW_LOG_STATUS(pw::Status::NotFound()), 5u);
  EXPECT_EQ(PW_LOG_STATUS(pw::StatusWithSize::ResourceExhausted(8)), 8u);
}

TEST(TokenizedArgs, LogStatusEvaluatesArgumentOnce_TokenizingBackend) {
  int calls = 0;
  auto next_status = [&calls]() {
    ++calls;
    return I2cStatus(I2cCode::kNack);
  };
  EXPECT_EQ(PW_LOG_STATUS(next_status()), 1u);
  EXPECT_EQ(calls, 1);
}

// Adds a line to a CSV token database. The text is quoted because bit field
// token formats contain a comma.
void AddToken(std::string& csv,
              uint32_t token,
              const char* domain,
              const char* text) {
  char line[256] = {};
  std::snprintf(
      line, sizeof(line), "%08" PRIx32 ",,%s,\"%s\"\n", token, domain, text);
  csv += line;
}

// Statuses are logged like any other tokenized argument. The C++ detokenizer
// resolves the code's token.
TEST(TokenizedArgs, LogStatusDetokenizes_TokenizingBackend) {
  const pw::Status not_found = pw::Status::NotFound();
  const I2cStatus nack(I2cCode::kNack);
  const pw::Result<int, I2cStatus> result(nack);

  constexpr const char* kFmtPwStatus = "pw=" PW_STATUS_FMT;
  constexpr const char* kFmtStatic = "i2c=" I2C_STATUS_FMT;
  constexpr const char* kFmtCodeOnly = "code=" PW_LOG_ENUM_VERSIONED_FMT();

  std::string csv;
  AddToken(csv, pw::tokenizer::Hash(kFmtPwStatus), "", kFmtPwStatus);
  AddToken(csv, pw::tokenizer::Hash(kFmtStatic), "", kFmtStatic);
  AddToken(csv, pw::tokenizer::Hash(kFmtCodeOnly), "", kFmtCodeOnly);
  AddToken(csv,
           pw::tokenizer::Hash("::this_is_a_test::I2cCode"),
           "enum_domain",
           "::this_is_a_test::I2cCode");
  AddToken(csv, PW_STATUS_NOT_FOUND, "::pw::Status", "NOT_FOUND");
  AddToken(csv,
           static_cast<uint32_t>(I2cCode::kNack),
           "::this_is_a_test::I2cCode",
           "NACK");

  pw::Result<pw::tokenizer::Detokenizer> detok =
      pw::tokenizer::Detokenizer::FromCsv(csv);
  PW_TEST_ASSERT_OK(detok);

  uint8_t buffer[64] = {};
  size_t size = 0;

#define EXPECT_DETOKENIZED(expected, ...)            \
  size = sizeof(buffer);                             \
  PW_TOKENIZE_TO_BUFFER(buffer, &size, __VA_ARGS__); \
  EXPECT_EQ(expected,                                \
            detok->RecursiveDetokenize(pw::span(buffer, size)).BestString())

  EXPECT_DETOKENIZED("code=NACK",
                     "code=" PW_LOG_ENUM_VERSIONED_FMT(),
                     PW_LOG_ENUM_VERSIONED(result));

  EXPECT_DETOKENIZED("i2c=NACK", "i2c=" I2C_STATUS_FMT, PW_LOG_STATUS(nack));
  EXPECT_DETOKENIZED(
      "pw=NOT_FOUND", "pw=" PW_STATUS_FMT, PW_LOG_STATUS(not_found));

#undef EXPECT_DETOKENIZED
}

}  // namespace
}  // namespace this_is_a_test
