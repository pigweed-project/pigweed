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

#include "pw_bluetooth_proxy/comms2/h4_packet.h"

#include "pw_allocator/testing.h"
#include "pw_bluetooth/hci_commands.emb.h"
#include "pw_bytes/array.h"
#include "pw_bytes/endian.h"
#include "pw_multibuf/v2/multibuf.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

namespace {

using pw::OkStatus;
using pw::Status;
using pw::allocator::test::AllocatorForTest;
using pw::bluetooth::proxy::H4Packet;
using pw::multibuf::v2::MultiBuf;

class H4PacketTest : public ::testing::Test {
 protected:
  AllocatorForTest<1024> allocator_;
};

TEST_F(H4PacketTest, CanConstructFrom) {
  auto h4_buffer = pw::bytes::Array<H4Packet::Type::COMMAND, 1, 2, 3, 4>();
  MultiBuf::Instance empty(allocator_);
  MultiBuf::Instance single_byte(allocator_);
  MultiBuf::Instance valid_packet(allocator_);
  single_byte->PushBack(pw::ByteSpan(h4_buffer).first(1));
  valid_packet->PushBack(h4_buffer);

  EXPECT_FALSE(H4Packet::CanConstructFrom(empty));
  EXPECT_FALSE(H4Packet::CanConstructFrom(single_byte));
  EXPECT_TRUE(H4Packet::CanConstructFrom(valid_packet));
}

TEST_F(H4PacketTest, ConstructEmpty) {
  H4Packet h4_packet(allocator_);
  EXPECT_EQ(h4_packet.type(), H4Packet::Type::UNKNOWN);
  EXPECT_EQ(h4_packet.size(), 0u);
}

TEST_F(H4PacketTest, Prepare_EmptyMultiBuf) {
  MultiBuf::Instance multibuf(allocator_);
  H4Packet h4_packet(allocator_);
  EXPECT_EQ(h4_packet.Prepare(multibuf), Status::InvalidArgument());
}

TEST_F(H4PacketTest, From_ValidMultiBuf) {
  auto h4_buffer = pw::bytes::Array<0, 1, 2, 3, 4>();
  h4_buffer[0] = static_cast<std::byte>(H4Packet::Type::COMMAND);
  MultiBuf::Instance multibuf(allocator_);
  multibuf->PushBack(h4_buffer);

  H4Packet h4_packet(allocator_);
  EXPECT_EQ(h4_packet.Prepare(multibuf), OkStatus());

  h4_packet.PopulateFrom(std::move(*multibuf));
  EXPECT_EQ(h4_packet.type(), H4Packet::Type::COMMAND);
  EXPECT_EQ(h4_packet.size(), 4u);

  // Payload should be the h4 buffer without the first byte.
  std::array<std::byte, 8> copy_buffer;
  pw::ConstByteSpan payload = h4_packet.Get(copy_buffer);
  ASSERT_EQ(payload.size(), h4_buffer.size() - 1);

  EXPECT_EQ(std::memcmp(payload.data(), h4_buffer.data() + 1, payload.size()),
            0);
  EXPECT_EQ(h4_packet[0], std::byte{1});
  EXPECT_EQ(h4_packet[1], std::byte{2});
  EXPECT_EQ(h4_packet[2], std::byte{3});
  EXPECT_EQ(h4_packet[3], std::byte{4});
}

TEST_F(H4PacketTest, From_IncompleteMultiBuf) {
  auto incomplete_h4_buffer = pw::bytes::Array<1>();
  MultiBuf::Instance multibuf(allocator_);
  multibuf->PushBack(incomplete_h4_buffer);
  H4Packet h4_packet(allocator_);
  EXPECT_EQ(h4_packet.Prepare(multibuf), Status::InvalidArgument());
}

TEST_F(H4PacketTest, Visit) {
  auto h4_buffer = pw::bytes::Concat(
      H4Packet::Type::COMMAND,
      pw::bytes::CopyInOrder(
          pw::endian::little,
          static_cast<uint32_t>(pw::bluetooth::emboss::OpCode::DISCONNECT)));

  MultiBuf::Instance multibuf(allocator_);
  multibuf->PushBack(h4_buffer);

  H4Packet h4_packet(allocator_);
  EXPECT_EQ(h4_packet.Prepare(multibuf), OkStatus());
  h4_packet.PopulateFrom(std::move(*multibuf));
  EXPECT_EQ(h4_packet.type(), H4Packet::Type::COMMAND);
  EXPECT_EQ(h4_packet.size(), 4u);

  pw::bluetooth::emboss::OpCode opcode =
      pw::bluetooth::emboss::OpCode::UNSPECIFIED;
  auto status =
      h4_packet
          .Visit<pw::bluetooth::emboss::CommandHeaderView,
                 pw::bluetooth::emboss::CommandHeader::IntrinsicSizeInBytes()>(
              [&opcode](auto header) { opcode = header.opcode().Read(); });
  EXPECT_EQ(status, OkStatus());
  EXPECT_EQ(opcode, pw::bluetooth::emboss::OpCode::DISCONNECT);
}

}  // namespace
