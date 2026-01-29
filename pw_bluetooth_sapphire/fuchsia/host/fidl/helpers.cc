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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"

#include <endian.h>
#include <fidl/fuchsia.bluetooth.bredr/cpp/natural_types.h>
#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <fuchsia/bluetooth/sys/cpp/fidl.h>
#include <fuchsia/media/cpp/fidl.h>
#include <pw_assert/check.h>
#include <pw_bluetooth/hci_data.emb.h>

#include <charconv>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

#include "fuchsia/bluetooth/bredr/cpp/fidl.h"
#include "fuchsia/bluetooth/cpp/fidl.h"
#include "lib/fpromise/result.h"
#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/hci/discovery_filter.h"
#include "pw_bluetooth_sapphire/internal/host/sco/sco.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/data_element.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/sdp.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/service_record.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

using fuchsia::bluetooth::Error;
using fuchsia::bluetooth::ErrorCode;
using fuchsia::bluetooth::Int8;
using fuchsia::bluetooth::Status;

namespace fble = fuchsia::bluetooth::le;
namespace fbredr = fuchsia::bluetooth::bredr;
namespace fbt = fuchsia::bluetooth;
namespace fgatt = fuchsia::bluetooth::gatt;
namespace fgatt2 = fuchsia::bluetooth::gatt2;
namespace fsys = fuchsia::bluetooth::sys;
namespace faudio = fuchsia::hardware::audio;
namespace fhbt = fuchsia_hardware_bluetooth;
namespace android_emb = pw::bluetooth::vendor::android_hci;

const uint8_t BIT_SHIFT_8 = 8;
const uint8_t BIT_SHIFT_16 = 16;

