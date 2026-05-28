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

#include <fidl/fuchsia.bluetooth.bredr/cpp/fidl.h>
#include <fidl/fuchsia.bluetooth.le/cpp/natural_types.h>
#include <fidl/fuchsia.bluetooth/cpp/fidl.h>
#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <fuchsia/bluetooth/gatt/cpp/fidl.h>
#include <fuchsia/bluetooth/gatt2/cpp/fidl.h>
#include <fuchsia/bluetooth/host/cpp/fidl.h>
#include <fuchsia/bluetooth/le/cpp/fidl.h>
#include <fuchsia/bluetooth/sys/cpp/fidl.h>
#include <lib/fidl/cpp/type_converter.h>
#include <lib/fpromise/result.h>
#include <pw_assert/check.h>

#include <optional>

#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/error.h"
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/gap/adapter.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_advertising_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/gap/types.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/types.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_common.h"

// Helpers for implementing the Bluetooth FIDL interfaces.

namespace bt::hci {
class DiscoveryFilter;
}  // namespace bt::hci

namespace bthost::fidl_helpers {

namespace android_emb = pw::bluetooth::vendor::android_hci;

::fuchsia_bluetooth_le::PhysicalLayer IsoPhyToFidl(
    pw::bluetooth::emboss::IsoPhyType phy);

::fuchsia_bluetooth_le::PhysicalLayer LEPhyToFidl(
    pw::bluetooth::emboss::LEPhy phy);

[[nodiscard]] std::optional<::fuchsia_bluetooth::Appearance>
AppearanceToNewFidl(uint16_t appearance_raw);

// TODO(fxbug.dev/42171179): Temporary logic for converting between the stack
// identifier type (integer) and FIDL identifier type (string). Remove these
// once all FIDL interfaces have been converted to use integer IDs.
std::optional<bt::PeerId> PeerIdFromString(const std::string& id);

// Functions for generating a FIDL bluetooth::Status

fuchsia::bluetooth::ErrorCode HostErrorToFidlDeprecated(
    bt::HostError host_error);

fuchsia::bluetooth::Status NewFidlError(
    fuchsia::bluetooth::ErrorCode error_code, const std::string& description);

template <typename ProtocolErrorCode>
fuchsia::bluetooth::Status ResultToFidlDeprecated(
    const fit::result<bt::Error<ProtocolErrorCode>>& result,
    std::string msg = "") {
  fuchsia::bluetooth::Status fidl_status;
  if (result.is_ok()) {
    return fidl_status;
  }

  auto error = std::make_unique<fuchsia::bluetooth::Error>();
  error->description = msg.empty() ? bt_str(result) : std::move(msg);
  if (result.is_error()) {
    result.error_value().Visit(
        [&error](bt::HostError c) {
          error->error_code = HostErrorToFidlDeprecated(c);
        },
        [&](ProtocolErrorCode c) {
          if constexpr (bt::Error<
                            ProtocolErrorCode>::may_hold_protocol_error()) {
            error->error_code = fuchsia::bluetooth::ErrorCode::PROTOCOL_ERROR;
            error->protocol_error_code = static_cast<uint32_t>(c);
          } else {
            PW_CRASH("Protocol branch visited by bt::Error<NoProtocolError>");
          }
        });
  }

  fidl_status.error = std::move(error);
  return fidl_status;
}

// Convert a bt::HostError to fuchsia.bluetooth.sys.Error. This function does
// only deals with bt::HostError types and does not support Bluetooth
// protocol-specific errors; to represent such errors use protocol-specific FIDL
// error types.
fuchsia::bluetooth::sys::Error HostErrorToFidl(bt::HostError error);

// Convert a bt::Error to fuchsia.bluetooth.sys.Error. This function does only
// deals with bt::HostError codes and does not support Bluetooth
// protocol-specific errors; to represent such errors use protocol-specific FIDL
// error types.
template <typename ProtocolErrorCode>
fuchsia::bluetooth::sys::Error HostErrorToFidl(
    const bt::Error<ProtocolErrorCode>& error) {
  if (!error.is_host_error()) {
    return fuchsia::bluetooth::sys::Error::FAILED;
  }
  return HostErrorToFidl(error.host_error());
}

// Convert any bt::Status to a fpromise::result that uses the
// fuchsia.bluetooth.sys library error codes.
template <typename ProtocolErrorCode>
fpromise::result<void, fuchsia::bluetooth::sys::Error> ResultToFidl(
    const fit::result<bt::Error<ProtocolErrorCode>>& status) {
  if (status.is_ok()) {
    return fpromise::ok();
  } else {
    return fpromise::error(HostErrorToFidl(std::move(status).error_value()));
  }
}

// Convert a bt::att::Error to fuchsia.bluetooth.gatt.Error.
fuchsia::bluetooth::gatt::Error GattErrorToFidl(const bt::att::Error& error);

// Convert a bt::att::Error to fuchsia.bluetooth.gatt2.Error.
fuchsia::bluetooth::gatt2::Error AttErrorToGattFidlError(
    const bt::att::Error& error);

// Convert a fuchsia::bluetooth::Uuid to bt::UUID for old HLCPP FIDL bindings
bt::UUID UuidFromFidl(const fuchsia::bluetooth::Uuid& input);
// Convert a bt::UUID to fuchsia::bluetooth::Uuid for old HLCPP FIDL bindings
fuchsia::bluetooth::Uuid UuidToFidl(const bt::UUID& uuid);

// Convert a fuchsia_bluetooth::Uuid to bt::UUID for new C++ FIDL bindings
bt::UUID NewUuidFromFidl(const fuchsia_bluetooth::Uuid& input);

// Functions that convert FIDL types to library objects.
bt::sm::IOCapability IoCapabilityFromFidl(
    const fuchsia::bluetooth::sys::InputCapability,
    const fuchsia::bluetooth::sys::OutputCapability);
std::optional<bt::gap::BrEdrSecurityMode> BrEdrSecurityModeFromFidl(
    const fuchsia::bluetooth::sys::BrEdrSecurityMode mode);
bt::gap::LESecurityMode LeSecurityModeFromFidl(
    const fuchsia::bluetooth::sys::LeSecurityMode mode);
std::optional<bt::sm::SecurityLevel> SecurityLevelFromFidl(
    const fuchsia::bluetooth::sys::PairingSecurityLevel level);

// fuchsia.bluetooth.sys library helpers.
fuchsia::bluetooth::sys::TechnologyType TechnologyTypeToFidl(
    bt::gap::TechnologyType type);
fuchsia::bluetooth::sys::HostInfo HostInfoToFidl(
    const bt::gap::Adapter& adapter);
fuchsia::bluetooth::sys::Peer PeerToFidl(const bt::gap::Peer& peer);

// Functions to convert bonding data structures from FIDL.
std::optional<bt::DeviceAddress> AddressFromFidlBondingData(
    const fuchsia::bluetooth::sys::BondingData& data);
bt::sm::PairingData LePairingDataFromFidl(
    bt::DeviceAddress peer_address,
    const fuchsia::bluetooth::sys::LeBondData& data);
std::optional<bt::sm::LTK> BredrKeyFromFidl(
    const fuchsia::bluetooth::sys::BredrBondData& data);
std::vector<bt::UUID> BredrServicesFromFidl(
    const fuchsia::bluetooth::sys::BredrBondData& data);

// Function to construct a bonding data structure for a peer.
fuchsia::bluetooth::sys::BondingData PeerToFidlBondingData(
    const bt::gap::Adapter& adapter, const bt::gap::Peer& peer);

// Functions to construct FIDL LE library objects from library objects. Returns
// nullptr if the peer is not LE or if the peer's advertising data failed to
// parse.
fuchsia::bluetooth::le::RemoteDevicePtr NewLERemoteDevice(
    const bt::gap::Peer& peer);

// Validates the contents of a ScanFilter.
bool IsScanFilterValid(const fuchsia::bluetooth::le::ScanFilter& fidl_filter);

// Populates a library DiscoveryFilter based on a FIDL ScanFilter. Returns false
// if |fidl_filter| contains any malformed data and leaves |out_filter|
// unmodified.
bool PopulateDiscoveryFilter(
    const fuchsia::bluetooth::le::ScanFilter& fidl_filter,
    bt::hci::DiscoveryFilter* out_filter);
bt::hci::DiscoveryFilter DiscoveryFilterFromFidl(
    const fuchsia::bluetooth::le::Filter& fidl_filter);

// Converts the given |mode_hint| to a stack interval value.
bt::gap::AdvertisingInterval AdvertisingIntervalFromFidl(
    fuchsia::bluetooth::le::AdvertisingModeHint mode_hint);

std::optional<bt::AdvertisingData> AdvertisingDataFromFidl(
    const fuchsia::bluetooth::le::AdvertisingData& input);
fuchsia::bluetooth::le::AdvertisingData AdvertisingDataToFidl(
    const bt::AdvertisingData& input);
fuchsia::bluetooth::le::AdvertisingDataDeprecated
AdvertisingDataToFidlDeprecated(const bt::AdvertisingData& input);
fuchsia::bluetooth::le::ScanData AdvertisingDataToFidlScanData(
    const bt::AdvertisingData& input,
    pw::chrono::SystemClock::time_point timestamp);
::fuchsia_bluetooth_le::ScanData AdvertisingDataToNewFidlScanData(
    const bt::AdvertisingData& input, zx::time timestamp);

::fuchsia_bluetooth_le::BroadcastIsochronousGroupInfo
BroadcastIsochronousGroupInfoToFidl(
    const bt::hci_spec::BroadcastIsochronousGroupInfo& info);

// Constructs a fuchsia.bluetooth.le Peer type from the stack representation.
fuchsia::bluetooth::le::Peer PeerToFidlLe(const bt::gap::Peer& peer);

// Functions that convert FIDL GATT types to library objects.
bt::gatt::ReliableMode ReliableModeFromFidl(
    const fuchsia::bluetooth::gatt::WriteOptions& write_options);
// TODO(fxbug.dev/42141942): The 64 bit `fidl_gatt_id` can overflow the 16 bits
// of a bt:att::Handle that underlies Characteristic/DescriptorHandles when
// directly casted. Fix this.
bt::gatt::CharacteristicHandle CharacteristicHandleFromFidl(
    uint64_t fidl_gatt_id);
bt::gatt::DescriptorHandle DescriptorHandleFromFidl(uint64_t fidl_gatt_id);

// Constructs a sdp::ServiceRecord from a FIDL ServiceDefinition |definition|
fpromise::result<bt::sdp::ServiceRecord, fuchsia::bluetooth::ErrorCode>
ServiceDefinitionToServiceRecord(
    const fuchsia_bluetooth_bredr::ServiceDefinition& definition);

// Constructs a sdp::ServiceRecord from a FIDL ServiceDefinition |definition|
fpromise::result<bt::sdp::ServiceRecord, fuchsia::bluetooth::ErrorCode>
ServiceDefinitionToServiceRecord(
    const fuchsia::bluetooth::bredr::ServiceDefinition& definition);

// Constructs a FIDL ServiceDefinition from a sdp::ServiceRecord
fpromise::result<fuchsia::bluetooth::bredr::ServiceDefinition,
                 fuchsia::bluetooth::ErrorCode>
ServiceRecordToServiceDefinition(const bt::sdp::ServiceRecord& record);

bt::gap::BrEdrSecurityRequirements FidlToBrEdrSecurityRequirements(
    const fuchsia::bluetooth::ChannelParameters& fidl);

fpromise::result<bt::StaticPacket<
    pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
FidlToScoParameters(
    const fuchsia::bluetooth::bredr::ScoConnectionParameters& params,
    uint8_t sco_offload_path_index);
fpromise::result<std::vector<bt::StaticPacket<
    pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>>
FidlToScoParametersVector(
    const std::vector<fuchsia::bluetooth::bredr::ScoConnectionParameters>&
        params,
    uint8_t sco_offload_path_index);

// Returns true if |handle| is within the valid handle range.
bool IsFidlGattHandleValid(fuchsia::bluetooth::gatt2::Handle handle);
bool IsFidlGattServiceHandleValid(
    fuchsia::bluetooth::gatt2::ServiceHandle handle);

fuchsia::bluetooth::bredr::RxPacketStatus ScoPacketStatusToFidl(
    bt::hci_spec::SynchronousDataPacketStatusFlag status);

bt::att::ErrorCode Gatt2ErrorCodeFromFidl(
    fuchsia::bluetooth::gatt2::Error error_code);

bt::att::AccessRequirements Gatt2AccessRequirementsFromFidl(
    const fuchsia::bluetooth::gatt2::SecurityRequirements& reqs);

void FillInAttributePermissionsDefaults(
    fuchsia::bluetooth::gatt2::AttributePermissions& reqs);

// Returns the bt-host representation of the FIDL descriptor, or nullptr if the
// conversion fails.
std::unique_ptr<bt::gatt::Descriptor> Gatt2DescriptorFromFidl(
    const fuchsia::bluetooth::gatt2::Descriptor& fidl_desc);

// Returns the bt-host representation of the FIDL characteristc, or nullptr if
// the conversion fails.
std::unique_ptr<bt::gatt::Characteristic> Gatt2CharacteristicFromFidl(
    const fuchsia::bluetooth::gatt2::Characteristic& fidl_chrc);

std::optional<android_emb::A2dpCodecType> FidlToCodecType(
    const fuchsia::bluetooth::bredr::AudioOffloadFeatures& codec);

bt::StaticPacket<android_emb::A2dpScmsTEnableWriter> FidlToScmsTEnable(
    bool scms_t_enable);

std::optional<android_emb::A2dpSamplingFrequency> FidlToSamplingFrequency(
    fuchsia::bluetooth::bredr::AudioSamplingFrequency sampling_frequency);

std::optional<android_emb::A2dpBitsPerSample> FidlToBitsPerSample(
    fuchsia::bluetooth::bredr::AudioBitsPerSample bits_per_sample);

std::optional<android_emb::A2dpChannelMode> FidlToChannelMode(
    fuchsia::bluetooth::bredr::AudioChannelMode channel_mode);

bt::StaticPacket<android_emb::SbcCodecInformationWriter>
FidlToEncoderSettingsSbc(
    const fuchsia::bluetooth::bredr::AudioEncoderSettings& encoder_settings,
    fuchsia::bluetooth::bredr::AudioSamplingFrequency sampling_frequency,
    fuchsia::bluetooth::bredr::AudioChannelMode channel_mode);

bt::StaticPacket<android_emb::AacCodecInformationWriter>
FidlToEncoderSettingsAac(
    const fuchsia::bluetooth::bredr::AudioEncoderSettings& encoder_settings,
    fuchsia::bluetooth::bredr::AudioSamplingFrequency sampling_frequency,
    fuchsia::bluetooth::bredr::AudioChannelMode channel_mode);

// For old HLCPP FIDL bindings
std::optional<bt::sdp::DataElement> FidlToDataElement(
    const fuchsia::bluetooth::bredr::DataElement& fidl);
// For new C++ FIDL bindings
std::optional<bt::sdp::DataElement> NewFidlToDataElement(
    const fuchsia_bluetooth_bredr::DataElement& fidl);

const char* DataPathDirectionToString(
    pw::bluetooth::emboss::DataPathDirection direction);

pw::bluetooth::emboss::DataPathDirection DataPathDirectionFromFidl(
    const fuchsia::bluetooth::DataDirection& fidl_direction);

pw::bluetooth::emboss::CodingFormat CodingFormatFromFidl(
    const fuchsia::bluetooth::AssignedCodingFormat& fidl_format);

bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter> CodecIdFromFidl(
    const fuchsia::bluetooth::CodecId& fidl_codec_id);

pw::bluetooth::emboss::LogicalTransportType LogicalTransportTypeFromFidl(
    const fuchsia::bluetooth::LogicalTransportType& fidl_transport_type);

pw::bluetooth::emboss::StatusCode FidlHciErrorToStatusCode(
    fuchsia_hardware_bluetooth::HciError code);

fuchsia::bluetooth::le::CisEstablishedParameters CisEstablishedParametersToFidl(
    const bt::iso::CisEstablishedParameters& params_in);

bt::iso::CigPacking FidlToCigPacking(
    const fuchsia::bluetooth::le::CigPacking& packing);

bt::iso::CigFraming FidlToCigFraming(
    const fuchsia::bluetooth::le::CigFramingOptions& framing);

std::optional<bt::iso::CigParams> FidlToCigParams(
    const fuchsia::bluetooth::le::CigParameters& params);

std::optional<bt::iso::CigCisParams> FidlToCigCisParams(
    const fuchsia::bluetooth::le::CisRequestedParameters& params,
    bt::iso::CisEstablishedCallback callback);

bt::DeviceAddress::Type FidlToDeviceAddressType(
    fuchsia::bluetooth::AddressType addr_type);

fuchsia::bluetooth::le::IsoPacketStatusFlag EmbossIsoPacketStatusFlagToFidl(
    pw::bluetooth::emboss::IsoDataPacketStatus status_in);

::fuchsia_bluetooth_le::SyncReport ReportFrom(
    const bt::gap::PeriodicAdvertisingReport& report, zx::time timestamp);

::fuchsia_bluetooth_le::SyncReport ReportFrom(
    const bt::hci_spec::BroadcastIsochronousGroupInfo& report,
    zx::time timestamp);

}  // namespace bthost::fidl_helpers

// fidl::TypeConverter specializations for ByteBuffer and friends.
template <>
struct fidl::TypeConverter<std::vector<uint8_t>, bt::ByteBuffer> {
  static std::vector<uint8_t> Convert(const bt::ByteBuffer& from);
};
