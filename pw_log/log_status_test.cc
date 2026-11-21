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

#include "log_status_test_enum.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_log/log.h"
#include "pw_log/status.h"
#include "pw_log/tokenized_args.h"
#include "pw_preprocessor/compiler.h"
#include "pw_result/result.h"
#include "pw_status/message.h"
#include "pw_status/status.h"
#include "pw_status/status_base.h"
#include "pw_status/status_with_size.h"
#include "pw_string/format.h"
#include "pw_string/string.h"
#include "pw_tokenizer/enum.h"
#include "pw_unit_test/framework.h"

#define BUS_DOMAIN "::pw::log_status_test::BusCode"

// Link is registered with PW_ENUM in log_status_test_enum.h, which generates
// its token domain.
#define LINK_DOMAIN PW_LOG_STATUS_TEST_LINK_DOMAIN

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

}  // namespace pw::log_status_test

_PW_TOKENIZE_ENUM_DOMAIN(::pw::log_status_test::BusCode,
                         BUS_DOMAIN,
                         (kOk, "OK"),
                         (kNack, "NACK"),
                         (kBusError, "BUS_ERROR"),
                         (kTimeout, "TIMEOUT"));

namespace pw::log_status_test {

// BusCode is not registered with PW_ENUM, so BusStatus does not support
// messages and its whole logged value is its code (32 bits).
#define BUS_STATUS_FMT PW_LOG_STATUS_DOMAIN_FMT(BUS_DOMAIN, 32)
#define BUS_STATUS_CODE_FMT PW_LOG_TOKEN_FMT(BUS_DOMAIN)

using BusStatus = StatusOf<BusCode::kOk>;

// Link is registered with PW_ENUM and has a bit width of 1, so LinkStatus
// supports messages and its code is the low bit of its logged value. Its OK
// code is not zero.
#define LINK_STATUS_FMT PW_LOG_STATUS_DOMAIN_FMT(LINK_DOMAIN, 1)

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

using LogString = InlineString<256>;

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

// Returns what logging an enumerator with PW_LOG_ENUM formats as: its name or,
// with a tokenizing backend, its token in its enum's domain.
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

LogString LoggedPwStatus(const char* name, pw_Status code) {
  return Logged(name, PW_STATUS_DOMAIN, code);
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

// Returns what logging a status that has no message with PW_LOG_STATUS formats
// as. The status is logged as its code is with PW_LOG_ENUM, unless a tokenizing
// backend logs status messages. Then it is a bit field token with the code in
// its low `code_bits` bits and the message's token, from the message domain of
// the code's domain, in the rest.
template <typename Enum>
LogString LoggedStatus(const char* name,
                       const char* domain,
                       int code_bits,
                       Enum code) {
  if (PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_TOKENIZED) {
    return FormatLog("${%s/%d," PW_STATUS_MSG_DOMAIN("%s") "}#%08" PRIx32,
                     domain,
                     code_bits,
                     domain,
                     static_cast<uint32_t>(code));
  }
  return Logged(name, domain, code);
}

LogString LoggedBusStatus(const char* name, BusCode code) {
  return LoggedStatus(name, BUS_DOMAIN, 32, code);
}

LogString LoggedLinkStatus(const char* name, Link link) {
  return LoggedStatus(name, LINK_DOMAIN, 1, link);
}

LogString LoggedStatus(const char* name, pw_Status code) {
  return LoggedStatus(name, PW_STATUS_DOMAIN, 5, code);
}

TEST(LogStatus, LogEnumAcceptsStatusAndCarriers) {
  EXPECT_EQ(LoggedPwStatus("NOT_FOUND", PW_STATUS_NOT_FOUND),
            FormatLog(PW_STATUS_CODE_FMT, PW_LOG_ENUM(PW_STATUS_NOT_FOUND)));
  EXPECT_EQ(LoggedPwStatus("NOT_FOUND", PW_STATUS_NOT_FOUND),
            FormatLog(PW_STATUS_CODE_FMT, PW_LOG_ENUM(Status::NotFound())));
  EXPECT_EQ(LoggedPwStatus("RESOURCE_EXHAUSTED", PW_STATUS_RESOURCE_EXHAUSTED),
            FormatLog(PW_STATUS_CODE_FMT,
                      PW_LOG_ENUM(StatusWithSize::ResourceExhausted(12))));
  EXPECT_EQ(LoggedPwStatus("UNAVAILABLE", PW_STATUS_UNAVAILABLE),
            FormatLog(PW_STATUS_CODE_FMT,
                      PW_LOG_ENUM(Result<int>(Status::Unavailable()))));
  EXPECT_EQ(LoggedPwStatus("OK", PW_STATUS_OK),
            FormatLog(PW_STATUS_CODE_FMT, PW_LOG_ENUM(Result<int>(99))));

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

  EXPECT_EQ(LoggedBusStatus("TIMEOUT", BusCode::kTimeout),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(next_status())));
  EXPECT_EQ(3, calls);
}

TEST(LogStatus, LogStatusCallsCarrierStatusOnce) {
  struct CountingCarrier {
    LinkStatus status() const {
      ++calls;
      return LinkStatus(Link::kDown, PW_STATUS_MSG("Cable unplugged"));
    }

    mutable int calls = 0;
  };

  const CountingCarrier carrier;
  static_cast<void>(FormatLog(LINK_STATUS_FMT, PW_LOG_STATUS(carrier)));
  EXPECT_EQ(1, carrier.calls);
}

TEST(LogStatus, PwStatus) {
  const Status ok = OkStatus();
  const Status not_found = Status::NotFound();

  EXPECT_EQ(LoggedStatus("OK", PW_STATUS_OK),
            FormatLog(PW_STATUS_FMT, PW_LOG_STATUS(ok)));
  EXPECT_EQ(LoggedStatus("NOT_FOUND", PW_STATUS_NOT_FOUND),
            FormatLog(PW_STATUS_FMT, PW_LOG_STATUS(not_found)));
  EXPECT_EQ(LoggedStatus("DATA_LOSS", PW_STATUS_DATA_LOSS),
            FormatLog(PW_STATUS_FMT, PW_LOG_STATUS(Status::DataLoss())));
}

TEST(LogStatus, Carriers) {
  const StatusWithSize sws = StatusWithSize::DataLoss(7);
  const Result<int> value = 42;
  const Result<int> error = Status::PermissionDenied();

  EXPECT_EQ(LoggedStatus("DATA_LOSS", PW_STATUS_DATA_LOSS),
            FormatLog(PW_STATUS_FMT, PW_LOG_STATUS(sws)));
  EXPECT_EQ(LoggedStatus("OK", PW_STATUS_OK),
            FormatLog(PW_STATUS_FMT, PW_LOG_STATUS(value)));
  EXPECT_EQ(LoggedStatus("PERMISSION_DENIED", PW_STATUS_PERMISSION_DENIED),
            FormatLog(PW_STATUS_FMT, PW_LOG_STATUS(error)));
}

TEST(LogStatus, CustomStatus) {
  const BusStatus ok;
  const BusStatus nack(BusCode::kNack);
  const Result<MoveOnly, BusStatus> error(nack);

  EXPECT_EQ(LoggedBusStatus("OK", BusCode::kOk),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(ok)));
  EXPECT_EQ(LoggedBusStatus("NACK", BusCode::kNack),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(nack)));
  EXPECT_EQ(LoggedBusStatus("NACK", BusCode::kNack),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(error)));

  const LinkStatus up;
  const LinkStatus down(Link::kDown);
  EXPECT_EQ(LoggedLinkStatus("UP", Link::kUp),
            FormatLog(LINK_STATUS_FMT, PW_LOG_STATUS(up)));
  EXPECT_EQ(LoggedLinkStatus("DOWN", Link::kDown),
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

  EXPECT_EQ(LoggedBusStatus("TIMEOUT", BusCode::kTimeout),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(holder_pointer->status)));
  EXPECT_EQ(LoggedBusStatus("NACK", BusCode::kNack),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(statuses[index++])));
  EXPECT_EQ(LoggedBusStatus("TIMEOUT", BusCode::kTimeout),
            FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(statuses[index++])));
  EXPECT_EQ(
      LoggedBusStatus("BUS_ERROR", BusCode::kBusError),
      FormatLog(BUS_STATUS_FMT, PW_LOG_STATUS(BusStatus(BusCode::kBusError))));
  EXPECT_EQ(2, index);
}

