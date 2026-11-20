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

#include <cinttypes>
#include <cstdarg>
#include <cstdint>

#include "pw_compilation_testing/negative_compilation.h"
#include "pw_log/log.h"
#include "pw_log/status.h"
#include "pw_log/tokenized_args.h"
#include "pw_preprocessor/compiler.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/status_base.h"
#include "pw_string/format.h"
#include "pw_string/string.h"
#include "pw_tokenizer/enum.h"
#include "pw_unit_test/framework.h"

#define BUS_DOMAIN "::pw::log_status_test::BusCode"
#define LINK_DOMAIN "::pw::log_status_test::Link"

namespace pw::log_status_test {

enum class BusCode : uint8_t {
  kOk = 0,
  kNack = 1,
  kBusError = 2,
  kTimeout = 3,
};

constexpr const char* PwEnumToString(BusCode code) {
  switch (code) {
    case BusCode::kOk:
      return "OK";
    case BusCode::kNack:
      return "NACK";
    case BusCode::kBusError:
      return "BUS_ERROR";
    case BusCode::kTimeout:
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

}  // namespace pw::log_status_test

_PW_TOKENIZE_ENUM_DOMAIN(::pw::log_status_test::BusCode,
                         BUS_DOMAIN,
                         (kOk, "OK"),
                         (kNack, "NACK"),
                         (kBusError, "BUS_ERROR"),
                         (kTimeout, "TIMEOUT"));

_PW_TOKENIZE_ENUM_DOMAIN(::pw::log_status_test::Link,
                         LINK_DOMAIN,
                         (kDown, "DOWN"),
                         (kUp, "UP"));

namespace pw::log_status_test {

#define BUS_STATUS_FMT PW_LOG_STATUS_DOMAIN_FMT(BUS_DOMAIN)
#define BUS_STATUS_CODE_FMT PW_LOG_TOKEN_FMT(BUS_DOMAIN)

using BusStatus = StatusOf<BusCode::kOk>;

// A status whose OK code is not zero.
#define LINK_STATUS_FMT PW_LOG_STATUS_DOMAIN_FMT(LINK_DOMAIN)

using LinkStatus = StatusOf<Link::kUp>;

class [[nodiscard]] IntStatus final : public StatusBase<IntStatus, 0> {
 public:
  constexpr IntStatus() = default;
  explicit constexpr IntStatus(int code) : StatusBase(code) {}
};

struct MoveOnly {
  MoveOnly() = default;
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;
  MoveOnly(MoveOnly&&) = default;
  MoveOnly& operator=(MoveOnly&&) = default;
};

namespace {

#if __has_include("pw_log_backend/log_backend_uses_pw_tokenizer.h")
constexpr bool kTokenizingBackend = true;
#else
constexpr bool kTokenizingBackend = false;
#endif  // __has_include("pw_log_backend/log_backend_uses_pw_tokenizer.h")

using LogString = InlineString<128>;

// Formats a log statement's format string and arguments with printf. With a
// tokenizing pw_log backend, the text has nested tokens in place of names.
LogString FormatLog(const char* format, ...) PW_PRINTF_FORMAT(1, 2);

LogString FormatLog(const char* format, ...) {
  LogString text;
  va_list args;
  va_start(args, format);
  const Status status = string::FormatVaList(text, format, args);
  va_end(args);
  PW_TEST_EXPECT_OK(status);
  return text;
}

// Returns what logging an enumerator with PW_LOG_ENUM or PW_LOG_STATUS formats
// as: its name or, with a tokenizing backend, its token in its enum's domain.
template <typename Enum>
LogString Logged(const char* name, const char* domain, Enum enumerator) {
  if (kTokenizingBackend) {
    return FormatLog(
        "${%s}#%08" PRIx32, domain, static_cast<uint32_t>(enumerator));
  }
  return LogString(name);
}

LogString LoggedBus(const char* name, BusCode code) {
  return Logged(name, BUS_DOMAIN, code);
}

LogString LoggedLink(const char* name, Link link) {
  return Logged(name, LINK_DOMAIN, link);
}

// Returns what logging an enumerator with PW_LOG_ENUM_VERSIONED formats as: its
// name or, with a tokenizing backend, the tokens of its enum's domain and its
// value.
template <typename Enum>
LogString LoggedVersioned(const char* name, Enum enumerator) {
  if (kTokenizingBackend) {
    return FormatLog("${${enum_domain}#%08" PRIx32 "}#%08" PRIx32,
                     tokenizer::EnumDomainToken<Enum>(),
                     static_cast<uint32_t>(enumerator));
  }
  return LogString(name);
}

TEST(LogStatus, LogEnumAcceptsStatusAndCarriers) {
  const BusStatus nack(BusCode::kNack);
  EXPECT_EQ(LoggedBus("NACK", BusCode::kNack),
            FormatLog(BUS_STATUS_CODE_FMT, PW_LOG_ENUM(nack)));
  EXPECT_EQ(
      LoggedVersioned("NACK", BusCode::kNack),
      FormatLog(PW_LOG_ENUM_VERSIONED_FMT(), PW_LOG_ENUM_VERSIONED(nack)));

  const Result<MoveOnly, BusStatus> move_only_err(nack);
  EXPECT_EQ(LoggedBus("NACK", BusCode::kNack),
            FormatLog(BUS_STATUS_CODE_FMT, PW_LOG_ENUM(move_only_err)));
  EXPECT_EQ(LoggedVersioned("NACK", BusCode::kNack),
            FormatLog(PW_LOG_ENUM_VERSIONED_FMT(),
                      PW_LOG_ENUM_VERSIONED(move_only_err)));
}

TEST(LogStatus, MacrosEvaluateArgumentOnce) {
  int calls = 0;
  auto next_status = [&calls]() {
    ++calls;
    return BusStatus(BusCode::kTimeout);
  };

  EXPECT_EQ(LoggedBus("TIMEOUT", BusCode::kTimeout),
            FormatLog(BUS_STATUS_CODE_FMT, PW_LOG_ENUM(next_status())));
  EXPECT_EQ(1, calls);

  EXPECT_EQ(LoggedVersioned("TIMEOUT", BusCode::kTimeout),
            FormatLog(PW_LOG_ENUM_VERSIONED_FMT(),
                      PW_LOG_ENUM_VERSIONED(next_status())));
  EXPECT_EQ(2, calls);

  EXPECT_EQ(LoggedBus("TIMEOUT", BusCode::kTimeout),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(next_status())));
  EXPECT_EQ(3, calls);
}

TEST(LogStatus, CustomStatus) {
  const BusStatus ok;
  const BusStatus nack(BusCode::kNack);
  const Result<MoveOnly, BusStatus> error(nack);

  EXPECT_EQ(LoggedBus("OK", BusCode::kOk),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(ok)));
  EXPECT_EQ(LoggedBus("NACK", BusCode::kNack),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(nack)));
  EXPECT_EQ(LoggedBus("NACK", BusCode::kNack),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(error)));

