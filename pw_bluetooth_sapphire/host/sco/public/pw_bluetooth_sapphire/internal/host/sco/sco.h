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

#pragma once

#include <pw_bluetooth/hci_commands.emb.h>
#include <pw_bluetooth/hci_common.emb.h>

#include <cstdint>

namespace bt::sco {

// TODO(fxbug.dev/42062077): This type should be deleted as it duplicates
// `ScoPacketType` in hci-protocol.emb. This may involve migrating
// `ParameterSet` below to Emboss.
enum class ScoPacketTypeBits : uint16_t {
  // SCO packet types
  kHv1 = (1 << 0),
  kHv2 = (1 << 1),
  kHv3 = (1 << 2),
  // eSCO packet types
  kEv3 = (1 << 3),
  kEv4 = (1 << 4),
  kEv5 = (1 << 5),
  kNot2Ev3 = (1 << 6),
  kNot3Ev3 = (1 << 7),
  kNot2Ev5 = (1 << 8),
  kNot3Ev5 = (1 << 9),
};

// Codec SCO parameter sets as defined in HFP specification (v1.8, section 5.7).
struct ParameterSet {
  uint16_t packet_types;
  uint32_t transmit_receive_bandwidth;
  pw::bluetooth::emboss::CodingFormat transmit_receive_format;
  uint16_t max_latency_ms;
  pw::bluetooth::emboss::SynchronousConnectionParameters::
      ScoRetransmissionEffort retransmission_effort;
};

constexpr ParameterSet kParameterSetT1{
    .packet_types = static_cast<uint16_t>(ScoPacketTypeBits::kEv3) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot2Ev5) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot3Ev5),
    .transmit_receive_bandwidth = 8000,
    .transmit_receive_format = pw::bluetooth::emboss::CodingFormat::TRANSPARENT,
    .max_latency_ms = 8,
    .retransmission_effort =
        pw::bluetooth::emboss::SynchronousConnectionParameters::
            ScoRetransmissionEffort::QUALITY_OPTIMIZED};

constexpr ParameterSet kParameterSetT2{
    .packet_types = static_cast<uint16_t>(ScoPacketTypeBits::kEv3) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot2Ev5) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot3Ev5),
    .transmit_receive_bandwidth = 8000,
    .transmit_receive_format = pw::bluetooth::emboss::CodingFormat::TRANSPARENT,
    .max_latency_ms = 13,
    .retransmission_effort =
        pw::bluetooth::emboss::SynchronousConnectionParameters::
            ScoRetransmissionEffort::QUALITY_OPTIMIZED};

constexpr ParameterSet kParameterSetS1{
    .packet_types = static_cast<uint16_t>(ScoPacketTypeBits::kHv1) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kHv2) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kHv3) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kEv3) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot2Ev5) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot3Ev5),
    .transmit_receive_bandwidth = 8000,
    .transmit_receive_format = pw::bluetooth::emboss::CodingFormat::CVSD,
    .max_latency_ms = 7,
    .retransmission_effort =
        pw::bluetooth::emboss::SynchronousConnectionParameters::
            ScoRetransmissionEffort::POWER_OPTIMIZED};

constexpr ParameterSet kParameterSetS2{
    .packet_types = static_cast<uint16_t>(ScoPacketTypeBits::kEv3) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot2Ev5) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot3Ev5),
    .transmit_receive_bandwidth = 8000,
    .transmit_receive_format = pw::bluetooth::emboss::CodingFormat::CVSD,
    .max_latency_ms = 7,
    .retransmission_effort =
        pw::bluetooth::emboss::SynchronousConnectionParameters::
            ScoRetransmissionEffort::POWER_OPTIMIZED};

constexpr ParameterSet kParameterSetS3{
    .packet_types = static_cast<uint16_t>(ScoPacketTypeBits::kEv3) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot2Ev5) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot3Ev5),
    .transmit_receive_bandwidth = 8000,
    .transmit_receive_format = pw::bluetooth::emboss::CodingFormat::CVSD,
    .max_latency_ms = 10,
    .retransmission_effort =
        pw::bluetooth::emboss::SynchronousConnectionParameters::
            ScoRetransmissionEffort::POWER_OPTIMIZED};

constexpr ParameterSet kParameterSetS4{
    .packet_types = static_cast<uint16_t>(ScoPacketTypeBits::kEv3) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot2Ev5) |
                    static_cast<uint16_t>(ScoPacketTypeBits::kNot3Ev5),
    .transmit_receive_bandwidth = 8000,
    .transmit_receive_format = pw::bluetooth::emboss::CodingFormat::CVSD,
    .max_latency_ms = 12,
    .retransmission_effort =
        pw::bluetooth::emboss::SynchronousConnectionParameters::
            ScoRetransmissionEffort::QUALITY_OPTIMIZED};

constexpr ParameterSet kParameterSetD0{
    .packet_types = static_cast<uint16_t>(ScoPacketTypeBits::kHv1),
    .transmit_receive_bandwidth = 8000,
    .transmit_receive_format = pw::bluetooth::emboss::CodingFormat::CVSD,
    .max_latency_ms = 0xFFFF,
    .retransmission_effort = pw::bluetooth::emboss::
        SynchronousConnectionParameters::ScoRetransmissionEffort::DONT_CARE};

constexpr ParameterSet kParameterSetD1{
    .packet_types = static_cast<uint16_t>(ScoPacketTypeBits::kHv3),
    .transmit_receive_bandwidth = 8000,
    .transmit_receive_format = pw::bluetooth::emboss::CodingFormat::CVSD,
    .max_latency_ms = 0xFFFF,
    .retransmission_effort = pw::bluetooth::emboss::
        SynchronousConnectionParameters::ScoRetransmissionEffort::DONT_CARE};

}  // namespace bt::sco