constexpr int kSectorCount = 64;

// DOCSTAG: [pw_log-status-example]
pw::Status ReadSector(int sector) {
  if (sector < 0) {
    return pw::Status::InvalidArgument(
        PW_STATUS_MSG("Sector cannot be negative"));
  }
  if (sector >= kSectorCount) {
    return pw::Status::OutOfRange();
  }
  return pw::OkStatus();
}

void LoadSector(int sector) {
  if (pw::Status status = ReadSector(sector); !status.ok()) {
    // Describe what failed, then log the status with PW_STATUS_FMT and
    // PW_LOG_STATUS.
    PW_LOG_ERROR("Read failed: " PW_STATUS_FMT, PW_LOG_STATUS(status));
  }
}
// DOCSTAG: [pw_log-status-example]

TEST(LogStatus, ExampleCompilesAndRuns) {
  LoadSector(0);
  LoadSector(kSectorCount);
  LoadSector(-1);
}

// The tests of statuses with messages check the text that non-tokenizing
// backends log. pw_log_tokenized's tokenized_args_test checks the tokens that
// tokenizing backends log.
#if !__has_include("pw_log_backend/log_backend_uses_pw_tokenizer.h")

// The expected text for a status logged with a message: "CODE (message)" if
// messages are enabled and "CODE" otherwise.
LogString WithMessage(const char* code, const char* message) {
  LogString text(code);
  if (PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_STRING) {
    text += " (";
    text += message;
    text += ")";
  }
  return text;
}

