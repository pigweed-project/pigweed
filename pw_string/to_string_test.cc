// Copyright 2019 The Pigweed Authors
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

#include "pw_string/to_string.h"

#include <array>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <string>

#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/status_base.h"
#include "pw_string/internal/config.h"
#include "pw_string/type_to_string.h"
#include "pw_unit_test/framework.h"

namespace pw {

struct CustomType {
  unsigned a;
  unsigned b;

  static constexpr const char* kToString = "This is a CustomType";

  CustomType() : a(0), b(0) {}

  // Non-copyable to verify that ToString doesn't copy it.
  CustomType(const CustomType&) = delete;
  CustomType& operator=(const CustomType&) = delete;
};

StatusWithSize ToString(const CustomType&, span<char> buffer) {
  int result =
      std::snprintf(buffer.data(), buffer.size(), CustomType::kToString);
  if (result < 0) {
    return StatusWithSize::Unknown();
  }
  if (static_cast<size_t>(result) < buffer.size()) {
    return StatusWithSize(result);
  }
  return StatusWithSize::ResourceExhausted(buffer.empty() ? 0u
                                                          : buffer.size() - 1);
}

namespace {

char buffer[128] = {};
char expected[128] = {};

TEST(ToString, Bool) {
  const volatile bool b = true;
  EXPECT_EQ(4u, ToString(b, buffer).size());
  EXPECT_STREQ("true", buffer);
  EXPECT_EQ(5u, ToString(false, buffer).size());
  EXPECT_STREQ("false", buffer);
}

TEST(ToString, Char) {
  EXPECT_EQ(1u, ToString('%', buffer).size());
  EXPECT_STREQ("%", buffer);
}

template <typename T>
constexpr T kInteger = 127;

TEST(ToString, Integer_AllTypesAreSupported) {
  EXPECT_EQ(3u, ToString(kInteger<unsigned char>, buffer).size());
  EXPECT_STREQ("127", buffer);
  EXPECT_EQ(3u, ToString(kInteger<signed char>, buffer).size());
  EXPECT_STREQ("127", buffer);
  EXPECT_EQ(3u, ToString(kInteger<unsigned short>, buffer).size());
  EXPECT_STREQ("127", buffer);
  EXPECT_EQ(3u, ToString(kInteger<signed short>, buffer).size());
  EXPECT_STREQ("127", buffer);
  EXPECT_EQ(3u, ToString(kInteger<unsigned int>, buffer).size());
  EXPECT_STREQ("127", buffer);
  EXPECT_EQ(3u, ToString(kInteger<signed int>, buffer).size());
  EXPECT_STREQ("127", buffer);
  EXPECT_EQ(3u, ToString(kInteger<unsigned long>, buffer).size());
  EXPECT_STREQ("127", buffer);
  EXPECT_EQ(3u, ToString(kInteger<signed long>, buffer).size());
  EXPECT_STREQ("127", buffer);
  EXPECT_EQ(3u, ToString(kInteger<unsigned long long>, buffer).size());
  EXPECT_STREQ("127", buffer);
  EXPECT_EQ(3u, ToString(kInteger<signed long long>, buffer).size());
  EXPECT_STREQ("127", buffer);
}

TEST(ToString, ScopedEnum) {
  enum class MyEnum : short { kLuckyNumber = 8 };

  auto result = ToString(MyEnum::kLuckyNumber, buffer);
  EXPECT_EQ(1u, result.size());
  EXPECT_EQ(OkStatus(), result.status());
  EXPECT_STREQ("8", buffer);
}

TEST(ToString, Integer_EmptyBuffer_WritesNothing) {
  auto result = ToString(-1234, span(buffer, 0));
  EXPECT_EQ(0u, result.size());
  EXPECT_EQ(Status::ResourceExhausted(), result.status());
}

TEST(ToString, Integer_BufferTooSmall_WritesNullTerminator) {
  auto result = ToString(-1234, span(buffer, 5));
  EXPECT_EQ(0u, result.size());
  EXPECT_FALSE(result.ok());
  EXPECT_STREQ("", buffer);
}

template <typename T>
void TestFloatingPointType() {
  static constexpr T kZero = 0.0;
  static constexpr T kValue = static_cast<T>(33.444);
  static constexpr T kInf = std::numeric_limits<T>::infinity();
  static constexpr T kNegativeInf = -std::numeric_limits<T>::infinity();
  static constexpr T kNan = std::numeric_limits<T>::quiet_NaN();

  EXPECT_EQ(3u, ToString(kInf, buffer).size());
  EXPECT_STREQ("inf", buffer);
  EXPECT_EQ(4u, ToString(kNegativeInf, buffer).size());
  EXPECT_STREQ("-inf", buffer);

  if (string::internal::config::kEnableDecimalFloatExpansion) {
    EXPECT_EQ(5u, ToString(kZero, buffer).size());
    EXPECT_STREQ("0.000", buffer);
    EXPECT_EQ(6u, ToString(kValue, buffer).size());
    EXPECT_STREQ("33.444", buffer);
    EXPECT_EQ(3u, ToString(kNan, buffer).size());
    EXPECT_STREQ("nan", buffer);
  } else {
    EXPECT_EQ(1u, ToString(kZero, buffer).size());
    EXPECT_STREQ("0", buffer);
    EXPECT_EQ(2u, ToString(kValue, buffer).size());
    EXPECT_STREQ("33", buffer);
    EXPECT_EQ(3u, ToString(kNan, buffer).size());
    EXPECT_STREQ("NaN", buffer);
  }
}

TEST(ToString, Float) { TestFloatingPointType<float>(); }
TEST(ToString, Double) { TestFloatingPointType<double>(); }
TEST(ToString, LongDouble) { TestFloatingPointType<long double>(); }

TEST(ToString, Pointer_NonNull_WritesValue) {
  CustomType custom;
  const size_t length =
      static_cast<size_t>(std::snprintf(expected,
                                        sizeof(expected),
                                        "%" PRIxPTR,
                                        reinterpret_cast<intptr_t>(&custom)));

  EXPECT_EQ(length, ToString(&custom, buffer).size());
  EXPECT_STREQ(expected, buffer);
  EXPECT_EQ(length, ToString(static_cast<void*>(&custom), buffer).size());
  EXPECT_STREQ(expected, buffer);
  EXPECT_EQ(1u, ToString(reinterpret_cast<int*>(4), buffer).size());
  EXPECT_STREQ("4", buffer);
}

TEST(ToString, Pointer_Nullptr_WritesNull) {
  EXPECT_EQ(string::kNullPointerString.size(),
            ToString(nullptr, buffer).size());
  EXPECT_EQ(string::kNullPointerString, buffer);
}

TEST(ToString, Pointer_NullValuedPointer_WritesNull) {
  EXPECT_EQ(string::kNullPointerString.size(),
            ToString(static_cast<const CustomType*>(nullptr), buffer).size());
  EXPECT_EQ(string::kNullPointerString, buffer);
}

TEST(ToString, Pointer_NullValuedCString_WritesNull) {
  EXPECT_EQ(string::kNullPointerString.size(),
            ToString(static_cast<char*>(nullptr), buffer).size());
  EXPECT_EQ(string::kNullPointerString, buffer);

  EXPECT_EQ(string::kNullPointerString.size(),
            ToString(static_cast<const char*>(nullptr), buffer).size());
  EXPECT_EQ(string::kNullPointerString, buffer);
}

TEST(ToString, String_Literal) {
  EXPECT_EQ(0u, ToString("", buffer).size());
  EXPECT_STREQ("", buffer);
  EXPECT_EQ(5u, ToString("hello", buffer).size());
  EXPECT_STREQ("hello", buffer);
}

TEST(ToString, String_Pointer) {
  EXPECT_EQ(0u, ToString(static_cast<const char*>(""), buffer).size());
  EXPECT_STREQ("", buffer);
  EXPECT_EQ(5u, ToString(static_cast<const char*>("hello"), buffer).size());
  EXPECT_STREQ("hello", buffer);
}

TEST(ToString, String_MutableBuffer) {
  char chars[] = {'C', 'o', 'o', 'l', '\0'};
  EXPECT_EQ(sizeof(chars) - 1, ToString(chars, buffer).size());
  EXPECT_STREQ("Cool", buffer);
}

TEST(ToString, String_MutablePointer) {
  char chars[] = {'b', 'o', 'o', 'l', '\0'};
  EXPECT_EQ(sizeof(chars) - 1,
            ToString(static_cast<char*>(chars), buffer).size());
  EXPECT_STREQ("bool", buffer);
}

TEST(ToString, Object) {
  CustomType custom;
  EXPECT_EQ(std::strlen(CustomType::kToString),
            ToString(custom, buffer).size());
  EXPECT_STREQ(CustomType::kToString, buffer);
}

enum Foo : uint8_t {
  BAR = 32,
  BAZ = 100,
};

TEST(ToString, Enum) {
  EXPECT_EQ(2u, ToString(Foo::BAR, buffer).size());
  EXPECT_STREQ("32", buffer);

  EXPECT_EQ(3u, ToString(Foo::BAZ, buffer).size());
  EXPECT_STREQ("100", buffer);
}

TEST(ToString, Status) {
  EXPECT_EQ(2u, ToString(Status(), buffer).size());
  EXPECT_STREQ(Status().str(), buffer);
}

TEST(ToString, StatusCode) {
  EXPECT_EQ(sizeof("UNAVAILABLE") - 1,
            ToString(Status::Unavailable(), buffer).size());
  EXPECT_STREQ("UNAVAILABLE", buffer);
}

enum class CustomCode : uint8_t { kOk, kTimeout };

constexpr const char* PwEnumToString(CustomCode code) {
  return code == CustomCode::kOk ? "Ok" : "Timeout";
}

PW_STATUS_TYPE(CustomStatus, CustomCode::kOk);

class [[nodiscard]] IntCodeStatus final
    : public StatusBase<IntCodeStatus, 200> {
 public:
  constexpr IntCodeStatus() = default;
  explicit constexpr IntCodeStatus(int code) : StatusBase(code) {}
};

TEST(ToString, CustomStatusWithEnumCode) {
  EXPECT_EQ(7u, ToString(CustomStatus(CustomCode::kTimeout), buffer).size());
  EXPECT_STREQ("Timeout", buffer);
  EXPECT_EQ(2u, ToString(CustomStatus(), buffer).size());
  EXPECT_STREQ("Ok", buffer);
}

TEST(ToString, CustomStatusWithIntCode) {
  EXPECT_EQ(3u, ToString(IntCodeStatus(404), buffer).size());
  EXPECT_STREQ("404", buffer);
}

TEST(ToString, StdArrayAsBuffer) {
  std::array<char, 128> test_buffer;
  EXPECT_EQ(5u, ToString(false, test_buffer).size());
  EXPECT_STREQ("false", test_buffer.data());
  EXPECT_EQ(2u, ToString("Hi", test_buffer).size());
  EXPECT_STREQ("Hi", test_buffer.data());
  EXPECT_EQ(string::kNullPointerString.size(),
            ToString(static_cast<void*>(nullptr), test_buffer).size());
  EXPECT_EQ(string::kNullPointerString, test_buffer.data());
}

TEST(ToString, StringView) {
  std::string_view view = "cool";
  EXPECT_EQ(4u, ToString(view, buffer).size());
  EXPECT_STREQ("cool", buffer);
}

TEST(ToString, StringView_TooSmall_Truncates) {
  std::string_view view = "kale!";
  EXPECT_EQ(3u, ToString(view, span(buffer, 4)).size());
  EXPECT_STREQ("kal", buffer);
}

TEST(ToString, StringView_EmptyBuffer_WritesNothing) {
  constexpr char kOriginal[] = {'@', '#', '$', '%'};
  char test_buffer[sizeof(kOriginal)];
  std::memcpy(test_buffer, kOriginal, sizeof(kOriginal));

  EXPECT_EQ(0u,
            ToString(std::string_view("Hello!"), span(test_buffer, 0)).size());
  ASSERT_EQ(0, std::memcmp(kOriginal, test_buffer, sizeof(kOriginal)));
}

TEST(ToString, StdString) {
  EXPECT_EQ(5u, ToString(std::string("Whoa!"), buffer).size());
  EXPECT_STREQ("Whoa!", buffer);

  EXPECT_EQ(0u, ToString(std::string(), buffer).size());
  EXPECT_STREQ("", buffer);
}

TEST(ToString, StdNullopt) {
  EXPECT_EQ(12u, ToString(std::nullopt, buffer).size());
  EXPECT_STREQ("std::nullopt", buffer);
}

TEST(ToString, StdOptionalWithoutValue) {
  std::optional<uint16_t> v = std::nullopt;
  EXPECT_EQ(12u, ToString(v, buffer).size());
  EXPECT_STREQ("std::nullopt", buffer);
}

TEST(ToString, StdOptionalWithValue) {
  std::optional<uint16_t> v = 5;
  EXPECT_EQ(1u, ToString(v, buffer).size());
  EXPECT_STREQ("5", buffer);
}

TEST(ToString, ResultWithNonOkStatus) {
  Result<int> v = Status::ResourceExhausted();
  EXPECT_EQ(18u, ToString(v, buffer).size());
  EXPECT_STREQ("RESOURCE_EXHAUSTED", buffer);
}

TEST(ToString, ResultWithValue) {
  Result<int> v = 27;
  EXPECT_EQ(6u, ToString(v, buffer).size());
  EXPECT_STREQ("Ok(27)", buffer);
}

TEST(ToString, VoidResult) {
  Result<void> ok = OkStatus();
  EXPECT_EQ(4u, ToString(ok, buffer).size());
  EXPECT_STREQ("Ok()", buffer);

  Result<void> error = Status::NotFound();
  EXPECT_EQ(9u, ToString(error, buffer).size());
  EXPECT_STREQ("NOT_FOUND", buffer);
}

TEST(ToString, ResultWithCustomStatus) {
  Result<int, CustomStatus> ok(5);
  EXPECT_EQ(5u, ToString(ok, buffer).size());
  EXPECT_STREQ("Ok(5)", buffer);

  Result<int, CustomStatus> error{CustomStatus(CustomCode::kTimeout)};
  EXPECT_EQ(7u, ToString(error, buffer).size());
  EXPECT_STREQ("Timeout", buffer);

  Result<void, CustomStatus> void_error{CustomStatus(CustomCode::kTimeout)};
  EXPECT_EQ(7u, ToString(void_error, buffer).size());
  EXPECT_STREQ("Timeout", buffer);
}

TEST(ToString, ResultOfStatus) {
  Result<Status> ok_val = OkStatus();
  EXPECT_EQ(6u, ToString(ok_val, buffer).size());
  EXPECT_STREQ("Ok(OK)", buffer);

  Result<Status> not_found_val = Status::NotFound();
  EXPECT_EQ(13u, ToString(not_found_val, buffer).size());
  EXPECT_STREQ("Ok(NOT_FOUND)", buffer);

  Result<Status> error;
  error.reset<PW_STATUS_RESOURCE_EXHAUSTED>();
  EXPECT_EQ(18u, ToString(error, buffer).size());
  EXPECT_STREQ("RESOURCE_EXHAUSTED", buffer);
}

TEST(ToString, EmptyArrayUsesIterableFormat) {
  std::array<int, 0> v = {};
  EXPECT_EQ(2u, ToString(v, buffer).size());
  EXPECT_STREQ("[]", buffer);
}

TEST(ToString, ArrayUsesIterableFormat) {
  std::array<int, 3> v = {1, 2, 3};
  EXPECT_EQ(9u, ToString(v, buffer).size());
  EXPECT_STREQ("[1, 2, 3]", buffer);
}

TEST(ToString, SpanUsesIterableFormat) {
  std::array<int, 3> arr = {1, 2, 3};
  span v(arr);
  EXPECT_EQ(9u, ToString(v, buffer).size());
  EXPECT_STREQ("[1, 2, 3]", buffer);
}

TEST(ToString, ArrayOfStringsUsesIterableFormat) {
  std::array<const char*, 3> v({"foo", "bar", "baz"});
  EXPECT_EQ(15u, ToString(v, buffer).size());
  EXPECT_STREQ("[foo, bar, baz]", buffer);
}

TEST(ToString, TimeVerySmallEndsInNsOrUnrepresentable) {
  size_t size =
      ToString(std::chrono::system_clock::time_point::min(), buffer).size();
  ASSERT_GE(size, 3u);
  if (std::strcmp("ns", &buffer[size - 2]) != 0) {
    EXPECT_STREQ("An unrepresentably long time ago (>292 years).", buffer);
  }
}

TEST(ToString, TimeVeryBigEndsInNsOrUnrepresentable) {
  size_t size =
      ToString(std::chrono::system_clock::time_point::max(), buffer).size();
  ASSERT_GE(size, 3u);
  if (std::strcmp("ns", &buffer[size - 2]) != 0) {
    EXPECT_STREQ("An unrepresentably long time in the future (>292 years).",
                 buffer);
  }
}

TEST(ToString, TimeJustRight) {
  EXPECT_TRUE(ToString(std::chrono::system_clock::time_point(), buffer).ok());
  EXPECT_STREQ("0ns", buffer);
}

int64_t sensor_clock_tick_count;

struct SensorClock {
  using rep = int64_t;
  using period = std::ratio<1, 32768>;
  using duration = std::chrono::duration<rep, period>;
  using time_point = std::chrono::time_point<SensorClock>;
  static time_point now() noexcept {
    return time_point(duration(sensor_clock_tick_count));
  }
};

TEST(ToString, TimeSensorClockZero) {
  sensor_clock_tick_count = 0;
  EXPECT_TRUE(ToString(SensorClock::now(), buffer).ok());
  EXPECT_STREQ("0ns", buffer);
}

TEST(ToString, TimeSensorClockMax) {
  sensor_clock_tick_count = std::numeric_limits<int64_t>::max();
  EXPECT_TRUE(ToString(SensorClock::now(), buffer).ok());
  EXPECT_STREQ("An unrepresentably long time in the future (>292 years).",
               buffer);
}

TEST(ToString, TimeSensorClockMin) {
  sensor_clock_tick_count = std::numeric_limits<int64_t>::min();
  EXPECT_TRUE(ToString(SensorClock::now(), buffer).ok());
  EXPECT_STREQ("An unrepresentably long time ago (>292 years).", buffer);
}

TEST(ToString, DurationEndsInNs) {
  EXPECT_TRUE(ToString(std::chrono::nanoseconds::zero(), buffer).ok());
  EXPECT_STREQ("0ns", buffer);
}

}  // namespace
}  // namespace pw
