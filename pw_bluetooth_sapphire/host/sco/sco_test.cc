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

#include <cstdint>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/transport/emboss_packet.h"
#include "pw_unit_test/framework.h"

namespace bt::sco {
namespace {

using pw::bluetooth::emboss::CodingFormat;
using pw::bluetooth::emboss::SynchronousConnectionParametersWriter;

void ExpectSingleSlotEscoPacketTypes(uint16_t raw_packet_types) {
  StaticPacket<SynchronousConnectionParametersWriter> params;
  params.view().packet_types().BackingStorage().WriteUInt(raw_packet_types);
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
  ExpectSingleSlotEscoPacketTypes(kParameterSetT1.packet_types);
  EXPECT_EQ(kParameterSetT1.transmit_receive_bandwidth, 8000u);
  EXPECT_EQ(kParameterSetT1.transmit_receive_format, CodingFormat::TRANSPARENT);
  EXPECT_EQ(kParameterSetT1.max_latency_ms, 8u);
  EXPECT_EQ(kParameterSetT1.retransmission_effort,
            hci_spec::ScoRetransmissionEffort::kQualityOptimized);
}

TEST(ScoParameterSetTest, T2) {
  ExpectSingleSlotEscoPacketTypes(kParameterSetT2.packet_types);
  EXPECT_EQ(kParameterSetT2.transmit_receive_bandwidth, 8000u);
  EXPECT_EQ(kParameterSetT2.transmit_receive_format, CodingFormat::TRANSPARENT);
  EXPECT_EQ(kParameterSetT2.max_latency_ms, 13u);
  EXPECT_EQ(kParameterSetT2.retransmission_effort,
            hci_spec::ScoRetransmissionEffort::kQualityOptimized);
}

TEST(ScoParameterSetTest, S1) {
  StaticPacket<SynchronousConnectionParametersWriter> params;
  params.view().packet_types().BackingStorage().WriteUInt(
      kParameterSetS1.packet_types);
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
  EXPECT_EQ(kParameterSetS1.transmit_receive_bandwidth, 8000u);
  EXPECT_EQ(kParameterSetS1.transmit_receive_format, CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetS1.max_latency_ms, 7u);
  EXPECT_EQ(kParameterSetS1.retransmission_effort,
            hci_spec::ScoRetransmissionEffort::kPowerOptimized);
}

TEST(ScoParameterSetTest, S2) {
  ExpectSingleSlotEscoPacketTypes(kParameterSetS2.packet_types);
  EXPECT_EQ(kParameterSetS2.transmit_receive_bandwidth, 8000u);
  EXPECT_EQ(kParameterSetS2.transmit_receive_format, CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetS2.max_latency_ms, 7u);
  EXPECT_EQ(kParameterSetS2.retransmission_effort,
            hci_spec::ScoRetransmissionEffort::kPowerOptimized);
}

TEST(ScoParameterSetTest, S3) {
  ExpectSingleSlotEscoPacketTypes(kParameterSetS3.packet_types);
  EXPECT_EQ(kParameterSetS3.transmit_receive_bandwidth, 8000u);
  EXPECT_EQ(kParameterSetS3.transmit_receive_format, CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetS3.max_latency_ms, 10u);
  EXPECT_EQ(kParameterSetS3.retransmission_effort,
            hci_spec::ScoRetransmissionEffort::kPowerOptimized);
}

TEST(ScoParameterSetTest, S4) {
  ExpectSingleSlotEscoPacketTypes(kParameterSetS4.packet_types);
  EXPECT_EQ(kParameterSetS4.transmit_receive_bandwidth, 8000u);
  EXPECT_EQ(kParameterSetS4.transmit_receive_format, CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetS4.max_latency_ms, 12u);
  EXPECT_EQ(kParameterSetS4.retransmission_effort,
            hci_spec::ScoRetransmissionEffort::kQualityOptimized);
}

TEST(ScoParameterSetTest, D0) {
  StaticPacket<SynchronousConnectionParametersWriter> params;
  params.view().packet_types().BackingStorage().WriteUInt(
      kParameterSetD0.packet_types);
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
  EXPECT_EQ(kParameterSetD0.transmit_receive_bandwidth, 8000u);
  EXPECT_EQ(kParameterSetD0.transmit_receive_format, CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetD0.max_latency_ms, 0xFFFFu);
  EXPECT_EQ(kParameterSetD0.retransmission_effort,
            hci_spec::ScoRetransmissionEffort::kDontCare);
}

TEST(ScoParameterSetTest, D1) {
  StaticPacket<SynchronousConnectionParametersWriter> params;
  params.view().packet_types().BackingStorage().WriteUInt(
      kParameterSetD1.packet_types);
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
  EXPECT_EQ(kParameterSetD1.transmit_receive_bandwidth, 8000u);
  EXPECT_EQ(kParameterSetD1.transmit_receive_format, CodingFormat::CVSD);
  EXPECT_EQ(kParameterSetD1.max_latency_ms, 0xFFFFu);
  EXPECT_EQ(kParameterSetD1.retransmission_effort,
            hci_spec::ScoRetransmissionEffort::kDontCare);
}

}  // namespace
}  // namespace bt::sco