// These are the log lines that the docs show for the example above.
TEST(LogStatus, ExampleLogsStatusAfterDescription) {
  EXPECT_EQ(LogString("Read failed: OUT_OF_RANGE"),
            FormatLog("Read failed: " PW_STATUS_FMT,
                      PW_LOG_STATUS(ReadSector(kSectorCount))));

  const LogString with_message =
      FormatLog("Read failed: " PW_STATUS_FMT, PW_LOG_STATUS(ReadSector(-1)));
  if (PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_STRING) {
    EXPECT_EQ(
        LogString("Read failed: INVALID_ARGUMENT (Sector cannot be negative)"),
        with_message);
  } else {
    EXPECT_EQ(LogString("Read failed: INVALID_ARGUMENT"), with_message);
  }
}

// The parentheses mark where a message ends, so a status can be followed by
// more text or by another status.
TEST(LogStatus, StatusWithMessageInTheMiddleOfALog) {
  const Status rx = Status::DataLoss(PW_STATUS_MSG("Bad CRC"));

  LogString expected("tx: OK, rx: ");
  expected += WithMessage("DATA_LOSS", "Bad CRC");
  expected += " after 3 tries";
  EXPECT_EQ(
      expected,
      FormatLog("tx: " PW_STATUS_FMT ", rx: " PW_STATUS_FMT " after %d tries",
                PW_LOG_STATUS(OkStatus()),
                PW_LOG_STATUS(rx),
                3));
}

TEST(LogStatus, LogStatusEvaluatesStatusWithMessageOnce) {
  int calls = 0;
  auto next_with_message = [&calls]() {
    ++calls;
    return LinkStatus(Link::kDown, PW_STATUS_MSG("Cable unplugged"));
  };

  EXPECT_EQ(WithMessage("DOWN", "Cable unplugged"),
            FormatLog(LINK_STATUS_FMT, PW_LOG_STATUS(next_with_message())));
  EXPECT_EQ(1, calls);
}

TEST(LogStatus, PwStatusWithMessage) {
  const Status with_message = Status::NotFound(PW_STATUS_MSG("No such key"));

  EXPECT_EQ(WithMessage("NOT_FOUND", "No such key"),
            FormatLog(PW_STATUS_FMT, PW_LOG_STATUS(with_message)));
  EXPECT_EQ(
      WithMessage("DATA_LOSS", "Bad CRC"),
      FormatLog(PW_STATUS_FMT,
                PW_LOG_STATUS(Status::DataLoss(PW_STATUS_MSG("Bad CRC")))));
}

