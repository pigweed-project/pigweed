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

#pragma once

#include <pw_bluetooth/hci_data.emb.h>
#include <pw_bluetooth/hci_events.emb.h>
#include <pw_function/function.h>

#include <optional>

#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"

namespace bt::iso {

// Maximum possible size of an Isochronous data packet.
// See Core Spec v5.4, Volume 4, Part E, Section 5.4.5
inline constexpr size_t kMaxIsochronousDataPacketSize =
    pw::bluetooth::emboss::IsoDataFrameHeader::MaxSizeInBytes() +
    hci_spec::kMaxIsochronousDataPacketPayloadSize;

using IsoDataPacket = std::vector<std::byte>;

// Possible outcomes from an AcceptCis call
enum class AcceptCisStatus {
  // We're now waiting for an incoming CIS request with the specified attributes
  kSuccess,

  // This connection is not operating as a peripheral
  kNotPeripheral,

  // A request is already pending for this CIG/CIS combination
  kAlreadyExists,
};

// Our internal representation of the parameters returned from the
// HCI_LE_CIS_Established event.
struct CisEstablishedParameters {
  struct CisUnidirectionalParams {
    // The actual transport latency, in microseconds.
    uint32_t transport_latency;

    // The transmitter PHY.
    pw::bluetooth::emboss::IsoPhyType phy;

    uint8_t burst_number;

    // The flush timeout, in multiples of the ISO_Interval for the CIS, for each
    // payload sent.
    uint8_t flush_timeout;

    // Maximum size, in octets, of the payload.
    uint16_t max_pdu_size;
  };

  // The maximum time, in microseconds, for transmission of PDUs of all CISes in
  // a CIG event.
  uint32_t cig_sync_delay;

  // The maximum time, in microseconds, for transmission of PDUs of the
  // specified CIS in a CIG event.
  uint32_t cis_sync_delay;

  // Maximum number of subevents in each CIS event.
  uint8_t max_subevents;

  // The "Iso Interval" is represented in units of 1.25ms.
  // (Core Spec v5.4, Vol 4, Part E, Sec 7.7.65.25)
  static constexpr size_t kIsoIntervalToMicroseconds = 1250;

  // The time between two consecutive CIS anchor points.
  uint16_t iso_interval;

  // Central => Peripheral parameters
  CisUnidirectionalParams c_to_p_params;

  // Peripheral => Central parameters
  CisUnidirectionalParams p_to_c_params;
};

class IsoStream;

using CisEstablishedCallback =
    pw::Callback<void(pw::bluetooth::emboss::StatusCode,
                      std::optional<WeakSelf<IsoStream>::WeakPtr>,
                      const std::optional<CisEstablishedParameters>&)>;

// A convenience class for holding an identifier that uniquely represents a
// CIG/CIS combination.
class CigCisIdentifier {
 public:
  CigCisIdentifier() = delete;
  CigCisIdentifier(hci_spec::CigIdentifier cig_id,
                   hci_spec::CisIdentifier cis_id)
      : cig_id_(cig_id), cis_id_(cis_id) {}

  bool operator==(const CigCisIdentifier other) const {
    return (other.cig_id() == cig_id_) && (other.cis_id() == cis_id());
  }

  hci_spec::CigIdentifier cig_id() const { return cig_id_; }
  hci_spec::CisIdentifier cis_id() const { return cis_id_; }

 private:
  hci_spec::CigIdentifier cig_id_;
  hci_spec::CisIdentifier cis_id_;
};

enum class CigFraming {
  kUnframed = 0x00,
  kFramed = 0x01,
};

enum class CigPacking {
  kSequential = 0x00,
  kInterleaved = 0x01,
};

struct CigParams {
  uint32_t sdu_interval_c_to_p;
  uint32_t sdu_interval_p_to_c;
  CigPacking packing;
  CigFraming framing;
  uint16_t max_transport_latency_c_to_p;
  uint16_t max_transport_latency_p_to_c;

  /// The worst-case sleep clock accuracy range to use. If not supplied,
  /// the system will assume the worst possible value as required by spec.
  std::optional<pw::bluetooth::emboss::LESleepClockAccuracyRange>
      worst_case_sca;
};

struct CisConfigParams {
  hci_spec::CisIdentifier cis_id;
  uint16_t max_sdu_c_to_p;
  uint16_t max_sdu_p_to_c;
};

struct CigCisParams {
  CisConfigParams config;
  CisEstablishedCallback on_established_cb;
};

// An interface for types which can create streams for an isochronous group. In
// production this is IsoStreamManager to centralize ISO stream management.
class CigStreamCreator {
 public:
  virtual ~CigStreamCreator() = default;

  virtual WeakSelf<IsoStream>::WeakPtr CreateCisConfiguration(
      CigCisIdentifier id,
      hci_spec::ConnectionHandle cis_handle,
      CisEstablishedCallback on_established_cb,
      pw::Callback<void()> on_closed_cb) = 0;

  using WeakPtr = WeakSelf<CigStreamCreator>::WeakPtr;
};

}  // namespace bt::iso

namespace std {
// Implement hash operator for CigCisIdentifier.
template <>
struct hash<bt::iso::CigCisIdentifier> {
 public:
  std::size_t operator()(const bt::iso::CigCisIdentifier& id) const {
    return ((static_cast<size_t>(id.cig_id()) << 8) | id.cis_id());
  }
};

}  // namespace std
