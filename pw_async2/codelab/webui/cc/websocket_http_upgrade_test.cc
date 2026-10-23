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

#include "websocket_http_upgrade.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>

#include "pw_bytes/span.h"
#include "pw_fuzzer/fuzztest.h"
#include "pw_hex_dump/log_bytes.h"
#include "pw_log/levels.h"
#include "pw_log/log.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::experimental::websocket::http_upgrade {
namespace {

using namespace std::string_view_literals;

constexpr auto AsBytes(std::string_view value) { return as_bytes(span(value)); }

bool Compare(span<const std::byte> actual,
             span<const std::byte> expected,
             const char* name) {
  if (actual.size() != expected.size() ||
      !std::equal(actual.begin(), actual.end(), expected.begin())) {
    PW_LOG_ERROR("%s actual   (%zu bytes):", name, actual.size());
    dump::LogBytes(PW_LOG_LEVEL_ERROR, actual);
    PW_LOG_ERROR("%s expected (%zu bytes):", name, expected.size());
    dump::LogBytes(PW_LOG_LEVEL_ERROR, expected);
    return false;
  }

  return true;
}

bool StartsWith(span<const std::byte> actual,
                span<const std::byte> expected_prefix,
                const char* name) {
  if (actual.size() < expected_prefix.size() ||
      !std::equal(
          expected_prefix.begin(), expected_prefix.end(), actual.begin())) {
    PW_LOG_ERROR("%s actual   (%zu bytes):", name, actual.size());
    dump::LogBytes(PW_LOG_LEVEL_ERROR, actual);
    PW_LOG_ERROR(
        "%s expected-prefix (%zu bytes):", name, expected_prefix.size());
    dump::LogBytes(PW_LOG_LEVEL_ERROR, expected_prefix);
    return false;
  }

  return true;
}

TEST(WebsocketHttpUpgrade, GenerateReplyKey) {
  // Straight from RFC 6455 (December 2011) "1.3. Opening handshake"
  constexpr auto kRequestKey = "dGhlIHNhbXBsZSBub25jZQ=="sv;
  constexpr auto kExpected = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="sv;

  std::array<std::byte, kBase64EncodedReplyKeySize> buffer{};
  auto result = GenerateReplyKey(
      AsBytes(kRequestKey).subspan<0, kBase64EncodedNonceKeySize>(), buffer);
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(Compare(result.value(), AsBytes(kExpected), "reply_key"));
}

TEST(WebsocketHttpUpgrade, GenerateWebsocketUpgrade400BadRequest) {
  constexpr auto kExpected =
      "HTTP/1.1 400 Bad Request\r\n"
      "\r\n"sv;

  std::array<std::byte, 1024> response_buffer{};
  const auto result = GenerateWebsocketUpgrade400BadRequest(response_buffer);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(Compare(*result, AsBytes(kExpected), "expected"));
}

TEST(WebsocketHttpUpgrade, GenerateWebsocketUpgrade400BadRequestWithVersion) {
  constexpr auto kExpected =
      "HTTP/1.1 400 Bad Request\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "\r\n"sv;

  std::array<std::byte, 1024> response_buffer{};
  const auto result =
      GenerateWebsocketUpgrade400BadRequestWithVersion(response_buffer);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(Compare(*result, AsBytes(kExpected), "expected"));
}

TEST(WebsocketHttpUpgrade, GenerateWebsocketUpgradeSuccess) {
  constexpr auto kResponseKey = "0123456789ABCDEFGHIJKLMNOPQR"sv;
  constexpr auto kExpected =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: 0123456789ABCDEFGHIJKLMNOPQR\r\n"
      "\r\n"sv;

  std::array<std::byte, 1024> response_buffer{};
  const auto result = GenerateWebsocketUpgradeSuccess(
      AsBytes(kResponseKey).subspan<0, kBase64EncodedReplyKeySize>(),
      response_buffer);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(Compare(*result, AsBytes(kExpected), "expected"));
}

TEST(WebsocketHttpUpgrade, IsHttpRequestHeaderOnly) {
  EXPECT_FALSE(IsHttpRequestHeaderOnly(AsBytes(""sv)));
  EXPECT_FALSE(IsHttpRequestHeaderOnly(AsBytes("\r\n"sv)));
  EXPECT_FALSE(IsHttpRequestHeaderOnly(AsBytes("\r\n\r"sv)));
  EXPECT_TRUE(IsHttpRequestHeaderOnly(AsBytes("\r\n\r\n"sv)));
  EXPECT_FALSE(IsHttpRequestHeaderOnly(AsBytes("\r\n\r\n\r"sv)));

  constexpr auto kRequest =
      "GET /ignored HTTP/1.1\r\n"
      "Header1: Value1\r\n"
      "Header2: Value2\r\n"
      "\r\n"sv;
  EXPECT_TRUE(IsHttpRequestHeaderOnly(AsBytes(kRequest)));
}

TEST(WebsocketHttpUpgrade, FindUniqueHttpHeaderPrefixAndReturnRemaining) {
  constexpr auto kRequest =
      "GET /ignored HTTP/1.1\r\n"
      "Header1: Value1\r\n"
      "Header2: Value2\r\n"
      "Header3: Value3a\r\n"
      "Header3: Value3b\r\n"
      "\r\n"sv;

  auto result = FindUniqueHttpHeaderPrefixAndReturnRemaining(AsBytes(kRequest),
                                                             AsBytes("GET "sv));
  EXPECT_FALSE(result.ok());

  result = FindUniqueHttpHeaderPrefixAndReturnRemaining(AsBytes(kRequest),
                                                        AsBytes("eader1: "sv));
  EXPECT_FALSE(result.ok());

  result = FindUniqueHttpHeaderPrefixAndReturnRemaining(AsBytes(kRequest),
                                                        AsBytes("Header1: "sv));
  EXPECT_TRUE(result.ok());
  EXPECT_TRUE(Compare(*result, AsBytes("Value1"), "header1 value"));

  result = FindUniqueHttpHeaderPrefixAndReturnRemaining(AsBytes(kRequest),
                                                        AsBytes("Header2: "sv));
  EXPECT_TRUE(result.ok());
  EXPECT_TRUE(Compare(*result, AsBytes("Value2"), "header2 value"));

  result = FindUniqueHttpHeaderPrefixAndReturnRemaining(AsBytes(kRequest),
                                                        AsBytes("Header3: "sv));
  EXPECT_FALSE(result.ok());
}

TEST(WebsocketHttpUpgrade, HasUniqueHttpHeader) {
  constexpr auto kRequest =
      "GET /ignored HTTP/1.1\r\n"
      "Header1: Value1\r\n"
      "Header2: Value2\r\n"
      "Header3: Value3\r\n"
      "Header3: Value3\r\n"
      "\r\n"sv;

  EXPECT_FALSE(HasUniqueHttpHeader(AsBytes(kRequest),
                                   AsBytes("GET /ignored HTTP/1.1"sv)));
  EXPECT_FALSE(HasUniqueHttpHeader(AsBytes(kRequest), AsBytes("eader1: "sv)));
  EXPECT_FALSE(HasUniqueHttpHeader(AsBytes(kRequest), AsBytes("Header1: "sv)));
  EXPECT_FALSE(
      HasUniqueHttpHeader(AsBytes(kRequest), AsBytes("Header1: Value"sv)));
  EXPECT_TRUE(
      HasUniqueHttpHeader(AsBytes(kRequest), AsBytes("Header1: Value1"sv)));
  EXPECT_TRUE(
      HasUniqueHttpHeader(AsBytes(kRequest), AsBytes("Header2: Value2"sv)));
  EXPECT_FALSE(
      HasUniqueHttpHeader(AsBytes(kRequest), AsBytes("Header3: Value3"sv)));
}

TEST(WebsocketHttpUpgrade, CheckWebsocketUpgradeRequestAndGetKeyOk) {
  constexpr auto kRequest =
      "GET /ignored HTTP/1.1\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Key: ABCDEFGHIJKLMNOPQRSTUVWX\r\n"
      "\r\n"sv;
  constexpr auto kExpectedKey = "ABCDEFGHIJKLMNOPQRSTUVWX"sv;

  auto result = CheckWebsocketUpgradeRequestAndGetKey(AsBytes(kRequest));
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(Compare(*result, AsBytes(kExpectedKey), "key"));
}

TEST(WebsocketHttpUpgrade, ProcessHttpWebsocketUpgradeRequestOk) {
  constexpr auto kRequest =
      "GET /ignored HTTP/1.1\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Key: ABCDEFGHIJKLMNOPQRSTUVWX\r\n"
      "\r\n"sv;
  constexpr auto kExpectedUpgradeSuccessPrefix = "HTTP/1.1 101 "sv;

  std::array<std::byte, 1024> response_buffer{};
  bool valid_upgrade_request = false;
  auto result = ProcessHttpWebsocketUpgradeRequest(
      AsBytes(kRequest), response_buffer, valid_upgrade_request);
  EXPECT_TRUE(valid_upgrade_request);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(StartsWith(*result,
                         AsBytes(kExpectedUpgradeSuccessPrefix),
                         "upgrade_success_prefix"));
}

TEST(WebsocketHttpUpgrade, ProcessHttpWebsocketUpgradeRequestFail) {
  constexpr auto kRequest =
      "GET /ignored HTTP/1.1\r\n"
      "Connection: Upgrade\r\n"
      "Upgrade: websocket\r\n"
      "\r\n"sv;
  constexpr auto kExpectedUpgradeFailPrefix = "HTTP/1.1 400 "sv;

  std::array<std::byte, 1024> response_buffer{};
  bool valid_upgrade_request = false;
  auto result = ProcessHttpWebsocketUpgradeRequest(
      AsBytes(kRequest), response_buffer, valid_upgrade_request);
  EXPECT_FALSE(valid_upgrade_request);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(StartsWith(
      *result, AsBytes(kExpectedUpgradeFailPrefix), "upgrade_success_prefix"));
}

void ProcessHttpWebsocketUpgradeRequestFuzzed(ConstByteSpan data) {
  std::array<std::byte, 1024> response_buffer{};
  bool valid_upgrade_request = false;
  ProcessHttpWebsocketUpgradeRequest(
      data, response_buffer, valid_upgrade_request)
      .IgnoreError();
}

FUZZ_TEST(WebsocketHttpUpgrade, ProcessHttpWebsocketUpgradeRequestFuzzed)
    .WithDomains(fuzzer::VectorOf<1024>(fuzzer::Arbitrary<std::byte>()));

}  // namespace
}  // namespace pw::experimental::websocket::http_upgrade
