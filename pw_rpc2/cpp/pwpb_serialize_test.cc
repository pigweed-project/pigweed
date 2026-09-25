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

#include "pw_rpc2/pwpb_serialize.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "pw_buf/buf.h"
#include "pw_protobuf/encoder.h"
#include "pw_protobuf/internal/codegen.h"
#include "pw_protobuf/serialized_size.h"
#include "pw_protobuf/stream_decoder.h"
#include "pw_protobuf/wire_format.h"
#include "pw_span/span.h"
#include "pw_status/status_with_size.h"
#include "pw_unit_test/framework.h"

namespace pw::rpc2 {
namespace {

namespace TestRequest {

struct Message {
  int64_t integer;
  uint32_t status_code;
};

inline constexpr protobuf::internal::MessageField kMessageFieldsTable[] = {
    {1,
     protobuf::WireType::kVarint,
     sizeof(int64_t),
     protobuf::internal::VarintType::kNormal,
     /*is_string=*/false,
     /*is_fixed_size=*/false,
     /*is_repeated=*/false,
     /*is_optional=*/false,
     protobuf::internal::CallbackType::kNone,
     offsetof(Message, integer),
     sizeof(Message::integer),
     nullptr},
    {2,
     protobuf::WireType::kVarint,
     sizeof(uint32_t),
     protobuf::internal::VarintType::kUnsigned,
     /*is_string=*/false,
     /*is_fixed_size=*/false,
     /*is_repeated=*/false,
     /*is_optional=*/false,
     protobuf::internal::CallbackType::kNone,
     offsetof(Message, status_code),
     sizeof(Message::status_code),
     nullptr},
};

inline constexpr span<const protobuf::internal::MessageField> kMessageFields =
    kMessageFieldsTable;

inline constexpr size_t kMaxEncodedSizeBytesWithoutValues =
    protobuf::SizeOfFieldInt64(1) + protobuf::SizeOfFieldUint32(2);

}  // namespace TestRequest

namespace CallbackMessage {

class StreamEncoder : public protobuf::StreamEncoder {
 public:
  using protobuf::StreamEncoder::StreamEncoder;
  Status WriteName(std::string_view value) { return WriteString(1, value); }
};

class StreamDecoder : public protobuf::StreamDecoder {
 public:
  using protobuf::StreamDecoder::StreamDecoder;
};

struct Message {
  protobuf::Callback<StreamEncoder, StreamDecoder> name;
};

inline constexpr protobuf::internal::MessageField kMessageFieldsTable[] = {
    {1,
     protobuf::WireType::kDelimited,
     sizeof(char),
     static_cast<protobuf::internal::VarintType>(0),
     /*is_string=*/true,
     /*is_fixed_size=*/false,
     /*is_repeated=*/false,
     /*is_optional=*/false,
     protobuf::internal::CallbackType::kSingleField,
     offsetof(Message, name),
     sizeof(Message::name),
     nullptr},
};

inline constexpr span<const protobuf::internal::MessageField> kMessageFields =
    kMessageFieldsTable;

inline constexpr size_t kMaxEncodedSizeBytesWithoutValues =
    protobuf::SizeOfDelimitedFieldWithoutValue(1);

}  // namespace CallbackMessage

namespace ParentWithCallbackChild {

struct Message {
  uint32_t id;
  CallbackMessage::Message child;
};

inline constexpr protobuf::internal::MessageField kMessageFieldsTable[] = {
    {1,
     protobuf::WireType::kVarint,
     sizeof(uint32_t),
     protobuf::internal::VarintType::kUnsigned,
     /*is_string=*/false,
     /*is_fixed_size=*/false,
     /*is_repeated=*/false,
     /*is_optional=*/false,
     protobuf::internal::CallbackType::kNone,
     offsetof(Message, id),
     sizeof(Message::id),
     nullptr},
    {2,
     protobuf::WireType::kDelimited,
     0,
     static_cast<protobuf::internal::VarintType>(0),
     /*is_string=*/false,
     /*is_fixed_size=*/false,
     /*is_repeated=*/false,
     /*is_optional=*/false,
     protobuf::internal::CallbackType::kNone,
     offsetof(Message, child),
     sizeof(Message::child),
     &CallbackMessage::kMessageFields},
};

inline constexpr span<const protobuf::internal::MessageField> kMessageFields =
    kMessageFieldsTable;

inline constexpr size_t kMaxEncodedSizeBytesWithoutValues =
    protobuf::SizeOfFieldUint32(1) +
    protobuf::SizeOfDelimitedFieldWithoutValue(2) +
    CallbackMessage::kMaxEncodedSizeBytesWithoutValues;

}  // namespace ParentWithCallbackChild

using TestRequestSerde = PwpbSerde<&TestRequest::kMessageFields>;
using TestRequestSerializer =
    PwpbSerializer<&TestRequest::kMessageFields,
                   TestRequest::kMaxEncodedSizeBytesWithoutValues>;
using CallbackMessageSerializer =
    PwpbSerializer<&CallbackMessage::kMessageFields,
                   CallbackMessage::kMaxEncodedSizeBytesWithoutValues>;
using ParentWithCallbackChildSerializer =
    PwpbSerializer<&ParentWithCallbackChild::kMessageFields,
                   ParentWithCallbackChild::kMaxEncodedSizeBytesWithoutValues>;

constexpr TestRequest::Message kProto{.integer = 42, .status_code = 0};

TEST(PwpbSerialize, SerializeAndDeserializeWithPwBuf) {
  std::byte backing_buffer[32] = {};
  Buf buf = Buf::Unowned(backing_buffer, sizeof(backing_buffer));

  StatusWithSize result = TestRequestSerde::Serialize(kProto, buf);
  EXPECT_EQ(OkStatus(), result.status());
  EXPECT_EQ(2u, result.size());

  Buf sliced_buf = Buf::Unowned(backing_buffer, result.size());
  auto decoded =
      TestRequestSerde::Deserialize<TestRequest::Message>(sliced_buf);
  ASSERT_EQ(OkStatus(), decoded.status());
  EXPECT_EQ(42, decoded->integer);
  EXPECT_EQ(0u, decoded->status_code);
}

TEST(PwpbSerialize, SerializeAndDeserializeWithConstBuf) {
  std::byte backing_buffer[32] = {};
  Buf buf = Buf::Unowned(backing_buffer, sizeof(backing_buffer));

  StatusWithSize result = TestRequestSerde::Serialize(kProto, buf);
  EXPECT_EQ(OkStatus(), result.status());

  ConstBuf const_buf(Buf::Unowned(backing_buffer, result.size()));
  auto decoded = TestRequestSerde::Deserialize<TestRequest::Message>(const_buf);
  ASSERT_EQ(OkStatus(), decoded.status());
  EXPECT_EQ(42, decoded->integer);
  EXPECT_EQ(0u, decoded->status_code);

  auto raw_decoded = Deserialize<ConstBuf>(std::move(const_buf));
  ASSERT_EQ(OkStatus(), raw_decoded.status());
  EXPECT_EQ(result.size(), raw_decoded->size());
}

TEST(PwpbSerialize, BufferTooSmall) {
  std::byte backing_buffer[1] = {};
  Buf buf = Buf::Unowned(backing_buffer, sizeof(backing_buffer));
  StatusWithSize result = TestRequestSerde::Serialize(kProto, buf);
  EXPECT_EQ(Status::ResourceExhausted(), result.status());
}

TEST(PwpbSerialize, MaxEncodedSizeStaticForBoundedFields) {
  static_assert(internal::HasNoCallbacks(&TestRequest::kMessageFields));
  constexpr size_t kSize = TestRequestSerializer::MaxEncodedSize(kProto);
  EXPECT_EQ(kSize, TestRequest::kMaxEncodedSizeBytesWithoutValues);
}

TEST(PwpbSerialize, MaxEncodedSizeDynamicForCallbackField) {
  static_assert(!internal::HasNoCallbacks(&CallbackMessage::kMessageFields));

  static constexpr std::string_view kPayload =
      "hello from a callback field that exceeds "
      "kMaxEncodedSizeBytesWithoutValues";
  CallbackMessage::Message msg{};
  msg.name.SetEncoder([](CallbackMessage::StreamEncoder& encoder) {
    return encoder.WriteName(kPayload);
  });

  const size_t max_size = CallbackMessageSerializer::MaxEncodedSize(msg);
  EXPECT_GT(max_size, kPayload.size());
  EXPECT_GT(max_size, CallbackMessage::kMaxEncodedSizeBytesWithoutValues);

  std::byte backing_buffer[128] = {};
  ASSERT_LE(max_size, sizeof(backing_buffer));
  StatusWithSize encoded =
      CallbackMessageSerializer::Serialize(msg, span(backing_buffer, max_size));
  EXPECT_EQ(OkStatus(), encoded.status());
  EXPECT_EQ(encoded.size(), 2u + kPayload.size());
}

TEST(PwpbSerialize, MaxEncodedSizeDynamicForNestedCallbackField) {
  static_assert(
      !internal::HasNoCallbacks(&ParentWithCallbackChild::kMessageFields));

  static constexpr std::string_view kPayload =
      "nested submessage callback payload";
  ParentWithCallbackChild::Message msg{};
  msg.id = 7;
  msg.child.name.SetEncoder([](CallbackMessage::StreamEncoder& encoder) {
    return encoder.WriteName(kPayload);
  });

  const size_t max_size =
      ParentWithCallbackChildSerializer::MaxEncodedSize(msg);
  EXPECT_GT(max_size, kPayload.size());
  EXPECT_GT(max_size,
            ParentWithCallbackChild::kMaxEncodedSizeBytesWithoutValues);

  std::byte backing_buffer[128] = {};
  ASSERT_LE(max_size, sizeof(backing_buffer));
  StatusWithSize encoded = ParentWithCallbackChildSerializer::Serialize(
      msg, span(backing_buffer, max_size));
  EXPECT_EQ(OkStatus(), encoded.status());
}

}  // namespace
}  // namespace pw::rpc2
