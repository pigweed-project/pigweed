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
#include "pw_bluetooth/avctp_avrcp.emb.h"    // IWYU pragma: keep
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
  std::array<uint8_t, 2048> buffer{};
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

TEST(EmbossTest, WriteSniffOffloadParametersZeroFields) {
  std::array<uint8_t,
             vendor::android_hci::WriteSniffOffloadParametersCommandWriter::
                 SizeInBytes()>
      buffer{};
  vendor::android_hci::WriteSniffOffloadParametersCommandWriter writer =
      vendor::android_hci::MakeWriteSniffOffloadParametersCommandView(&buffer);

  writer.header().opcode().Write(emboss::OpCode::UNSPECIFIED);
  writer.header().parameter_total_size().Write(
      vendor::android_hci::WriteSniffOffloadParametersCommandView::
          SizeInBytes() -
      emboss::CommandHeaderView::SizeInBytes());

  writer.connection_handle().Write(0x0004);
  writer.max_interval().Write(0x0000);
  writer.min_interval().Write(0x0000);
  writer.attempts().Write(0x0000);
  writer.sniff_timeout().Write(0x0000);
  writer.link_inactivity_timeout().Write(0x0000);
  writer.max_latency().Write(0x0000);
  writer.min_remote_timeout().Write(0x0000);
  writer.min_local_timeout().Write(0x0000);

  EXPECT_TRUE(writer.Ok());
  EXPECT_TRUE(writer.IsComplete());
}

TEST(EmbossTest, ReadWriteMaxSlotsChange) {
  std::array<uint8_t, emboss::MaxSlotsChangeEventView::SizeInBytes()> buffer{};
  auto writer = emboss::MakeMaxSlotsChangeEventView(&buffer);

  writer.header().event_code().Write(emboss::EventCode::MAX_SLOTS_CHANGE);
  writer.header().parameter_total_size().Write(
      emboss::MaxSlotsChangeEventView::SizeInBytes() -
      emboss::EventHeaderWriter::SizeInBytes());
  writer.connection_handle().Write(0x0123);
  writer.lmp_max_slots().Write(5);

  EXPECT_TRUE(writer.IsComplete());

  auto reader = emboss::MakeMaxSlotsChangeEventView(&buffer);
  EXPECT_TRUE(reader.Ok());
  EXPECT_EQ(reader.header().event_code().Read(),
            emboss::EventCode::MAX_SLOTS_CHANGE);
  EXPECT_EQ(reader.connection_handle().Read(), 0x0123);
  EXPECT_EQ(reader.lmp_max_slots().Read(), 5);
}

TEST(EmbossTest, ReadWriteConnectionPacketTypeChanged) {
  std::array<uint8_t,
             emboss::ConnectionPacketTypeChangedEventView::SizeInBytes()>
      buffer{};
  auto writer = emboss::MakeConnectionPacketTypeChangedEventView(&buffer);

  writer.header().event_code().Write(
      emboss::EventCode::CONNECTION_PACKET_TYPE_CHANGED);
  writer.header().parameter_total_size().Write(
      emboss::ConnectionPacketTypeChangedEventView::SizeInBytes() -
      emboss::EventHeaderWriter::SizeInBytes());
  writer.status().Write(emboss::StatusCode::SUCCESS);
  writer.connection_handle().Write(0x0123);
  writer.packet_type().BackingStorage().WriteUInt(0xcc18);

  EXPECT_TRUE(writer.IsComplete());

  auto reader = emboss::MakeConnectionPacketTypeChangedEventView(&buffer);
  EXPECT_TRUE(reader.Ok());
  EXPECT_EQ(reader.header().event_code().Read(),
            emboss::EventCode::CONNECTION_PACKET_TYPE_CHANGED);
  EXPECT_EQ(reader.status().Read(), emboss::StatusCode::SUCCESS);
  EXPECT_EQ(reader.connection_handle().Read(), 0x0123);
  EXPECT_EQ(reader.packet_type().BackingStorage().ReadUInt(), 0xcc18);
}