  const LinkStatus up;
  const LinkStatus down(Link::kDown);
  EXPECT_EQ(LoggedLink("UP", Link::kUp),
            FormatLog(LINK_STATUS_FMT, PW_LOG_STATUS(up)));
  EXPECT_EQ(LoggedLink("DOWN", Link::kDown),
            FormatLog(LINK_STATUS_FMT, PW_LOG_STATUS(down)));
}

TEST(LogStatus, AcceptsAnyExpression) {
  struct Holder {
    BusStatus status;
  };
  const BusStatus statuses[] = {BusStatus(BusCode::kNack),
                                BusStatus(BusCode::kTimeout)};
  const Holder holder{statuses[1]};
  const Holder* holder_pointer = &holder;
  int index = 0;

  EXPECT_EQ(LoggedBus("TIMEOUT", BusCode::kTimeout),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(holder_pointer->status)));
  EXPECT_EQ(LoggedBus("NACK", BusCode::kNack),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(statuses[index++])));
  EXPECT_EQ(LoggedBus("TIMEOUT", BusCode::kTimeout),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(statuses[index++])));
  EXPECT_EQ(
      LoggedBus("BUS_ERROR", BusCode::kBusError),
      FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(BusStatus(BusCode::kBusError))));
  EXPECT_EQ(2, index);
}

TEST(LogStatus, CompilesAndRunsWithPwLogMacros) {
  const BusStatus bus_status(BusCode::kBusError);
  PW_LOG_INFO("Versioned code: " PW_LOG_ENUM_VERSIONED_FMT(),
              PW_LOG_ENUM_VERSIONED(bus_status));
  PW_LOG_INFO("Bus: " BUS_STATUS_FMT, PW_LOG_STATUS(bus_status));
  PW_LOG_INFO("Link: " LINK_STATUS_FMT, PW_LOG_STATUS(LinkStatus(Link::kDown)));
}

#if PW_NC_TEST(LogEnumRejectsInt)
PW_NC_EXPECT("require an enum, a status with an enum code");
[[maybe_unused]] void TestLogEnumRejectsInt() { (void)PW_LOG_ENUM(123); }
#elif PW_NC_TEST(LogEnumRejectsIntCodedStatus)
PW_NC_EXPECT("require an enum, a status with an enum code");
[[maybe_unused]] void TestLogEnumRejectsIntCodedStatus() {
  const IntStatus status(1);
  (void)PW_LOG_ENUM(status);
}
#elif PW_NC_TEST(LogStatusRejectsPlainEnum)
PW_NC_EXPECT("PW_LOG_STATUS requires a status");
[[maybe_unused]] void TestLogStatusRejectsPlainEnum() {
  const BusCode code = BusCode::kNack;
  PW_LOG_INFO("Bad: " BUS_STATUS_CODE_FMT, PW_LOG_STATUS(code));
}
#elif PW_NC_TEST(LogStatusRejectsIntCodedStatus)
PW_NC_EXPECT("require an enum, a status with an enum code");
[[maybe_unused]] void TestLogStatusRejectsIntCodedStatus() {
  const IntStatus status(1);
  PW_LOG_INFO("Bad: " BUS_STATUS_FMT, PW_LOG_STATUS(status));
}
#endif  // PW_NC_TEST

}  // namespace
}  // namespace pw::log_status_test