namespace bthost::fidl_helpers {
::fuchsia_bluetooth_le::PhysicalLayer IsoPhyToFidl(
    pw::bluetooth::emboss::IsoPhyType phy) {
  switch (phy) {
    case pw::bluetooth::emboss::IsoPhyType::LE_1M:
      return ::fuchsia_bluetooth_le::PhysicalLayer::kLe1M;
    case pw::bluetooth::emboss::IsoPhyType::LE_2M:
      return ::fuchsia_bluetooth_le::PhysicalLayer::kLe2M;
    case pw::bluetooth::emboss::IsoPhyType::LE_CODED:
      return ::fuchsia_bluetooth_le::PhysicalLayer::kLeCoded;
    default:
      ZX_PANIC("invalid PHY");
  }
}

::fuchsia_bluetooth_le::PhysicalLayer LEPhyToFidl(
    pw::bluetooth::emboss::LEPhy phy) {
  switch (phy) {
    case pw::bluetooth::emboss::LEPhy::LE_1M:
      return ::fuchsia_bluetooth_le::PhysicalLayer::kLe1M;
    case pw::bluetooth::emboss::LEPhy::LE_2M:
      return ::fuchsia_bluetooth_le::PhysicalLayer::kLe2M;
    case pw::bluetooth::emboss::LEPhy::LE_CODED:
      return ::fuchsia_bluetooth_le::PhysicalLayer::kLeCoded;
    default:
      ZX_PANIC("invalid PHY");
  }
}

// TODO(fxbug.dev/42076395): Add remaining codecs
std::optional<android_emb::A2dpCodecType> FidlToCodecType(
    const fbredr::AudioOffloadFeatures& codec) {
  switch (codec.Which()) {
    case fuchsia::bluetooth::bredr::AudioOffloadFeatures::kSbc:
      return android_emb::A2dpCodecType::SBC;
    case fuchsia::bluetooth::bredr::AudioOffloadFeatures::kAac:
      return android_emb::A2dpCodecType::AAC;
    default:
      bt_log(WARN,
             "fidl",
             "Codec type not yet handled: %u",
             static_cast<unsigned int>(codec.Which()));
      return std::nullopt;
  }
}

bt::StaticPacket<android_emb::A2dpScmsTEnableWriter> FidlToScmsTEnable(
    bool scms_t_enable) {
  bt::StaticPacket<android_emb::A2dpScmsTEnableWriter> scms_t_enable_struct;

  if (scms_t_enable) {
    scms_t_enable_struct.view().enabled().Write(
        pw::bluetooth::emboss::GenericEnableParam::ENABLE);
  } else {
    scms_t_enable_struct.view().enabled().Write(
        pw::bluetooth::emboss::GenericEnableParam::DISABLE);
  }

  scms_t_enable_struct.view().header().Write(0x00);
  return scms_t_enable_struct;
}

std::optional<android_emb::A2dpSamplingFrequency> FidlToSamplingFrequency(
    fbredr::AudioSamplingFrequency sampling_frequency) {
  switch (sampling_frequency) {
    case fbredr::AudioSamplingFrequency::HZ_44100:
      return android_emb::A2dpSamplingFrequency::HZ_44100;
    case fbredr::AudioSamplingFrequency::HZ_48000:
      return android_emb::A2dpSamplingFrequency::HZ_48000;
    case fbredr::AudioSamplingFrequency::HZ_88200:
      return android_emb::A2dpSamplingFrequency::HZ_88200;
    case fbredr::AudioSamplingFrequency::HZ_96000:
      return android_emb::A2dpSamplingFrequency::HZ_96000;
    default:
      return std::nullopt;
  }
}

std::optional<android_emb::A2dpBitsPerSample> FidlToBitsPerSample(
    fbredr::AudioBitsPerSample bits_per_sample) {
  switch (bits_per_sample) {
    case fbredr::AudioBitsPerSample::BPS_16:
      return android_emb::A2dpBitsPerSample::BITS_PER_SAMPLE_16;
    case fbredr::AudioBitsPerSample::BPS_24:
      return android_emb::A2dpBitsPerSample::BITS_PER_SAMPLE_24;
    case fbredr::AudioBitsPerSample::BPS_32:
      return android_emb::A2dpBitsPerSample::BITS_PER_SAMPLE_32;
    default:
      return std::nullopt;
  }
}

std::optional<android_emb::A2dpChannelMode> FidlToChannelMode(
    fbredr::AudioChannelMode channel_mode) {
  switch (channel_mode) {
    case fbredr::AudioChannelMode::MONO:
      return android_emb::A2dpChannelMode::MONO;
    case fbredr::AudioChannelMode::STEREO:
      return android_emb::A2dpChannelMode::STEREO;
    default:
      return std::nullopt;
  }
}

bt::StaticPacket<android_emb::SbcCodecInformationWriter>
FidlToEncoderSettingsSbc(const fbredr::AudioEncoderSettings& encoder_settings,
                         fbredr::AudioSamplingFrequency sampling_frequency,
                         fbredr::AudioChannelMode channel_mode) {
  bt::StaticPacket<android_emb::SbcCodecInformationWriter> sbc;

  switch (encoder_settings.sbc().allocation) {
    case fuchsia::media::SbcAllocation::ALLOC_LOUDNESS:
      sbc.view().allocation_method().Write(
          android_emb::SbcAllocationMethod::LOUDNESS);
      break;
    case fuchsia::media::SbcAllocation::ALLOC_SNR:
      sbc.view().allocation_method().Write(
          android_emb::SbcAllocationMethod::SNR);
      break;
  }

  switch (encoder_settings.sbc().sub_bands) {
    case fuchsia::media::SbcSubBands::SUB_BANDS_4:
      sbc.view().subbands().Write(android_emb::SbcSubBands::SUBBANDS_4);
      break;
    case fuchsia::media::SbcSubBands::SUB_BANDS_8:
      sbc.view().subbands().Write(android_emb::SbcSubBands::SUBBANDS_8);
      break;
  }

  switch (encoder_settings.sbc().block_count) {
    case fuchsia::media::SbcBlockCount::BLOCK_COUNT_4:
      sbc.view().block_length().Write(android_emb::SbcBlockLen::BLOCK_LEN_4);
      break;
    case fuchsia::media::SbcBlockCount::BLOCK_COUNT_8:
      sbc.view().block_length().Write(android_emb::SbcBlockLen::BLOCK_LEN_8);
      break;
    case fuchsia::media::SbcBlockCount::BLOCK_COUNT_12:
      sbc.view().block_length().Write(android_emb::SbcBlockLen::BLOCK_LEN_12);
      break;
    case fuchsia::media::SbcBlockCount::BLOCK_COUNT_16:
      sbc.view().block_length().Write(android_emb::SbcBlockLen::BLOCK_LEN_16);
      break;
  }

  sbc.view().min_bitpool_value().Write(encoder_settings.sbc().bit_pool);
  sbc.view().max_bitpool_value().Write(encoder_settings.sbc().bit_pool);

  switch (channel_mode) {
    case fbredr::AudioChannelMode::MONO:
      sbc.view().channel_mode().Write(android_emb::SbcChannelMode::MONO);
      break;
    case fbredr::AudioChannelMode::STEREO:
      sbc.view().channel_mode().Write(android_emb::SbcChannelMode::STEREO);
      break;
  }

  switch (sampling_frequency) {
    case fbredr::AudioSamplingFrequency::HZ_44100:
      sbc.view().sampling_frequency().Write(
          android_emb::SbcSamplingFrequency::HZ_44100);
      break;
    case fbredr::AudioSamplingFrequency::HZ_48000:
      sbc.view().sampling_frequency().Write(
          android_emb::SbcSamplingFrequency::HZ_48000);
      break;
    default:
      bt_log(WARN,
             "fidl",
             "%s: sbc encoder cannot use sampling frequency %hhu",
             __FUNCTION__,
             static_cast<uint8_t>(sampling_frequency));
  }

  return sbc;
}

bt::StaticPacket<android_emb::AacCodecInformationWriter>
FidlToEncoderSettingsAac(const fbredr::AudioEncoderSettings& encoder_settings,
                         fbredr::AudioSamplingFrequency sampling_frequency,
                         fbredr::AudioChannelMode channel_mode) {
  bt::StaticPacket<android_emb::AacCodecInformationWriter> aac;
  aac.view().object_type().Write(
      static_cast<uint8_t>(encoder_settings.aac().aot));

  if (encoder_settings.aac().bit_rate.is_variable()) {
    aac.view().variable_bit_rate().Write(
        android_emb::AacEnableVariableBitRate::ENABLE);
  }

  if (encoder_settings.aac().bit_rate.is_constant()) {
    aac.view().variable_bit_rate().Write(
        android_emb::AacEnableVariableBitRate::DISABLE);
  }

  return aac;
}

std::optional<bt::sdp::DataElement> FidlToDataElement(
    const fbredr::DataElement& fidl) {
  bt::sdp::DataElement out;
  switch (fidl.Which()) {
    case fbredr::DataElement::Tag::kInt8:
      return bt::sdp::DataElement(fidl.int8());
    case fbredr::DataElement::Tag::kInt16:
      return bt::sdp::DataElement(fidl.int16());
    case fbredr::DataElement::Tag::kInt32:
      return bt::sdp::DataElement(fidl.int32());
    case fbredr::DataElement::Tag::kInt64:
      return bt::sdp::DataElement(fidl.int64());
    case fbredr::DataElement::Tag::kUint8:
      return bt::sdp::DataElement(fidl.uint8());
    case fbredr::DataElement::Tag::kUint16:
      return bt::sdp::DataElement(fidl.uint16());
    case fbredr::DataElement::Tag::kUint32:
      return bt::sdp::DataElement(fidl.uint32());
    case fbredr::DataElement::Tag::kUint64:
      return bt::sdp::DataElement(fidl.uint64());
    case fbredr::DataElement::Tag::kStr: {
      const std::vector<uint8_t>& str = fidl.str();
      bt::DynamicByteBuffer bytes((bt::BufferView(str)));
      return bt::sdp::DataElement(bytes);
    }
    case fbredr::DataElement::Tag::kUrl:
      out.SetUrl(fidl.url());
      break;
    case fbredr::DataElement::Tag::kB:
      return bt::sdp::DataElement(fidl.b());
    case fbredr::DataElement::Tag::kUuid:
      out.Set(fidl_helpers::UuidFromFidl(fidl.uuid()));
      break;
    case fbredr::DataElement::Tag::kSequence: {
      std::vector<bt::sdp::DataElement> seq;
      for (const auto& fidl_elem : fidl.sequence()) {
        std::optional<bt::sdp::DataElement> elem =
            FidlToDataElement(*fidl_elem);
        if (!elem) {
          return std::nullopt;
        }
        seq.emplace_back(std::move(elem.value()));
      }
      out.Set(std::move(seq));
      break;
    }
    case fbredr::DataElement::Tag::kAlternatives: {
      std::vector<bt::sdp::DataElement> alts;
      for (const auto& fidl_elem : fidl.alternatives()) {
        auto elem = FidlToDataElement(*fidl_elem);
        if (!elem) {
          return std::nullopt;
        }
        alts.emplace_back(std::move(elem.value()));
      }
      out.SetAlternative(std::move(alts));
      break;
    }
    default:
      // Types not handled: Null datatype (never used)
      bt_log(WARN, "fidl", "Encountered FidlToDataElement type not handled.");
      return std::nullopt;
  }
  return out;
}

std::optional<bt::sdp::DataElement> NewFidlToDataElement(
    const fuchsia_bluetooth_bredr::DataElement& fidl) {
  bt::sdp::DataElement out;
  switch (fidl.Which()) {
    case fuchsia_bluetooth_bredr::DataElement::Tag::kInt8:
      return bt::sdp::DataElement(fidl.int8().value());
    case fuchsia_bluetooth_bredr::DataElement::Tag::kInt16:
      return bt::sdp::DataElement(fidl.int16().value());
    case fuchsia_bluetooth_bredr::DataElement::Tag::kInt32:
      return bt::sdp::DataElement(fidl.int32().value());
    case fuchsia_bluetooth_bredr::DataElement::Tag::kInt64:
      return bt::sdp::DataElement(fidl.int64().value());
    case fuchsia_bluetooth_bredr::DataElement::Tag::kUint8:
      return bt::sdp::DataElement(fidl.uint8().value());
    case fuchsia_bluetooth_bredr::DataElement::Tag::kUint16:
      return bt::sdp::DataElement(fidl.uint16().value());
    case fuchsia_bluetooth_bredr::DataElement::Tag::kUint32:
      return bt::sdp::DataElement(fidl.uint32().value());
    case fuchsia_bluetooth_bredr::DataElement::Tag::kUint64:
      return bt::sdp::DataElement(fidl.uint64().value());
    case fuchsia_bluetooth_bredr::DataElement::Tag::kStr: {
      bt::DynamicByteBuffer bytes((bt::BufferView(fidl.str().value())));
      return bt::sdp::DataElement(bytes);
    }
    case fuchsia_bluetooth_bredr::DataElement::Tag::kUrl: {
      out.SetUrl(fidl.url().value());
      break;
    }
    case fuchsia_bluetooth_bredr::DataElement::Tag::kB:
      return bt::sdp::DataElement(fidl.b().value());
    case fuchsia_bluetooth_bredr::DataElement::Tag::kUuid:
      out.Set(NewUuidFromFidl(fidl.uuid()->value()));
      break;
    case fuchsia_bluetooth_bredr::DataElement::Tag::kSequence: {
      std::vector<bt::sdp::DataElement> seq;
      for (const auto& fidl_elem : fidl.sequence().value()) {
        std::optional<bt::sdp::DataElement> elem =
            NewFidlToDataElement(*fidl_elem);
        if (!elem) {
          return std::nullopt;
        }
        seq.emplace_back(std::move(elem.value()));
      }
      out.Set(std::move(seq));
      break;
    }
    case fuchsia_bluetooth_bredr::DataElement::Tag::kAlternatives: {
      std::vector<bt::sdp::DataElement> alts;
      for (const auto& fidl_elem : fidl.alternatives().value()) {
        auto elem = NewFidlToDataElement(*fidl_elem);
        if (!elem) {
          return std::nullopt;
        }
        alts.emplace_back(std::move(elem.value()));
      }
      out.SetAlternative(std::move(alts));
      break;
    }
    default:
      // Types not handled: Null datatype (never used)
      bt_log(
          WARN, "fidl", "Encountered NewFidlToDataElement type not handled.");
      return std::nullopt;
  }
  return out;
}

std::optional<fbredr::DataElement> DataElementToFidl(
    const bt::sdp::DataElement& data_element) {
  fbredr::DataElement out;
  switch (data_element.type()) {
    case bt::sdp::DataElement::Type::kNull:
      return std::nullopt;
    case bt::sdp::DataElement::Type::kUnsignedInt:
      switch (data_element.size()) {
        case bt::sdp::DataElement::Size::kOneByte:
          out.set_uint8(*data_element.Get<uint8_t>());
          break;
        case bt::sdp::DataElement::Size::kTwoBytes:
          out.set_uint16(*data_element.Get<uint16_t>());
          break;
        case bt::sdp::DataElement::Size::kFourBytes:
          out.set_uint32(*data_element.Get<uint32_t>());
          break;
        case bt::sdp::DataElement::Size::kEightBytes:
          out.set_uint64(*data_element.Get<uint64_t>());
          break;
        case bt::sdp::DataElement::Size::kSixteenBytes:
        case bt::sdp::DataElement::Size::kNextOne:
        case bt::sdp::DataElement::Size::kNextTwo:
        case bt::sdp::DataElement::Size::kNextFour:
          bt_log(
              WARN, "fidl", "Encountered DataElementToFidl type not handled.");
          return std::nullopt;
      }
      break;
    case bt::sdp::DataElement::Type::kSignedInt:
      switch (data_element.size()) {
        case bt::sdp::DataElement::Size::kOneByte:
          out.set_int8(*data_element.Get<int8_t>());
          break;
        case bt::sdp::DataElement::Size::kTwoBytes:
          out.set_int16(*data_element.Get<int16_t>());
          break;
        case bt::sdp::DataElement::Size::kFourBytes:
          out.set_int32(*data_element.Get<int32_t>());
          break;
        case bt::sdp::DataElement::Size::kEightBytes:
          out.set_int64(*data_element.Get<int64_t>());
          break;
        case bt::sdp::DataElement::Size::kSixteenBytes:
        case bt::sdp::DataElement::Size::kNextOne:
        case bt::sdp::DataElement::Size::kNextTwo:
        case bt::sdp::DataElement::Size::kNextFour:
          bt_log(
              WARN, "fidl", "Encountered DataElementToFidl type not handled.");
          return std::nullopt;
      }
      break;
    case bt::sdp::DataElement::Type::kUuid:
      out.set_uuid(UuidToFidl(*data_element.Get<bt::UUID>()));
      break;
    case bt::sdp::DataElement::Type::kString:
      out.set_str(data_element.Get<bt::DynamicByteBuffer>()->ToVector());
      break;
    case bt::sdp::DataElement::Type::kBoolean:
      out.set_b(*data_element.Get<bool>());
      break;
    case bt::sdp::DataElement::Type::kSequence: {
      std::vector<std::unique_ptr<fbredr::DataElement>> seq;
      auto data_element_sequence =
          data_element.Get<std::vector<bt::sdp::DataElement>>();
      for (const auto& elem : data_element_sequence.value()) {
        std::optional<fbredr::DataElement> fidl_elem = DataElementToFidl(elem);
        if (!fidl_elem) {
          bt_log(WARN,
                 "fidl",
                 "Encountered DataElementToFidl sequence type not handled.");
          return std::nullopt;
        }
        seq.emplace_back(std::make_unique<fbredr::DataElement>(
            std::move(fidl_elem.value())));
      }
      out.set_sequence(std::move(seq));
      break;
    }
    case bt::sdp::DataElement::Type::kAlternative: {
      std::vector<std::unique_ptr<fbredr::DataElement>> alt;
      auto data_element_alt =
          data_element.Get<std::vector<bt::sdp::DataElement>>();
      for (const auto& elem : data_element_alt.value()) {
        std::optional<fbredr::DataElement> fidl_elem = DataElementToFidl(elem);
        if (!fidl_elem) {
          bt_log(WARN,
                 "fidl",
                 "Encountered DataElementToFidl alternate type not handled.");
          return std::nullopt;
        }
        alt.emplace_back(std::make_unique<fbredr::DataElement>(
            std::move(fidl_elem.value())));
      }
      out.set_alternatives(std::move(alt));
      break;
    }
    case bt::sdp::DataElement::Type::kUrl:
      out.set_url(*data_element.GetUrl());
      break;
  }
  return out;
}

namespace {

fbt::AddressType AddressTypeToFidl(bt::DeviceAddress::Type type) {
  switch (type) {
    case bt::DeviceAddress::Type::kBREDR:
      [[fallthrough]];
    case bt::DeviceAddress::Type::kLEPublic:
      return fbt::AddressType::PUBLIC;
    case bt::DeviceAddress::Type::kLERandom:
      [[fallthrough]];
    case bt::DeviceAddress::Type::kLEAnonymous:
      return fbt::AddressType::RANDOM;
  }
  return fbt::AddressType::PUBLIC;
}

fbt::Address AddressToFidl(fbt::AddressType type,
                           const bt::DeviceAddressBytes& value) {
  fbt::Address output;
  output.type = type;
  bt::MutableBufferView value_dst(output.bytes.data(), output.bytes.size());
  value_dst.Write(value.bytes());
  return output;
}

fbt::Address AddressToFidl(const bt::DeviceAddress& input) {
  return AddressToFidl(AddressTypeToFidl(input.type()), input.value());
}

bt::sm::SecurityProperties SecurityPropsFromFidl(
    const fsys::SecurityProperties& sec_prop) {
  auto level = bt::sm::SecurityLevel::kEncrypted;
  if (sec_prop.authenticated) {
    level = bt::sm::SecurityLevel::kAuthenticated;
  }
  return bt::sm::SecurityProperties(
      level, sec_prop.encryption_key_size, sec_prop.secure_connections);
}

fsys::SecurityProperties SecurityPropsToFidl(
    const bt::sm::SecurityProperties& sec_prop) {
  return fsys::SecurityProperties{
      .authenticated = sec_prop.authenticated(),
      .secure_connections = sec_prop.secure_connections(),

      // TODO(armansito): Declare the key size as uint8_t in bt::sm?
      .encryption_key_size = static_cast<uint8_t>(sec_prop.enc_key_size()),
  };
}

bt::sm::LTK LtkFromFidl(const fsys::Ltk& ltk) {
  return bt::sm::LTK(
      SecurityPropsFromFidl(ltk.key.security),
      bt::hci_spec::LinkKey(ltk.key.data.value, ltk.rand, ltk.ediv));
}

fsys::PeerKey LtkToFidlPeerKey(const bt::sm::LTK& ltk) {
  return fsys::PeerKey{
      .security = SecurityPropsToFidl(ltk.security()),
      .data = fsys::Key{ltk.key().value()},
  };
}

fsys::Ltk LtkToFidl(const bt::sm::LTK& ltk) {
  return fsys::Ltk{
      .key = LtkToFidlPeerKey(ltk),
      .ediv = ltk.key().ediv(),
      .rand = ltk.key().rand(),
  };
}

bt::sm::Key PeerKeyFromFidl(const fsys::PeerKey& key) {
  return bt::sm::Key(SecurityPropsFromFidl(key.security), key.data.value);
}

fsys::PeerKey PeerKeyToFidl(const bt::sm::Key& key) {
  return fsys::PeerKey{
      .security = SecurityPropsToFidl(key.security()),
      .data = {key.value()},
  };
}

fbt::DeviceClass DeviceClassToFidl(bt::DeviceClass input) {
  auto bytes = input.bytes();
  fbt::DeviceClass output{static_cast<uint32_t>(
      bytes[0] | (bytes[1] << BIT_SHIFT_8) | (bytes[2] << BIT_SHIFT_16))};
  return output;
}

std::optional<fbredr::ServiceClassProfileIdentifier>
UuidToServiceClassIdentifier(const bt::UUID uuid) {
  std::optional<uint16_t> uuid_16 = uuid.As16Bit();
  if (!uuid_16) {
    return std::nullopt;
  }
  return fbredr::ServiceClassProfileIdentifier(*uuid_16);
}

std::optional<fbredr::ProtocolIdentifier> UuidToProtocolIdentifier(
    const bt::UUID uuid) {
  std::optional<uint16_t> uuid_16 = uuid.As16Bit();
  if (!uuid_16) {
    return std::nullopt;
  }
  return fbredr::ProtocolIdentifier(*uuid_16);
}

fbredr::Information InformationToFidl(
    const bt::sdp::ServiceRecord::Information& info) {
  fbredr::Information out;
  out.set_language(info.language_code);
  if (info.name) {
    out.set_name(info.name.value());
  }
  if (info.description) {
    out.set_description(info.description.value());
  }
  if (info.provider) {
    out.set_provider(info.provider.value());
  }
  return out;
}

fpromise::result<std::vector<fuchsia::bluetooth::Uuid>,
                 fuchsia::bluetooth::ErrorCode>
DataElementToServiceUuids(const bt::sdp::DataElement& uuids_element) {
  std::vector<fuchsia::bluetooth::Uuid> out;

  const auto service_uuids_list =
      uuids_element.Get<std::vector<bt::sdp::DataElement>>();
  if (!service_uuids_list) {
    return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
  }

  for (const auto& uuid_element : service_uuids_list.value()) {
    if (uuid_element.type() != bt::sdp::DataElement::Type::kUuid) {
      return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
    }
    out.push_back(UuidToFidl(*uuid_element.Get<bt::UUID>()));
  }

  return fpromise::ok(std::move(out));
}

fpromise::result<std::vector<fbredr::ProtocolDescriptor>,
                 fuchsia::bluetooth::ErrorCode>
DataElementToProtocolDescriptorList(
    const bt::sdp::DataElement& protocols_element) {
  std::vector<fbredr::ProtocolDescriptor> out;

  const auto protocol_list =
      protocols_element.Get<std::vector<bt::sdp::DataElement>>();
  if (!protocol_list) {
    return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
  }

  for (const auto& protocol_elt : protocol_list.value()) {
    if (protocol_elt.type() != bt::sdp::DataElement::Type::kSequence) {
      return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
    }

    const auto protocol =
        protocol_elt.Get<std::vector<bt::sdp::DataElement>>().value();
    if (protocol.size() < 1 ||
        protocol.at(0).type() != bt::sdp::DataElement::Type::kUuid) {
      return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
    }

    fbredr::ProtocolDescriptor desc;
    std::vector<fbredr::DataElement> params;
    for (size_t i = 0; i < protocol.size(); i++) {
      if (i == 0) {
        std::optional<fbredr::ProtocolIdentifier> protocol_id =
            UuidToProtocolIdentifier((protocol.at(i).Get<bt::UUID>()).value());
        if (!protocol_id) {
          return fpromise::error(
              fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
        }
        desc.set_protocol(protocol_id.value());
      } else {
        std::optional<fbredr::DataElement> param =
            DataElementToFidl(protocol.at(i));
        if (!param) {
          return fpromise::error(
              fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
        }
        params.emplace_back(std::move(*param));
      }
    }
    desc.set_params(std::move(params));
    out.emplace_back(std::move(desc));
  }

  return fpromise::ok(std::move(out));
}

// Returns the major and minor versions from a combined |version|.
std::pair<uint8_t, uint8_t> VersionToMajorMinor(uint16_t version) {
  const uint16_t kMajorBitmask = 0xFF00;
  const uint16_t kMinorBitmask = 0x00FF;
  uint8_t major = static_cast<uint8_t>((version & kMajorBitmask) >>
                                       std::numeric_limits<uint8_t>::digits);
  uint8_t minor = static_cast<uint8_t>(version & kMinorBitmask);
  return std::make_pair(major, minor);
}

fpromise::result<std::vector<fbredr::ProfileDescriptor>,
                 fuchsia::bluetooth::ErrorCode>
DataElementToProfileDescriptors(const bt::sdp::DataElement& profile_element) {
  std::vector<fbredr::ProfileDescriptor> out;

  const auto profile_desc_list =
      profile_element.Get<std::vector<bt::sdp::DataElement>>();
  if (!profile_desc_list) {
    return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
  }

  // [[UUID, Version]]
  for (const auto& profile_desc_element : (*profile_desc_list)) {
    if (profile_desc_element.type() != bt::sdp::DataElement::Type::kSequence) {
      return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
    }

    // Each profile descriptor entry contains a UUID and uint16_t version.
    const auto profile_desc =
        *profile_desc_element.Get<std::vector<bt::sdp::DataElement>>();
    if (profile_desc.size() != 2) {
      return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
    }

    std::optional<bt::UUID> profile_id = profile_desc.at(0).Get<bt::UUID>();
    std::optional<uint16_t> version = profile_desc.at(1).Get<uint16_t>();
    if (!profile_id || !version) {
      return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
    }

    fbredr::ProfileDescriptor desc;
    std::optional<fbredr::ServiceClassProfileIdentifier> service_class_id =
        UuidToServiceClassIdentifier(*profile_id);
    if (!service_class_id) {
      return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
    }
    desc.set_profile_id(service_class_id.value());
    auto [major, minor] = VersionToMajorMinor(version.value());
    desc.set_major_version(major);
    desc.set_minor_version(minor);
    out.push_back(std::move(desc));
  }

  return fpromise::ok(std::move(out));
}

bool NewAddProtocolDescriptorList(
    bt::sdp::ServiceRecord* rec,
    bt::sdp::ServiceRecord::ProtocolListId id,
    const std::vector<fuchsia_bluetooth_bredr::ProtocolDescriptor>&
        descriptor_list) {
  bt_log(TRACE, "fidl", "ProtocolDescriptorList %d", id);
  for (auto& descriptor : descriptor_list) {
    if (!descriptor.params().has_value() ||
        !descriptor.protocol().has_value()) {
      return false;
    }
    bt::sdp::DataElement protocol_params;
    if (descriptor.params()->size() > 1) {
      std::vector<bt::sdp::DataElement> params;
      for (auto& fidl_param : *descriptor.params()) {
        auto bt_param = NewFidlToDataElement(fidl_param);
        if (bt_param) {
          params.emplace_back(std::move(bt_param.value()));
        } else {
          return false;
        }
      }
      protocol_params.Set(std::move(params));
    } else if (descriptor.params()->size() == 1) {
      auto param = NewFidlToDataElement(descriptor.params()->front());
      if (param) {
        protocol_params = std::move(param).value();
      } else {
        return false;
      }
      protocol_params =
          NewFidlToDataElement(descriptor.params()->front()).value();
    }

    bt_log(TRACE,
           "fidl",
           "Adding protocol descriptor: {%d : %s}",
           fidl::ToUnderlying(*descriptor.protocol()),
           protocol_params.ToString().c_str());
    rec->AddProtocolDescriptor(
        id,
        bt::UUID(static_cast<uint16_t>(*descriptor.protocol())),
        std::move(protocol_params));
  }
  return true;
}

bool AddProtocolDescriptorList(
    bt::sdp::ServiceRecord* rec,
    bt::sdp::ServiceRecord::ProtocolListId id,
    const ::std::vector<fbredr::ProtocolDescriptor>& descriptor_list) {
  bt_log(TRACE, "fidl", "ProtocolDescriptorList %d", id);
  for (auto& descriptor : descriptor_list) {
    bt::sdp::DataElement protocol_params;
    if (!descriptor.has_params() || !descriptor.has_protocol()) {
      bt_log(WARN, "fidl", "ProtocolDescriptor missing params/protocol field");
      return false;
    }
    if (descriptor.params().size() > 1) {
      std::vector<bt::sdp::DataElement> params;
      for (auto& fidl_param : descriptor.params()) {
        auto bt_param = FidlToDataElement(fidl_param);
        if (bt_param) {
          params.emplace_back(std::move(bt_param.value()));
        } else {
          return false;
        }
      }
      protocol_params.Set(std::move(params));
    } else if (descriptor.params().size() == 1) {
      auto param = FidlToDataElement(descriptor.params().front());
      if (param) {
        protocol_params = std::move(param).value();
      } else {
        return false;
      }
      protocol_params = FidlToDataElement(descriptor.params().front()).value();
    }

    bt_log(TRACE,
           "fidl",
           "Adding protocol descriptor: {%d : %s}",
           uint16_t(descriptor.protocol()),
           bt_str(protocol_params));
    rec->AddProtocolDescriptor(
        id,
        bt::UUID(static_cast<uint16_t>(descriptor.protocol())),
        std::move(protocol_params));
  }
  return true;
}

// Returns true if the appearance value (in host byte order) is included in
// fuchsia.bluetooth.Appearance, which is a subset of Bluetooth Assigned
// Numbers, "Appearance Values"
// (https://www.bluetooth.com/specifications/assigned-numbers/).
//
// TODO(fxbug.dev/42145156): Remove this compatibility check with the strict
// Appearance enum.
[[nodiscard]] bool IsAppearanceValid(uint16_t appearance_raw) {
  switch (appearance_raw) {
    case 0u:  // UNKNOWN
      [[fallthrough]];
    case 64u:  // PHONE
      [[fallthrough]];
    case 128u:  // COMPUTER
      [[fallthrough]];
    case 192u:  // WATCH
      [[fallthrough]];
    case 193u:  // WATCH_SPORTS
      [[fallthrough]];
    case 256u:  // CLOCK
      [[fallthrough]];
    case 320u:  // DISPLAY
      [[fallthrough]];
    case 384u:  // REMOTE_CONTROL
      [[fallthrough]];
    case 448u:  // EYE_GLASSES
      [[fallthrough]];
    case 512u:  // TAG
      [[fallthrough]];
    case 576u:  // KEYRING
      [[fallthrough]];
    case 640u:  // MEDIA_PLAYER
      [[fallthrough]];
    case 704u:  // BARCODE_SCANNER
      [[fallthrough]];
    case 768u:  // THERMOMETER
      [[fallthrough]];
    case 769u:  // THERMOMETER_EAR
      [[fallthrough]];
    case 832u:  // HEART_RATE_SENSOR
      [[fallthrough]];
    case 833u:  // HEART_RATE_SENSOR_BELT
      [[fallthrough]];
    case 896u:  // BLOOD_PRESSURE
      [[fallthrough]];
    case 897u:  // BLOOD_PRESSURE_ARM
      [[fallthrough]];
    case 898u:  // BLOOD_PRESSURE_WRIST
      [[fallthrough]];
    case 960u:  // HID
      [[fallthrough]];
    case 961u:  // HID_KEYBOARD
      [[fallthrough]];
    case 962u:  // HID_MOUSE
      [[fallthrough]];
    case 963u:  // HID_JOYSTICK
      [[fallthrough]];
    case 964u:  // HID_GAMEPAD
      [[fallthrough]];
    case 965u:  // HID_DIGITIZER_TABLET
      [[fallthrough]];
    case 966u:  // HID_CARD_READER
      [[fallthrough]];
    case 967u:  // HID_DIGITAL_PEN
      [[fallthrough]];
    case 968u:  // HID_BARCODE_SCANNER
      [[fallthrough]];
    case 1024u:  // GLUCOSE_METER
      [[fallthrough]];
    case 1088u:  // RUNNING_WALKING_SENSOR
      [[fallthrough]];
    case 1089u:  // RUNNING_WALKING_SENSOR_IN_SHOE
      [[fallthrough]];
    case 1090u:  // RUNNING_WALKING_SENSOR_ON_SHOE
      [[fallthrough]];
    case 1091u:  // RUNNING_WALKING_SENSOR_ON_HIP
      [[fallthrough]];
    case 1152u:  // CYCLING
      [[fallthrough]];
    case 1153u:  // CYCLING_COMPUTER
      [[fallthrough]];
    case 1154u:  // CYCLING_SPEED_SENSOR
      [[fallthrough]];
    case 1155u:  // CYCLING_CADENCE_SENSOR
      [[fallthrough]];
    case 1156u:  // CYCLING_POWER_SENSOR
      [[fallthrough]];
    case 1157u:  // CYCLING_SPEED_AND_CADENCE_SENSOR
      [[fallthrough]];
    case 3136u:  // PULSE_OXIMETER
      [[fallthrough]];
    case 3137u:  // PULSE_OXIMETER_FINGERTIP
      [[fallthrough]];
    case 3138u:  // PULSE_OXIMETER_WRIST
      [[fallthrough]];
    case 3200u:  // WEIGHT_SCALE
      [[fallthrough]];
    case 3264u:  // PERSONAL_MOBILITY
      [[fallthrough]];
    case 3265u:  // PERSONAL_MOBILITY_WHEELCHAIR
      [[fallthrough]];
    case 3266u:  // PERSONAL_MOBILITY_SCOOTER
      [[fallthrough]];
    case 3328u:  // GLUCOSE_MONITOR
      [[fallthrough]];
    case 5184u:  // SPORTS_ACTIVITY
      [[fallthrough]];
    case 5185u:  // SPORTS_ACTIVITY_LOCATION_DISPLAY
      [[fallthrough]];
    case 5186u:  // SPORTS_ACTIVITY_LOCATION_AND_NAV_DISPLAY
      [[fallthrough]];
    case 5187u:  // SPORTS_ACTIVITY_LOCATION_POD
      [[fallthrough]];
    case 5188u:  // SPORTS_ACTIVITY_LOCATION_AND_NAV_POD
      return true;
    default:
      return false;
  }
}

[[nodiscard]] std::optional<fbt::Appearance> AppearanceToFidl(
    uint16_t appearance_raw) {
  if (IsAppearanceValid(appearance_raw)) {
    return static_cast<fbt::Appearance>(appearance_raw);
  }
  return std::nullopt;
}

}  // namespace

std::optional<::fuchsia_bluetooth::Appearance> AppearanceToNewFidl(
    uint16_t appearance_raw) {
  if (IsAppearanceValid(appearance_raw)) {
    return static_cast<::fuchsia_bluetooth::Appearance>(appearance_raw);
  }
  return std::nullopt;
}

std::optional<bt::PeerId> PeerIdFromString(const std::string& id) {
  if (id.empty()) {
    return std::nullopt;
  }

  uint64_t value = 0;
  auto [_, error] =
      std::from_chars(id.data(), id.data() + id.size(), value, /*base=*/16);
  if (error != std::errc()) {
    return std::nullopt;
  }
  return bt::PeerId(value);
}

ErrorCode HostErrorToFidlDeprecated(bt::HostError host_error) {
  switch (host_error) {
    case bt::HostError::kFailed:
      return ErrorCode::FAILED;
    case bt::HostError::kTimedOut:
      return ErrorCode::TIMED_OUT;
    case bt::HostError::kInvalidParameters:
      return ErrorCode::INVALID_ARGUMENTS;
    case bt::HostError::kCanceled:
      return ErrorCode::CANCELED;
    case bt::HostError::kInProgress:
      return ErrorCode::IN_PROGRESS;
    case bt::HostError::kNotSupported:
      return ErrorCode::NOT_SUPPORTED;
    case bt::HostError::kNotFound:
      return ErrorCode::NOT_FOUND;
    default:
      break;
  }

  return ErrorCode::FAILED;
}

Status NewFidlError(ErrorCode error_code, const std::string& description) {
  Status status;
  status.error = std::make_unique<Error>();
  status.error->error_code = error_code;
  status.error->description = description;
  return status;
}

fsys::Error HostErrorToFidl(bt::HostError error) {
  switch (error) {
    case bt::HostError::kFailed:
      return fsys::Error::FAILED;
    case bt::HostError::kTimedOut:
      return fsys::Error::TIMED_OUT;
    case bt::HostError::kInvalidParameters:
      return fsys::Error::INVALID_ARGUMENTS;
    case bt::HostError::kCanceled:
      return fsys::Error::CANCELED;
    case bt::HostError::kInProgress:
      return fsys::Error::IN_PROGRESS;
    case bt::HostError::kNotSupported:
      return fsys::Error::NOT_SUPPORTED;
    case bt::HostError::kNotFound:
      return fsys::Error::PEER_NOT_FOUND;
    default:
      break;
  }
  return fsys::Error::FAILED;
}

fuchsia::bluetooth::gatt::Error GattErrorToFidl(const bt::att::Error& error) {
  return error.Visit(
      [](bt::HostError host_error) {
        return host_error == bt::HostError::kPacketMalformed
                   ? fuchsia::bluetooth::gatt::Error::INVALID_RESPONSE
                   : fuchsia::bluetooth::gatt::Error::FAILURE;
      },
      [](bt::att::ErrorCode att_error) {
        switch (att_error) {
          case bt::att::ErrorCode::kInsufficientAuthorization:
            return fuchsia::bluetooth::gatt::Error::INSUFFICIENT_AUTHORIZATION;
          case bt::att::ErrorCode::kInsufficientAuthentication:
            return fuchsia::bluetooth::gatt::Error::INSUFFICIENT_AUTHENTICATION;
          case bt::att::ErrorCode::kInsufficientEncryptionKeySize:
            return fuchsia::bluetooth::gatt::Error::
                INSUFFICIENT_ENCRYPTION_KEY_SIZE;
          case bt::att::ErrorCode::kInsufficientEncryption:
            return fuchsia::bluetooth::gatt::Error::INSUFFICIENT_ENCRYPTION;
          case bt::att::ErrorCode::kReadNotPermitted:
            return fuchsia::bluetooth::gatt::Error::READ_NOT_PERMITTED;
          default:
            break;
        }
        return fuchsia::bluetooth::gatt::Error::FAILURE;
      });
}

fuchsia::bluetooth::gatt2::Error AttErrorToGattFidlError(
    const bt::att::Error& error) {
  return error.Visit(
      [](bt::HostError host_error) {
        switch (host_error) {
          case bt::HostError::kPacketMalformed:
            return fuchsia::bluetooth::gatt2::Error::INVALID_PDU;
          case bt::HostError::kInvalidParameters:
            return fuchsia::bluetooth::gatt2::Error::INVALID_PARAMETERS;
          default:
            break;
        }
        return fuchsia::bluetooth::gatt2::Error::UNLIKELY_ERROR;
      },
      [](bt::att::ErrorCode att_error) {
        switch (att_error) {
          case bt::att::ErrorCode::kInsufficientAuthorization:
            return fuchsia::bluetooth::gatt2::Error::INSUFFICIENT_AUTHORIZATION;
          case bt::att::ErrorCode::kInsufficientAuthentication:
            return fuchsia::bluetooth::gatt2::Error::
                INSUFFICIENT_AUTHENTICATION;
          case bt::att::ErrorCode::kInsufficientEncryptionKeySize:
            return fuchsia::bluetooth::gatt2::Error::
                INSUFFICIENT_ENCRYPTION_KEY_SIZE;
          case bt::att::ErrorCode::kInsufficientEncryption:
            return fuchsia::bluetooth::gatt2::Error::INSUFFICIENT_ENCRYPTION;
          case bt::att::ErrorCode::kReadNotPermitted:
            return fuchsia::bluetooth::gatt2::Error::READ_NOT_PERMITTED;
          case bt::att::ErrorCode::kInvalidHandle:
            return fuchsia::bluetooth::gatt2::Error::INVALID_HANDLE;
          default:
            break;
        }
        return fuchsia::bluetooth::gatt2::Error::UNLIKELY_ERROR;
      });
}

bt::UUID UuidFromFidl(const fuchsia::bluetooth::Uuid& input) {
  // Conversion must always succeed given the defined size of |input|.
  static_assert(sizeof(input.value) == 16, "FIDL UUID definition malformed!");
  return bt::UUID(bt::BufferView(input.value.data(), input.value.size()));
}

fuchsia::bluetooth::Uuid UuidToFidl(const bt::UUID& uuid) {
  fuchsia::bluetooth::Uuid output;
  // Conversion must always succeed given the defined size of |input|.
  static_assert(sizeof(output.value) == 16, "FIDL UUID definition malformed!");
  output.value = uuid.value();
  return output;
}

bt::UUID NewUuidFromFidl(const fuchsia_bluetooth::Uuid& input) {
  // Conversion must always succeed given the defined size of |input|.
  static_assert(sizeof(input.value()) == 16, "FIDL UUID definition malformed!");
  return bt::UUID(bt::BufferView(input.value().data(), input.value().size()));
}

bt::sm::IOCapability IoCapabilityFromFidl(fsys::InputCapability input,
                                          fsys::OutputCapability output) {
  if (input == fsys::InputCapability::NONE &&
      output == fsys::OutputCapability::NONE) {
    return bt::sm::IOCapability::kNoInputNoOutput;
  }

  if (input == fsys::InputCapability::KEYBOARD &&
      output == fsys::OutputCapability::DISPLAY) {
    return bt::sm::IOCapability::kKeyboardDisplay;
  }

  if (input == fsys::InputCapability::KEYBOARD &&
      output == fsys::OutputCapability::NONE) {
    return bt::sm::IOCapability::kKeyboardOnly;
  }

  if (input == fsys::InputCapability::NONE &&
      output == fsys::OutputCapability::DISPLAY) {
    return bt::sm::IOCapability::kDisplayOnly;
  }

  if (input == fsys::InputCapability::CONFIRMATION &&
      output == fsys::OutputCapability::DISPLAY) {
    return bt::sm::IOCapability::kDisplayYesNo;
  }

  return bt::sm::IOCapability::kNoInputNoOutput;
}

std::optional<bt::gap::BrEdrSecurityMode> BrEdrSecurityModeFromFidl(
    const fsys::BrEdrSecurityMode mode) {
  switch (mode) {
    case fsys::BrEdrSecurityMode::MODE_4:
      return bt::gap::BrEdrSecurityMode::Mode4;
    case fsys::BrEdrSecurityMode::SECURE_CONNECTIONS_ONLY:
      return bt::gap::BrEdrSecurityMode::SecureConnectionsOnly;
    default:
      bt_log(WARN, "fidl", "BR/EDR security mode not recognized");
      return std::nullopt;
  }
}

bt::gap::LESecurityMode LeSecurityModeFromFidl(
    const fsys::LeSecurityMode mode) {
  switch (mode) {
    case fsys::LeSecurityMode::MODE_1:
      return bt::gap::LESecurityMode::Mode1;
    case fsys::LeSecurityMode::SECURE_CONNECTIONS_ONLY:
      return bt::gap::LESecurityMode::SecureConnectionsOnly;
  }
  bt_log(
      WARN,
      "fidl",
      "FIDL security mode not recognized, defaulting to SecureConnectionsOnly");
  return bt::gap::LESecurityMode::SecureConnectionsOnly;
}

std::optional<bt::sm::SecurityLevel> SecurityLevelFromFidl(
    const fsys::PairingSecurityLevel level) {
  switch (level) {
    case fsys::PairingSecurityLevel::ENCRYPTED:
      return bt::sm::SecurityLevel::kEncrypted;
    case fsys::PairingSecurityLevel::AUTHENTICATED:
      return bt::sm::SecurityLevel::kAuthenticated;
    default:
      return std::nullopt;
  };
}

fsys::TechnologyType TechnologyTypeToFidl(bt::gap::TechnologyType type) {
  switch (type) {
    case bt::gap::TechnologyType::kLowEnergy:
      return fsys::TechnologyType::LOW_ENERGY;
    case bt::gap::TechnologyType::kClassic:
      return fsys::TechnologyType::CLASSIC;
    case bt::gap::TechnologyType::kDualMode:
      return fsys::TechnologyType::DUAL_MODE;
    default:
      PW_CRASH("invalid technology type: %u", static_cast<unsigned int>(type));
      break;
  }

  // This should never execute.
  return fsys::TechnologyType::DUAL_MODE;
}

fsys::HostInfo HostInfoToFidl(const bt::gap::Adapter& adapter) {
  fsys::HostInfo info;
  info.set_id(fbt::HostId{adapter.identifier().value()});
  info.set_technology(TechnologyTypeToFidl(adapter.state().type()));
  info.set_local_name(adapter.local_name());
  info.set_discoverable(adapter.IsDiscoverable());
  info.set_discovering(adapter.IsDiscovering());
  std::vector<fbt::Address> addresses;
  addresses.emplace_back(AddressToFidl(fbt::AddressType::PUBLIC,
                                       adapter.state().controller_address));
  if (adapter.le() && adapter.le()->PrivacyEnabled() &&
      (!adapter.le()->CurrentAddress().IsPublic())) {
    addresses.emplace_back(AddressToFidl(adapter.le()->CurrentAddress()));
  }
  info.set_addresses(std::move(addresses));
  return info;
}

fsys::Peer PeerToFidl(const bt::gap::Peer& peer) {
  fsys::Peer output;
  output.set_id(fbt::PeerId{peer.identifier().value()});
  output.set_address(AddressToFidl(peer.address()));
  output.set_technology(TechnologyTypeToFidl(peer.technology()));
  output.set_connected(peer.connected());
  output.set_bonded(peer.bonded());

  if (peer.name()) {
    output.set_name(*peer.name());
  }

  if (peer.le()) {
    const std::optional<std::reference_wrapper<const bt::AdvertisingData>>
        adv_data = peer.le()->parsed_advertising_data();
    if (adv_data.has_value()) {
      if (adv_data->get().appearance().has_value()) {
        if (auto appearance =
                AppearanceToFidl(adv_data->get().appearance().value())) {
          output.set_appearance(appearance.value());
        } else {
          bt_log(DEBUG,
                 "fidl",
                 "omitting unencodeable appearance %#.4x of peer %s",
                 adv_data->get().appearance().value(),
                 bt_str(peer.identifier()));
        }
      }
      if (adv_data->get().tx_power()) {
        output.set_tx_power(adv_data->get().tx_power().value());
      }
    }
  }
  if (peer.bredr() && peer.bredr()->device_class()) {
    output.set_device_class(DeviceClassToFidl(*peer.bredr()->device_class()));
  }
  if (peer.rssi() != bt::hci_spec::kRSSIInvalid) {
    output.set_rssi(peer.rssi());
  }

  if (peer.bredr()) {
    std::transform(peer.bredr()->services().begin(),
                   peer.bredr()->services().end(),
                   std::back_inserter(*output.mutable_bredr_services()),
                   UuidToFidl);
  }

  // TODO(fxbug.dev/42135180): Populate le_service UUIDs based on GATT results
  // as well as advertising and inquiry data.

  return output;
}

std::optional<bt::DeviceAddress> AddressFromFidlBondingData(
    const fuchsia::bluetooth::sys::BondingData& bond) {
  bt::DeviceAddressBytes bytes(bond.address().bytes);
  bt::DeviceAddress::Type type;
  if (bond.has_bredr_bond()) {
    // A random identity address can only be present in a LE-only bond.
    if (bond.address().type == fbt::AddressType::RANDOM) {
      bt_log(WARN,
             "fidl",
             "BR/EDR or Dual-Mode bond cannot have a random identity address!");
      return std::nullopt;
    }
    // TODO(fxbug.dev/42102158): We currently assign kBREDR as the address type
    // for dual-mode bonds. This makes address management for dual-mode devices
    // a bit confusing as we have two "public" address types (i.e. kBREDR and
    // kLEPublic). We should align the stack address types with the FIDL address
    // types, such that both kBREDR and kLEPublic are represented as the same
    // kind of "PUBLIC".
    type = bt::DeviceAddress::Type::kBREDR;
  } else {
    type = bond.address().type == fbt::AddressType::RANDOM
               ? bt::DeviceAddress::Type::kLERandom
               : bt::DeviceAddress::Type::kLEPublic;
  }

  bt::DeviceAddress address(type, bytes);

  if (!address.IsPublic() && !address.IsStaticRandom()) {
    bt_log(
        ERROR,
        "fidl",
        "%s: BondingData address is not public or static random (address: %s)",
        __FUNCTION__,
        bt_str(address));
    return std::nullopt;
  }

  return address;
}

bt::sm::PairingData LePairingDataFromFidl(
    bt::DeviceAddress peer_address,
    const fuchsia::bluetooth::sys::LeBondData& data) {
  bt::sm::PairingData result;

  if (data.has_peer_ltk()) {
    result.peer_ltk = LtkFromFidl(data.peer_ltk());
  }
  if (data.has_local_ltk()) {
    result.local_ltk = LtkFromFidl(data.local_ltk());
  }
  if (data.has_irk()) {
    result.irk = PeerKeyFromFidl(data.irk());
    // If there is an IRK, there must also be an identity address. Assume that
    // the identity address is the peer address, since the peer address is set
    // to the identity address upon bonding.
    result.identity_address = peer_address;
  }
  if (data.has_csrk()) {
    result.csrk = PeerKeyFromFidl(data.csrk());
  }
  return result;
}

std::optional<bt::sm::LTK> BredrKeyFromFidl(const fsys::BredrBondData& data) {
  if (!data.has_link_key()) {
    return std::nullopt;
  }
  auto key = PeerKeyFromFidl(data.link_key());
  return bt::sm::LTK(key.security(), bt::hci_spec::LinkKey(key.value(), 0, 0));
}

std::vector<bt::UUID> BredrServicesFromFidl(
    const fuchsia::bluetooth::sys::BredrBondData& data) {
  std::vector<bt::UUID> services_out;
  if (data.has_services()) {
    std::transform(data.services().begin(),
                   data.services().end(),
                   std::back_inserter(services_out),
                   UuidFromFidl);
  }
  return services_out;
}

fuchsia::bluetooth::sys::BondingData PeerToFidlBondingData(
    const bt::gap::Adapter& adapter, const bt::gap::Peer& peer) {
  fsys::BondingData out;

  out.set_identifier(fbt::PeerId{peer.identifier().value()});
  out.set_local_address(AddressToFidl(fbt::AddressType::PUBLIC,
                                      adapter.state().controller_address));
  out.set_address(AddressToFidl(peer.address()));

  if (peer.name()) {
    out.set_name(*peer.name());
  }

  if (peer.bredr() && peer.bredr()->device_class()) {
    out.set_device_class(DeviceClassToFidl(*peer.bredr()->device_class()));
  }

  // LE
  if (peer.le() && peer.le()->bond_data()) {
    fsys::LeBondData out_le;
    const bt::sm::PairingData& bond = *peer.le()->bond_data();

    // TODO(armansito): Store the peer's preferred connection parameters.
    // TODO(fxbug.dev/42137736): Store GATT and AD service UUIDs.

    if (bond.local_ltk) {
      out_le.set_local_ltk(LtkToFidl(*bond.local_ltk));
    }
    if (bond.peer_ltk) {
      out_le.set_peer_ltk(LtkToFidl(*bond.peer_ltk));
    }
    if (bond.irk) {
      out_le.set_irk(PeerKeyToFidl(*bond.irk));
    }
    if (bond.csrk) {
      out_le.set_csrk(PeerKeyToFidl(*bond.csrk));
    }

    out.set_le_bond(std::move(out_le));
  }

  // BR/EDR
  if (peer.bredr() && peer.bredr()->link_key()) {
    fsys::BredrBondData out_bredr;

    // TODO(fxbug.dev/42076955): Populate with history of role switches.

    const auto& services = peer.bredr()->services();
    std::transform(services.begin(),
                   services.end(),
                   std::back_inserter(*out_bredr.mutable_services()),
                   UuidToFidl);
    out_bredr.set_link_key(LtkToFidlPeerKey(*peer.bredr()->link_key()));
    out.set_bredr_bond(std::move(out_bredr));
  }

  return out;
}

fble::RemoteDevicePtr NewLERemoteDevice(const bt::gap::Peer& peer) {
  bt::AdvertisingData ad;
  if (!peer.le()) {
    return nullptr;
  }

  auto fidl_device = std::make_unique<fble::RemoteDevice>();
  fidl_device->identifier = peer.identifier().ToString();
  fidl_device->connectable = peer.connectable();

  // Initialize advertising data only if its non-empty.
  const std::optional<std::reference_wrapper<const bt::AdvertisingData>>
      adv_data = peer.le()->parsed_advertising_data();
  if (adv_data.has_value()) {
    auto data = fidl_helpers::AdvertisingDataToFidlDeprecated(adv_data.value());
    fidl_device->advertising_data =
        std::make_unique<fble::AdvertisingDataDeprecated>(std::move(data));
  } else if (peer.le()->advertising_data_error().has_value()) {
    // If the peer advertising data has failed to parse, then this conversion
    // failed.
    return nullptr;
  }

  if (peer.rssi() != bt::hci_spec::kRSSIInvalid) {
    fidl_device->rssi = std::make_unique<Int8>();
    fidl_device->rssi->value = peer.rssi();
  }

  return fidl_device;
}

bool IsScanFilterValid(const fble::ScanFilter& fidl_filter) {
  // |service_uuids| and |service_data_uuids| are the only fields that can
  // potentially contain invalid data, since they are represented as strings.
  if (fidl_filter.service_uuids) {
    for (const auto& uuid_str : *fidl_filter.service_uuids) {
      if (!bt::IsStringValidUuid(uuid_str)) {
        return false;
      }
    }
  }

  if (fidl_filter.service_data_uuids) {
    for (const auto& uuid_str : *fidl_filter.service_data_uuids) {
      if (!bt::IsStringValidUuid(uuid_str))
        return false;
    }
  }

  return true;
}

bool PopulateDiscoveryFilter(const fble::ScanFilter& fidl_filter,
                             bt::hci::DiscoveryFilter* out_filter) {
  PW_DCHECK(out_filter);

  if (fidl_filter.service_uuids) {
    std::vector<bt::UUID> uuids;
    for (const auto& uuid_str : *fidl_filter.service_uuids) {
      bt::UUID uuid;
      if (!bt::StringToUuid(uuid_str, &uuid)) {
        bt_log(WARN,
               "fidl",
               "invalid service UUID given to scan filter: %s",
               uuid_str.c_str());
        return false;
      }
      uuids.push_back(uuid);
    }

    if (!uuids.empty()) {
      out_filter->set_service_uuids(uuids);
    }
  }

  if (fidl_filter.service_data_uuids) {
    std::vector<bt::UUID> uuids;
    for (const auto& uuid_str : *fidl_filter.service_data_uuids) {
      bt::UUID uuid;
      if (!bt::StringToUuid(uuid_str, &uuid)) {
        bt_log(WARN,
               "fidl",
               "invalid service data UUID given to scan filter: %s",
               uuid_str.c_str());
        return false;
      }
      uuids.push_back(uuid);
    }

    if (!uuids.empty()) {
      out_filter->set_service_data_uuids(uuids);
    }
  }

  if (fidl_filter.connectable) {
    out_filter->set_connectable(fidl_filter.connectable->value);
  }

  if (fidl_filter.manufacturer_identifier) {
    out_filter->set_manufacturer_code(
        fidl_filter.manufacturer_identifier->value);
  }

  if (fidl_filter.name_substring &&
      !fidl_filter.name_substring.value_or("").empty()) {
    out_filter->set_name_substring(fidl_filter.name_substring.value_or(""));
  }

  if (fidl_filter.max_path_loss) {
    out_filter->set_pathloss(fidl_filter.max_path_loss->value);
  }

  return true;
}

bt::hci::DiscoveryFilter DiscoveryFilterFromFidl(
    const fuchsia::bluetooth::le::Filter& fidl_filter) {
  bt::hci::DiscoveryFilter out;

  if (fidl_filter.has_service_uuid()) {
    out.set_service_uuids({bt::UUID(fidl_filter.service_uuid().value)});
  }

  if (fidl_filter.has_service_data_uuid()) {
    out.set_service_data_uuids(
        {bt::UUID(fidl_filter.service_data_uuid().value)});
  }

  if (fidl_filter.has_solicitation_uuid()) {
    out.set_solicitation_uuids(
        {bt::UUID(fidl_filter.solicitation_uuid().value)});
  }

  if (fidl_filter.has_manufacturer_id()) {
    out.set_manufacturer_code(fidl_filter.manufacturer_id());
  }

  if (fidl_filter.has_connectable()) {
    out.set_connectable(fidl_filter.connectable());
  }

  if (fidl_filter.has_name()) {
    out.set_name_substring(fidl_filter.name());
  }

  if (fidl_filter.has_max_path_loss()) {
    out.set_pathloss(fidl_filter.max_path_loss());
  }

  return out;
}

bt::gap::AdvertisingInterval AdvertisingIntervalFromFidl(
    fble::AdvertisingModeHint mode_hint) {
  switch (mode_hint) {
    case fble::AdvertisingModeHint::VERY_FAST:
      return bt::gap::AdvertisingInterval::FAST1;
    case fble::AdvertisingModeHint::FAST:
      return bt::gap::AdvertisingInterval::FAST2;
    case fble::AdvertisingModeHint::SLOW:
      return bt::gap::AdvertisingInterval::SLOW;
  }
  return bt::gap::AdvertisingInterval::SLOW;
}

std::optional<bt::AdvertisingData> AdvertisingDataFromFidl(
    const fble::AdvertisingData& input) {
  bt::AdvertisingData output;

  if (input.has_name()) {
    if (!output.SetLocalName(input.name())) {
      return std::nullopt;
    }
  }
  if (input.has_appearance()) {
    output.SetAppearance(static_cast<uint16_t>(input.appearance()));
  }
  if (input.has_tx_power_level()) {
    output.SetTxPower(input.tx_power_level());
  }
  if (input.has_service_uuids()) {
    for (const auto& uuid : input.service_uuids()) {
      bt::UUID bt_uuid = UuidFromFidl(uuid);
      if (!output.AddServiceUuid(bt_uuid)) {
        bt_log(WARN,
               "fidl",
               "Received more Service UUIDs than fit in a single AD - "
               "truncating UUID %s",
               bt_str(bt_uuid));
      }
    }
  }
  if (input.has_service_data()) {
    for (const auto& entry : input.service_data()) {
      if (!output.SetServiceData(UuidFromFidl(entry.uuid),
                                 bt::BufferView(entry.data))) {
        return std::nullopt;
      }
    }
  }
  if (input.has_manufacturer_data()) {
    for (const auto& entry : input.manufacturer_data()) {
      bt::BufferView data(entry.data);
      if (!output.SetManufacturerData(entry.company_id, data)) {
        return std::nullopt;
      }
    }
  }
  if (input.has_uris()) {
    for (const auto& uri : input.uris()) {
      if (!output.AddUri(uri)) {
        return std::nullopt;
      }
    }
  }
  if (input.has_broadcast_name()) {
    output.SetBroadcastName(input.broadcast_name());
  }
  if (input.has_resolvable_set_identifier()) {
    output.SetResolvableSetIdentifier(input.resolvable_set_identifier());
  }

  return output;
}

fble::AdvertisingData AdvertisingDataToFidl(const bt::AdvertisingData& input) {
  fble::AdvertisingData output;

  if (input.local_name()) {
    output.set_name(input.local_name()->name);
  }
  if (input.appearance()) {
    // TODO(fxbug.dev/42145156): Remove this to allow for passing arbitrary
    // appearance values to clients in a way that's forward-compatible with
    // future BLE revisions.
    const uint16_t appearance_raw = input.appearance().value();
    if (auto appearance = AppearanceToFidl(appearance_raw)) {
      output.set_appearance(appearance.value());
    } else {
      bt_log(DEBUG,
             "fidl",
             "omitting unencodeable appearance %#.4x of peer %s",
             appearance_raw,
             input.local_name().has_value() ? input.local_name()->name.c_str()
                                            : "");
    }
  }
  if (input.tx_power()) {
    output.set_tx_power_level(*input.tx_power());
  }
  std::unordered_set<bt::UUID> service_uuids = input.service_uuids();
  if (!service_uuids.empty()) {
    std::vector<fbt::Uuid> uuids;
    uuids.reserve(service_uuids.size());
    for (const auto& uuid : service_uuids) {
      uuids.push_back(fbt::Uuid{uuid.value()});
    }
    output.set_service_uuids(std::move(uuids));
  }
  if (!input.service_data_uuids().empty()) {
    std::vector<fble::ServiceData> entries;
    for (const auto& uuid : input.service_data_uuids()) {
      auto data = input.service_data(uuid);
      fble::ServiceData entry{fbt::Uuid{uuid.value()}, data.ToVector()};
      entries.push_back(std::move(entry));
    }
    output.set_service_data(std::move(entries));
  }
  if (!input.manufacturer_data_ids().empty()) {
    std::vector<fble::ManufacturerData> entries;
    for (const auto& id : input.manufacturer_data_ids()) {
      auto data = input.manufacturer_data(id);
      fble::ManufacturerData entry{id, data.ToVector()};
      entries.push_back(std::move(entry));
    }
    output.set_manufacturer_data(std::move(entries));
  }
  if (!input.uris().empty()) {
    std::vector<std::string> uris;
    for (const auto& uri : input.uris()) {
      uris.push_back(uri);
    }
    output.set_uris(std::move(uris));
  }
  if (input.broadcast_name()) {
    output.set_broadcast_name(*input.broadcast_name());
  }
  if (input.resolvable_set_identifier()) {
    output.set_resolvable_set_identifier(*input.resolvable_set_identifier());
  }

  return output;
}

fble::AdvertisingDataDeprecated AdvertisingDataToFidlDeprecated(
    const bt::AdvertisingData& input) {
  fble::AdvertisingDataDeprecated output;

  if (input.local_name()) {
    output.name = input.local_name()->name;
  }
  if (input.appearance()) {
    output.appearance = std::make_unique<fbt::UInt16>();
    output.appearance->value = *input.appearance();
  }
  if (input.tx_power()) {
    output.tx_power_level = std::make_unique<fbt::Int8>();
    output.tx_power_level->value = *input.tx_power();
  }
  if (!input.service_uuids().empty()) {
    output.service_uuids.emplace();
    for (const auto& uuid : input.service_uuids()) {
      output.service_uuids->push_back(uuid.ToString());
    }
  }
  if (!input.service_data_uuids().empty()) {
    output.service_data.emplace();
    for (const auto& uuid : input.service_data_uuids()) {
      auto data = input.service_data(uuid);
      fble::ServiceDataEntry entry{uuid.ToString(), data.ToVector()};
      output.service_data->push_back(std::move(entry));
    }
  }
  if (!input.manufacturer_data_ids().empty()) {
    output.manufacturer_specific_data.emplace();
    for (const auto& id : input.manufacturer_data_ids()) {
      auto data = input.manufacturer_data(id);
      fble::ManufacturerSpecificDataEntry entry{id, data.ToVector()};
      output.manufacturer_specific_data->push_back(std::move(entry));
    }
  }
  if (!input.uris().empty()) {
    output.uris.emplace();
    for (const auto& uri : input.uris()) {
      output.uris->push_back(uri);
    }
  }

  return output;
}

fuchsia::bluetooth::le::ScanData AdvertisingDataToFidlScanData(
    const bt::AdvertisingData& input,
    pw::chrono::SystemClock::time_point timestamp) {
  // Reuse bt::AdvertisingData -> fble::AdvertisingData utility, since most
  // fields are the same as fble::ScanData.
  fble::AdvertisingData fidl_adv_data = AdvertisingDataToFidl(input);
  fble::ScanData out;
  if (fidl_adv_data.has_tx_power_level()) {
    out.set_tx_power(fidl_adv_data.tx_power_level());
  }
  if (fidl_adv_data.has_appearance()) {
    out.set_appearance(fidl_adv_data.appearance());
  }
  if (fidl_adv_data.has_service_uuids()) {
    out.set_service_uuids(std::move(*fidl_adv_data.mutable_service_uuids()));
  }
  if (fidl_adv_data.has_service_data()) {
    out.set_service_data(std::move(*fidl_adv_data.mutable_service_data()));
  }
  if (fidl_adv_data.has_manufacturer_data()) {
    out.set_manufacturer_data(
        std::move(*fidl_adv_data.mutable_manufacturer_data()));
  }
  if (fidl_adv_data.has_uris()) {
    out.set_uris(std::move(*fidl_adv_data.mutable_uris()));
  }
  zx_time_t timestamp_ns = timestamp.time_since_epoch().count();
  out.set_timestamp(timestamp_ns);
  return out;
}

::fuchsia_bluetooth_le::ScanData AdvertisingDataToNewFidlScanData(
    const bt::AdvertisingData& input, zx::time timestamp) {
  ::fuchsia_bluetooth_le::ScanData out;

  if (input.tx_power()) {
    out.tx_power() = input.tx_power().value();
  }

  if (input.appearance()) {
    // TODO(fxbug.dev/42145156): Remove this to allow for passing arbitrary
    // appearance values to clients in a way that's forward-compatible with
    // future BLE revisions.
    const uint16_t appearance_raw = input.appearance().value();
    if (auto appearance = AppearanceToNewFidl(appearance_raw)) {
      out.appearance() = appearance.value();
    } else {
      bt_log(DEBUG,
             "fidl",
             "omitting unencodeable appearance %#.4x of peer %s",
             appearance_raw,
             input.local_name().has_value() ? input.local_name()->name.c_str()
                                            : "");
    }
  }

  std::unordered_set<bt::UUID> service_uuids = input.service_uuids();
  if (!service_uuids.empty()) {
    std::vector<::fuchsia_bluetooth::Uuid> uuids;
    uuids.reserve(service_uuids.size());
    for (const bt::UUID& uuid : service_uuids) {
      uuids.push_back(::fuchsia_bluetooth::Uuid{uuid.value()});
    }
    out.service_uuids() = std::move(uuids);
  }

  if (!input.service_data_uuids().empty()) {
    std::vector<::fuchsia_bluetooth_le::ServiceData> entries;
    entries.reserve(input.service_data_uuids().size());
    for (const bt::UUID& uuid : input.service_data_uuids()) {
      bt::BufferView data = input.service_data(uuid);
      ::fuchsia_bluetooth_le::ServiceData entry{
          ::fuchsia_bluetooth::Uuid{uuid.value()}, data.ToVector()};
      entries.emplace_back(std::move(entry));
    }
    out.service_data() = std::move(entries);
  }

  if (!input.manufacturer_data_ids().empty()) {
    std::vector<::fuchsia_bluetooth_le::ManufacturerData> entries;
    for (const uint16_t& id : input.manufacturer_data_ids()) {
      bt::BufferView data = input.manufacturer_data(id);
      ::fuchsia_bluetooth_le::ManufacturerData entry{id, data.ToVector()};
      entries.emplace_back(std::move(entry));
    }
    out.manufacturer_data() = std::move(entries);
  }

  if (!input.uris().empty()) {
    out.uris().emplace(input.uris().begin(), input.uris().end());
  }

  out.timestamp() = timestamp.get();
  return out;
}

::fuchsia_bluetooth_le::BroadcastIsochronousGroupInfo
BroadcastIsochronousGroupInfoToFidl(
    const bt::hci_spec::BroadcastIsochronousGroupInfo& info) {
  ::fuchsia_bluetooth_le::BroadcastIsochronousGroupInfo out;
  out.streams_count() = info.num_bis;
  out.max_sdu_size() = info.max_sdu;
  out.phy() = IsoPhyToFidl(info.phy);
  out.encryption() = info.encryption;
  return out;
}

fble::Peer PeerToFidlLe(const bt::gap::Peer& peer) {
  PW_CHECK(peer.le());

  fble::Peer output;
  output.set_id(fbt::PeerId{peer.identifier().value()});
  output.set_connectable(peer.connectable());

  if (peer.rssi() != bt::hci_spec::kRSSIInvalid) {
    output.set_rssi(peer.rssi());
  }

  const std::optional<std::reference_wrapper<const bt::AdvertisingData>>
      advertising_data = peer.le()->parsed_advertising_data();
  if (advertising_data.has_value()) {
    std::optional<pw::chrono::SystemClock::time_point> timestamp =
        peer.le()->parsed_advertising_data_timestamp();
    output.set_advertising_data(
        AdvertisingDataToFidl(advertising_data.value()));
    output.set_data(AdvertisingDataToFidlScanData(advertising_data.value(),
                                                  timestamp.value()));
  }

  if (peer.name()) {
    output.set_name(peer.name().value());
  }

  output.set_bonded(peer.bonded());
  zx_time_t last_updated_ns = peer.last_updated().time_since_epoch().count();
  output.set_last_updated(last_updated_ns);

  uint8_t advertising_sid = peer.le()->advertising_sid();
  if (advertising_sid != bt::hci_spec::kAdvertisingSidInvalid) {
    output.set_advertising_sid(advertising_sid);
  }
  uint16_t periodic_advertising_interval =
      peer.le()->periodic_advertising_interval();
  if (periodic_advertising_interval !=
      bt::hci_spec::kPeriodicAdvertisingIntervalInvalid) {
    output.set_periodic_advertising_interval(periodic_advertising_interval);
  }

  return output;
}

bt::gatt::ReliableMode ReliableModeFromFidl(
    const fgatt::WriteOptions& write_options) {
  return (write_options.has_reliable_mode() &&
          write_options.reliable_mode() == fgatt::ReliableMode::ENABLED)
             ? bt::gatt::ReliableMode::kEnabled
             : bt::gatt::ReliableMode::kDisabled;
}

// TODO(fxbug.dev/42141942): The 64 bit `fidl_gatt_id` can overflow the 16 bits
// of a bt:att::Handle that underlies CharacteristicHandles when directly
// casted. Fix this.
bt::gatt::CharacteristicHandle CharacteristicHandleFromFidl(
    uint64_t fidl_gatt_id) {
  if (fidl_gatt_id > std::numeric_limits<bt::att::Handle>::max()) {
    bt_log(ERROR,
           "fidl",
           "Casting a 64-bit FIDL GATT ID with `bits[16, 63] != 0` (0x%lX) to "
           "16-bit "
           "Characteristic Handle",
           fidl_gatt_id);
  }
  return bt::gatt::CharacteristicHandle(
      static_cast<bt::att::Handle>(fidl_gatt_id));
}

// TODO(fxbug.dev/42141942): The 64 bit `fidl_gatt_id` can overflow the 16 bits
// of a bt:att::Handle that underlies DescriptorHandles when directly casted.
// Fix this.
bt::gatt::DescriptorHandle DescriptorHandleFromFidl(uint64_t fidl_gatt_id) {
  if (fidl_gatt_id > std::numeric_limits<bt::att::Handle>::max()) {
    bt_log(ERROR,
           "fidl",
           "Casting a 64-bit FIDL GATT ID with `bits[16, 63] != 0` (0x%lX) to "
           "16-bit Descriptor "
           "Handle",
           fidl_gatt_id);
  }
  return bt::gatt::DescriptorHandle(static_cast<bt::att::Handle>(fidl_gatt_id));
}

fpromise::result<bt::sdp::ServiceRecord, fuchsia::bluetooth::ErrorCode>
ServiceDefinitionToServiceRecord(
    const fuchsia_bluetooth_bredr::ServiceDefinition& definition) {
  bt::sdp::ServiceRecord rec;
  std::vector<bt::UUID> classes;

  if (!definition.service_class_uuids().has_value()) {
    bt_log(WARN, "fidl", "Advertised service contains no Service UUIDs");
    return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
  }

  for (auto& uuid : definition.service_class_uuids().value()) {
    bt::UUID btuuid = fidl_helpers::NewUuidFromFidl(uuid);
    bt_log(TRACE, "fidl", "Setting Service Class UUID %s", bt_str(btuuid));
    classes.emplace_back(btuuid);
  }

  rec.SetServiceClassUUIDs(classes);

  if (definition.protocol_descriptor_list().has_value()) {
    if (!NewAddProtocolDescriptorList(
            &rec,
            bt::sdp::ServiceRecord::kPrimaryProtocolList,
            definition.protocol_descriptor_list().value())) {
      bt_log(ERROR, "fidl", "Failed to add protocol descriptor list");
      return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
    }
  }

  if (definition.additional_protocol_descriptor_lists().has_value()) {
    // It's safe to iterate through this list with a ProtocolListId as
    // ProtocolListId = uint8_t, and std::numeric_limits<uint8_t>::max() == 255
    // == the MAX_SEQUENCE_LENGTH vector limit from
    // fuchsia.bluetooth.bredr/ServiceDefinition.additional_protocol_descriptor_lists.
    PW_CHECK(
        definition.additional_protocol_descriptor_lists()->size() <=
        std::numeric_limits<bt::sdp::ServiceRecord::ProtocolListId>::max());
    bt::sdp::ServiceRecord::ProtocolListId protocol_list_id = 1;
    for (const auto& descriptor_list :
         definition.additional_protocol_descriptor_lists().value()) {
      if (!NewAddProtocolDescriptorList(
              &rec, protocol_list_id, descriptor_list)) {
        bt_log(
            ERROR, "fidl", "Failed to add additional protocol descriptor list");
        return fpromise::error(
            fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
      }
      protocol_list_id++;
    }
  }

  if (definition.profile_descriptors().has_value()) {
    for (const auto& profile : definition.profile_descriptors().value()) {
      if (!profile.profile_id().has_value() ||
          !profile.major_version().has_value() ||
          !profile.minor_version().has_value()) {
        bt_log(WARN, "fidl", "ProfileDescriptor missing required fields");
        return fpromise::error(
            fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
      }
      bt_log(TRACE,
             "fidl",
             "Adding Profile %#hx v%d.%d",
             static_cast<unsigned short>(*profile.profile_id()),
             *profile.major_version(),
             *profile.minor_version());
      rec.AddProfile(bt::UUID(static_cast<uint16_t>(*profile.profile_id())),
                     *profile.major_version(),
                     *profile.minor_version());
    }
  }

  if (definition.information().has_value()) {
    for (const auto& info : definition.information().value()) {
      if (!info.language().has_value()) {
        return fpromise::error(
            fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
      }

      const std::string& language = info.language().value();
      std::string name, description, provider;
      if (info.name().has_value()) {
        name = info.name().value();
      }
      if (info.description().has_value()) {
        description = info.description().value();
      }
      if (info.provider().has_value()) {
        provider = info.provider().value();
      }
      bt_log(TRACE,
             "fidl",
             "Adding Info (%s): (%s, %s, %s)",
             language.c_str(),
             name.c_str(),
             description.c_str(),
             provider.c_str());
      rec.AddInfo(language, name, description, provider);
    }
  }

  if (definition.additional_attributes().has_value()) {
    for (const auto& attribute : definition.additional_attributes().value()) {
      if (!attribute.element() || !attribute.id()) {
        bt_log(WARN, "fidl", "Attribute missing required fields");
        return fpromise::error(
            fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
      }
      auto elem = NewFidlToDataElement(*attribute.element());
      if (elem) {
        bt_log(TRACE,
               "fidl",
               "Adding attribute %#x : %s",
               *attribute.id(),
               elem.value().ToString().c_str());
        rec.SetAttribute(*attribute.id(), std::move(elem.value()));
      }
    }
  }
  return fpromise::ok(std::move(rec));
}

fpromise::result<bt::sdp::ServiceRecord, fuchsia::bluetooth::ErrorCode>
ServiceDefinitionToServiceRecord(
    const fuchsia::bluetooth::bredr::ServiceDefinition& definition) {
  bt::sdp::ServiceRecord rec;
  std::vector<bt::UUID> classes;

  if (!definition.has_service_class_uuids()) {
    bt_log(WARN, "fidl", "Advertised service contains no Service UUIDs");
    return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
  }

  for (auto& uuid : definition.service_class_uuids()) {
    bt::UUID btuuid = fidl_helpers::UuidFromFidl(uuid);
    bt_log(TRACE, "fidl", "Setting Service Class UUID %s", bt_str(btuuid));
    classes.emplace_back(btuuid);
  }

  rec.SetServiceClassUUIDs(classes);

  if (definition.has_protocol_descriptor_list()) {
    if (!AddProtocolDescriptorList(&rec,
                                   bt::sdp::ServiceRecord::kPrimaryProtocolList,
                                   definition.protocol_descriptor_list())) {
      bt_log(ERROR, "fidl", "Failed to add protocol descriptor list");
      return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
    }
  }

  if (definition.has_additional_protocol_descriptor_lists()) {
    // It's safe to iterate through this list with a ProtocolListId as
    // ProtocolListId = uint8_t, and std::numeric_limits<uint8_t>::max() == 255
    // == the MAX_SEQUENCE_LENGTH vector limit from
    // fuchsia.bluetooth.bredr/ServiceDefinition.additional_protocol_descriptor_lists.
    PW_CHECK(
        definition.additional_protocol_descriptor_lists().size() <=
        std::numeric_limits<bt::sdp::ServiceRecord::ProtocolListId>::max());
    bt::sdp::ServiceRecord::ProtocolListId protocol_list_id = 1;
    for (const auto& descriptor_list :
         definition.additional_protocol_descriptor_lists()) {
      if (!AddProtocolDescriptorList(&rec, protocol_list_id, descriptor_list)) {
        bt_log(
            ERROR, "fidl", "Failed to add additional protocol descriptor list");
        return fpromise::error(
            fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
      }
      protocol_list_id++;
    }
  }

  if (definition.has_profile_descriptors()) {
    for (const auto& profile : definition.profile_descriptors()) {
      if (!profile.has_profile_id() || !profile.has_major_version() ||
          !profile.has_minor_version()) {
        bt_log(ERROR, "fidl", "ProfileDescriptor missing required fields");
        return fpromise::error(
            fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
      }
      bt_log(TRACE,
             "fidl",
             "Adding Profile %#hx v%d.%d",
             static_cast<unsigned short>(profile.profile_id()),
             profile.major_version(),
             profile.minor_version());
      rec.AddProfile(bt::UUID(static_cast<uint16_t>(profile.profile_id())),
                     profile.major_version(),
                     profile.minor_version());
    }
  }

  if (definition.has_information()) {
    for (const auto& info : definition.information()) {
      if (!info.has_language()) {
        return fpromise::error(
            fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
      }
      const std::string& language = info.language();
      std::string name, description, provider;
      if (info.has_name()) {
        name = info.name();
      }
      if (info.has_description()) {
        description = info.description();
      }
      if (info.has_provider()) {
        provider = info.provider();
      }
      bt_log(TRACE,
             "fidl",
             "Adding Info (%s): (%s, %s, %s)",
             language.c_str(),
             name.c_str(),
             description.c_str(),
             provider.c_str());
      rec.AddInfo(language, name, description, provider);
    }
  }

  if (definition.has_additional_attributes()) {
    for (const auto& attribute : definition.additional_attributes()) {
      if (!attribute.has_element() || !attribute.has_id()) {
        bt_log(WARN, "fidl", "Attribute missing required fields");
        return fpromise::error(
            fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
      }
      auto elem = FidlToDataElement(attribute.element());
      if (elem) {
        bt_log(TRACE,
               "fidl",
               "Adding attribute %#x : %s",
               attribute.id(),
               elem.value().ToString().c_str());
        rec.SetAttribute(attribute.id(), std::move(elem.value()));
      }
    }
  }
  return fpromise::ok(std::move(rec));
}

fpromise::result<fuchsia::bluetooth::bredr::ServiceDefinition,
                 fuchsia::bluetooth::ErrorCode>
ServiceRecordToServiceDefinition(const bt::sdp::ServiceRecord& record) {
  fuchsia::bluetooth::bredr::ServiceDefinition out;

  // Service class UUIDs are mandatory
  if (!record.HasAttribute(bt::sdp::kServiceClassIdList)) {
    return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
  }
  const bt::sdp::DataElement& service_uuids_element =
      record.GetAttribute(bt::sdp::kServiceClassIdList);
  auto service_uuids = DataElementToServiceUuids(service_uuids_element);
  if (service_uuids.is_error()) {
    return fpromise::error(service_uuids.error());
  }
  out.set_service_class_uuids(std::move(service_uuids.value()));

  // Primary protocol descriptor list (optional)
  if (record.HasAttribute(bt::sdp::kProtocolDescriptorList)) {
    const bt::sdp::DataElement& primary_protocol_element =
        record.GetAttribute(bt::sdp::kProtocolDescriptorList);
    auto primary_protocol =
        DataElementToProtocolDescriptorList(primary_protocol_element);
    if (primary_protocol.is_error()) {
      return fpromise::error(primary_protocol.error());
    }
    out.set_protocol_descriptor_list(std::move(primary_protocol.value()));
  }

  // Additional protocol descriptor lists (optional)
  if (record.HasAttribute(bt::sdp::kAdditionalProtocolDescriptorList)) {
    const bt::sdp::DataElement& additional_protocols =
        record.GetAttribute(bt::sdp::kAdditionalProtocolDescriptorList);
    // Sequence of protocol descriptor list sequences.
    if (additional_protocols.type() != bt::sdp::DataElement::Type::kSequence) {
      bt_log(WARN, "fidl", "Invalid additional protocol descriptor list");
      return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
    }

    const auto additional_protocol_list =
        additional_protocols.Get<std::vector<bt::sdp::DataElement>>();
    for (const auto& addl_element : additional_protocol_list.value()) {
      auto additional_protocol =
          DataElementToProtocolDescriptorList(addl_element);
      if (additional_protocol.is_error()) {
        return fpromise::error(additional_protocol.error());
      }
      out.mutable_additional_protocol_descriptor_lists()->emplace_back(
          std::move(additional_protocol.value()));
    }
  }

  // Profile descriptors (optional)
  if (record.HasAttribute(bt::sdp::kBluetoothProfileDescriptorList)) {
    const bt::sdp::DataElement& profile_descriptors_element =
        record.GetAttribute(bt::sdp::kBluetoothProfileDescriptorList);
    auto profile_descriptors =
        DataElementToProfileDescriptors(profile_descriptors_element);
    if (profile_descriptors.is_error()) {
      return fpromise::error(profile_descriptors.error());
    }
    out.set_profile_descriptors(std::move(profile_descriptors.value()));
  }

  // Human-readable information (optional)
  const std::vector<bt::sdp::ServiceRecord::Information> information =
      record.GetInfo();
  for (const auto& info : information) {
    fbredr::Information fidl_info = InformationToFidl(info);
    out.mutable_information()->emplace_back(std::move(fidl_info));
  }

  // Additional attributes (optional)
  const bt::sdp::AttributeId kMinAdditionalAttribute = 0x200;
  const std::set<bt::sdp::AttributeId> additional_attribute_ids =
      record.GetAttributesInRange(kMinAdditionalAttribute, 0xffff);
  for (const auto additional_attr_id : additional_attribute_ids) {
    const bt::sdp::DataElement& additional_attr_elt =
        record.GetAttribute(additional_attr_id);
    std::optional<fbredr::DataElement> element =
        DataElementToFidl(additional_attr_elt);
    if (!element) {
      bt_log(WARN, "fidl", "Invalid additional attribute data element");
      return fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
    }
    fbredr::Attribute attr;
    attr.set_id(additional_attr_id);
    attr.set_element(std::move(element.value()));
    out.mutable_additional_attributes()->emplace_back(std::move(attr));
  }
  return fpromise::ok(std::move(out));
}

bt::gap::BrEdrSecurityRequirements FidlToBrEdrSecurityRequirements(
    const fbt::ChannelParameters& fidl) {
  bt::gap::BrEdrSecurityRequirements requirements{.authentication = false,
                                                  .secure_connections = false};
  if (fidl.has_security_requirements()) {
    if (fidl.security_requirements().has_authentication_required()) {
      requirements.authentication =
          fidl.security_requirements().authentication_required();
    }

    if (fidl.security_requirements().has_secure_connections_required()) {
      requirements.secure_connections =
          fidl.security_requirements().secure_connections_required();
    }
  }
  return requirements;
}

std::optional<bt::sco::ParameterSet> FidlToScoParameterSet(
    const fbredr::HfpParameterSet param_set) {
  switch (param_set) {
    case fbredr::HfpParameterSet::T1:
      return bt::sco::kParameterSetT1;
    case fbredr::HfpParameterSet::T2:
      return bt::sco::kParameterSetT2;
    case fbredr::HfpParameterSet::S1:
      return bt::sco::kParameterSetS1;
    case fbredr::HfpParameterSet::S2:
      return bt::sco::kParameterSetS2;
    case fbredr::HfpParameterSet::S3:
      return bt::sco::kParameterSetS3;
    case fbredr::HfpParameterSet::S4:
      return bt::sco::kParameterSetS4;
    case fbredr::HfpParameterSet::D0:
      return bt::sco::kParameterSetD0;
    case fbredr::HfpParameterSet::D1:
      return bt::sco::kParameterSetD1;
    default:
      return std::nullopt;
  }
}

std::optional<
    bt::StaticPacket<pw::bluetooth::emboss::SynchronousConnectionParameters::
                         VendorCodingFormatWriter>>
FidlToScoCodingFormat(const fbt::AssignedCodingFormat format) {
  bt::StaticPacket<pw::bluetooth::emboss::SynchronousConnectionParameters::
                       VendorCodingFormatWriter>
      out;
  auto view = out.view();
  // Set to 0 since vendor specific coding formats are not supported.
  view.company_id().Write(0);
  view.vendor_codec_id().Write(0);
  switch (format) {
    case fbt::AssignedCodingFormat::A_LAW_LOG:
      view.coding_format().Write(pw::bluetooth::emboss::CodingFormat::A_LAW);
      break;
    case fbt::AssignedCodingFormat::U_LAW_LOG:
      view.coding_format().Write(pw::bluetooth::emboss::CodingFormat::U_LAW);
      break;
    case fbt::AssignedCodingFormat::CVSD:
      view.coding_format().Write(pw::bluetooth::emboss::CodingFormat::CVSD);
      break;
    case fbt::AssignedCodingFormat::TRANSPARENT:
      view.coding_format().Write(
          pw::bluetooth::emboss::CodingFormat::TRANSPARENT);
      break;
    case fbt::AssignedCodingFormat::LINEAR_PCM:
      view.coding_format().Write(
          pw::bluetooth::emboss::CodingFormat::LINEAR_PCM);
      break;
    case fbt::AssignedCodingFormat::MSBC:
      view.coding_format().Write(pw::bluetooth::emboss::CodingFormat::MSBC);
      break;
    case fbt::AssignedCodingFormat::LC3:
      view.coding_format().Write(pw::bluetooth::emboss::CodingFormat::LC3);
      break;
    case fbt::AssignedCodingFormat::G_729A:
      view.coding_format().Write(pw::bluetooth::emboss::CodingFormat::G729A);
      break;
    default:
      return std::nullopt;
  }
  return out;
}

fpromise::result<pw::bluetooth::emboss::PcmDataFormat> FidlToPcmDataFormat(
    const faudio::SampleFormat& format) {
  switch (format) {
    case faudio::SampleFormat::PCM_SIGNED:
      return fpromise::ok(
          pw::bluetooth::emboss::PcmDataFormat::TWOS_COMPLEMENT);
    case faudio::SampleFormat::PCM_UNSIGNED:
      return fpromise::ok(pw::bluetooth::emboss::PcmDataFormat::UNSIGNED);
    default:
      // Other sample formats are not supported by SCO.
      return fpromise::error();
  }
}

pw::bluetooth::emboss::ScoDataPath FidlToScoDataPath(
    const fbredr::DataPath& path, uint8_t offload_value) {
  switch (path) {
    case fbredr::DataPath::HOST:
      return pw::bluetooth::emboss::ScoDataPath::HCI;
    case fbredr::DataPath::OFFLOAD: {
      return static_cast<pw::bluetooth::emboss::ScoDataPath>(offload_value);
    }
    case fbredr::DataPath::TEST:
      return pw::bluetooth::emboss::ScoDataPath::AUDIO_TEST_MODE;
  }
}

fpromise::result<bt::StaticPacket<
    pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
FidlToScoParameters(const fbredr::ScoConnectionParameters& params,
                    uint8_t sco_offload_path_index) {
  bt::StaticPacket<pw::bluetooth::emboss::SynchronousConnectionParametersWriter>
      out;
  auto view = out.view();

  if (!params.has_parameter_set()) {
    bt_log(WARN, "fidl", "SCO parameters missing parameter_set");
    return fpromise::error();
  }
  auto param_set = FidlToScoParameterSet(params.parameter_set());
  if (!param_set.has_value()) {
    bt_log(WARN, "fidl", "Unrecognized SCO parameters parameter_set");
    return fpromise::error();
  }
  view.transmit_bandwidth().Write(param_set.value().transmit_receive_bandwidth);
  view.receive_bandwidth().Write(param_set.value().transmit_receive_bandwidth);

  if (!params.has_air_coding_format()) {
    bt_log(WARN, "fidl", "SCO parameters missing air_coding_format");
    return fpromise::error();
  }
  std::optional<
      bt::StaticPacket<pw::bluetooth::emboss::SynchronousConnectionParameters::
                           VendorCodingFormatWriter>>
      air_coding_format = FidlToScoCodingFormat(params.air_coding_format());
  if (!air_coding_format) {
    bt_log(WARN, "fidl", "SCO parameters contains unknown air_coding_format");
    return fpromise::error();
  }
  view.transmit_coding_format().CopyFrom(air_coding_format->view());
  view.receive_coding_format().CopyFrom(air_coding_format->view());

  if (!params.has_air_frame_size()) {
    bt_log(WARN, "fidl", "SCO parameters missing air_frame_size");
    return fpromise::error();
  }
  view.transmit_codec_frame_size_bytes().Write(params.air_frame_size());
  view.receive_codec_frame_size_bytes().Write(params.air_frame_size());

  if (!params.has_io_bandwidth()) {
    bt_log(WARN, "fidl", "SCO parameters missing io_bandwidth");
    return fpromise::error();
  }
  view.input_bandwidth().Write(params.io_bandwidth());
  view.output_bandwidth().Write(params.io_bandwidth());

  if (!params.has_io_coding_format()) {
    bt_log(WARN, "fidl", "SCO parameters missing io_coding_format");
    return fpromise::error();
  }
  std::optional<
      bt::StaticPacket<pw::bluetooth::emboss::SynchronousConnectionParameters::
                           VendorCodingFormatWriter>>
      io_coding_format = FidlToScoCodingFormat(params.io_coding_format());
  if (!io_coding_format) {
    bt_log(WARN, "fidl", "SCO parameters contains unknown io_coding_format");
    return fpromise::error();
  }
  view.input_coding_format().CopyFrom(io_coding_format->view());
  view.output_coding_format().CopyFrom(io_coding_format->view());

  if (!params.has_io_frame_size()) {
    bt_log(WARN, "fidl", "SCO parameters missing io_frame_size");
    return fpromise::error();
  }
  view.input_coded_data_size_bits().Write(params.io_frame_size());
  view.output_coded_data_size_bits().Write(params.io_frame_size());

  if (params.has_io_pcm_data_format() &&
      view.input_coding_format().coding_format().Read() ==
          pw::bluetooth::emboss::CodingFormat::LINEAR_PCM) {
    auto io_pcm_format = FidlToPcmDataFormat(params.io_pcm_data_format());
    if (io_pcm_format.is_error()) {
      bt_log(WARN, "fidl", "Unsupported IO PCM data format in SCO parameters");
      return fpromise::error();
    }
    view.input_pcm_data_format().Write(io_pcm_format.value());
    view.output_pcm_data_format().Write(io_pcm_format.value());

  } else if (view.input_coding_format().coding_format().Read() ==
             pw::bluetooth::emboss::CodingFormat::LINEAR_PCM) {
    bt_log(WARN,
           "fidl",
           "SCO parameters missing io_pcm_data_format (required for linear PCM "
           "IO coding format)");
    return fpromise::error();
  } else {
    view.input_pcm_data_format().Write(
        pw::bluetooth::emboss::PcmDataFormat::NOT_APPLICABLE);
    view.output_pcm_data_format().Write(
        pw::bluetooth::emboss::PcmDataFormat::NOT_APPLICABLE);
  }

  if (params.has_io_pcm_sample_payload_msb_position() &&
      view.input_coding_format().coding_format().Read() ==
          pw::bluetooth::emboss::CodingFormat::LINEAR_PCM) {
    view.input_pcm_sample_payload_msb_position().Write(
        params.io_pcm_sample_payload_msb_position());
    view.output_pcm_sample_payload_msb_position().Write(
        params.io_pcm_sample_payload_msb_position());
  } else {
    view.input_pcm_sample_payload_msb_position().Write(0u);
    view.output_pcm_sample_payload_msb_position().Write(0u);
  }

  if (!params.has_path()) {
    bt_log(WARN, "fidl", "SCO parameters missing data path");
    return fpromise::error();
  }
  auto path = FidlToScoDataPath(params.path(), sco_offload_path_index);
  view.input_data_path().Write(path);
  view.output_data_path().Write(path);

  // For HCI Host transport the transport unit size should be "0". For PCM
  // transport the unit size is vendor specific. A unit size of "0" indicates
  // "not applicable".
  // TODO(fxbug.dev/42136417): Use unit size from stack configuration file
  // instead of hardcoding "not applicable".
  view.input_transport_unit_size_bits().Write(0u);
  view.output_transport_unit_size_bits().Write(0u);

  view.max_latency_ms().Write(param_set.value().max_latency_ms);
  view.packet_types().BackingStorage().WriteUInt(
      param_set.value().packet_types);
  view.retransmission_effort().Write(
      static_cast<pw::bluetooth::emboss::SynchronousConnectionParameters::
                      ScoRetransmissionEffort>(
          param_set.value().retransmission_effort));

  return fpromise::ok(out);
}

fpromise::result<std::vector<bt::StaticPacket<
    pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>>
FidlToScoParametersVector(
    const std::vector<fbredr::ScoConnectionParameters>& params,
    uint8_t sco_offload_path_index) {
  std::vector<bt::StaticPacket<
      pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
      out;
  out.reserve(params.size());
  for (const fbredr::ScoConnectionParameters& param : params) {
    fpromise::result<bt::StaticPacket<
        pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
        result = FidlToScoParameters(param, sco_offload_path_index);
    if (result.is_error()) {
      return fpromise::error();
    }
    out.push_back(result.take_value());
  }
  return fpromise::ok(std::move(out));
}

bool IsFidlGattHandleValid(fuchsia::bluetooth::gatt2::Handle handle) {
  if (handle.value > std::numeric_limits<bt::att::Handle>::max()) {
    bt_log(ERROR,
           "fidl",
           "Invalid 64-bit FIDL GATT ID with `bits[16, 63] != 0` (0x%lX)",
           handle.value);
    return false;
  }
  return true;
}

bool IsFidlGattServiceHandleValid(
    fuchsia::bluetooth::gatt2::ServiceHandle handle) {
  if (handle.value > std::numeric_limits<bt::att::Handle>::max()) {
    bt_log(ERROR,
           "fidl",
           "Invalid 64-bit FIDL GATT ID with `bits[16, 63] != 0` (0x%lX)",
           handle.value);
    return false;
  }
  return true;
}

fuchsia::bluetooth::bredr::RxPacketStatus ScoPacketStatusToFidl(
    bt::hci_spec::SynchronousDataPacketStatusFlag status) {
  switch (status) {
    case bt::hci_spec::SynchronousDataPacketStatusFlag::kCorrectlyReceived:
      return fuchsia::bluetooth::bredr::RxPacketStatus::CORRECTLY_RECEIVED_DATA;
    case bt::hci_spec::SynchronousDataPacketStatusFlag::kPossiblyInvalid:
      return fuchsia::bluetooth::bredr::RxPacketStatus::POSSIBLY_INVALID_DATA;
    case bt::hci_spec::SynchronousDataPacketStatusFlag::kNoDataReceived:
      return fuchsia::bluetooth::bredr::RxPacketStatus::NO_DATA_RECEIVED;
    case bt::hci_spec::SynchronousDataPacketStatusFlag::kDataPartiallyLost:
      return fuchsia::bluetooth::bredr::RxPacketStatus::DATA_PARTIALLY_LOST;
  }
}

bt::att::ErrorCode Gatt2ErrorCodeFromFidl(fgatt2::Error error_code) {
  switch (error_code) {
    case fgatt2::Error::INVALID_HANDLE:
      return bt::att::ErrorCode::kInvalidHandle;
    case fgatt2::Error::READ_NOT_PERMITTED:
      return bt::att::ErrorCode::kReadNotPermitted;
    case fgatt2::Error::WRITE_NOT_PERMITTED:
      return bt::att::ErrorCode::kWriteNotPermitted;
    case fgatt2::Error::INVALID_OFFSET:
      return bt::att::ErrorCode::kInvalidOffset;
    case fgatt2::Error::INVALID_ATTRIBUTE_VALUE_LENGTH:
      return bt::att::ErrorCode::kInvalidAttributeValueLength;
    case fgatt2::Error::INSUFFICIENT_RESOURCES:
      return bt::att::ErrorCode::kInsufficientResources;
    case fgatt2::Error::VALUE_NOT_ALLOWED:
      return bt::att::ErrorCode::kValueNotAllowed;
    default:
      break;
  }
  return bt::att::ErrorCode::kUnlikelyError;
}

bt::att::AccessRequirements Gatt2AccessRequirementsFromFidl(
    const fuchsia::bluetooth::gatt2::SecurityRequirements& reqs) {
  return bt::att::AccessRequirements(
      reqs.has_encryption_required() ? reqs.encryption_required() : false,
      reqs.has_authentication_required() ? reqs.authentication_required()
                                         : false,
      reqs.has_authorization_required() ? reqs.authorization_required()
                                        : false);
}

std::unique_ptr<bt::gatt::Descriptor> Gatt2DescriptorFromFidl(
    const fgatt2::Descriptor& fidl_desc) {
  if (!fidl_desc.has_permissions()) {
    bt_log(
        WARN, "fidl", "FIDL descriptor missing required `permissions` field");
    return nullptr;
  }
  const fgatt2::AttributePermissions& perm = fidl_desc.permissions();
  bt::att::AccessRequirements read_reqs =
      perm.has_read() ? Gatt2AccessRequirementsFromFidl(perm.read())
                      : bt::att::AccessRequirements();
  bt::att::AccessRequirements write_reqs =
      perm.has_write() ? Gatt2AccessRequirementsFromFidl(perm.write())
                       : bt::att::AccessRequirements();

  if (!fidl_desc.has_type()) {
    bt_log(WARN, "fidl", "FIDL descriptor missing required `type` field");
    return nullptr;
  }
  bt::UUID type(fidl_desc.type().value);

  if (!fidl_desc.has_handle()) {
    bt_log(WARN, "fidl", "FIDL characteristic missing required `handle` field");
    return nullptr;
  }
  return std::make_unique<bt::gatt::Descriptor>(
      fidl_desc.handle().value, type, read_reqs, write_reqs);
}

std::unique_ptr<bt::gatt::Characteristic> Gatt2CharacteristicFromFidl(
    const fgatt2::Characteristic& fidl_chrc) {
  if (!fidl_chrc.has_properties()) {
    bt_log(WARN,
           "fidl",
           "FIDL characteristic missing required `properties` field");
    return nullptr;
  }
  if (!fidl_chrc.has_permissions()) {
    bt_log(WARN,
           "fidl",
           "FIDL characteristic missing required `permissions` field");
    return nullptr;
  }
  if (!fidl_chrc.has_type()) {
    bt_log(WARN, "fidl", "FIDL characteristic missing required `type` field");
    return nullptr;
  }
  if (!fidl_chrc.has_handle()) {
    bt_log(WARN, "fidl", "FIDL characteristic missing required `handle` field");
    return nullptr;
  }

  uint8_t props = static_cast<uint8_t>(fidl_chrc.properties());
  const uint16_t ext_props =
      static_cast<uint16_t>(fidl_chrc.properties()) >> CHAR_BIT;
  if (ext_props) {
    props |= bt::gatt::Property::kExtendedProperties;
  }

  const fgatt2::AttributePermissions& permissions = fidl_chrc.permissions();
  bool supports_update = (props & bt::gatt::Property::kNotify) ||
                         (props & bt::gatt::Property::kIndicate);
  if (supports_update != permissions.has_update()) {
    bt_log(WARN,
           "fidl",
           "Characteristic update permission %s",
           supports_update ? "required" : "must be null");
    return nullptr;
  }

  bt::att::AccessRequirements read_reqs =
      permissions.has_read()
          ? Gatt2AccessRequirementsFromFidl(permissions.read())
          : bt::att::AccessRequirements();
  bt::att::AccessRequirements write_reqs =
      permissions.has_write()
          ? Gatt2AccessRequirementsFromFidl(permissions.write())
          : bt::att::AccessRequirements();
  bt::att::AccessRequirements update_reqs =
      permissions.has_update()
          ? Gatt2AccessRequirementsFromFidl(permissions.update())
          : bt::att::AccessRequirements();

  bt::UUID type(fidl_chrc.type().value);

  auto chrc =
      std::make_unique<bt::gatt::Characteristic>(fidl_chrc.handle().value,
                                                 type,
                                                 props,
                                                 ext_props,
                                                 read_reqs,
                                                 write_reqs,
                                                 update_reqs);
  if (fidl_chrc.has_descriptors()) {
    for (const auto& fidl_desc : fidl_chrc.descriptors()) {
      std::unique_ptr<bt::gatt::Descriptor> maybe_desc =
          fidl_helpers::Gatt2DescriptorFromFidl(fidl_desc);
      if (!maybe_desc) {
        // Specific failures are logged in Gatt2DescriptorFromFidl
        return nullptr;
      }

      chrc->AddDescriptor(std::move(maybe_desc));
    }
  }

  return chrc;
}

const char* DataPathDirectionToString(
    pw::bluetooth::emboss::DataPathDirection direction) {
  switch (direction) {
    case pw::bluetooth::emboss::DataPathDirection::INPUT:
      return "input";
    case pw::bluetooth::emboss::DataPathDirection::OUTPUT:
      return "output";
    default:
      return "invalid";
  }
}

pw::bluetooth::emboss::DataPathDirection DataPathDirectionFromFidl(
    const fuchsia::bluetooth::DataDirection& fidl_direction) {
  switch (fidl_direction) {
    case fuchsia::bluetooth::DataDirection::INPUT:
      return pw::bluetooth::emboss::DataPathDirection::INPUT;
    case fuchsia::bluetooth::DataDirection::OUTPUT:
      return pw::bluetooth::emboss::DataPathDirection::OUTPUT;
  }
  PW_CRASH("Unrecognized value for data direction: %" PRIu8, fidl_direction);
}

// Both of these types use the spec representation, so we can just assign the
// underlying value directly.
pw::bluetooth::emboss::CodingFormat CodingFormatFromFidl(
    const fuchsia::bluetooth::AssignedCodingFormat& fidl_format) {
  switch (fidl_format) {
    case fuchsia::bluetooth::AssignedCodingFormat::U_LAW_LOG:
      return pw::bluetooth::emboss::CodingFormat::U_LAW;
    case fuchsia::bluetooth::AssignedCodingFormat::A_LAW_LOG:
      return pw::bluetooth::emboss::CodingFormat::A_LAW;
    case fuchsia::bluetooth::AssignedCodingFormat::CVSD:
      return pw::bluetooth::emboss::CodingFormat::CVSD;
    case fuchsia::bluetooth::AssignedCodingFormat::TRANSPARENT:
      return pw::bluetooth::emboss::CodingFormat::TRANSPARENT;
    case fuchsia::bluetooth::AssignedCodingFormat::LINEAR_PCM:
      return pw::bluetooth::emboss::CodingFormat::LINEAR_PCM;
    case fuchsia::bluetooth::AssignedCodingFormat::MSBC:
      return pw::bluetooth::emboss::CodingFormat::MSBC;
    case fuchsia::bluetooth::AssignedCodingFormat::LC3:
      return pw::bluetooth::emboss::CodingFormat::LC3;
    case fuchsia::bluetooth::AssignedCodingFormat::G_729A:
      return pw::bluetooth::emboss::CodingFormat::G729A;
  }
  PW_CRASH("Unrecognized value for coding format: %u",
           static_cast<unsigned>(fidl_format));
}

bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter> CodecIdFromFidl(
    const fuchsia::bluetooth::CodecId& fidl_codec_id) {
  bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter> result;
  auto result_view = result.view();

  if (fidl_codec_id.is_assigned_format()) {
    pw::bluetooth::emboss::CodingFormat out_coding_format =
        CodingFormatFromFidl(fidl_codec_id.assigned_format());
    result_view.coding_format().Write(out_coding_format);
  } else {
    PW_CHECK(fidl_codec_id.is_vendor_format());
    result_view.coding_format().Write(
        pw::bluetooth::emboss::CodingFormat::VENDOR_SPECIFIC);
    result_view.company_id().Write(fidl_codec_id.vendor_format().company_id());
    result_view.vendor_codec_id().Write(
        fidl_codec_id.vendor_format().vendor_id());
  }
  return result;
}

// Note that:
// a) The FIDL values used do not necessarily correspond to Core Spec values.
// b) Only a subset of valid values are implemented in the FIDL type at the
// moment.
pw::bluetooth::emboss::LogicalTransportType LogicalTransportTypeFromFidl(
    const fuchsia::bluetooth::LogicalTransportType& fidl_transport_type) {
  switch (fidl_transport_type) {
    case fuchsia::bluetooth::LogicalTransportType::LE_CIS:
      return pw::bluetooth::emboss::LogicalTransportType::LE_CIS;
    case fuchsia::bluetooth::LogicalTransportType::LE_BIS:
      return pw::bluetooth::emboss::LogicalTransportType::LE_BIS;
  }
  PW_CRASH("Unrecognized value for logical transport type: %u",
           static_cast<unsigned>(fidl_transport_type));
}

pw::bluetooth::emboss::StatusCode FidlHciErrorToStatusCode(
    fhbt::HciError code) {
  switch (code) {
    case fuchsia_hardware_bluetooth::HciError::kSuccess:
      return pw::bluetooth::emboss::StatusCode::SUCCESS;
    case fuchsia_hardware_bluetooth::HciError::kUnknownCommand:
      return pw::bluetooth::emboss::StatusCode::UNKNOWN_COMMAND;
    case fuchsia_hardware_bluetooth::HciError::kUnknownConnectionId:
      return pw::bluetooth::emboss::StatusCode::UNKNOWN_CONNECTION_ID;
    case fuchsia_hardware_bluetooth::HciError::kHardwareFailure:
      return pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE;
    case fuchsia_hardware_bluetooth::HciError::kPageTimeout:
      return pw::bluetooth::emboss::StatusCode::PAGE_TIMEOUT;
    case fuchsia_hardware_bluetooth::HciError::kAuthenticationFailure:
      return pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE;
    case fuchsia_hardware_bluetooth::HciError::kPinOrKeyMissing:
      return pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING;
    case fuchsia_hardware_bluetooth::HciError::kMemoryCapacityExceeded:
      return pw::bluetooth::emboss::StatusCode::MEMORY_CAPACITY_EXCEEDED;
    case fuchsia_hardware_bluetooth::HciError::kConnectionTimeout:
      return pw::bluetooth::emboss::StatusCode::CONNECTION_TIMEOUT;
    case fuchsia_hardware_bluetooth::HciError::kConnectionLimitExceeded:
      return pw::bluetooth::emboss::StatusCode::CONNECTION_LIMIT_EXCEEDED;
    case fuchsia_hardware_bluetooth::HciError::
        kSynchronousConnectionLimitExceeded:
      return pw::bluetooth::emboss::StatusCode::
          SYNCHRONOUS_CONNECTION_LIMIT_EXCEEDED;
    case fuchsia_hardware_bluetooth::HciError::kConnectionAlreadyExists:
      return pw::bluetooth::emboss::StatusCode::CONNECTION_ALREADY_EXISTS;
    case fuchsia_hardware_bluetooth::HciError::kCommandDisallowed:
      return pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED;
    case fuchsia_hardware_bluetooth::HciError::
        kConnectionRejectedLimitedResources:
      return pw::bluetooth::emboss::StatusCode::
          CONNECTION_REJECTED_LIMITED_RESOURCES;
    case fuchsia_hardware_bluetooth::HciError::kConnectionRejectedSecurity:
      return pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_SECURITY;
    case fuchsia_hardware_bluetooth::HciError::kConnectionRejectedBadBdAddr:
      return pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_BAD_BD_ADDR;
    case fuchsia_hardware_bluetooth::HciError::kConnectionAcceptTimeoutExceeded:
      return pw::bluetooth::emboss::StatusCode::
          CONNECTION_ACCEPT_TIMEOUT_EXCEEDED;
    case fuchsia_hardware_bluetooth::HciError::kUnsupportedFeatureOrParameter:
      return pw::bluetooth::emboss::StatusCode::
          UNSUPPORTED_FEATURE_OR_PARAMETER;
    case fuchsia_hardware_bluetooth::HciError::kInvalidHcicommandParameters:
      return pw::bluetooth::emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS;
    case fuchsia_hardware_bluetooth::HciError::kRemoteUserTerminatedConnection:
      return pw::bluetooth::emboss::StatusCode::
          REMOTE_USER_TERMINATED_CONNECTION;
    case fuchsia_hardware_bluetooth::HciError::
        kRemoteDeviceTerminatedConnectionLowResources:
      return pw::bluetooth::emboss::StatusCode::
          REMOTE_DEVICE_TERMINATED_CONNECTION_LOW_RESOURCES;
    case fuchsia_hardware_bluetooth::HciError::
        kRemoteDeviceTerminatedConnectionPowerOff:
      return pw::bluetooth::emboss::StatusCode::
          REMOTE_DEVICE_TERMINATED_CONNECTION_POWER_OFF;
    case fuchsia_hardware_bluetooth::HciError::kConnectionTerminatedByLocalHost:
      return pw::bluetooth::emboss::StatusCode::
          CONNECTION_TERMINATED_BY_LOCAL_HOST;
    case fuchsia_hardware_bluetooth::HciError::kRepeatedAttempts:
      return pw::bluetooth::emboss::StatusCode::REPEATED_ATTEMPTS;
    case fuchsia_hardware_bluetooth::HciError::kPairingNotAllowed:
      return pw::bluetooth::emboss::StatusCode::PAIRING_NOT_ALLOWED;
    case fuchsia_hardware_bluetooth::HciError::kUnknownLmpPdu:
      return pw::bluetooth::emboss::StatusCode::UNKNOWN_LMP_PDU;
    case fuchsia_hardware_bluetooth::HciError::kUnsupportedRemoteFeature:
      return pw::bluetooth::emboss::StatusCode::UNSUPPORTED_REMOTE_FEATURE;
    case fuchsia_hardware_bluetooth::HciError::kScoOffsetRejected:
      return pw::bluetooth::emboss::StatusCode::SCO_OFFSET_REJECTED;
    case fuchsia_hardware_bluetooth::HciError::kScoIntervalRejected:
      return pw::bluetooth::emboss::StatusCode::SCO_INTERVAL_REJECTED;
    case fuchsia_hardware_bluetooth::HciError::kScoAirModeRejected:
      return pw::bluetooth::emboss::StatusCode::SCO_AIRMODE_REJECTED;
    case fuchsia_hardware_bluetooth::HciError::kInvalidLmpOrLlParameters:
      return pw::bluetooth::emboss::StatusCode::INVALID_LMP_OR_LL_PARAMETERS;
    case fuchsia_hardware_bluetooth::HciError::kUnspecifiedError:
      return pw::bluetooth::emboss::StatusCode::UNSPECIFIED_ERROR;
    case fuchsia_hardware_bluetooth::HciError::
        kUnsupportedLmpOrLlParameterValue:
      return pw::bluetooth::emboss::StatusCode::
          UNSUPPORTED_LMP_OR_LL_PARAMETER_VALUE;
    case fuchsia_hardware_bluetooth::HciError::kRoleChangeNotAllowed:
      return pw::bluetooth::emboss::StatusCode::ROLE_CHANGE_NOT_ALLOWED;
    case fuchsia_hardware_bluetooth::HciError::kLmpOrLlResponseTimeout:
      return pw::bluetooth::emboss::StatusCode::LMP_OR_LL_RESPONSE_TIMEOUT;
    case fuchsia_hardware_bluetooth::HciError::kLmpErrorTransactionCollision:
      return pw::bluetooth::emboss::StatusCode::LMP_ERROR_TRANSACTION_COLLISION;
    case fuchsia_hardware_bluetooth::HciError::kLmpPduNotAllowed:
      return pw::bluetooth::emboss::StatusCode::LMP_PDU_NOT_ALLOWED;
    case fuchsia_hardware_bluetooth::HciError::kEncryptionModeNotAcceptable:
      return pw::bluetooth::emboss::StatusCode::ENCRYPTION_MODE_NOT_ACCEPTABLE;
    case fuchsia_hardware_bluetooth::HciError::kLinkKeyCannotBeChanged:
      return pw::bluetooth::emboss::StatusCode::LINK_KEY_CANNOT_BE_CHANGED;
    case fuchsia_hardware_bluetooth::HciError::kRequestedQosNotSupported:
      return pw::bluetooth::emboss::StatusCode::REQUESTED_QOS_NOT_SUPPORTED;
    case fuchsia_hardware_bluetooth::HciError::kInstantPassed:
      return pw::bluetooth::emboss::StatusCode::INSTANT_PASSED;
    case fuchsia_hardware_bluetooth::HciError::kPairingWithUnitKeyNotSupported:
      return pw::bluetooth::emboss::StatusCode::
          PAIRING_WITH_UNIT_KEY_NOT_SUPPORTED;
    case fuchsia_hardware_bluetooth::HciError::kDifferentTransactionCollision:
      return pw::bluetooth::emboss::StatusCode::DIFFERENT_TRANSACTION_COLLISION;
    case fuchsia_hardware_bluetooth::HciError::kReserved0:
      return pw::bluetooth::emboss::StatusCode::RESERVED_0;
    case fuchsia_hardware_bluetooth::HciError::kQosUnacceptableParameter:
      return pw::bluetooth::emboss::StatusCode::QOS_UNACCEPTABLE_PARAMETER;
    case fuchsia_hardware_bluetooth::HciError::kQosRejected:
      return pw::bluetooth::emboss::StatusCode::QOS_REJECTED;
    case fuchsia_hardware_bluetooth::HciError::
        kChannelClassificationNotSupported:
      return pw::bluetooth::emboss::StatusCode::
          CHANNEL_CLASSIFICATION_NOT_SUPPORTED;
    case fuchsia_hardware_bluetooth::HciError::kInsufficientSecurity:
      return pw::bluetooth::emboss::StatusCode::INSUFFICIENT_SECURITY;
    case fuchsia_hardware_bluetooth::HciError::kParameterOutOfMandatoryRange:
      return pw::bluetooth::emboss::StatusCode::
          PARAMETER_OUT_OF_MANDATORY_RANGE;
    case fuchsia_hardware_bluetooth::HciError::kReserved1:
      return pw::bluetooth::emboss::StatusCode::RESERVED_1;
    case fuchsia_hardware_bluetooth::HciError::kRoleSwitchPending:
      return pw::bluetooth::emboss::StatusCode::ROLE_SWITCH_PENDING;
    case fuchsia_hardware_bluetooth::HciError::kReserved2:
      return pw::bluetooth::emboss::StatusCode::RESERVED_2;
    case fuchsia_hardware_bluetooth::HciError::kReservedSlotViolation:
      return pw::bluetooth::emboss::StatusCode::RESERVED_SLOT_VIOLATION;
    case fuchsia_hardware_bluetooth::HciError::kRoleSwitchFailed:
      return pw::bluetooth::emboss::StatusCode::ROLE_SWITCH_FAILED;
    case fuchsia_hardware_bluetooth::HciError::kExtendedInquiryResponseTooLarge:
      return pw::bluetooth::emboss::StatusCode::
          EXTENDED_INQUIRY_RESPONSE_TOO_LARGE;
    case fuchsia_hardware_bluetooth::HciError::
        kSecureSimplePairingNotSupportedByHost:
      return pw::bluetooth::emboss::StatusCode::
          SECURE_SIMPLE_PAIRING_NOT_SUPPORTED_BY_HOST;
    case fuchsia_hardware_bluetooth::HciError::kHostBusyPairing:
      return pw::bluetooth::emboss::StatusCode::HOST_BUSY_PAIRING;
    case fuchsia_hardware_bluetooth::HciError::
        kConnectionRejectedNoSuitableChannelFound:
      return pw::bluetooth::emboss::StatusCode::
          CONNECTION_REJECTED_NO_SUITABLE_CHANNEL_FOUND;
    case fuchsia_hardware_bluetooth::HciError::kControllerBusy:
      return pw::bluetooth::emboss::StatusCode::CONTROLLER_BUSY;
    case fuchsia_hardware_bluetooth::HciError::
        kUnacceptableConnectionParameters:
      return pw::bluetooth::emboss::StatusCode::
          UNACCEPTABLE_CONNECTION_PARAMETERS;
    case fuchsia_hardware_bluetooth::HciError::kDirectedAdvertisingTimeout:
      return pw::bluetooth::emboss::StatusCode::DIRECTED_ADVERTISING_TIMEOUT;
    case fuchsia_hardware_bluetooth::HciError::kConnectionTerminatedMicFailure:
      return pw::bluetooth::emboss::StatusCode::
          CONNECTION_TERMINATED_MIC_FAILURE;
    case fuchsia_hardware_bluetooth::HciError::kConnectionFailedToBeEstablished:
      return pw::bluetooth::emboss::StatusCode::
          CONNECTION_FAILED_TO_BE_ESTABLISHED;
    case fuchsia_hardware_bluetooth::HciError::kMacConnectionFailed:
      return pw::bluetooth::emboss::StatusCode::MAC_CONNECTION_FAILED;
    case fuchsia_hardware_bluetooth::HciError::kCoarseClockAdjustmentRejected:
      return pw::bluetooth::emboss::StatusCode::
          COARSE_CLOCK_ADJUSTMENT_REJECTED;
    case fuchsia_hardware_bluetooth::HciError::kType0SubmapNotDefined:
      return pw::bluetooth::emboss::StatusCode::TYPE_0_SUBMAP_NOT_DEFINED;
    case fuchsia_hardware_bluetooth::HciError::kUnknownAdvertisingIdentifier:
      return pw::bluetooth::emboss::StatusCode::UNKNOWN_ADVERTISING_IDENTIFIER;
    case fuchsia_hardware_bluetooth::HciError::kLimitReached:
      return pw::bluetooth::emboss::StatusCode::LIMIT_REACHED;
    case fuchsia_hardware_bluetooth::HciError::kOperationCancelledByHost:
      return pw::bluetooth::emboss::StatusCode::OPERATION_CANCELLED_BY_HOST;
    case fuchsia_hardware_bluetooth::HciError::kPacketTooLong:
      return pw::bluetooth::emboss::StatusCode::PACKET_TOO_LONG;
    case fuchsia_hardware_bluetooth::HciError::kTooLate:
      return pw::bluetooth::emboss::StatusCode::TOO_LATE;
    case fuchsia_hardware_bluetooth::HciError::kTooEarly:
      return pw::bluetooth::emboss::StatusCode::TOO_EARLY;
    default:
      return pw::bluetooth::emboss::StatusCode::UNKNOWN_COMMAND;
  }
}

fuchsia::bluetooth::le::CisEstablishedParameters CisEstablishedParametersToFidl(
    const bt::iso::CisEstablishedParameters& params_in) {
  fuchsia::bluetooth::le::CisEstablishedParameters params_out;

  // General parameters
  zx::duration cig_sync_delay = zx::usec(params_in.cig_sync_delay);
  params_out.set_cig_sync_delay(cig_sync_delay.get());
  zx::duration cis_sync_delay = zx::usec(params_in.cis_sync_delay);
  params_out.set_cis_sync_delay(cis_sync_delay.get());
  params_out.set_max_subevents(params_in.max_subevents);
  zx::duration iso_interval =
      zx::usec(params_in.iso_interval *
               bt::iso::CisEstablishedParameters::kIsoIntervalToMicroseconds);
  params_out.set_iso_interval(iso_interval.get());

  // Central => Peripheral parameters
  // phy and max_pdu_size are not passed back to FIDL client
  if (params_in.c_to_p_params.burst_number > 0) {
    fuchsia::bluetooth::le::CisUnidirectionalParams c_to_p_params;
    zx::duration transport_latency =
        zx::usec(params_in.c_to_p_params.transport_latency);
    c_to_p_params.set_transport_latency(transport_latency.get());
    c_to_p_params.set_burst_number(params_in.c_to_p_params.burst_number);
    c_to_p_params.set_flush_timeout(params_in.c_to_p_params.flush_timeout);
    params_out.set_central_to_peripheral_params(std::move(c_to_p_params));
  }

  // Peripheral => Central parameters
  // phy and max_pdu_size are not passed back to FIDL client
  if (params_in.p_to_c_params.burst_number > 0) {
    fuchsia::bluetooth::le::CisUnidirectionalParams p_to_c_params;
    zx::duration transport_latency =
        zx::usec(params_in.p_to_c_params.transport_latency);
    p_to_c_params.set_transport_latency(transport_latency.get());
    p_to_c_params.set_burst_number(params_in.p_to_c_params.burst_number);
    p_to_c_params.set_flush_timeout(params_in.p_to_c_params.flush_timeout);
    params_out.set_peripheral_to_central_params(std::move(p_to_c_params));
  }

  return params_out;
}

bt::DeviceAddress::Type FidlToDeviceAddressType(fbt::AddressType addr_type) {
  switch (addr_type) {
    case fbt::AddressType::PUBLIC:
      return bt::DeviceAddress::Type::kLEPublic;
    case fbt::AddressType::RANDOM:
      return bt::DeviceAddress::Type::kLERandom;
  }
}

fble::IsoPacketStatusFlag EmbossIsoPacketStatusFlagToFidl(
    pw::bluetooth::emboss::IsoDataPacketStatus status_in) {
  switch (status_in) {
    case pw::bluetooth::emboss::IsoDataPacketStatus::VALID_DATA:
      return fble::IsoPacketStatusFlag::VALID_DATA;
    case pw::bluetooth::emboss::IsoDataPacketStatus::POSSIBLY_INVALID_DATA:
      return fble::IsoPacketStatusFlag::DATA_WITH_POSSIBLE_ERRORS;
    case pw::bluetooth::emboss::IsoDataPacketStatus::LOST_DATA:
      return fble::IsoPacketStatusFlag::LOST_DATA;
  }
}

::fuchsia_bluetooth_le::SyncReport ReportFrom(
    const bt::gap::PeriodicAdvertisingReport& report, zx::time timestamp) {
  ::fuchsia_bluetooth_le::PeriodicAdvertisingReport advertising_report;
  advertising_report.rssi() = report.rssi;
  if (report.event_counter.has_value()) {
    advertising_report.event_counter() = report.event_counter.value();
  }
  advertising_report.data() =
      fidl_helpers::AdvertisingDataToNewFidlScanData(report.data, timestamp);
  advertising_report.timestamp() = timestamp.get();
  ::fuchsia_bluetooth_le::SyncReport sync_report =
      ::fuchsia_bluetooth_le::SyncReport::WithPeriodicAdvertisingReport(
          std::move(advertising_report));
  return sync_report;
}

::fuchsia_bluetooth_le::SyncReport ReportFrom(
    const bt::hci_spec::BroadcastIsochronousGroupInfo& report,
    zx::time timestamp) {
  ::fuchsia_bluetooth_le::BroadcastIsochronousGroupInfoReport big_report;
  big_report.info() = fidl_helpers::BroadcastIsochronousGroupInfoToFidl(report);
  big_report.timestamp() = timestamp.get();
  ::fuchsia_bluetooth_le::SyncReport sync_report =
      ::fuchsia_bluetooth_le::SyncReport::
          WithBroadcastIsochronousGroupInfoReport(std::move(big_report));
  return sync_report;
}

}  // namespace bthost::fidl_helpers

// static
std::vector<uint8_t>
fidl::TypeConverter<std::vector<uint8_t>, bt::ByteBuffer>::Convert(
    const bt::ByteBuffer& from) {
  std::vector<uint8_t> to(from.size());
  bt::MutableBufferView view(to.data(), to.size());
  view.Write(from);
  return to;
}
