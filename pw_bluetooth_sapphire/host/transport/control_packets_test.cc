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

#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"

#include <array>
#include <cstddef>
#include <utility>

#include "pw_allocator/testing.h"
#include "pw_multibuf/v2/multibuf.h"
#include "pw_unit_test/framework.h"

namespace bt::hci {

const size_t BITS_IN_BYTE = 8;

TEST(EmbossControlPackets, TooSmallCommandCompleteEventWhenReadingStatus) {
  StaticByteBuffer buffer(
      hci_spec::kCommandCompleteEventCode,
      0x03,  // size
      0x01,  // num hci packets
      0x02,
      0x03  // command opcode
            // There should be a status field here, but there isn't
  );
  EventPacket packet = EventPacket::New(buffer.size());
  packet.mutable_data().Write(buffer);
  EXPECT_FALSE(packet.StatusCode().has_value());
}

TEST(EmbossControlPackets, CommandCompleteEventWithStatus) {
  StaticByteBuffer buffer(hci_spec::kCommandCompleteEventCode,
                          0x04,  // size
                          0x01,  // num hci packets
                          0x02,
                          0x03,  // command opcode
                          0x00   // status (success)
  );
  EventPacket packet = EventPacket::New(buffer.size());
  packet.mutable_data().Write(buffer);
  ASSERT_TRUE(packet.StatusCode().has_value());
  EXPECT_EQ(packet.StatusCode().value(),
            pw::bluetooth::emboss::StatusCode::SUCCESS);
}

TEST(EmbossControlPackets, ArrayFieldWithVariableLengthElements) {
  // Size of `data` arrays in the two Advertising Reports.
  constexpr size_t first_report_data_size = 3, second_report_data_size = 2;
  // Size of `reports` array in the Advertising Report Subevent.
  size_t reports_size =
      first_report_data_size + second_report_data_size +
      2ul * pw::bluetooth::emboss::LEExtendedAdvertisingReportData::
                MinSizeInBytes();
  // Size of LEMetaEvent containing the Advertising Report Subevent.
  size_t packet_size =
      reports_size + pw::bluetooth::emboss::
                         LEExtendedAdvertisingReportSubevent::MinSizeInBytes();

  auto packet = EventPacket::New<
      pw::bluetooth::emboss::LEExtendedAdvertisingReportSubeventWriter>(
      hci_spec::kLEMetaEventCode, packet_size);
  auto view = packet.view_t();
  view.num_reports().Write(2);
  ASSERT_TRUE(view.Ok());
  EXPECT_EQ(view.reports().SizeInBytes(), reports_size);

  // Create view over the first Report in the `reports` array of the subevent.
  auto first_report_view =
      pw::bluetooth::emboss::MakeLEExtendedAdvertisingReportDataView(
          view.reports().BackingStorage().data(),
          pw::bluetooth::emboss::LEExtendedAdvertisingReportData::
                  MinSizeInBytes() +
              first_report_data_size);
  first_report_view.data_length().Write(
      static_cast<int32_t>(first_report_data_size));
  first_report_view.data()
      .BackingStorage()
      .WriteBigEndianUInt<first_report_data_size * BITS_IN_BYTE>(0x123456);
  ASSERT_TRUE(first_report_view.Ok());
  EXPECT_EQ(first_report_view.data().SizeInBytes(), first_report_data_size);
  EXPECT_EQ(first_report_view.data()[0].Read(), 0x12);
  EXPECT_EQ(first_report_view.data()[1].Read(), 0x34);
  EXPECT_EQ(first_report_view.data()[2].Read(), 0x56);

  // Create view over the second Report in the `reports` array of the subevent.
  auto second_report_view =
      pw::bluetooth::emboss::MakeLEExtendedAdvertisingReportDataView(
          view.reports().BackingStorage().data() + first_report_data_size,
          pw::bluetooth::emboss::LEExtendedAdvertisingReportData::
                  MinSizeInBytes() +
              second_report_data_size);
  second_report_view.data_length().Write(
      static_cast<int32_t>(second_report_data_size));
  second_report_view.data()
      .BackingStorage()
      .WriteBigEndianUInt<second_report_data_size * BITS_IN_BYTE>(0x1234);
  ASSERT_TRUE(second_report_view.Ok());
  EXPECT_EQ(second_report_view.data().SizeInBytes(), second_report_data_size);
  EXPECT_EQ(second_report_view.data()[0].Read(), 0x12);
  EXPECT_EQ(second_report_view.data()[1].Read(), 0x34);
}

TEST(EmbossControlPackets, NewWritesOpcodeAndParameterTotalSize) {
  auto packet = CommandPacket::New<
      pw::bluetooth::emboss::LEPeriodicAdvertisingCreateSyncCommandWriter>(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto view = packet.view_t();
  EXPECT_EQ(view.header().opcode().Read(),
            pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  EXPECT_EQ(view.header().parameter_total_size().Read(), 14u);
}

TEST(EmbossControlPackets, ConstructFromEmptyMultiBuf) {
  pw::allocator::test::AllocatorForTest<256> allocator;
  pw::multibuf::v2::MultiBuf::Instance buf(allocator);
  EventPacket packet = EventPacket::New(std::move(buf));
  EXPECT_EQ(packet.size(), 0u);
}

TEST(EmbossControlPackets, ConstructFromSingleReleasableFragmentMultiBuf) {
  static constexpr std::array<std::byte, 6> data = {
      std::byte{hci_spec::kCommandCompleteEventCode},
      std::byte{0x04},  // size
      std::byte{0x01},  // num hci packets
      std::byte{0x02},
      std::byte{0x03},  // command opcode
      std::byte{0x00}   // status (success)
  };

  pw::allocator::test::AllocatorForTest<256> allocator;
  pw::UniquePtr<std::byte[]> bytes =
      allocator.MakeUnique<std::byte[]>(data.size());
  std::memcpy(bytes.get(), data.data(), data.size());

  pw::multibuf::v2::MultiBuf::Instance buf(allocator);
  buf->Insert(buf->end(), std::move(bytes));

  EventPacket packet = EventPacket::New(std::move(buf));
  EXPECT_EQ(packet.size(), 6u);
  EXPECT_EQ(packet.event_code(), hci_spec::kCommandCompleteEventCode);
  ASSERT_TRUE(packet.StatusCode().has_value());
  EXPECT_EQ(packet.StatusCode().value(),
            pw::bluetooth::emboss::StatusCode::SUCCESS);
}

TEST(EmbossControlPackets, ConstructFromSingleNonReleasableFragmentMultiBuf) {
  static constexpr std::array<std::byte, 6> data = {
      std::byte{hci_spec::kCommandCompleteEventCode},
      std::byte{0x04},  // size
      std::byte{0x01},  // num hci packets
      std::byte{0x02},
      std::byte{0x03},  // command opcode
      std::byte{0x00}   // status (success)
  };

  pw::allocator::test::AllocatorForTest<256> allocator;
  pw::multibuf::v2::MultiBuf::Instance buf(allocator);
  buf->Insert(buf->end(), data);

  EventPacket packet = EventPacket::New(std::move(buf));

  EXPECT_EQ(packet.size(), 6u);
  EXPECT_EQ(packet.event_code(), hci_spec::kCommandCompleteEventCode);
  ASSERT_TRUE(packet.StatusCode().has_value());
  EXPECT_EQ(packet.StatusCode().value(),
            pw::bluetooth::emboss::StatusCode::SUCCESS);
}

TEST(EmbossControlPackets, ConstructFromMultipleReleasableFragmentMultiBuf) {
  static constexpr std::array<std::byte, 3> first_chunk = {
      std::byte{hci_spec::kCommandCompleteEventCode},
      std::byte{0x04},  // size
      std::byte{0x01},  // num hci packets
  };
  static constexpr std::array<std::byte, 3> second_chunk = {
      std::byte{0x02},
      std::byte{0x03},  // command opcode
      std::byte{0x00}   // status (success)
  };

  pw::allocator::test::AllocatorForTest<256> allocator;
  pw::UniquePtr<std::byte[]> first_chunk_bytes =
      allocator.MakeUnique<std::byte[]>(first_chunk.size());
  std::memcpy(first_chunk_bytes.get(), first_chunk.data(), first_chunk.size());
  pw::UniquePtr<std::byte[]> second_chunk_bytes =
      allocator.MakeUnique<std::byte[]>(second_chunk.size());
  std::memcpy(
      second_chunk_bytes.get(), second_chunk.data(), second_chunk.size());

  pw::multibuf::v2::MultiBuf::Instance buf(allocator);
  buf->Insert(buf->end(), std::move(first_chunk_bytes));
  buf->Insert(buf->end(), std::move(second_chunk_bytes));

  EventPacket packet = EventPacket::New(std::move(buf));

  EXPECT_EQ(packet.size(), 6u);
  EXPECT_EQ(packet.event_code(), hci_spec::kCommandCompleteEventCode);
  ASSERT_TRUE(packet.StatusCode().has_value());
  EXPECT_EQ(packet.StatusCode().value(),
            pw::bluetooth::emboss::StatusCode::SUCCESS);
}

}  // namespace bt::hci
