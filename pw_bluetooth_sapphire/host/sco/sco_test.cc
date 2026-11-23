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

#include "pw_bluetooth_sapphire/internal/host/sco/sco.h"

#include <pw_bluetooth/hci_commands.emb.h>
#include <pw_bluetooth/hci_common.emb.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/emboss_packet.h"
#include "pw_unit_test/framework.h"

namespace bt::sco {
namespace {

static_assert(std::is_trivially_copyable_v<ParameterSet>);
static_assert(!std::is_default_constructible_v<ParameterSet>);
static_assert(sizeof(ParameterSet) == 12);

using pw::bluetooth::emboss::CodingFormat;
using pw::bluetooth::emboss::EnhancedSetupSynchronousConnectionCommandWriter;
using pw::bluetooth::emboss::OpCode;
using pw::bluetooth::emboss::SynchronousConnectionParametersWriter;
using ScoRetransmissionEffort = pw::bluetooth::emboss::
    SynchronousConnectionParameters::ScoRetransmissionEffort;

void ExpectSingleSlotEscoPacketTypes(const ParameterSet& param_set) {
  StaticPacket<SynchronousConnectionParametersWriter> params;
  param_set.WritePacketTypes(params.view());
  auto packet_types = params.view().packet_types();
  EXPECT_FALSE(packet_types.hv1().Read());
  EXPECT_FALSE(packet_types.hv2().Read());
  EXPECT_FALSE(packet_types.hv3().Read());
  EXPECT_TRUE(packet_types.ev3().Read());
  EXPECT_FALSE(packet_types.ev4().Read());
  EXPECT_FALSE(packet_types.ev5().Read());
  EXPECT_FALSE(packet_types.not_2_ev3().Read());
  EXPECT_FALSE(packet_types.not_3_ev3().Read());
  EXPECT_TRUE(packet_types.not_2_ev5().Read());
  EXPECT_TRUE(packet_types.not_3_ev5().Read());
  EXPECT_EQ(packet_types.BackingStorage().ReadUInt(), 0x0308u);
}

TEST(ScoParameterSetTest, T1) {
  ExpectSingleSlotEscoPacketTypes(kParameterSetT1);
  EXPECT_EQ(kParameterSetT1.transmit_receive_bandwidth(), 8000u);
  EXPECT_EQ(kParameterSetT1.transmit_receive_format(),
            CodingFormat::TRANSPARENT);
  EXPECT_EQ(kParameterSetT1.max_latency_ms(), 8u);
  EXPECT_EQ(kParameterSetT1.retransmission_effort(),
            ScoRetransmissionEffort::QUALITY_OPTIMIZED);
}

TEST(ScoParameterSetTest, T2) {
  ExpectSingleSlotEscoPacketTypes(kParameterSetT2);
  EXPECT_EQ(kParameterSetT2.transmit_receive_bandwidth(), 8000u);
  EXPECT_EQ(kParameterSetT2.transmit_receive_format(),
            CodingFormat::TRANSPARENT);
  EXPECT_EQ(kParameterSetT2.max_latency_ms(), 13u);
  EXPECT_EQ(kParameterSetT2.retransmission_effort(),
            ScoRetransmissionEffort::QUALITY_OPTIMIZED);
}

TEST(ScoParameterSetTest, S1) {
  StaticPacket<SynchronousConnectionParametersWriter> params;
  kParameterSetS1.WritePacketTypes(params.view());
  auto packet_types = params.view().packet_types();
  EXPECT_TRUE(packet_types.hv1().Read());
  EXPECT_TRUE(packet_types.hv2().Read());
  EXPECT_TRUE(packet_types.hv3().Read());
  EXPECT_TRUE(packet_types.ev3().Read());
  EXPECT_FALSE(packet_types.ev4().Read());
  EXPECT_FALSE(packet_types.ev5().Read());
  EXPECT_FALSE(packet_types.not_2_ev3().Read());
  EXPECT_FALSE(packet_types.not_3_ev3().Read());
  EXPECT_TRUE(packet_types.not_2_ev5().Read());
  EXPECT_TRUE(packet_types.not_3_ev5().Read());
  EXPECT_EQ(packet_types.BackingStorage().ReadUInt(), 0x030Fu);
  EXPECT_EQ(kParameterSetS1.transmit_receive_bandwidth(), 8000u);
  EXPECT_EQ(kParameterSetS1.transmit_receive_format(), CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetS1.max_latency_ms(), 7u);
  EXPECT_EQ(kParameterSetS1.retransmission_effort(),
            ScoRetransmissionEffort::POWER_OPTIMIZED);
}

TEST(ScoParameterSetTest, S2) {
  ExpectSingleSlotEscoPacketTypes(kParameterSetS2);
  EXPECT_EQ(kParameterSetS2.transmit_receive_bandwidth(), 8000u);
  EXPECT_EQ(kParameterSetS2.transmit_receive_format(), CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetS2.max_latency_ms(), 7u);
  EXPECT_EQ(kParameterSetS2.retransmission_effort(),
            ScoRetransmissionEffort::POWER_OPTIMIZED);
}

TEST(ScoParameterSetTest, S3) {
  ExpectSingleSlotEscoPacketTypes(kParameterSetS3);
  EXPECT_EQ(kParameterSetS3.transmit_receive_bandwidth(), 8000u);
  EXPECT_EQ(kParameterSetS3.transmit_receive_format(), CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetS3.max_latency_ms(), 10u);
  EXPECT_EQ(kParameterSetS3.retransmission_effort(),
            ScoRetransmissionEffort::POWER_OPTIMIZED);
}

TEST(ScoParameterSetTest, S4) {
  ExpectSingleSlotEscoPacketTypes(kParameterSetS4);
  EXPECT_EQ(kParameterSetS4.transmit_receive_bandwidth(), 8000u);
  EXPECT_EQ(kParameterSetS4.transmit_receive_format(), CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetS4.max_latency_ms(), 12u);
  EXPECT_EQ(kParameterSetS4.retransmission_effort(),
            ScoRetransmissionEffort::QUALITY_OPTIMIZED);
}

TEST(ScoParameterSetTest, D0) {
  StaticPacket<SynchronousConnectionParametersWriter> params;
  kParameterSetD0.WritePacketTypes(params.view());
  auto packet_types = params.view().packet_types();
  EXPECT_TRUE(packet_types.hv1().Read());
  EXPECT_FALSE(packet_types.hv2().Read());
  EXPECT_FALSE(packet_types.hv3().Read());
  EXPECT_FALSE(packet_types.ev3().Read());
  EXPECT_FALSE(packet_types.ev4().Read());
  EXPECT_FALSE(packet_types.ev5().Read());
  EXPECT_FALSE(packet_types.not_2_ev3().Read());
  EXPECT_FALSE(packet_types.not_3_ev3().Read());
  EXPECT_FALSE(packet_types.not_2_ev5().Read());
  EXPECT_FALSE(packet_types.not_3_ev5().Read());
  EXPECT_EQ(packet_types.BackingStorage().ReadUInt(), 0x0001u);
  EXPECT_EQ(kParameterSetD0.transmit_receive_bandwidth(), 8000u);
  EXPECT_EQ(kParameterSetD0.transmit_receive_format(), CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetD0.max_latency_ms(), 0xFFFFu);
  EXPECT_EQ(kParameterSetD0.retransmission_effort(),
            ScoRetransmissionEffort::DONT_CARE);
}

TEST(ScoParameterSetTest, D1) {
  StaticPacket<SynchronousConnectionParametersWriter> params;
  kParameterSetD1.WritePacketTypes(params.view());
  auto packet_types = params.view().packet_types();
  EXPECT_FALSE(packet_types.hv1().Read());
  EXPECT_FALSE(packet_types.hv2().Read());
  EXPECT_TRUE(packet_types.hv3().Read());
  EXPECT_FALSE(packet_types.ev3().Read());
  EXPECT_FALSE(packet_types.ev4().Read());
  EXPECT_FALSE(packet_types.ev5().Read());
  EXPECT_FALSE(packet_types.not_2_ev3().Read());
  EXPECT_FALSE(packet_types.not_3_ev3().Read());
  EXPECT_FALSE(packet_types.not_2_ev5().Read());
  EXPECT_FALSE(packet_types.not_3_ev5().Read());
  EXPECT_EQ(packet_types.BackingStorage().ReadUInt(), 0x0004u);
  EXPECT_EQ(kParameterSetD1.transmit_receive_bandwidth(), 8000u);
  EXPECT_EQ(kParameterSetD1.transmit_receive_format(), CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetD1.max_latency_ms(), 0xFFFFu);
  EXPECT_EQ(kParameterSetD1.retransmission_effort(),
            ScoRetransmissionEffort::DONT_CARE);
}

TEST(ScoParameterSetTest, WritePacketTypesClearsPreviousBits) {
  StaticPacket<SynchronousConnectionParametersWriter> params;
  // In SynchronousConnectionParameters (57 bytes), the 16-bit packet_types
  // field occupies bytes [54..55], followed by the 1-byte
  // retransmission_effort field at byte 56.
  constexpr size_t kPacketTypesOffset = 54;
  static_assert(
      SynchronousConnectionParametersWriter::IntrinsicSizeInBytes().Read() ==
      57);
  auto read_packet_types_u16 = [&params]() -> uint16_t {
    return static_cast<uint16_t>(
        static_cast<uint16_t>(params.data()[kPacketTypesOffset]) |
        (static_cast<uint16_t>(params.data()[kPacketTypesOffset + 1]) << 8));
  };

  params.mutable_data().Fill(0xFF);
  kParameterSetD0.WritePacketTypes(params.view());
  EXPECT_EQ(params.view().packet_types().BackingStorage().ReadUInt(), 0x0001u);
  // Reserved bits [10..15] (0xFC00) remain intact.
  EXPECT_EQ(read_packet_types_u16(), 0xFC01u);

  params.mutable_data().Fill(0xFF);
  kParameterSetT1.WritePacketTypes(params.view());
  EXPECT_EQ(params.view().packet_types().BackingStorage().ReadUInt(), 0x0308u);
  // 0xFC00 | 0x0308 == 0xFF08.
  EXPECT_EQ(read_packet_types_u16(), 0xFF08u);
}

TEST(ScoParameterSetTest, WritePacketTypesOnCommandSubView) {
  auto packet =
      hci::CommandPacket::New<EnhancedSetupSynchronousConnectionCommandWriter>(
          OpCode::ENHANCED_SETUP_SYNCHRONOUS_CONNECTION);
  kParameterSetS1.WritePacketTypes(packet.view_t().connection_parameters());
  EXPECT_EQ(packet.view_t()
                .connection_parameters()
                .packet_types()
                .BackingStorage()
                .ReadUInt(),
            0x030Fu);
}

}  // namespace
}  // namespace bt::sco
