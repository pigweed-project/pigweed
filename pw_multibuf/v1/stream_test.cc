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

#include "pw_multibuf/v1/stream.h"

#include "pw_bytes/array.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf_private/test_utils.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace pw::multibuf::v1 {
namespace {

using namespace pw::multibuf::v1::test_utils;

constexpr auto kData64 = bytes::Initialized<64>([](size_t i) { return i; });

TEST(MultibufStream, Write_SingleChunkMultibuf_Succeeds) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 128, kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Write(kData64), OkStatus());

  ExpectElementsEqual(buf, kData64);
  buf.DiscardPrefix(kData64.size());
  ExpectElementsAre(buf, std::byte{kPoisonByte});
}

TEST(MultibufStream, Write_SingleChunkMultibuf_ExactSize_Succeeds) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, kData64.size(), kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Write(kData64), OkStatus());

  EXPECT_EQ(buf.size(), kData64.size());
  ExpectElementsEqual(buf, kData64);
}

TEST(MultibufStream, Write_MultiChunkMultibuf_Succeeds) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 24, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 8, kPoisonByte));
  Stream writer(buf);
  ASSERT_EQ(writer.Write(kData64), OkStatus());

  ExpectElementsEqual(buf, kData64);
}

TEST(MultibufStream, Write_MultiChunkMultibuf_OutOfRange) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 8, kPoisonByte));
  Stream writer(buf);
  ASSERT_EQ(writer.Write(kData64), Status::OutOfRange());

  ExpectElementsEqual(buf, span(kData64).first(24));
}

TEST(MultibufStream, Write_EmptyMultibuf_ReturnsOutOfRange) {
  MultiBuf buf;
  Stream writer(buf);
  EXPECT_EQ(writer.Write(kData64), Status::OutOfRange());
}

TEST(MultibufStream, Seek_Empty) {
  MultiBuf buf;
  Stream writer(buf);
  EXPECT_EQ(writer.Seek(0), Status::OutOfRange());
  EXPECT_EQ(writer.Seek(-100), Status::OutOfRange());
  EXPECT_EQ(writer.Seek(100), Status::OutOfRange());
}

TEST(MultibufStream, Seek_OutOfBounds) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 16, kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Seek(-1), Status::OutOfRange());
  EXPECT_EQ(writer.Seek(buf.size()), Status::OutOfRange());
}

TEST(MultibufStream, Seek_SingleChunkMultibuf_Succeeds) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 64, kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Seek(32), OkStatus());
  EXPECT_EQ(writer.Write(bytes::Initialized<8>(2)), OkStatus());
  EXPECT_EQ(writer.Seek(40), OkStatus());
  EXPECT_EQ(writer.Write(bytes::Initialized<24>(1)), OkStatus());

  constexpr auto kExpected =
      bytes::Concat(bytes::Initialized<32>(static_cast<uint8_t>(kPoisonByte)),
                    bytes::Initialized<8>(2),
                    bytes::Initialized<24>(1));

  ExpectElementsEqual(buf, kExpected);
}

TEST(MultibufStream, Seek_MultiChunkMultiBuf_Succeeds) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 8, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 8, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 16, kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Seek(32), OkStatus());
  EXPECT_EQ(writer.Write(bytes::Initialized<8>(1)), OkStatus());
  EXPECT_EQ(writer.Seek(40), OkStatus());
  EXPECT_EQ(writer.Write(bytes::Initialized<24>(2)), OkStatus());

  constexpr auto kExpected =
      bytes::Concat(bytes::Initialized<32>(static_cast<uint8_t>(kPoisonByte)),
                    bytes::Initialized<8>(1),
                    bytes::Initialized<24>(2));

  ExpectElementsEqual(buf, kExpected);
}

TEST(MultibufStream, Seek_Backwards_ReturnsOutOfRange) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 8, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 8, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(allocator, 16, kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Seek(32), OkStatus());
  EXPECT_EQ(writer.Seek(30), Status::OutOfRange());
  EXPECT_EQ(writer.Seek(48), OkStatus());
  EXPECT_EQ(writer.Seek(-4, Stream::Whence::kCurrent), Status::OutOfRange());
  EXPECT_EQ(writer.Seek(60), OkStatus());
  EXPECT_EQ(writer.Seek(64), Status::OutOfRange());
}

TEST(MultibufStream, Read_EmptyMultibuf_ReturnsOutOfRange) {
  auto destination = bytes::Initialized<64>(static_cast<uint8_t>(kPoisonByte));
  MultiBuf buf;
  Stream reader(buf);
  EXPECT_EQ(reader.Read(destination).status(), Status::OutOfRange());
  ExpectElementsAre(destination, kPoisonByte);
}

TEST(MultibufStream, Read_SingleChunkMultiBuf_Succeeds) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  auto destination = bytes::Initialized<64>(static_cast<uint8_t>(kPoisonByte));
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16, std::byte{1}));
  Stream reader(buf);

  Result<ByteSpan> result = reader.Read(destination);
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result->size(), 16u);
  ExpectElementsAre(*result, std::byte{1});

  result = reader.Read(destination);
  EXPECT_EQ(result.status(), Status::OutOfRange());
  ExpectElementsAre(span(destination).first(16), std::byte{1});
  ExpectElementsAre(span(destination).subspan(16), kPoisonByte);
}

TEST(MultibufStream, Read_MultiChunkMultiBuf_Succeeds) {
  AllocatorForTest<kArbitraryAllocatorSize> allocator;
  auto destination = bytes::Initialized<64>(static_cast<uint8_t>(kPoisonByte));
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(allocator, 16, std::byte{2}));
  buf.PushFrontChunk(MakeChunk(allocator, 8, std::byte{3}));
  buf.PushFrontChunk(MakeChunk(allocator, 8, std::byte{4}));
  Stream reader(buf);

  constexpr auto kExpected = bytes::Concat(bytes::Initialized<8>(4),
                                           bytes::Initialized<8>(3),
                                           bytes::Initialized<16>(2));

  Result<ByteSpan> result = reader.Read(destination);
  EXPECT_EQ(result.status(), OkStatus());
  EXPECT_EQ(result->size(), 32u);
  ExpectElementsEqual(*result, kExpected);

  result = reader.Read(destination);
  EXPECT_EQ(result.status(), Status::OutOfRange());
  ExpectElementsEqual(span(destination).first(32), kExpected);
  ExpectElementsAre(span(destination).subspan(32), kPoisonByte);
}

}  // namespace
}  // namespace pw::multibuf::v1
