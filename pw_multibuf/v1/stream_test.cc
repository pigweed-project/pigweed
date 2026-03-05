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
#include "pw_multibuf_private/chunk_testing.h"
#include "pw_multibuf_private/test_utils.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::bytes::Initialized;
using pw::multibuf::MultiBuf;
using pw::multibuf::test::ExpectElementsAre;
using pw::multibuf::test::ExpectElementsEqual;
using pw::multibuf::v1::Stream;

using StreamTest = pw::multibuf::test::ChunkTest;

constexpr auto kPoisonByte = std::byte{0x9d};

constexpr auto kData64 = Initialized<64>([](size_t i) { return i; });

TEST_F(StreamTest, Write_SingleChunkMultibuf_Succeeds) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(128, kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Write(kData64), pw::OkStatus());

  ExpectElementsEqual(buf, kData64);
  buf.DiscardPrefix(kData64.size());
  ExpectElementsAre(buf, std::byte{kPoisonByte});
}

TEST_F(StreamTest, Write_SingleChunkMultibuf_ExactSize_Succeeds) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(kData64.size(), kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Write(kData64), pw::OkStatus());

  EXPECT_EQ(buf.size(), kData64.size());
  ExpectElementsEqual(buf, kData64);
}

TEST_F(StreamTest, Write_MultiChunkMultibuf_Succeeds) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(24, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(8, kPoisonByte));
  Stream writer(buf);
  ASSERT_EQ(writer.Write(kData64), pw::OkStatus());

  ExpectElementsEqual(buf, kData64);
}

TEST_F(StreamTest, Write_MultiChunkMultibuf_OutOfRange) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(8, kPoisonByte));
  Stream writer(buf);
  ASSERT_EQ(writer.Write(kData64), pw::Status::OutOfRange());

  ExpectElementsEqual(buf, pw::span(kData64).first(24));
}

TEST_F(StreamTest, Write_EmptyMultibuf_ReturnsOutOfRange) {
  MultiBuf buf;
  Stream writer(buf);
  EXPECT_EQ(writer.Write(kData64), pw::Status::OutOfRange());
}

TEST_F(StreamTest, Seek_Empty) {
  MultiBuf buf;
  Stream writer(buf);
  EXPECT_EQ(writer.Seek(0), pw::Status::OutOfRange());
  EXPECT_EQ(writer.Seek(-100), pw::Status::OutOfRange());
  EXPECT_EQ(writer.Seek(100), pw::Status::OutOfRange());
}

TEST_F(StreamTest, Seek_OutOfBounds) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(16, kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Seek(-1), pw::Status::OutOfRange());
  EXPECT_EQ(writer.Seek(buf.size()), pw::Status::OutOfRange());
}

TEST_F(StreamTest, Seek_SingleChunkMultibuf_Succeeds) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(64, kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Seek(32), pw::OkStatus());
  EXPECT_EQ(writer.Write(Initialized<8>(2)), pw::OkStatus());
  EXPECT_EQ(writer.Seek(40), pw::OkStatus());
  EXPECT_EQ(writer.Write(Initialized<24>(1)), pw::OkStatus());

  constexpr auto kExpected =
      pw::bytes::Concat(Initialized<32>(static_cast<uint8_t>(kPoisonByte)),
                        Initialized<8>(2),
                        Initialized<24>(1));

  ExpectElementsEqual(buf, kExpected);
}

TEST_F(StreamTest, Seek_MultiChunkMultiBuf_Succeeds) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(8, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(8, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(16, kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Seek(32), pw::OkStatus());
  EXPECT_EQ(writer.Write(Initialized<8>(1)), pw::OkStatus());
  EXPECT_EQ(writer.Seek(40), pw::OkStatus());
  EXPECT_EQ(writer.Write(Initialized<24>(2)), pw::OkStatus());

  constexpr auto kExpected =
      pw::bytes::Concat(Initialized<32>(static_cast<uint8_t>(kPoisonByte)),
                        Initialized<8>(1),
                        Initialized<24>(2));

  ExpectElementsEqual(buf, kExpected);
}

TEST_F(StreamTest, Seek_Backwards_ReturnsOutOfRange) {
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(8, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(16, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(8, kPoisonByte));
  buf.PushFrontChunk(MakeChunk(16, kPoisonByte));
  Stream writer(buf);
  EXPECT_EQ(writer.Seek(32), pw::OkStatus());
  EXPECT_EQ(writer.Seek(30), pw::Status::OutOfRange());
  EXPECT_EQ(writer.Seek(48), pw::OkStatus());
  EXPECT_EQ(writer.Seek(-4, Stream::Whence::kCurrent),
            pw::Status::OutOfRange());
  EXPECT_EQ(writer.Seek(60), pw::OkStatus());
  EXPECT_EQ(writer.Seek(64), pw::Status::OutOfRange());
}

TEST_F(StreamTest, Read_EmptyMultibuf_ReturnsOutOfRange) {
  auto destination = Initialized<64>(static_cast<uint8_t>(kPoisonByte));
  MultiBuf buf;
  Stream reader(buf);
  EXPECT_EQ(reader.Read(destination).status(), pw::Status::OutOfRange());
  ExpectElementsAre(destination, kPoisonByte);
}

TEST_F(StreamTest, Read_SingleChunkMultiBuf_Succeeds) {
  auto destination = Initialized<64>(static_cast<uint8_t>(kPoisonByte));
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16, std::byte{1}));
  Stream reader(buf);

  pw::Result<pw::ByteSpan> result = reader.Read(destination);
  EXPECT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(result->size(), 16u);
  ExpectElementsAre(*result, std::byte{1});

  result = reader.Read(destination);
  EXPECT_EQ(result.status(), pw::Status::OutOfRange());
  ExpectElementsAre(pw::span(destination).first(16), std::byte{1});
  ExpectElementsAre(pw::span(destination).subspan(16), kPoisonByte);
}

TEST_F(StreamTest, Read_MultiChunkMultiBuf_Succeeds) {
  auto destination = Initialized<64>(static_cast<uint8_t>(kPoisonByte));
  MultiBuf buf;
  buf.PushFrontChunk(MakeChunk(16, std::byte{2}));
  buf.PushFrontChunk(MakeChunk(8, std::byte{3}));
  buf.PushFrontChunk(MakeChunk(8, std::byte{4}));
  Stream reader(buf);

  constexpr auto kExpected = pw::bytes::Concat(
      Initialized<8>(4), Initialized<8>(3), Initialized<16>(2));

  pw::Result<pw::ByteSpan> result = reader.Read(destination);
  EXPECT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(result->size(), 32u);
  ExpectElementsEqual(*result, kExpected);

  result = reader.Read(destination);
  EXPECT_EQ(result.status(), pw::Status::OutOfRange());
  ExpectElementsEqual(pw::span(destination).first(32), kExpected);
  ExpectElementsAre(pw::span(destination).subspan(32), kPoisonByte);
}

}  // namespace
