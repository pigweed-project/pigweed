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

namespace internal {
struct Presets;
}  // namespace internal

// Codec SCO parameter sets as defined in HFP specification (v1.8, section 5.7).
class ParameterSet {
 public:
  constexpr uint32_t transmit_receive_bandwidth() const {
    return transmit_receive_bandwidth_;
  }
  constexpr uint16_t max_latency_ms() const { return max_latency_ms_; }
  constexpr pw::bluetooth::emboss::CodingFormat transmit_receive_format()
      const {
    return transmit_receive_format_;
  }
  constexpr pw::bluetooth::emboss::SynchronousConnectionParameters::
      ScoRetransmissionEffort
      retransmission_effort() const {
    return retransmission_effort_;
  }

  // Populates `params.packet_types()` using Emboss `ScoPacketType` flags for
  // this parameter set.
  //
  // Clears bits [0..9] (`ScoPacketType`) before setting the enabled flags.
  // Reserved bits [10..15] of the surrounding 16-bit HCI field are left
  // unchanged; callers wrapping non-zeroed storage must ensure those reserved
  // bits are already 0.
  void WritePacketTypes(
      pw::bluetooth::emboss::SynchronousConnectionParametersWriter params)
      const {
    auto packet_types = params.packet_types();
    packet_types.BackingStorage().WriteUInt(0);
    switch (packet_type_preset_) {
      case PacketTypePreset::kSingleSlotEsco:
        // Permits EV3 (and EDR 2-EV3/3-EV3) while disabling 2-EV5 and 3-EV5.
        packet_types.ev3().Write(true);
        packet_types.not_2_ev5().Write(true);
        packet_types.not_3_ev5().Write(true);
        break;
      case PacketTypePreset::kS1:
        // Permits HV1, HV2, HV3, and EV3 (and EDR 2-EV3/3-EV3) while disabling
        // 2-EV5 and 3-EV5.
        packet_types.hv1().Write(true);
        packet_types.hv2().Write(true);
        packet_types.hv3().Write(true);
        packet_types.ev3().Write(true);
        packet_types.not_2_ev5().Write(true);
        packet_types.not_3_ev5().Write(true);
        break;
      case PacketTypePreset::kD0:
        // Sets the `hv1` flag (0x0001; does not set the negative EDR eSCO
        // bits).
        packet_types.hv1().Write(true);
        break;
      case PacketTypePreset::kD1:
        // Sets the `hv3` flag (0x0004; does not set the negative EDR eSCO
        // bits).
        packet_types.hv3().Write(true);
        break;
    }
  }

 private:
  friend struct internal::Presets;

  enum class PacketTypePreset : uint8_t {
    kSingleSlotEsco,
    kS1,
    kD0,
    kD1,
  };

  constexpr ParameterSet(
      PacketTypePreset preset,
      uint32_t bandwidth,
      pw::bluetooth::emboss::CodingFormat format,
      uint16_t max_latency,
      pw::bluetooth::emboss::SynchronousConnectionParameters::
          ScoRetransmissionEffort effort)
      : transmit_receive_bandwidth_(bandwidth),
        max_latency_ms_(max_latency),
        transmit_receive_format_(format),
        retransmission_effort_(effort),
        packet_type_preset_(preset) {}

  uint32_t transmit_receive_bandwidth_;
  uint16_t max_latency_ms_;
  pw::bluetooth::emboss::CodingFormat transmit_receive_format_;
  pw::bluetooth::emboss::SynchronousConnectionParameters::
      ScoRetransmissionEffort retransmission_effort_;
  PacketTypePreset packet_type_preset_;
};

namespace internal {

struct Presets {
  using Format = pw::bluetooth::emboss::CodingFormat;
  using Effort = pw::bluetooth::emboss::SynchronousConnectionParameters::
      ScoRetransmissionEffort;
  using Preset = ParameterSet::PacketTypePreset;

  static constexpr ParameterSet kT1{Preset::kSingleSlotEsco,
                                    8000,
                                    Format::TRANSPARENT,
                                    8,
                                    Effort::QUALITY_OPTIMIZED};
  static constexpr ParameterSet kT2{Preset::kSingleSlotEsco,
                                    8000,
                                    Format::TRANSPARENT,
                                    13,
                                    Effort::QUALITY_OPTIMIZED};
  static constexpr ParameterSet kS1{
      Preset::kS1, 8000, Format::CVSD, 7, Effort::POWER_OPTIMIZED};
  static constexpr ParameterSet kS2{
      Preset::kSingleSlotEsco, 8000, Format::CVSD, 7, Effort::POWER_OPTIMIZED};
  static constexpr ParameterSet kS3{
      Preset::kSingleSlotEsco, 8000, Format::CVSD, 10, Effort::POWER_OPTIMIZED};
  static constexpr ParameterSet kS4{Preset::kSingleSlotEsco,
                                    8000,
                                    Format::CVSD,
                                    12,
                                    Effort::QUALITY_OPTIMIZED};
  static constexpr ParameterSet kD0{
      Preset::kD0, 8000, Format::CVSD, 0xFFFF, Effort::DONT_CARE};
  static constexpr ParameterSet kD1{
      Preset::kD1, 8000, Format::CVSD, 0xFFFF, Effort::DONT_CARE};
};

}  // namespace internal

inline constexpr ParameterSet kParameterSetT1 = internal::Presets::kT1;
inline constexpr ParameterSet kParameterSetT2 = internal::Presets::kT2;
inline constexpr ParameterSet kParameterSetS1 = internal::Presets::kS1;
inline constexpr ParameterSet kParameterSetS2 = internal::Presets::kS2;
inline constexpr ParameterSet kParameterSetS3 = internal::Presets::kS3;
inline constexpr ParameterSet kParameterSetS4 = internal::Presets::kS4;
inline constexpr ParameterSet kParameterSetD0 = internal::Presets::kD0;
inline constexpr ParameterSet kParameterSetD1 = internal::Presets::kD1;

}  // namespace bt::sco