TEST(LogStatus, CarrierWithMessage) {
  const Result<int> error = Status::PermissionDenied(PW_STATUS_MSG("Locked"));

  EXPECT_EQ(WithMessage("PERMISSION_DENIED", "Locked"),
            FormatLog(PW_STATUS_FMT, PW_LOG_STATUS(error)));
}

TEST(LogStatus, CustomStatusWithMessage) {
  const LinkStatus no_message(Link::kDown);
  const LinkStatus empty_message(Link::kDown, PW_STATUS_MSG(""));
  const LinkStatus with_message(Link::kDown, PW_STATUS_MSG("Cable unplugged"));
  const Result<MoveOnly, LinkStatus> error(with_message);

  EXPECT_EQ(LogString("DOWN"),
            FormatLog(LINK_STATUS_FMT, PW_LOG_STATUS(no_message)));
  // A message with no text is still a message, so it logs as "DOWN ()".
  EXPECT_EQ(WithMessage("DOWN", ""),
            FormatLog(LINK_STATUS_FMT, PW_LOG_STATUS(empty_message)));
  EXPECT_EQ(WithMessage("DOWN", "Cable unplugged"),
            FormatLog(LINK_STATUS_FMT, PW_LOG_STATUS(with_message)));
  EXPECT_EQ(WithMessage("DOWN", "Cable unplugged"),
            FormatLog(LINK_STATUS_FMT, PW_LOG_STATUS(error)));
}

#if PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_STRING

TEST(LogStatus, StringLongerThanBufferIsTruncated) {
#define PW_LOG_STATUS_TEST_LONG_MESSAGE                                    \
  "0123456789012345678901234567890123456789012345678901234567890123456789" \
  "0123456789012345678901234567890123456789012345678901234567890123456789"
  static constexpr char kLongMessage[] = PW_LOG_STATUS_TEST_LONG_MESSAGE;
  static_assert(sizeof(kLongMessage) > PW_LOG_STATUS_STRING_SIZE);

  const Status status =
      Status::Internal(PW_STATUS_MSG(PW_LOG_STATUS_TEST_LONG_MESSAGE));
#undef PW_LOG_STATUS_TEST_LONG_MESSAGE

  // The closing parenthesis is truncated with the rest of the message.
  InlineString<256> expected("INTERNAL (");
  expected += kLongMessage;
  expected.resize(PW_LOG_STATUS_STRING_SIZE - 1);
  EXPECT_EQ(expected, FormatLog(PW_STATUS_FMT, PW_LOG_STATUS(status)));
}

#endif  // PW_STATUS_CFG_MESSAGES == PW_STATUS_CFG_MESSAGES_STRING

#endif  // !__has_include("pw_log_backend/log_backend_uses_pw_tokenizer.h")

TEST(LogStatus, CompilesAndRunsWithPwLogMacros) {
  const BusStatus bus_status(BusCode::kBusError);
  const LinkStatus link_status(Link::kDown, PW_STATUS_MSG("No carrier"));
  const Status status = Status::Aborted(PW_STATUS_MSG("Stopped"));
  const Result<int> result = status;

  PW_LOG_INFO("Code: " PW_STATUS_CODE_FMT, PW_LOG_ENUM(status));
  PW_LOG_INFO("Versioned code: " PW_LOG_ENUM_VERSIONED_FMT(),
              PW_LOG_ENUM_VERSIONED(bus_status));
  PW_LOG_INFO("Status: " PW_STATUS_FMT, PW_LOG_STATUS(status));
  PW_LOG_INFO("Result: " PW_STATUS_FMT, PW_LOG_STATUS(result));
  PW_LOG_INFO("Bus: " BUS_STATUS_FMT, PW_LOG_STATUS(bus_status));
  PW_LOG_INFO("Link: " LINK_STATUS_FMT, PW_LOG_STATUS(link_status));
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
