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

#include "pw_protobuf/buffer_encoder.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_bytes/array.h"
#include "pw_bytes/span.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

namespace pw::protobuf {
namespace {

constexpr uint32_t kTestProtoMagicNumberField = 1;
constexpr uint32_t kTestProtoZiggyField = 2;
constexpr uint32_t kTestProtoCyclesField = 3;
constexpr uint32_t kTestProtoRatioField = 4;
constexpr uint32_t kTestProtoErrorMessageField = 5;
constexpr uint32_t kTestProtoNestedField = 6;

constexpr uint32_t kNestedProtoHelloField = 1;
constexpr uint32_t kNestedProtoIdField = 2;
constexpr uint32_t kNestedProtoPairField = 3;

enum class TestEnum : int32_t {
  kZero = 0,
  kOne = 1,
  kNegativeOne = -1,
};

TEST(BufferEncoder, EncodePrimitivesChecked) {
  // Hand-encoded version of the fields.
  // clang-format off
  constexpr uint8_t kExpectedProto[] = {
    // magic_number [varint k=1] (42)
    0x08, 0x2a,
    // ziggy [varint k=2] (-13 zigzag -> zig-zag encoded)
    // Wait, standard sint32 is zigzag. zig-zag of -13 is 25 (0x19).
    0x10, 0x19,
    // cycles [fixed64 k=3] (0xdeadbeef8badf00d)
    0x19, 0x0d, 0xf0, 0xad, 0x8b, 0xef, 0xbe, 0xad, 0xde,
    // ratio [fixed32 k=4] (1.618034f)
    0x25, 0xbd, 0x1b, 0xcf, 0x3f,
    // error_message [delimited k=5] ("broken")
    0x2a, 0x06, 'b', 'r', 'o', 'k', 'e', 'n',
  };
  // clang-format on

  std::byte encode_buffer[64];
  std::memset(encode_buffer, 0, sizeof(encode_buffer));

  BufferEncoder encoder(encode_buffer);
  auto view = encoder.view();

  EXPECT_EQ(view.WriteUint32(kTestProtoMagicNumberField, 42), OkStatus());
  EXPECT_EQ(view.WriteSint32(kTestProtoZiggyField, -13), OkStatus());
  EXPECT_EQ(view.WriteFixed64(kTestProtoCyclesField, 0xdeadbeef8badf00d),
            OkStatus());
  EXPECT_EQ(view.WriteFloat(kTestProtoRatioField, 1.618034f), OkStatus());
  EXPECT_EQ(view.WriteString(kTestProtoErrorMessageField, "broken"),
            OkStatus());

  EXPECT_EQ(encoder.status(), OkStatus());
  EXPECT_EQ(encoder.size(), sizeof(kExpectedProto));
  EXPECT_EQ(
      std::memcmp(
          encoder.buffer().data(), kExpectedProto, sizeof(kExpectedProto)),
      0);
}

TEST(BufferEncoder, EncodeInsufficientSpaceChecked) {
  std::byte encode_buffer[10];
  BufferEncoder encoder(encode_buffer);
  auto view = encoder.view();

  // 2 bytes.
  EXPECT_EQ(view.WriteUint32(kTestProtoMagicNumberField, 42), OkStatus());
  // 2 bytes.
  EXPECT_EQ(view.WriteSint32(kTestProtoZiggyField, -13), OkStatus());
  // 9 bytes needed (1 tag + 8 content), only 6 left; not enough space!
  EXPECT_EQ(view.WriteFixed64(kTestProtoCyclesField, 0xdeadbeef8badf00d),
            Status::ResourceExhausted());

  // Any further writes should fail.
  EXPECT_EQ(view.WriteFloat(kTestProtoRatioField, 1.618034f),
            Status::ResourceExhausted());
  EXPECT_EQ(encoder.status(), Status::ResourceExhausted());
}

TEST(BufferEncoder, AggregatedBoundsChecking) {
  // Hand-encoded version of the fields.
  // clang-format off
  constexpr uint8_t kExpectedProto[] = {
    0x08, 0x2a,
    0x10, 0x19,
    0x25, 0xbd, 0x1b, 0xcf, 0x3f,
  };
  // clang-format on

  std::byte encode_buffer[64];
  std::memset(encode_buffer, 0, sizeof(encode_buffer));

  BufferEncoder encoder(encode_buffer);
  auto view = encoder.view();

  // Worst-case max encoded size constants:
  constexpr size_t kMagicNumberMaxSize = 1 + kMaxSizeBytesUint32;
  constexpr size_t kZiggyMaxSize = 1 + kMaxSizeBytesSint32;
  constexpr size_t kRatioMaxSize = 1 + kMaxSizeBytesFloat;
  constexpr size_t kTotalSize =
      kMagicNumberMaxSize + kZiggyMaxSize + kRatioMaxSize;

  // Verify space for all fields upfront!
  ASSERT_TRUE(view.EnsureSpace(kTotalSize));

  // Perform unchecked writes!
  view.UncheckedWriteUint32(kTestProtoMagicNumberField, 42);
  view.UncheckedWriteSint32(kTestProtoZiggyField, -13);
  view.UncheckedWriteFloat(kTestProtoRatioField, 1.618034f);

  EXPECT_EQ(encoder.status(), OkStatus());
  EXPECT_EQ(encoder.size(), sizeof(kExpectedProto));
  EXPECT_EQ(
      std::memcmp(
          encoder.buffer().data(), kExpectedProto, sizeof(kExpectedProto)),
      0);
}

TEST(BufferEncoder, NestedMessages) {
  // Hand-encoded double-nested proto.
  // clang-format off
  constexpr auto kExpectedProto = bytes::Array<
    // magic_number [varint k=1] (42)
    0x08, 0x2a,
    // nested header (delimited k=6, length=13 encoded in 2-bytes)
    0x32, 0x8d, 0x00,
      // nested.hello [delimited k=1, length=5] ("world")
      0x0a, 0x05, 'w', 'o', 'r', 'l', 'd',
      // nested.id [varint k=2] (999)
      0x10, 0xe7, 0x07,
      // nested.pair header (delimited k=3, length=0 encoded in 2-bytes)
      0x1a, 0x80, 0x00
  >();
  // clang-format on

  std::byte encode_buffer[64];
  std::memset(encode_buffer, 0, sizeof(encode_buffer));

  BufferEncoder encoder(encode_buffer);
  auto view = encoder.view();

  // Checked / Unchecked nested writing
  EXPECT_EQ(view.WriteUint32(kTestProtoMagicNumberField, 42), OkStatus());

  // Pre-calculate and ensure space
  constexpr size_t kNestedHeaderSize = 1 + 2;  // Tag + 2-byte length prefix
  constexpr size_t kHelloSize = 1 + 1 + 5;     // Tag + length prefix + "world"
  constexpr size_t kIdSize = 1 + kMaxSizeBytesUint32;
  constexpr size_t kPairHeaderSize =
      1 + 2;  // Tag + 2-byte length prefix for pair
  constexpr size_t kTotalNestedSize =
      kNestedHeaderSize + kHelloSize + kIdSize + kPairHeaderSize;

  ASSERT_TRUE(view.EnsureSpace(kTotalNestedSize));

  // Start nested message encoding using the zero-copy view model.
  view.UncheckedWriteTag(kTestProtoNestedField, WireType::kDelimited);
  size_t nested_len_offset = view.UncheckedReserveLength(2);

  // Create a nested view of the encoder state
  BufferEncoderView nested_view = view;

  // Write fields into the nested message
  nested_view.UncheckedWriteString(kNestedProtoHelloField, "world");
  nested_view.UncheckedWriteUint32(kNestedProtoIdField, 999);

  // Double nested
  nested_view.UncheckedWriteTag(kNestedProtoPairField, WireType::kDelimited);
  size_t pair_len_offset = nested_view.UncheckedReserveLength(2);
  nested_view.PatchLength(pair_len_offset);  // close pair (empty)

  // Patch the length of the main nested message
  view.PatchLength(nested_len_offset);

  EXPECT_EQ(encoder.status(), OkStatus());
  EXPECT_EQ(encoder.size(), kExpectedProto.size());
  EXPECT_TRUE(std::equal(
      encoder.buffer().begin(),
      encoder.buffer().begin() + static_cast<std::ptrdiff_t>(encoder.size()),
      kExpectedProto.begin(),
      kExpectedProto.end()));
}

TEST(BufferEncoder, PackedRepeatedVarints) {
  // Hand-encoded packed repeated enums and varints
  // clang-format off
  constexpr uint8_t kExpectedProto[] = {
    // packed enums [delimited k=10, length=2] (kOne, kZero)
    // kOne (1) -> 0x01, kZero (0) -> 0x00
    0x52, 0x02, 0x01, 0x00,
  };
  // clang-format on

  std::byte encode_buffer[64];
  std::memset(encode_buffer, 0, sizeof(encode_buffer));

  BufferEncoder encoder(encode_buffer);
  auto view = encoder.view();

  static constexpr TestEnum enums[] = {TestEnum::kOne, TestEnum::kZero};
  span<const TestEnum> enum_span(enums);

  EXPECT_EQ(view.WritePackedEnum(10, enum_span), OkStatus());

  EXPECT_EQ(encoder.status(), OkStatus());
  EXPECT_EQ(encoder.size(), sizeof(kExpectedProto));
  EXPECT_EQ(
      std::memcmp(
          encoder.buffer().data(), kExpectedProto, sizeof(kExpectedProto)),
      0);
}

TEST(BufferEncoder, PatchLengthBoundsCheck) {
  std::byte encode_buffer[8];

  // 1. Test out of bounds len_offset + size
  {
    BufferEncoder encoder(encode_buffer);
    auto view = encoder.view();
    view.PatchLength(7);
    EXPECT_EQ(encoder.status(), Status::ResourceExhausted());
  }

  // 2. Test offset_ < len_offset + size (invalid state)
  {
    BufferEncoder encoder(encode_buffer);
    auto view = encoder.view();
    view.PatchLength(2);
    EXPECT_EQ(encoder.status(), Status::Internal());
  }
}

TEST(BufferEncoder, PatchLengthOverflowSize1) {
  std::byte encode_buffer[1024];

  // 1. Test overflow for size = 1 (max length 127)
  {
    BufferEncoder encoder(encode_buffer);
    auto view = encoder.view();
    size_t len_offset = view.UncheckedReserveLength(1);

    // Write 128 bytes
    for (int i = 0; i < 128; ++i) {
      view.EnsureSpace(1);
      view.UncheckedWriteBytes("\x00", 1);
    }

    view.PatchLength<1>(len_offset);
    EXPECT_EQ(encoder.status(), Status::OutOfRange());
  }

  // 2. Test max value fits for size = 1 (length 127)
  {
    BufferEncoder encoder(encode_buffer);
    auto view = encoder.view();
    size_t len_offset = view.UncheckedReserveLength(1);

    // Write 127 bytes
    for (int i = 0; i < 127; ++i) {
      view.EnsureSpace(1);
      view.UncheckedWriteBytes("\x00", 1);
    }

    view.PatchLength<1>(len_offset);
    EXPECT_EQ(encoder.status(), OkStatus());
    EXPECT_EQ(encoder.buffer()[len_offset], static_cast<std::byte>(127));
  }
}

TEST(BufferEncoder, PatchLengthOverflowSize2) {
  static std::byte buffer[20000];
  span<std::byte> view_buffer(buffer, sizeof(buffer));

  // 1. Test overflow for size = 2 (max length 16383)
  {
    BufferEncoder encoder(view_buffer);
    auto view = encoder.view();
    size_t len_offset = view.UncheckedReserveLength(2);

    // Write 16384 bytes
    for (int i = 0; i < 16384; ++i) {
      view.EnsureSpace(1);
      view.UncheckedWriteBytes("\x00", 1);
    }

    view.PatchLength(len_offset);
    EXPECT_EQ(encoder.status(), Status::OutOfRange());
  }

  // 2. Test max value fits for size = 2 (length 16383)
  {
    BufferEncoder encoder(view_buffer);
    auto view = encoder.view();
    size_t len_offset = view.UncheckedReserveLength(2);

    // Write 16383 bytes
    for (int i = 0; i < 16383; ++i) {
      view.EnsureSpace(1);
      view.UncheckedWriteBytes("\x00", 1);
    }

    view.PatchLength(len_offset);
    EXPECT_EQ(encoder.status(), OkStatus());
    EXPECT_EQ(encoder.buffer()[len_offset], static_cast<std::byte>(0xFF));
    EXPECT_EQ(encoder.buffer()[len_offset + 1], static_cast<std::byte>(0x7F));
  }
}

TEST(BufferEncoder, NestedMessageEnsureSpaceFailure) {
  std::byte encode_buffer[10];
  BufferEncoder encoder(encode_buffer);
  auto view = encoder.view();

  // Write some tag to take up space (2 bytes)
  EXPECT_EQ(view.WriteUint32(kTestProtoMagicNumberField, 42), OkStatus());
  EXPECT_EQ(encoder.size(), 2u);

  // Try to start a nested message but fail the space check.
  // We want to check for 9 bytes (requires 9, but only 8 left).
  ASSERT_FALSE(view.EnsureSpace(9));
  EXPECT_EQ(encoder.status(), Status::ResourceExhausted());

  // Simulate what the generated code does: even if the check failed,
  // if we still try to use a nested view, it should be in error state.
  BufferEncoderView nested_view = view;
  EXPECT_EQ(nested_view.WriteUint32(kTestProtoZiggyField, 99),
            Status::ResourceExhausted());

  // Verify root encoder status remains failed, and size didn't change.
  EXPECT_EQ(encoder.status(), Status::ResourceExhausted());
  EXPECT_EQ(encoder.size(), 2u);
}

}  // namespace
}  // namespace pw::protobuf
