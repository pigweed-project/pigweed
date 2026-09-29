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

#include "pw_rpc2/internal/serialize.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "pw_buf/buf.h"
#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::ConstBuf;
using ::pw::ConstByteSpan;
using ::pw::OkStatus;
using ::pw::Result;
using ::pw::Status;
using ::pw::StatusWithSize;
using ::pw::rpc2::internal::Deserialize;
using ::pw::rpc2::internal::MaxEncodedSize;
using ::pw::rpc2::internal::Serialize;
using ::pw::rpc2::internal::SerializerFor;

struct TestSerializer {
  template <typename T>
  static Result<T> Deserialize(ConstByteSpan source) {
    if (source.size() != 1) {
      return Status::DataLoss();
    }
    return T{static_cast<uint8_t>(source[0])};
  }
};

struct TestMessage {
  using Serializer = TestSerializer;
  uint8_t value;
};

static_assert(std::is_same_v<SerializerFor<TestMessage>::type, TestSerializer>);

TEST(Serialize, ConstBufMaxEncodedSizeIsExactSize) {
  std::byte data[5] = {};
  ConstBuf buf = ConstBuf::Unowned(data);
  EXPECT_EQ(MaxEncodedSize(buf), sizeof(data));
}

TEST(Serialize, ConstBufCopiesIntoDestination) {
  const std::byte data[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
  ConstBuf buf = ConstBuf::Unowned(data);

  std::byte destination[4] = {};
  StatusWithSize result = Serialize(buf, destination);
  ASSERT_EQ(OkStatus(), result.status());
  EXPECT_EQ(result.size(), 3u);
  EXPECT_EQ(destination[0], std::byte{1});
  EXPECT_EQ(destination[2], std::byte{3});
  EXPECT_EQ(destination[3], std::byte{0});
}

TEST(Serialize, ConstBufDestinationTooSmall) {
  const std::byte data[3] = {};
  ConstBuf buf = ConstBuf::Unowned(data);

  std::byte destination[2] = {};
  EXPECT_EQ(Status::ResourceExhausted(), Serialize(buf, destination).status());
}

TEST(Serialize, DeserializeFromSpanUsesSerializer) {
  const std::byte data[1] = {std::byte{11}};
  Result<TestMessage> result = Deserialize<TestMessage>(ConstByteSpan(data));
  ASSERT_EQ(OkStatus(), result.status());
  EXPECT_EQ(result->value, 11u);
}

TEST(Serialize, DeserializePropagatesErrors) {
  const std::byte data[2] = {};
  EXPECT_EQ(Status::DataLoss(),
            Deserialize<TestMessage>(ConstByteSpan(data)).status());
}

}  // namespace
