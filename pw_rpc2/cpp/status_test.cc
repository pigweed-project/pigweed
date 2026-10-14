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

#include "pw_rpc2/status.h"

#include <iterator>
#include <type_traits>

#include "pw_enum/to_string.h"
#include "pw_enum/traits.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::rpc2::RpcCode;
using ::pw::rpc2::RpcResult;
using ::pw::rpc2::RpcStatus;

// `RpcStatus` is a single `RpcCode`, so it is cheap to return and store.
static_assert(sizeof(RpcStatus) == 1);
static_assert(std::is_same_v<RpcStatus::Code, RpcCode>);
static_assert(std::is_trivially_copyable_v<RpcStatus>);

static_assert(RpcStatus().ok());
static_assert(RpcStatus().code() == RpcCode::kOk);
static_assert(RpcStatus(RpcCode::kOk).ok());
static_assert(!RpcStatus::EndOfStream().ok());

static_assert(RpcStatus::Aborted() == RpcCode::kAborted);
static_assert(RpcCode::kAborted == RpcStatus::Aborted());
static_assert(RpcStatus::Aborted() != RpcCode::kCancelled);
static_assert(RpcStatus::Aborted() == RpcStatus(RpcCode::kAborted));
static_assert(RpcStatus::Aborted() != RpcStatus::Cancelled());

static_assert(RpcStatus::Aborted().IsAborted());
static_assert(!RpcStatus::Aborted().IsCancelled());
static_assert(!RpcStatus().IsAborted());

TEST(RpcStatus, DefaultsToOk) {
  const RpcStatus status;
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(status.code(), RpcCode::kOk);
  EXPECT_EQ(status, RpcStatus(RpcCode::kOk));
  EXPECT_STREQ(status.str(), "OK");
}

TEST(RpcStatus, FactoriesAndQueries) {
  struct Case {
    RpcStatus status;
    bool (RpcStatus::*is_code)() const;
    RpcCode code;
  };
  constexpr Case kCases[] = {
      {RpcStatus::EndOfStream(),
       &RpcStatus::IsEndOfStream,
       RpcCode::kEndOfStream},
      {RpcStatus::Cancelled(), &RpcStatus::IsCancelled, RpcCode::kCancelled},
      {RpcStatus::Closed(), &RpcStatus::IsClosed, RpcCode::kClosed},
      {RpcStatus::WriteClosed(),
       &RpcStatus::IsWriteClosed,
       RpcCode::kWriteClosed},
      {RpcStatus::Disconnected(),
       &RpcStatus::IsDisconnected,
       RpcCode::kDisconnected},
      {RpcStatus::ResourceExhausted(),
       &RpcStatus::IsResourceExhausted,
       RpcCode::kResourceExhausted},
      {RpcStatus::Unavailable(),
       &RpcStatus::IsUnavailable,
       RpcCode::kUnavailable},
      {RpcStatus::Aborted(), &RpcStatus::IsAborted, RpcCode::kAborted},
      {RpcStatus::Unimplemented(),
       &RpcStatus::IsUnimplemented,
       RpcCode::kUnimplemented},
      {RpcStatus::MessageTooLarge(),
       &RpcStatus::IsMessageTooLarge,
       RpcCode::kMessageTooLarge},
      {RpcStatus::DataError(), &RpcStatus::IsDataError, RpcCode::kDataError},
      {RpcStatus::ProtocolError(),
       &RpcStatus::IsProtocolError,
       RpcCode::kProtocolError},
  };
  // Every error code has a factory and an Is*() function.
  static_assert(std::size(kCases) + 1 ==
                pw::EnumTraits<RpcCode>::kDistinctValueCount);

  for (const Case& test : kCases) {
    const char* name = pw::EnumToString(test.code);
    EXPECT_FALSE(test.status.ok()) << name;
    EXPECT_EQ(test.status.code(), test.code) << name;
    EXPECT_FALSE((RpcStatus().*test.is_code)()) << name;

    // Each Is*() function matches only its own code.
    for (const Case& other : kCases) {
      EXPECT_EQ((test.status.*other.is_code)(), test.code == other.code)
          << name << " vs. " << pw::EnumToString(other.code);
    }
  }
}

TEST(RpcStatus, EveryCode) {
  struct Case {
    RpcCode code;
    const char* name;
  };
  constexpr Case kCases[] = {
      {RpcCode::kOk, "OK"},
      {RpcCode::kEndOfStream, "END_OF_STREAM"},
      {RpcCode::kCancelled, "CANCELLED"},
      {RpcCode::kClosed, "CLOSED"},
      {RpcCode::kWriteClosed, "WRITE_CLOSED"},
      {RpcCode::kDisconnected, "DISCONNECTED"},
      {RpcCode::kResourceExhausted, "RESOURCE_EXHAUSTED"},
      {RpcCode::kUnavailable, "UNAVAILABLE"},
      {RpcCode::kAborted, "ABORTED"},
      {RpcCode::kUnimplemented, "UNIMPLEMENTED"},
      {RpcCode::kMessageTooLarge, "MESSAGE_TOO_LARGE"},
      {RpcCode::kDataError, "DATA_ERROR"},
      {RpcCode::kProtocolError, "PROTOCOL_ERROR"},
  };
  // Every code is covered.
  static_assert(std::size(kCases) ==
                pw::EnumTraits<RpcCode>::kDistinctValueCount);

  for (const Case& test : kCases) {
    const RpcStatus status(test.code);
    EXPECT_STREQ(status.str(), test.name);
    EXPECT_STREQ(pw::EnumToString(test.code), test.name);

    // Only `kOk` is success; every other code is an error.
    EXPECT_EQ(status.ok(), test.code == RpcCode::kOk) << test.name;
    EXPECT_EQ(status.code(), test.code) << test.name;
    EXPECT_EQ(status, test.code) << test.name;
    EXPECT_EQ(status == RpcStatus(), test.code == RpcCode::kOk) << test.name;
  }
}

TEST(RpcStatus, Update) {
  RpcStatus status;
  status.Update(RpcStatus::Disconnected());
  EXPECT_EQ(status, RpcStatus::Disconnected());

  // Update keeps the first error.
  status.Update(RpcStatus::ProtocolError());
  EXPECT_EQ(status, RpcStatus::Disconnected());
}

TEST(RpcResult, HoldsValueOrStatus) {
  const RpcResult<int> value = 42;
  ASSERT_TRUE(value.ok());
  EXPECT_EQ(*value, 42);
  PW_TEST_EXPECT_OK(value.status());

  const RpcResult<int> error = RpcStatus::Unavailable();
  EXPECT_FALSE(error.ok());
  EXPECT_EQ(error.status(), RpcStatus::Unavailable());

  const RpcResult<int> from_code(RpcCode::kUnimplemented);
  EXPECT_EQ(from_code.status(), RpcCode::kUnimplemented);
}

}  // namespace