TEST(EmbossTest, AvrcpVolumeControlPacket) {
  // 1. SetAbsoluteVolume Command
  std::array<uint8_t, 14> volume_cmd = {
      0x00,  // Transaction Label (0) | Packet Type (0, Single) | C/R (0,
             // Command)
      0x11,  // Profile ID (0x110E, AVRCP)
      0x0E,
      0x00,  // AV/C Header: ctype (0, CONTROL)
      0x48,  // Subunit type (9, Panel) & Subunit ID (0)
      0x00,  // Opcode (0x00, Vendor Dependent)
      0x00,  // Company ID (0x001958, Bluetooth SIG)
      0x19,
      0x58,
      0x50,  // PDU ID (0x50, SetAbsoluteVolume)
      0x00,  // Packet Type (AVRCP fragmentation, usually 0)
      0x00,  // Parameter Length (MSB)
      0x01,  // Parameter Length (LSB)
      0x00   // Volume (0%)
  };

  auto view1 = emboss::MakeAvrcpVolumeControlPacketView(&volume_cmd);
  ASSERT_TRUE(view1.Ok());
  EXPECT_TRUE(view1.IsComplete());
  EXPECT_EQ(view1.avctp_header().packet_type().Read(),
            emboss::AvctpPacketType::SINGLE_PACKET);
  EXPECT_FALSE(view1.avctp_header().cr().Read());
  EXPECT_FALSE(view1.avctp_header().ipid().Read());
  EXPECT_EQ(view1.avctp_header().transaction_label().Read(), 0u);
  EXPECT_EQ(view1.profile_id().Read(), 0x110E);
  EXPECT_EQ(view1.avc_header().ctype().Read(), emboss::AvcCtype::CONTROL);
  EXPECT_EQ(view1.avc_header().subunit_address().subunit_type().Read(), 9);
  EXPECT_EQ(view1.avc_header().subunit_address().subunit_id().Read(), 0);
  EXPECT_EQ(view1.avc_header().opcode().Read(), 0x00);
  EXPECT_EQ(view1.avrcp_header().company_id().Read(), 0x001958u);
  EXPECT_EQ(view1.avrcp_header().pdu_id().Read(), 0x50);
  EXPECT_EQ(view1.packet_type().Read(), 0);
  EXPECT_EQ(view1.parameter_length().Read(), 1);
  ASSERT_TRUE(view1.absolute_volume().Ok());
  EXPECT_EQ(view1.absolute_volume().Read(), 0);
  EXPECT_FALSE(view1.event_id().Ok());
  EXPECT_FALSE(view1.changed_volume().Ok());

  // 2. SetAbsoluteVolume Response (ACCEPTED)
  std::array<uint8_t, 14> volume_resp = {
      0x22,  // Transaction Label (2) | Packet Type (0, Single) | C/R (1,
             // Response)
      0x11,  // Profile ID (0x110E, AVRCP)
      0x0E,
      0x09,  // AV/C Header: response (0x09, ACCEPTED)
      0x48,  // Subunit type (9, Panel) & Subunit ID (0)
      0x00,  // Opcode (0x00, Vendor Dependent)
      0x00,  // Company ID (0x001958, Bluetooth SIG)
      0x19,
      0x58,
      0x50,  // PDU ID (0x50, SetAbsoluteVolume)
      0x00,  // Packet Type
      0x00,  // Parameter Length (MSB)
      0x01,  // Parameter Length (LSB)
      0x3f   // Volume (50%)
  };

  auto view_resp = emboss::MakeAvrcpVolumeControlPacketView(&volume_resp);
  ASSERT_TRUE(view_resp.Ok());
  EXPECT_TRUE(view_resp.IsComplete());
  EXPECT_EQ(view_resp.avctp_header().packet_type().Read(),
            emboss::AvctpPacketType::SINGLE_PACKET);
  EXPECT_TRUE(view_resp.avctp_header().cr().Read());
  EXPECT_FALSE(view_resp.avctp_header().ipid().Read());
  EXPECT_EQ(view_resp.avctp_header().transaction_label().Read(), 2u);
  EXPECT_EQ(view_resp.profile_id().Read(), 0x110E);
  EXPECT_EQ(view_resp.avc_header().response().Read(),
            emboss::AvcResponseCode::ACCEPTED);
  EXPECT_EQ(view_resp.avrcp_header().pdu_id().Read(), 0x50);
  EXPECT_EQ(view_resp.parameter_length().Read(), 1);
  ASSERT_TRUE(view_resp.absolute_volume().Ok());
  EXPECT_EQ(view_resp.absolute_volume().Read(), 0x3f);
  EXPECT_FALSE(view_resp.event_id().Ok());

  // 3. RegisterNotification Command
  std::array<uint8_t, 18> volume_notify_cmd = {
      0x30,  // Transaction Label (3) | Packet Type (0, Single) | C/R (0,
             // Command)
      0x11,  // Profile ID (0x110E, AVRCP)
      0x0E,
      0x00,  // AV/C Header: ctype (0, CONTROL)
      0x48,  // Subunit type (9, Panel) & Subunit ID (0)
      0x00,  // Opcode (0x00, Vendor Dependent)
      0x00,  // Company ID (0x001958, Bluetooth SIG)
      0x19,
      0x58,
      0x31,  // PDU ID (0x31, RegisterNotification)
      0x00,  // Packet Type
      0x00,  // Parameter Length (MSB)
      0x05,  // Parameter Length (LSB)
      0x0d,  // Event ID (0x0d, VolumeChanged)
      0x00,  // Playback Interval (byte 1)
      0x00,  // Playback Interval (byte 2)
      0x00,  // Playback Interval (byte 3)
      0x00,  // Playback Interval (byte 4)
  };

  auto view2 = emboss::MakeAvrcpVolumeControlPacketView(&volume_notify_cmd);
  ASSERT_TRUE(view2.Ok());
  EXPECT_TRUE(view2.IsComplete());
  EXPECT_EQ(view2.avctp_header().packet_type().Read(),
            emboss::AvctpPacketType::SINGLE_PACKET);
  EXPECT_FALSE(view2.avctp_header().cr().Read());
  EXPECT_EQ(view2.avctp_header().transaction_label().Read(), 3u);
  EXPECT_EQ(view2.profile_id().Read(), 0x110E);
  EXPECT_EQ(view2.avc_header().opcode().Read(), 0x00);
  EXPECT_EQ(view2.avrcp_header().company_id().Read(), 0x001958u);
  EXPECT_EQ(view2.avrcp_header().pdu_id().Read(), 0x31);
  EXPECT_EQ(view2.packet_type().Read(), 0);
  EXPECT_EQ(view2.parameter_length().Read(), 5);
  ASSERT_TRUE(view2.event_id().Ok());
  EXPECT_EQ(view2.event_id().Read(), 0x0D);
  ASSERT_TRUE(view2.playback_interval().Ok());
  EXPECT_EQ(view2.playback_interval().Read(), 0u);
  EXPECT_FALSE(view2.changed_volume().Ok());
  EXPECT_FALSE(view2.absolute_volume().Ok());

  // 4. RegisterNotification Interim Response
  std::array<uint8_t, 15> volume_notify_interim_resp = {
      0x32,  // Transaction Label (3) | Packet Type (0, Single) | C/R (1,
             // Response)
      0x11,  // Profile ID (0x110E, AVRCP)
      0x0E,
      0x0f,  // AV/C Header: response (0x0f, INTERIM)
      0x48,  // Subunit type (9, Panel) & Subunit ID (0)
      0x00,  // Opcode (0x00, Vendor Dependent)
      0x00,  // Company ID (0x001958, Bluetooth SIG)
      0x19,
      0x58,
      0x31,  // PDU ID (0x31, RegisterNotification)
      0x00,  // Packet Type
      0x00,  // Parameter Length (MSB)
      0x02,  // Parameter Length (LSB)
      0x0d,  // Event ID (0x0d, VolumeChanged)
      0x40,  // Volume (0x40, 50%)
  };

  auto view_interim =
      emboss::MakeAvrcpVolumeControlPacketView(&volume_notify_interim_resp);
  ASSERT_TRUE(view_interim.Ok());
  EXPECT_TRUE(view_interim.IsComplete());
  EXPECT_EQ(view_interim.avctp_header().packet_type().Read(),
            emboss::AvctpPacketType::SINGLE_PACKET);
  EXPECT_TRUE(view_interim.avctp_header().cr().Read());
  EXPECT_EQ(view_interim.avctp_header().transaction_label().Read(), 3u);
  EXPECT_EQ(view_interim.profile_id().Read(), 0x110E);
  EXPECT_EQ(view_interim.avc_header().response().Read(),
            emboss::AvcResponseCode::INTERIM);
  EXPECT_EQ(view_interim.avrcp_header().pdu_id().Read(), 0x31);
  EXPECT_EQ(view_interim.packet_type().Read(), 0);
  EXPECT_EQ(view_interim.parameter_length().Read(), 2);
  ASSERT_TRUE(view_interim.event_id().Ok());
  EXPECT_EQ(view_interim.event_id().Read(), 0x0D);
  ASSERT_TRUE(view_interim.changed_volume().Ok());
  EXPECT_EQ(view_interim.changed_volume().Read(), 0x40);
  EXPECT_FALSE(view_interim.playback_interval().Ok());

  // 5. RegisterNotification Changed Response
  std::array<uint8_t, 15> volume_notify_changed_resp = {
      0x32,  // Transaction Label (3) | Packet Type (0, Single) | C/R (1,
             // Response)
      0x11,  // Profile ID (0x110E, AVRCP)
      0x0E,
      0x0d,  // AV/C Header: response (0x0d, CHANGED)
      0x48,  // Subunit type (9, Panel) & Subunit ID (0)
      0x00,  // Opcode (0x00, Vendor Dependent)
      0x00,  // Company ID (0x001958, Bluetooth SIG)
      0x19,
      0x58,
      0x31,  // PDU ID (0x31, RegisterNotification)
      0x00,  // Packet Type
      0x00,  // Parameter Length (MSB)
      0x02,  // Parameter Length (LSB)
      0x0d,  // Event ID (0x0d, VolumeChanged)
      0x48,  // Volume (0x48, 56.5%)
  };

  auto view_changed =
      emboss::MakeAvrcpVolumeControlPacketView(&volume_notify_changed_resp);
  ASSERT_TRUE(view_changed.Ok());
  EXPECT_TRUE(view_changed.IsComplete());
  EXPECT_EQ(view_changed.avctp_header().packet_type().Read(),
            emboss::AvctpPacketType::SINGLE_PACKET);
  EXPECT_TRUE(view_changed.avctp_header().cr().Read());
  EXPECT_FALSE(view_changed.avctp_header().ipid().Read());
  EXPECT_EQ(view_changed.profile_id().Read(), 0x110E);
  EXPECT_EQ(view_changed.avc_header().response().Read(),
            emboss::AvcResponseCode::CHANGED);
  EXPECT_EQ(view_changed.avrcp_header().pdu_id().Read(), 0x31);
  EXPECT_EQ(view_changed.packet_type().Read(), 0);
  EXPECT_EQ(view_changed.parameter_length().Read(), 2);
  ASSERT_TRUE(view_changed.event_id().Ok());
  EXPECT_EQ(view_changed.event_id().Read(), 0x0D);
  ASSERT_TRUE(view_changed.changed_volume().Ok());
  EXPECT_EQ(view_changed.changed_volume().Read(), 0x48);
  EXPECT_FALSE(view_changed.playback_interval().Ok());
  EXPECT_FALSE(view_changed.absolute_volume().Ok());

  // 6. Insufficient buffer size test
  std::array<uint8_t, 10> small_buffer = {
      0x00, 0x11, 0x0E, 0x00, 0x48, 0x00, 0x00, 0x19, 0x58, 0x31};
  auto view4 = emboss::MakeAvrcpVolumeControlPacketView(&small_buffer);
  EXPECT_FALSE(view4.Ok());
}

}  // namespace
}  // namespace pw::bluetooth
