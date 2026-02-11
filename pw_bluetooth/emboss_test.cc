// Copyright 2023 The Pigweed Authors
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

#include <numeric>

#include "lib/stdcompat/utility.h"
#include "pw_unit_test/framework.h"

// clang-format off
// All emboss headers are listed (even if they don't have explicit tests) to
// ensure they are compiled.
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/att.emb.h"  // IWYU pragma: keep
#include "pw_bluetooth/hci_commands.emb.h"  // IWYU pragma: keep
#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/hci_events.emb.h"  // IWYU pragma: keep
#include "pw_bluetooth/hci_h4.emb.h"      // IWYU pragma: keep
#include "pw_bluetooth/hci_test.emb.h"
#include "pw_bluetooth/hci_android.emb.h"    // IWYU pragma: keep
#include "pw_bluetooth/l2cap_frames.emb.h"  // IWYU pragma: keep
#include "pw_bluetooth/rfcomm_frames.emb.h"  // IWYU pragma: keep
// clang-format on

namespace pw::bluetooth {
namespace {

// Examples are used in docs.rst.
TEST(EmbossExamples, MakeView) {
  // DOCSTAG: [pw_bluetooth-examples-make_view]
  std::array<uint8_t, 4> buffer = {0x00, 0x01, 0x02, 0x03};
  auto view = emboss::MakeTestCommandPacketView(&buffer);
  EXPECT_TRUE(view.IsComplete());
  EXPECT_EQ(view.payload().Read(), 0x03);
  // DOCSTAG: [pw_bluetooth-examples-make_view]
}

TEST(EmbossTest, MakeView) {
  std::array<uint8_t, 4> buffer = {0x00, 0x01, 0x02, 0x03};
  auto view = emboss::MakeTestCommandPacketView(&buffer);
  EXPECT_TRUE(view.IsComplete());
  EXPECT_EQ(view.payload().Read(), 0x03);
}

static void InitializeIsoPacket(const emboss::IsoDataFramePacketWriter& view,
                                emboss::TsFlag ts_flag,
                                emboss::IsoDataPbFlag pb_flag,
                                size_t sdu_fragment_size) {
  view.header().connection_handle().Write(0x123);
  view.header().ts_flag().Write(ts_flag);
  view.header().pb_flag().Write(pb_flag);

  size_t optional_fields_total_size = 0;
  if (ts_flag == emboss::TsFlag::TIMESTAMP_PRESENT) {
    optional_fields_total_size += 4;
  }

  if ((pb_flag == emboss::IsoDataPbFlag::FIRST_FRAGMENT) ||
      (pb_flag == emboss::IsoDataPbFlag::COMPLETE_SDU)) {
    optional_fields_total_size += 4;
  }

  view.header().data_total_length().Write(sdu_fragment_size +
                                          optional_fields_total_size);
}

// This definition has a mix of full-width values and bitfields and includes
// conditional bitfields. Let's add this to verify that the structure itself
// doesn't get changed incorrectly and that emboss' size calculation matches
// ours.
TEST(EmbossTest, CheckIsoPacketSize) {
  std::array<uint8_t, 2048> buffer;
  const size_t kSduFragmentSize = 100;
  auto view = emboss::MakeIsoDataFramePacketView(&buffer);

  InitializeIsoPacket(view,
                      emboss::TsFlag::TIMESTAMP_NOT_PRESENT,
                      emboss::IsoDataPbFlag::FIRST_FRAGMENT,
                      kSduFragmentSize);
  ASSERT_TRUE(view.IntrinsicSizeInBytes().Ok());
  EXPECT_EQ(static_cast<size_t>(view.IntrinsicSizeInBytes().Read()),
            view.hdr_size().Read() + kSduFragmentSize + 4);

  InitializeIsoPacket(view,
                      emboss::TsFlag::TIMESTAMP_NOT_PRESENT,
                      emboss::IsoDataPbFlag::INTERMEDIATE_FRAGMENT,
                      kSduFragmentSize);
  ASSERT_TRUE(view.IntrinsicSizeInBytes().Ok());
  EXPECT_EQ(static_cast<size_t>(view.IntrinsicSizeInBytes().Read()),
            view.hdr_size().Read() + kSduFragmentSize);

  InitializeIsoPacket(view,
                      emboss::TsFlag::TIMESTAMP_NOT_PRESENT,
                      emboss::IsoDataPbFlag::COMPLETE_SDU,
                      kSduFragmentSize);
  ASSERT_TRUE(view.IntrinsicSizeInBytes().Ok());
  EXPECT_EQ(static_cast<size_t>(view.IntrinsicSizeInBytes().Read()),
            view.hdr_size().Read() + kSduFragmentSize + 4);

  InitializeIsoPacket(view,
                      emboss::TsFlag::TIMESTAMP_NOT_PRESENT,
                      emboss::IsoDataPbFlag::LAST_FRAGMENT,
                      kSduFragmentSize);
  ASSERT_TRUE(view.IntrinsicSizeInBytes().Ok());
  EXPECT_EQ(static_cast<size_t>(view.IntrinsicSizeInBytes().Read()),
            view.hdr_size().Read() + kSduFragmentSize);

  InitializeIsoPacket(view,
                      emboss::TsFlag::TIMESTAMP_PRESENT,
                      emboss::IsoDataPbFlag::FIRST_FRAGMENT,
                      kSduFragmentSize);
  ASSERT_TRUE(view.IntrinsicSizeInBytes().Ok());
  EXPECT_EQ(static_cast<size_t>(view.IntrinsicSizeInBytes().Read()),
            view.hdr_size().Read() + kSduFragmentSize + 8);

  InitializeIsoPacket(view,
                      emboss::TsFlag::TIMESTAMP_PRESENT,
                      emboss::IsoDataPbFlag::INTERMEDIATE_FRAGMENT,
                      kSduFragmentSize);
  ASSERT_TRUE(view.IntrinsicSizeInBytes().Ok());
  EXPECT_EQ(static_cast<size_t>(view.IntrinsicSizeInBytes().Read()),
            view.hdr_size().Read() + kSduFragmentSize + 4);

  InitializeIsoPacket(view,
                      emboss::TsFlag::TIMESTAMP_PRESENT,
                      emboss::IsoDataPbFlag::COMPLETE_SDU,
                      kSduFragmentSize);
  ASSERT_TRUE(view.IntrinsicSizeInBytes().Ok());
  EXPECT_EQ(static_cast<size_t>(view.IntrinsicSizeInBytes().Read()),
            view.hdr_size().Read() + kSduFragmentSize + 8);

  InitializeIsoPacket(view,
                      emboss::TsFlag::TIMESTAMP_PRESENT,
                      emboss::IsoDataPbFlag::LAST_FRAGMENT,
                      kSduFragmentSize);
  ASSERT_TRUE(view.IntrinsicSizeInBytes().Ok());
  EXPECT_EQ(static_cast<size_t>(view.IntrinsicSizeInBytes().Read()),
            view.hdr_size().Read() + kSduFragmentSize + 4);
}

// Test and demonstrate various ways of reading opcodes.
TEST(EmbossTest, ReadOpcodesFromCommandHeader) {
  // First two bytes will be used as opcode.
  std::array<uint8_t, 4> buffer = {0x00, 0x00, 0x02, 0x03};
  auto view = emboss::MakeTestCommandPacketView(&buffer);
  EXPECT_TRUE(view.IsComplete());
  auto header = view.header();

  EXPECT_EQ(header.opcode().Read(), emboss::OpCode::UNSPECIFIED);
  EXPECT_EQ(header.opcode_bits().BackingStorage().ReadUInt(), 0x0000);
  EXPECT_EQ(header.opcode_bits().ogf().Read(), 0x00);
  EXPECT_EQ(header.opcode_bits().ocf().Read(), 0x00);

  // LINK_KEY_REQUEST_REPLY is OGF 0x01 and OCF 0x0B.
  header.opcode().Write(emboss::OpCode::LINK_KEY_REQUEST_REPLY);
  EXPECT_EQ(header.opcode().Read(), emboss::OpCode::LINK_KEY_REQUEST_REPLY);
  EXPECT_EQ(header.opcode_bits().BackingStorage().ReadUInt(), 0x040B);
  EXPECT_EQ(header.opcode_bits().ogf().Read(), 0x01);
  EXPECT_EQ(header.opcode_bits().ocf().Read(), 0x0B);
}

// Test and demonstrate various ways of writing opcodes.
TEST(EmbossTest, WriteOpcodesFromCommandHeader) {
  std::array<uint8_t, 4> buffer = {};
  buffer.fill(0xFF);
  auto view = emboss::MakeTestCommandPacketView(&buffer);
  EXPECT_TRUE(view.IsComplete());
  auto header = view.header();

  header.opcode().Write(emboss::OpCode::UNSPECIFIED);
  EXPECT_EQ(header.opcode_bits().BackingStorage().ReadUInt(), 0x0000);

  header.opcode_bits().ocf().Write(0x0B);
  EXPECT_EQ(header.opcode_bits().BackingStorage().ReadUInt(), 0x000B);

  header.opcode_bits().ogf().Write(0x01);
  EXPECT_EQ(header.opcode_bits().BackingStorage().ReadUInt(), 0x040B);
  // LINK_KEY_REQUEST_REPLY is OGF 0x01 and OCF 0x0B.
  EXPECT_EQ(header.opcode().Read(), emboss::OpCode::LINK_KEY_REQUEST_REPLY);
}

// Test and demonstrate using to_underlying with OpCodes enums
TEST(EmbossTest, OPCodeEnumsWithToUnderlying) {
  EXPECT_EQ(0x0000, cpp23::to_underlying(emboss::OpCode::UNSPECIFIED));
}

TEST(EmbossTest, ReadAndWriteOpcodesInCommandResponseHeader) {
  // First two bytes will be used as opcode.
  std::array<uint8_t,
             emboss::ReadBufferSizeCommandCompleteEventView::SizeInBytes()>
      buffer;
  std::iota(buffer.begin(), buffer.end(), 100);
  auto view = emboss::MakeReadBufferSizeCommandCompleteEventView(&buffer);
  EXPECT_TRUE(view.IsComplete());
  auto header = view.command_complete();

  header.command_opcode_uint().Write(0x0000);
  EXPECT_EQ(header.command_opcode().Read(), emboss::OpCode::UNSPECIFIED);
  EXPECT_EQ(header.command_opcode_bits().BackingStorage().ReadUInt(), 0x0000);
  EXPECT_EQ(header.command_opcode_bits().ogf().Read(), 0x00);
  EXPECT_EQ(header.command_opcode_bits().ocf().Read(), 0x00);

  // LINK_KEY_REQUEST_REPLY is OGF 0x01 and OCF 0x0B.
  header.command_opcode().Write(emboss::OpCode::LINK_KEY_REQUEST_REPLY);
  EXPECT_EQ(header.command_opcode().Read(),
            emboss::OpCode::LINK_KEY_REQUEST_REPLY);
  EXPECT_EQ(header.command_opcode_bits().BackingStorage().ReadUInt(), 0x040B);
  EXPECT_EQ(header.command_opcode_bits().ogf().Read(), 0x01);
  EXPECT_EQ(header.command_opcode_bits().ocf().Read(), 0x0B);
}

TEST(EmbossTest, ReadAndWriteEventCodesInEventHeader) {
  std::array<uint8_t, emboss::EventHeaderWriter::SizeInBytes()> buffer;
  std::iota(buffer.begin(), buffer.end(), 100);
  auto header = emboss::MakeEventHeaderView(&buffer);
  EXPECT_TRUE(header.IsComplete());

  header.event_code_uint().Write(
      cpp23::to_underlying(emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS));
  EXPECT_EQ(header.event_code().Read(),
            emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);
  EXPECT_EQ(
      header.event_code_uint().Read(),
      cpp23::to_underlying(emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS));

  EXPECT_EQ(
      header.event_code_uint().Read(),
      cpp23::to_underlying(emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS));

  header.event_code().Write(emboss::EventCode::CONNECTION_REQUEST);
  EXPECT_EQ(header.event_code_uint().Read(),
            cpp23::to_underlying(emboss::EventCode::CONNECTION_REQUEST));
}

TEST(EmbossTest, ReadCommandPayloadLength) {
  std::array<uint8_t, 8> hci_buffer = {
      0x4c, 0xfc, 0x05, 0x73, 0x86, 0x30, 0x00, 0x00};
  emboss::CommandHeaderView command = emboss::MakeCommandHeaderView(
      hci_buffer.data(), emboss::CommandHeaderView::SizeInBytes());
  EXPECT_TRUE(command.IsComplete());
  EXPECT_EQ(command.parameter_total_size().Read(), 5);
}

TEST(EmbossTest, ReadEventPayloadLength) {
  std::array<uint8_t, 8> hci_buffer = {0x0e, 0x04, 0x01, 0x2e, 0xfc, 0x00};
  emboss::EventHeaderView event = emboss::MakeEventHeaderView(
      hci_buffer.data(), emboss::EventHeaderView::SizeInBytes());
  EXPECT_TRUE(event.IsComplete());
  EXPECT_EQ(event.parameter_total_size().Read(), 4);
}

TEST(EmbossTest, ReadAclPayloadLength) {
  std::array<uint8_t, 16> hci_buffer = {0x0c,
                                        0x00,
                                        0x0c,
                                        0x00,
                                        0x08,
                                        0x00,
                                        0x01,
                                        0x00,
                                        0x06,
                                        0x06,
                                        0x04,
                                        0x00,
                                        0x5b,
                                        0x00,
                                        0x41,
                                        0x00};
  emboss::AclDataFrameHeaderView acl = emboss::MakeAclDataFrameHeaderView(
      hci_buffer.data(), emboss::AclDataFrameHeaderView::SizeInBytes());
  EXPECT_TRUE(acl.IsComplete());
  EXPECT_EQ(acl.data_total_length().Read(), 12);
}

TEST(EmbossTest, ReadScoPayloadLength) {
  std::array<uint8_t, 9> hci_buffer = {
      0x02, 0x00, 0x06, 0xFF, 0xD3, 0x4A, 0x1B, 0x2C, 0x3D};
  emboss::ScoDataHeaderView sco = emboss::ScoDataHeaderView(
      hci_buffer.data(), emboss::ScoDataHeaderView::SizeInBytes());
  EXPECT_TRUE(sco.IsComplete());
  EXPECT_EQ(sco.data_total_length().Read(), 6);
}

TEST(EmbossTest, WriteSniffMode) {
  std::array<uint8_t, emboss::SniffModeCommandWriter::SizeInBytes()> buffer{};
  emboss::SniffModeCommandWriter writer =
      emboss::MakeSniffModeCommandView(&buffer);
  writer.header().opcode().Write(emboss::OpCode::SNIFF_MODE);
  writer.header().parameter_total_size().Write(
      emboss::SniffModeCommandWriter::SizeInBytes() -
      emboss::CommandHeaderWriter::SizeInBytes());
  writer.connection_handle().Write(0x0004);
  writer.sniff_max_interval().Write(0x0330);
  writer.sniff_min_interval().Write(0x0190);
  writer.sniff_attempt().Write(0x0004);
  writer.sniff_timeout().Write(0x0001);
  std::array<uint8_t, emboss::SniffModeCommandView::SizeInBytes()> expected{
      // Opcode (LSB, MSB)
      0x03,
      0x08,
      // Parameter Total Size
      0x0A,
      // Connection Handle (LSB, MSB)
      0x04,
      0x00,
      // Sniff Max Interval (LSB, MSB)
      0x30,
      0x03,
      // Sniff Min Interval (LSB, MSB)
      0x90,
      0x01,
      // Sniff Attempt (LSB, MSB)
      0x04,
      0x00,
      // Sniff Timeout (LSB, MSB)
      0x01,
      0x00};
  EXPECT_EQ(buffer, expected);
}

TEST(EmbossTest, ReadSniffMode) {
  std::array<uint8_t, emboss::SniffModeCommandView::SizeInBytes()> buffer{
      // Opcode (LSB, MSB)
      0x03,
      0x08,
      // Parameter Total Size
      0x0A,
      // Connection Handle (LSB, MSB)
      0x04,
      0x00,
      // Sniff Max Interval (LSB, MSB)
      0x30,
      0x03,
      // Sniff Min Interval (LSB, MSB)
      0x90,
      0x01,
      // Sniff Attempt (LSB, MSB)
      0x04,
      0x00,
      // Sniff Timeout (LSB, MSB)
      0x01,
      0x00};
  emboss::SniffModeCommandView view = emboss::MakeSniffModeCommandView(&buffer);
  EXPECT_EQ(view.header().opcode().Read(), emboss::OpCode::SNIFF_MODE);
  EXPECT_TRUE(view.header().IsComplete());
  EXPECT_EQ(view.connection_handle().Read(), 0x0004);
  EXPECT_EQ(view.sniff_max_interval().Read(), 0x0330);
  EXPECT_EQ(view.sniff_min_interval().Read(), 0x0190);
  EXPECT_EQ(view.sniff_attempt().Read(), 0x0004);
  EXPECT_EQ(view.sniff_timeout().Read(), 0x0001);
}

TEST(EmbossTest, ReadRfcomm) {
  std::array<uint8_t,
             emboss::RfcommFrame::MinSizeInBytes() + /*credits*/ 1 +
                 /*payload*/ 3>
      buffer_with_credits = {// Address
                             0x19,
                             // UIH Poll/Final
                             0xFF,
                             // Information Length
                             0x07,
                             // Credits
                             0x0A,
                             // Payload/Information
                             0xAB,
                             0xCD,
                             0xEF,
                             // FCS
                             0x49};

  emboss::RfcommFrameView rfcomm =
      emboss::MakeRfcommFrameView(&buffer_with_credits);
  EXPECT_TRUE(rfcomm.Ok());
  EXPECT_EQ(rfcomm.credits().Read(), 10);

  EXPECT_EQ(rfcomm.information()[0].Read(), 0xAB);
  EXPECT_EQ(rfcomm.information()[1].Read(), 0xCD);
  EXPECT_EQ(rfcomm.information()[2].Read(), 0xEF);

  EXPECT_EQ(rfcomm.fcs().Read(), 0x49);

  std::array<uint8_t,
             emboss::RfcommFrame::MinSizeInBytes() +
                 /*payload*/ 3>
      buffer_without_credits = {// Address
                                0x19,
                                // UIH
                                0xEF,
                                // Information Length
                                0x07,
                                // Payload/Information
                                0xAB,
                                0xCD,
                                0xEF,
                                // FCS
                                0x55};

  rfcomm = emboss::MakeRfcommFrameView(&buffer_without_credits);
  EXPECT_TRUE(rfcomm.Ok());
  EXPECT_FALSE(rfcomm.has_credits().ValueOrDefault());
  EXPECT_EQ(rfcomm.information()[0].Read(), 0xAB);
  EXPECT_EQ(rfcomm.information()[1].Read(), 0xCD);
  EXPECT_EQ(rfcomm.information()[2].Read(), 0xEF);
  EXPECT_EQ(rfcomm.fcs().Read(), 0x55);
}

TEST(EmbossTest, ReadRfcommExtended) {
  constexpr size_t kMaxShortLength = 0x7f;
  std::array<uint8_t,
             emboss::RfcommFrame::MinSizeInBytes() + /*length_extended*/ 1 +
                 /*credits*/ 1 +
                 /*payload*/ (kMaxShortLength + 1)>
      buffer_extended_length_with_credits = {
          // Address
          0x19,
          // UIH Poll/Final
          0xFF,
          // Information Length
          0x00,
          0x01,
          // Credits
          0x0A,
          // Payload/Information
          0xAB,
          0xCD,
          0xEF,
      };

  // FCS
  buffer_extended_length_with_credits
      [buffer_extended_length_with_credits.size() - 1] = 0x49;

  emboss::RfcommFrameView rfcomm =
      emboss::MakeRfcommFrameView(&buffer_extended_length_with_credits);
  EXPECT_TRUE(rfcomm.Ok());
  EXPECT_TRUE(rfcomm.has_credits().ValueOrDefault());
  EXPECT_TRUE(rfcomm.has_length_extended().ValueOrDefault());
  EXPECT_EQ(rfcomm.information_length().Read(), 128);
  EXPECT_EQ(rfcomm.information()[0].Read(), 0xAB);
  EXPECT_EQ(rfcomm.information()[1].Read(), 0xCD);
  EXPECT_EQ(rfcomm.information()[2].Read(), 0xEF);
  EXPECT_EQ(rfcomm.fcs().Read(), 0x49);
}

TEST(EmbossTest, WriteRfcomm) {
  const std::array<uint8_t, 3> expected_payload = {0xAB, 0xCD, 0xEF};
  constexpr size_t kFrameSize = emboss::RfcommFrame::MinSizeInBytes() +
                                /*credits*/ 1 + expected_payload.size();
  std::array<uint8_t, kFrameSize> buffer{};

  emboss::RfcommFrameWriter rfcomm = emboss::MakeRfcommFrameView(&buffer);
  rfcomm.extended_address().Write(true);
  rfcomm.command_response().Write(false);
  rfcomm.direction().Write(false);
  rfcomm.channel().Write(3);
  rfcomm.control().Write(
      emboss::RfcommFrameType::
          UNNUMBERED_INFORMATION_WITH_HEADER_CHECK_AND_POLL_FINAL);

  rfcomm.length_extended_flag().Write(emboss::RfcommLengthExtended::NORMAL);
  rfcomm.length().Write(expected_payload.size());

  EXPECT_TRUE(rfcomm.has_credits().ValueOrDefault());
  rfcomm.credits().Write(10);

  std::memcpy(rfcomm.information().BackingStorage().data(),
              expected_payload.data(),
              expected_payload.size());
  rfcomm.fcs().Write(0x49);

  std::array<uint8_t, kFrameSize> expected{// Address
                                           0x19,
                                           // UIH Poll/Final
                                           0xFF,
                                           // Information Length
                                           0x07,
                                           // Credits
                                           0x0A,
                                           // Payload/Information
                                           0xAB,
                                           0xCD,
                                           0xEF,
                                           // FCS
                                           0x49};
  EXPECT_EQ(buffer, expected);
}

TEST(EmbossTest, WriteRfcommExtended) {
  const std::array<uint8_t, 128> expected_payload = {0xAB, 0xCD, 0xEF};
  constexpr size_t kFrameSize = emboss::RfcommFrame::MinSizeInBytes() +
                                /* length_extended */ 1 +
                                /*credits*/ 1 + expected_payload.size();
  std::array<uint8_t, kFrameSize> buffer{};

  emboss::RfcommFrameWriter rfcomm = emboss::MakeRfcommFrameView(&buffer);
  rfcomm.extended_address().Write(true);
  rfcomm.command_response().Write(false);
  rfcomm.direction().Write(false);
  rfcomm.channel().Write(3);
  rfcomm.control().Write(
      emboss::RfcommFrameType::
          UNNUMBERED_INFORMATION_WITH_HEADER_CHECK_AND_POLL_FINAL);

  rfcomm.length_extended_flag().Write(emboss::RfcommLengthExtended::EXTENDED);
  rfcomm.length_extended().Write(expected_payload.size());

  EXPECT_TRUE(rfcomm.has_credits().ValueOrDefault());
  rfcomm.credits().Write(10);

  std::memcpy(rfcomm.information().BackingStorage().data(),
              expected_payload.data(),
              expected_payload.size());
  rfcomm.fcs().Write(0x49);

  std::array<uint8_t, kFrameSize> expected{
      // Address
      0x19,
      // UIH Poll/Final
      0xFF,
      // Information Length
      0x00,
      0x01,
      // Credits
      0x0A,
      // Payload/Information
      0xAB,
      0xCD,
      0xEF,
  };
  // FCS
  expected[expected.size() - 1] = 0x49;

  EXPECT_EQ(expected[2], buffer[2]);
  EXPECT_EQ(expected[3], buffer[3]);

  EXPECT_EQ(buffer, expected);
}
}  // namespace
}  // namespace pw::bluetooth
