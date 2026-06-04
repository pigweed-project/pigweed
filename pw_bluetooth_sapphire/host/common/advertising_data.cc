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

#include "pw_bluetooth_sapphire/internal/host/common/advertising_data.h"

#include <cpp-string/string_printf.h>
#include <pw_assert/check.h>
#include <pw_bytes/endian.h>
#include <pw_preprocessor/compiler.h>
#include <pw_string/utf_codecs.h>

#include <string>
#include <type_traits>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/to_string.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"

namespace bt {

namespace {

DataType ServiceUuidTypeForUuidSize(UUIDElemSize size, bool complete) {
  switch (size) {
    case UUIDElemSize::k16Bit:
      return complete ? DataType::kComplete16BitServiceUuids
                      : DataType::kIncomplete16BitServiceUuids;
    case UUIDElemSize::k32Bit:
      return complete ? DataType::kComplete32BitServiceUuids
                      : DataType::kIncomplete32BitServiceUuids;
    case UUIDElemSize::k128Bit:
      return complete ? DataType::kComplete128BitServiceUuids
                      : DataType::kIncomplete128BitServiceUuids;
    default:
      PW_CRASH(
          "called ServiceUuidTypeForUuidSize with unknown UUIDElemSize %du",
          size);
  }
}

DataType ServiceDataTypeForUuidSize(UUIDElemSize size) {
  switch (size) {
    case UUIDElemSize::k16Bit:
      return DataType::kServiceData16Bit;
    case UUIDElemSize::k32Bit:
      return DataType::kServiceData32Bit;
    case UUIDElemSize::k128Bit:
      return DataType::kServiceData128Bit;
    default:
      PW_CRASH(
          "called ServiceDataTypeForUuidSize with unknown UUIDElemSize %du",
          size);
  };
}

DataType SolicitationUuidTypeForUuidSize(UUIDElemSize size) {
  switch (size) {
    case UUIDElemSize::k16Bit:
      return DataType::kSolicitationUuid16Bit;
    case UUIDElemSize::k32Bit:
      return DataType::kSolicitationUuid32Bit;
    case UUIDElemSize::k128Bit:
      return DataType::kSolicitationUuid128Bit;
    default:
      PW_CRASH(
          "called SolicitationUuidTypeForUuidSize with unknown UUIDElemSize "
          "%du",
          size);
  };
}

size_t EncodedServiceDataSize(const UUID& uuid, const BufferView data) {
  return uuid.CompactSize() + data.size();
}

// clang-format off
// https://www.bluetooth.com/specifications/assigned-numbers/uri-scheme-name-string-mapping
constexpr const char* kUriSchemes[] = {"aaa:", "aaas:", "about:", "acap:", "acct:", "cap:", "cid:",
        "coap:", "coaps:", "crid:", "data:", "dav:", "dict:", "dns:", "file:", "ftp:", "geo:",
        "go:", "gopher:", "h323:", "http:", "https:", "iax:", "icap:", "im:", "imap:", "info:",
        "ipp:", "ipps:", "iris:", "iris.beep:", "iris.xpc:", "iris.xpcs:", "iris.lwz:", "jabber:",
        "ldap:", "mailto:", "mid:", "msrp:", "msrps:", "mtqp:", "mupdate:", "news:", "nfs:", "ni:",
        "nih:", "nntp:", "opaquelocktoken:", "pop:", "pres:", "reload:", "rtsp:", "rtsps:", "rtspu:",
        "service:", "session:", "shttp:", "sieve:", "sip:", "sips:", "sms:", "snmp:", "soap.beep:",
        "soap.beeps:", "stun:", "stuns:", "tag:", "tel:", "telnet:", "tftp:", "thismessage:",
        "tn3270:", "tip:", "turn:", "turns:", "tv:", "urn:", "vemmi:", "ws:", "wss:", "xcon:",
        "xcon-userid:", "xmlrpc.beep:", "xmlrpc.beeps:", "xmpp:", "z39.50r:", "z39.50s:", "acr:",
        "adiumxtra:", "afp:", "afs:", "aim:", "apt:", "attachment:", "aw:", "barion:", "beshare:",
        "bitcoin:", "bolo:", "callto:", "chrome:", "chrome-extension:", "com-eventbrite-attendee:",
        "content:", "cvs:", "dlna-playsingle:", "dlna-playcontainer:", "dtn:", "dvb:", "ed2k:",
        "facetime:", "feed:", "feedready:", "finger:", "fish:", "gg:", "git:", "gizmoproject:",
        "gtalk:", "ham:", "hcp:", "icon:", "ipn:", "irc:", "irc6:", "ircs:", "itms:", "jar:",
        "jms:", "keyparc:", "lastfm:", "ldaps:", "magnet:", "maps:", "market:", "message:", "mms:",
        "ms-help:", "ms-settings-power:", "msnim:", "mumble:", "mvn:", "notes:", "oid:", "palm:",
        "paparazzi:", "pkcs11:", "platform:", "proxy:", "psyc:", "query:", "res:", "resource:",
        "rmi:", "rsync:", "rtmfp:", "rtmp:", "secondlife:", "sftp:", "sgn:", "skype:", "smb:",
        "smtp:", "soldat:", "spotify:", "ssh:", "steam:", "submit:", "svn:", "teamspeak:",
        "teliaeid:", "things:", "udp:", "unreal:", "ut2004:", "ventrilo:", "view-source:",
        "webcal:", "wtai:", "wyciwyg:", "xfire:", "xri:", "ymsgr:", "example:",
        "ms-settings-cloudstorage:"};
// clang-format on

const size_t kUriSchemesSize = std::extent<decltype(kUriSchemes)>::value;

std::string EncodeUri(const std::string& uri) {
  for (uint32_t i = 0; i < kUriSchemesSize; i++) {
    const char* scheme = kUriSchemes[i];
    size_t scheme_len = strlen(scheme);
    if (std::strncmp(uri.c_str(), scheme, scheme_len) == 0) {
      const pw::Result<pw::utf8::EncodedCodePoint> encoded_scheme =
          pw::utf8::EncodeCodePoint(i + 2);
      PW_DCHECK(encoded_scheme.ok());
      return std::string(encoded_scheme->as_view()) + uri.substr(scheme_len);
    }
  }
  // First codepoint (U+0001) is for uncompressed schemes.
  const pw::Result<pw::utf8::EncodedCodePoint> encoded_scheme =
      pw::utf8::EncodeCodePoint(1u);
  PW_DCHECK(encoded_scheme.ok());
  return std::string(encoded_scheme->as_view()) + uri;
}

const char kUndefinedScheme = 0x01;

std::string DecodeUri(const std::string& uri) {
  if (uri[0] == kUndefinedScheme) {
    return uri.substr(1);
  }

  // NOTE: as we are reading UTF-8 from `uri`, it is possible that `code_point`
  // corresponds to > 1 byte of `uri` (even for valid URI encoding schemes, as
  // U+00(>7F) encodes to 2 bytes).
  const auto result = pw::utf8::ReadCodePoint(uri);
  if (!result.ok()) {
    bt_log(INFO,
           "gap-le",
           "Attempted to decode malformed UTF-8 in AdvertisingData URI");
    return "";
  }
  const uint32_t code_point = result->code_point();
  // `uri` is not a c-string, so URIs that start with '\0' after c_str
  // conversion (i.e. both empty URIs and URIs with leading null bytes '\0') are
  // caught by the code_point < 2 check. We check
  // "< 2" instead of "== 0" for redundancy (extra safety!) with the
  // kUndefinedScheme check above.
  if (code_point >= kUriSchemesSize + 2 || code_point < 2) {
    bt_log(
        ERROR,
        "gap-le",
        "Failed to decode URI - supplied UTF-8 encoding scheme codepoint %u "
        "must be in the "
        "range 2-kUriSchemesSize + 1 (2-%zu) to correspond to a URI encoding",
        code_point,
        kUriSchemesSize + 1);
    return "";
  }
  return kUriSchemes[code_point - 2] + uri.substr(result->size());
}

template <typename T>
inline size_t BufferWrite(MutableByteBuffer* buffer, size_t pos, const T& var) {
  buffer->Write((const uint8_t*)(uintptr_t)(&var), sizeof(T), pos);
  return sizeof(T);
}

}  // namespace

AdvertisingData::AdvertisingData(AdvertisingData&& other) noexcept {
  *this = std::move(other);
}

AdvertisingData& AdvertisingData::operator=(AdvertisingData&& other) noexcept {
  // Reset `other`'s state to that of a fresh, empty AdvertisingData
  local_name_ = std::exchange(other.local_name_, {});
  tx_power_ = std::exchange(other.tx_power_, {});
  appearance_ = std::exchange(other.appearance_, {});
  service_uuids_ = std::exchange(other.service_uuids_, kEmptyServiceUuidMap);
  solicitation_uuids_ =
      std::exchange(other.solicitation_uuids_, kEmptyServiceUuidMap);
  manufacturer_data_ = std::exchange(other.manufacturer_data_, {});
  service_data_ = std::exchange(other.service_data_, {});
  uris_ = std::exchange(other.uris_, {});
  flags_ = std::exchange(other.flags_, {});
  resolvable_set_identifier_ =
      std::exchange(other.resolvable_set_identifier_, {});
  broadcast_name_ = std::exchange(other.broadcast_name_, {});
  return *this;
}

std::string AdvertisingData::ParseErrorToString(ParseError e) {
  switch (e) {
    case ParseError::kInvalidTlvFormat:
      return "provided bytes are not a valid type-length-value container";
    case ParseError::kTxPowerLevelMalformed:
      return "malformed tx power level";
    case ParseError::kLocalNameTooLong:
      return "local name exceeds max length (248)";
    case ParseError::kUuidsMalformed:
      return "malformed UUIDs list";
    case ParseError::kManufacturerSpecificDataTooSmall:
      return "manufacturer specific data too small";
    case ParseError::kServiceDataTooSmall:
      return "service data too small to fit UUIDs";
    case ParseError::kServiceDataUuidMalformed:
      return "UUIDs associated with service data are malformed";
    case ParseError::kAppearanceMalformed:
      return "malformed appearance field";
    case ParseError::kMissing:
      return "data missing";
    case ParseError::kResolvableSetIdentifierSize:
      return "resolvable set identifier is wrong size";
    case ParseError::kBroadcastNameTooShort:
      return "broadcast name is too short";
    case ParseError::kBroadcastNameTooLong:
      return "broadcast name is too long";
  }
}

std::string AdvFlagsToString(const std::optional<AdvFlags>& flags_opt) {
  std::string result = "Flags: {";

  if (!flags_opt.has_value()) {
    return result += "} ";
  }

  const AdvFlags& flags = flags_opt.value();

  if (flags & kLELimitedDiscoverableMode) {
    result += " LE Limited Discoverable Mode,";
  }
  if (flags & kLEGeneralDiscoverableMode) {
    result += " LE General Discoverable Mode,";
  }
  if (flags & kBREDRNotSupported) {
    result += " BR/EDR Not Supported,";
  }
  if (flags & kSimultaneousLEAndBREDRController) {
    result += " Simultaneous LE And BR/EDR Controller,";
  }
  if (flags & kSimultaneousLEAndBREDRHost) {
    result += " Simultaneous LE And BR/EDR Host,";
  }
  return result += " }, ";
}

std::string AdvertisingData::ToString() const {
  std::string result = "Advertising Data { ";

  if (local_name_) {
    bt_lib_cpp_string::StringAppendf(
        &result,
        "%s Name: %s, ",
        (local_name_->is_complete ? "Complete" : "Short"),
        local_name_->name.c_str());
  }

  if (tx_power_) {
    bt_lib_cpp_string::StringAppendf(&result, "TX Power: %hhd, ", *tx_power_);
  }

  if (appearance_) {
    bt_lib_cpp_string::StringAppendf(
        &result, "Appearance: 0x%04x, ", *appearance_);
  }

  if (!uris_.empty()) {
    result += "URIs: { ";
    for (const auto& uri : uris_) {
      bt_lib_cpp_string::StringAppendf(&result, "%s, ", uri.c_str());
    }
    result += "}, ";
  }

  if (flags_.has_value()) {
    result += AdvFlagsToString(flags_);
  }

  bool has_service_uuids = false;
  for (const auto& [_, bounded_uuids] : service_uuids_) {
    if (!bounded_uuids.set().empty()) {
      has_service_uuids = true;
      break;
    }
  }

  if (has_service_uuids) {
    result += "Service UUIDs: { ";

    std::vector<std::string> uuids;
    for (const auto& [_, bounded_uuids] : service_uuids_) {
      for (const auto& uuid : bounded_uuids.set()) {
        uuids.emplace_back(bt_str(uuid));
      }
    }

    std::sort(uuids.begin(), uuids.end());
    for (const std::string& uuid : uuids) {
      bt_lib_cpp_string::StringAppendf(&result, "%s, ", uuid.c_str());
    }

    result += "}, ";
  }

  if (!service_data_.empty()) {
    result += "Service Data: { ";
    for (const auto& [uuid, data_buffer] : service_data_) {
      bt_lib_cpp_string::StringAppendf(
          &result,
          "{ UUID:%s, Data: {%s} }, ",
          bt_str(uuid),
          data_buffer.ToString(/*as_hex*/ true).c_str());
    }
    result += "}, ";
  }

  bool has_solicitation_uuids = false;
  for (const auto& [_, bounded_uuids] : solicitation_uuids_) {
    if (!bounded_uuids.set().empty()) {
      has_solicitation_uuids = true;
      break;
    }
  }

  if (has_solicitation_uuids) {
    result += "Solicitation UUIDs: { ";

    std::vector<std::string> uuids;
    for (const auto& [_, bounded_uuids] : solicitation_uuids_) {
      for (const auto& uuid : bounded_uuids.set()) {
        uuids.emplace_back(bt_str(uuid));
      }
    }

    std::sort(uuids.begin(), uuids.end());
    for (const std::string& uuid : uuids) {
      bt_lib_cpp_string::StringAppendf(&result, "%s, ", uuid.c_str());
    }
    result += "}, ";
  }

  if (!manufacturer_data_.empty()) {
    result += "Manufacturer Data: { ";
    for (const auto& [company_id, data_buffer] : manufacturer_data_) {
      bt_lib_cpp_string::StringAppendf(
          &result,
          "{ Company ID: 0x%04x, Data: {%s} }, ",
          company_id,
          data_buffer.ToString(/*as_hex*/ true).c_str());
    }
    result += "}, ";
  }

  if (resolvable_set_identifier_.has_value()) {
    result += "Resolvable Set Idenfidier: { ";
    BufferView view(resolvable_set_identifier_->data(),
                    resolvable_set_identifier_->size());
    result += view.ToString(/*as_hex=*/true);
    result += "}, ";
  }

  if (broadcast_name_) {
    bt_lib_cpp_string::StringAppendf(
        &result, "Broadcast Name: %s, ", broadcast_name_->c_str());
  }
  result += "}";
  return result;
}

AdvertisingData::ParseResult AdvertisingData::FromBytes(
    const ByteBuffer& data) {
  if (data.size() == 0) {
    return fit::error(ParseError::kMissing);
  }
  SupplementDataReader reader(data);
  if (!reader.is_valid()) {
    return fit::error(ParseError::kInvalidTlvFormat);
  }

  AdvertisingData out_ad;
  DataType type;
  BufferView field;
  while (reader.GetNextField(&type, &field)) {
    // While parsing through the advertising data fields, we do not need to
    // validate that per-field sizes do not overflow a uint8_t because they, by
    // construction, are obtained from a uint8_t.
    PW_DCHECK(field.size() <= std::numeric_limits<uint8_t>::max());
    PW_MODIFY_DIAGNOSTICS_PUSH();
    PW_MODIFY_DIAGNOSTIC(ignored, "-Wswitch-enum");
    switch (type) {
      case DataType::kTxPowerLevel: {
        if (field.size() != kTxPowerLevelSize) {
          return fit::error(ParseError::kTxPowerLevelMalformed);
        }

        out_ad.SetTxPower(static_cast<int8_t>(field[0]));
        break;
      }
      case DataType::kShortenedLocalName: {
        if (field.ToString().size() > kMaxNameLength) {
          return fit::error(ParseError::kLocalNameTooLong);
        }

        (void)out_ad.SetLocalName(field.ToString(), /*is_complete=*/false);
        break;
      }
      case DataType::kCompleteLocalName: {
        if (field.ToString().size() > kMaxNameLength) {
          return fit::error(ParseError::kLocalNameTooLong);
        }

        (void)out_ad.SetLocalName(field.ToString(), /*is_complete=*/true);
        break;
      }
      case DataType::kIncomplete16BitServiceUuids:
      case DataType::kComplete16BitServiceUuids:
      case DataType::kIncomplete32BitServiceUuids:
      case DataType::kComplete32BitServiceUuids:
      case DataType::kIncomplete128BitServiceUuids:
      case DataType::kComplete128BitServiceUuids: {
        // AddServiceUuid fails when the number of N bit UUIDs exceed the
        // kMaxNBitUuids bounds. These bounds are based on the number of UUIDs
        // that fit in the wire (byte) representation of an AdvertisingData, so
        // for valid AdvertisingData packets, the number of N bit service UUIDs
        // cannot exceed the bounds limits. However, because invalid packets may
        // provide multiple DataType fields for the same UUID (not allowed by
        // CSS v9 Part A 1.1.1), this limit may be exceeded, in which case we
        // reject the packet.
        if (!ParseUuids(
                field,
                SizeForType(type),
                fit::bind_member<&AdvertisingData::AddServiceUuid>(&out_ad))) {
          return fit::error(ParseError::kUuidsMalformed);
        }
        break;
      }
      case DataType::kSolicitationUuid16Bit:
      case DataType::kSolicitationUuid32Bit:
      case DataType::kSolicitationUuid128Bit: {
        if (!ParseUuids(field,
                        SizeForType(type),
                        fit::bind_member<&AdvertisingData::AddSolicitationUuid>(
                            &out_ad))) {
          return fit::error(ParseError::kUuidsMalformed);
        }
        break;
      }
      case DataType::kManufacturerSpecificData: {
        if (field.size() < kManufacturerSpecificDataSizeMin) {
          return fit::error(ParseError::kManufacturerSpecificDataTooSmall);
        }

        uint16_t id = pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                                  field.To<uint16_t>());
        const BufferView manuf_data(field.data() + kManufacturerIdSize,
                                    field.size() - kManufacturerIdSize);

        PW_CHECK(out_ad.SetManufacturerData(id, manuf_data));
        break;
      }
      case DataType::kServiceData16Bit:
      case DataType::kServiceData32Bit:
      case DataType::kServiceData128Bit: {
        UUID uuid;
        size_t uuid_size = SizeForType(type);
        if (field.size() < uuid_size) {
          return fit::error(ParseError::kServiceDataTooSmall);
        }
        const BufferView uuid_bytes(field.data(), uuid_size);
        if (!UUID::FromBytes(uuid_bytes, &uuid)) {
          return fit::error(ParseError::kServiceDataUuidMalformed);
        }
        const BufferView service_data(field.data() + uuid_size,
                                      field.size() - uuid_size);
        PW_CHECK(out_ad.SetServiceData(uuid, service_data));
        break;
      }
      case DataType::kAppearance: {
        // TODO(armansito): Peer should have a function to return the
        // device appearance, as it can be obtained either from advertising data
        // or via GATT.
        if (field.size() != kAppearanceSize) {
          return fit::error(ParseError::kAppearanceMalformed);
        }

        out_ad.SetAppearance(pw::bytes::ConvertOrderFrom(cpp20::endian::little,
                                                         field.To<uint16_t>()));
        break;
      }
      case DataType::kURI: {
        // Assertion is safe as AddUri only fails when field size > uint8_t,
        // which is impossible.
        PW_CHECK(out_ad.AddUri(DecodeUri(field.ToString())));
        break;
      }
      case DataType::kFlags: {
        // Flags field may be zero or more octets long but we only store the
        // first octet.
        if (field.size() > 0) {
          out_ad.SetFlags(field[0]);
        } else {
          out_ad.SetFlags(0);
        }
        break;
      }
      case DataType::kResolvableSetIdentifier: {
        if (field.size() != kResolvableSetIdentifierSize) {
          return fit::error(ParseError::kResolvableSetIdentifierSize);
        }
        std::array<uint8_t, kResolvableSetIdentifierSize> ident{
            field[0], field[1], field[2], field[3], field[4], field[5]};
        out_ad.SetResolvableSetIdentifier(ident);
        break;
      }
      case DataType::kBroadcastName: {
        if (field.size() < kMinBroadcastNameBytes) {
          return fit::error(ParseError::kBroadcastNameTooShort);
        }
        if (field.size() > kMaxBroadcastNameBytes) {
          return fit::error(ParseError::kBroadcastNameTooLong);
        }
        std::string name = field.ToString();
        out_ad.SetBroadcastName(name);
        break;
      }
      default:
        bt_log(DEBUG,
               "gap",
               "ignored advertising field (type %#.2x)",
               static_cast<unsigned int>(type));
        break;
    }
    PW_MODIFY_DIAGNOSTICS_POP();
  }

  return fit::ok(std::move(out_ad));
}

void AdvertisingData::Copy(AdvertisingData* out) const {
  *out = AdvertisingData();

  if (local_name_) {
    PW_CHECK(out->SetLocalName(*local_name_));
  }

  if (tx_power_) {
    out->SetTxPower(*tx_power_);
  }

  if (appearance_) {
    out->SetAppearance(*appearance_);
  }

  out->service_uuids_ = service_uuids_;
  out->solicitation_uuids_ = solicitation_uuids_;
  out->resolvable_set_identifier_ = resolvable_set_identifier_;
  out->broadcast_name_ = broadcast_name_;

  for (const auto& it : manufacturer_data_) {
    PW_CHECK(out->SetManufacturerData(it.first, it.second.view()));
  }

  for (const auto& it : service_data_) {
    PW_CHECK(out->SetServiceData(it.first, it.second.view()));
  }

  for (const auto& it : uris_) {
    PW_CHECK(out->AddUri(it), "Copying invalid AD with too-long URI");
  }
}

[[nodiscard]] bool AdvertisingData::AddServiceUuid(const UUID& uuid) {
  auto iter = service_uuids_.find(uuid.CompactSize());
  PW_CHECK(iter != service_uuids_.end());
  BoundedUuids& uuids = iter->second;
  return uuids.AddUuid(uuid);
}

std::unordered_set<UUID> AdvertisingData::service_uuids() const {
  std::unordered_set<UUID> out;
  for (auto& [_elemsize, uuids] : service_uuids_) {
    out.insert(uuids.set().begin(), uuids.set().end());
  }
  return out;
}

[[nodiscard]] bool AdvertisingData::SetServiceData(const UUID& uuid,
                                                   const ByteBuffer& data) {
  size_t encoded_size = EncodedServiceDataSize(uuid, data.view());
  if (encoded_size > kMaxEncodedServiceDataLength) {
    bt_log(DEBUG,
           "gap-le",
           "SetServiceData for UUID %s failed: (UUID+data) size %zu > maximum "
           "allowed size %du",
           bt_str(uuid),
           encoded_size,
           kMaxEncodedServiceDataLength);
    return false;
  }
  service_data_[uuid] = DynamicByteBuffer(data);
  return true;
}

std::unordered_set<UUID> AdvertisingData::service_data_uuids() const {
  std::unordered_set<UUID> uuids;
  for (const auto& it : service_data_) {
    uuids.emplace(it.first);
  }
  return uuids;
}

BufferView AdvertisingData::service_data(const UUID& uuid) const {
  auto iter = service_data_.find(uuid);
  if (iter == service_data_.end())
    return BufferView();
  return BufferView(iter->second);
}

[[nodiscard]] bool AdvertisingData::AddSolicitationUuid(const UUID& uuid) {
  auto iter = solicitation_uuids_.find(uuid.CompactSize());
  PW_CHECK(iter != solicitation_uuids_.end());
  BoundedUuids& uuids = iter->second;
  return uuids.AddUuid(uuid);
}

std::unordered_set<UUID> AdvertisingData::solicitation_uuids() const {
  std::unordered_set<UUID> out;
  for (auto& [_elemsize, uuids] : solicitation_uuids_) {
    out.insert(uuids.set().begin(), uuids.set().end());
  }
  return out;
}

[[nodiscard]] bool AdvertisingData::SetManufacturerData(
    const uint16_t company_id, const BufferView& data) {
  size_t field_size = data.size();
  if (field_size > kMaxManufacturerDataLength) {
    bt_log(WARN,
           "gap-le",
           "SetManufacturerData for company id %#.4x failed: (UUID+data) size "
           "%zu > maximum allowed "
           "size %hhu",
           company_id,
           field_size,
           kMaxManufacturerDataLength);
    return false;
  }
  manufacturer_data_[company_id] = DynamicByteBuffer(data);
  return true;
}

std::unordered_set<uint16_t> AdvertisingData::manufacturer_data_ids() const {
  std::unordered_set<uint16_t> manuf_ids;
  for (const auto& it : manufacturer_data_) {
    manuf_ids.emplace(it.first);
  }
  return manuf_ids;
}

BufferView AdvertisingData::manufacturer_data(const uint16_t company_id) const {
  auto iter = manufacturer_data_.find(company_id);
  if (iter == manufacturer_data_.end())
    return BufferView();
  return BufferView(iter->second);
}

void AdvertisingData::SetTxPower(int8_t dbm) { tx_power_ = dbm; }

std::optional<int8_t> AdvertisingData::tx_power() const { return tx_power_; }

bool AdvertisingData::SetLocalName(const LocalName& local_name) {
  if (local_name.name.size() > kMaxNameLength) {
    return false;
  }
  if (local_name_.has_value() && local_name_->is_complete &&
      !local_name.is_complete) {
    return false;
  }
  local_name_ = local_name;
  return true;
}

std::optional<AdvertisingData::LocalName> AdvertisingData::local_name() const {
  return local_name_;
}

void AdvertisingData::SetResolvableSetIdentifier(
    std::array<uint8_t, kResolvableSetIdentifierSize> identifier) {
  resolvable_set_identifier_ = identifier;
}

const std::optional<std::array<uint8_t, kResolvableSetIdentifierSize>>&
AdvertisingData::resolvable_set_identifier() const {
  return resolvable_set_identifier_;
}

void AdvertisingData::SetBroadcastName(const std::string& name) {
  broadcast_name_ = name;
}

const std::optional<std::string>& AdvertisingData::broadcast_name() const {
  return broadcast_name_;
}

[[nodiscard]] bool AdvertisingData::AddUri(const std::string& uri) {
  if (EncodeUri(uri).size() > kMaxEncodedUriLength) {
    bt_log(WARN,
           "gap-le",
           "not inserting uri %s as it exceeds the max URI size for AD",
           uri.c_str());
    return false;
  }
  if (uri.empty()) {
    bt_log(WARN, "gap-le", "skipping insertion of empty uri to AD");
    return true;
  }
  uris_.insert(uri);
  return true;
}

const std::unordered_set<std::string>& AdvertisingData::uris() const {
  return uris_;
}

void AdvertisingData::SetAppearance(uint16_t appearance) {
  appearance_ = appearance;
}

std::optional<uint16_t> AdvertisingData::appearance() const {
  return appearance_;
}

void AdvertisingData::SetFlags(AdvFlags flags) { flags_ = flags; }

std::optional<AdvFlags> AdvertisingData::flags() const { return flags_; }

size_t AdvertisingData::CalculateBlockSize(bool include_flags) const {
  size_t len = 0;
  if (include_flags) {
    len += kTLVFlagsSize;
  }

  if (tx_power_) {
    len += kTLVTxPowerLevelSize;
  }

  if (appearance_) {
    len += kTLVAppearanceSize;
  }

  if (local_name_) {
    len += 2 + local_name_->name.size();
  }

  for (const auto& manuf_pair : manufacturer_data_) {
    len += 2 + 2 + manuf_pair.second.size();
  }

  for (const auto& service_data_pair : service_data_) {
    len += 2 + service_data_pair.first.CompactSize() +
           service_data_pair.second.size();
  }

  for (const auto& uri : uris_) {
    len += 2 + EncodeUri(uri).size();
  }

  for (const auto& [uuid_size, bounded_uuids] : service_uuids_) {
    if (bounded_uuids.set().empty()) {
      continue;
    }
    len += 2;  // 1 byte for # of UUIDs and 1 for UUID type
    len += uuid_size * bounded_uuids.set().size();
  }

  for (const auto& [uuid_size, bounded_uuids] : solicitation_uuids_) {
    if (bounded_uuids.set().empty()) {
      continue;
    }
    len += 2;  // 1 byte for # of UUIDs and 1 for UUID type
    len += uuid_size * bounded_uuids.set().size();
  }

  if (resolvable_set_identifier_.has_value()) {
    len += kTLVResolvableSetIdentifierSize;
  }

  if (broadcast_name_) {
    len += 2 + broadcast_name_->size();
  }

  return len;
}

bool AdvertisingData::WriteBlock(MutableByteBuffer* buffer,
                                 std::optional<AdvFlags> flags) const {
  PW_DCHECK(buffer);

  size_t min_buf_size = CalculateBlockSize(flags.has_value());
  if (buffer->size() < min_buf_size) {
    return false;
  }

  size_t pos = 0;
  if (flags) {
    (*buffer)[pos++] =
        kTLVFlagsSize - 1;  // size variable includes current field, subtract 1
    (*buffer)[pos++] = static_cast<uint8_t>(DataType::kFlags);
    (*buffer)[pos++] = static_cast<uint8_t>(flags.value());
  }

  if (tx_power_) {
    (*buffer)[pos++] = kTLVTxPowerLevelSize -
                       1;  // size variable includes current field, subtract 1
    (*buffer)[pos++] = static_cast<uint8_t>(DataType::kTxPowerLevel);
    (*buffer)[pos++] = static_cast<uint8_t>(tx_power_.value());
  }

  if (appearance_) {
    (*buffer)[pos++] = kTLVAppearanceSize -
                       1;  // size variable includes current field, subtract 1
    (*buffer)[pos++] = static_cast<uint8_t>(DataType::kAppearance);
    pos += BufferWrite(buffer, pos, appearance_.value());
  }

  if (local_name_) {
    PW_CHECK(local_name_->name.size() <= kMaxNameLength);
    (*buffer)[pos++] =
        static_cast<uint8_t>(local_name_->name.size()) + 1;  // 1 for null char
    (*buffer)[pos++] = static_cast<uint8_t>(DataType::kCompleteLocalName);
    buffer->Write(reinterpret_cast<const uint8_t*>(local_name_->name.c_str()),
                  local_name_->name.size(),
                  pos);
    pos += local_name_->name.size();
  }

  for (const auto& manuf_pair : manufacturer_data_) {
    size_t data_size = manuf_pair.second.size();
    PW_CHECK(data_size <= kMaxManufacturerDataLength);
    (*buffer)[pos++] =
        1 + 2 +
        static_cast<uint8_t>(data_size);  // 1 for type, 2 for Manuf. Code
    (*buffer)[pos++] =
        static_cast<uint8_t>(DataType::kManufacturerSpecificData);
    pos += BufferWrite(buffer, pos, manuf_pair.first);
    buffer->Write(manuf_pair.second, pos);
    pos += data_size;
  }

  for (const auto& service_data_pair : service_data_) {
    UUID uuid = service_data_pair.first;
    size_t encoded_service_data_size =
        EncodedServiceDataSize(uuid, service_data_pair.second.view());
    PW_CHECK(encoded_service_data_size <= kMaxEncodedServiceDataLength);
    (*buffer)[pos++] =
        1 + static_cast<uint8_t>(encoded_service_data_size);  // 1 for type
    (*buffer)[pos++] =
        static_cast<uint8_t>(ServiceDataTypeForUuidSize(uuid.CompactSize()));
    auto target = buffer->mutable_view(pos);
    pos += service_data_pair.first.ToBytes(&target);
    buffer->Write(service_data_pair.second, pos);
    pos += service_data_pair.second.size();
  }

  for (const auto& uri : uris_) {
    std::string s = EncodeUri(uri);
    PW_CHECK(s.size() <= kMaxEncodedUriLength);
    (*buffer)[pos++] = 1 + static_cast<uint8_t>(s.size());  // 1 for type
    (*buffer)[pos++] = static_cast<uint8_t>(DataType::kURI);
    buffer->Write(reinterpret_cast<const uint8_t*>(s.c_str()), s.length(), pos);
    pos += s.size();
  }

  for (const auto& [uuid_width, bounded_uuids] : service_uuids_) {
    if (bounded_uuids.set().empty()) {
      continue;
    }

    // 1 for type
    PW_CHECK(1 + uuid_width * bounded_uuids.set().size() <=
             std::numeric_limits<uint8_t>::max());
    (*buffer)[pos++] =
        1 + uuid_width * static_cast<uint8_t>(bounded_uuids.set().size());
    (*buffer)[pos++] = static_cast<uint8_t>(
        ServiceUuidTypeForUuidSize(uuid_width, /*complete=*/false));
    for (const auto& uuid : bounded_uuids.set()) {
      PW_CHECK(uuid.CompactSize() == uuid_width,
               "UUID: %s - Expected Width: %d",
               bt_str(uuid),
               uuid_width);
      auto target = buffer->mutable_view(pos);
      pos += uuid.ToBytes(&target);
    }
  }

  for (const auto& [uuid_width, bounded_uuids] : solicitation_uuids_) {
    if (bounded_uuids.set().empty()) {
      continue;
    }

    // 1 for type
    PW_CHECK(1 + uuid_width * bounded_uuids.set().size() <=
             std::numeric_limits<uint8_t>::max());
    (*buffer)[pos++] =
        1 + uuid_width * static_cast<uint8_t>(bounded_uuids.set().size());
    (*buffer)[pos++] =
        static_cast<uint8_t>(SolicitationUuidTypeForUuidSize(uuid_width));
    for (const auto& uuid : bounded_uuids.set()) {
      PW_CHECK(uuid.CompactSize() == uuid_width,
               "UUID: %s - Expected Width: %d",
               bt_str(uuid),
               uuid_width);
      auto target = buffer->mutable_view(pos);
      pos += uuid.ToBytes(&target);
    }
  }

  if (resolvable_set_identifier_) {
    (*buffer)[pos++] =
        1 +
        static_cast<uint8_t>(resolvable_set_identifier_->size());  // 1 for type
    (*buffer)[pos++] = static_cast<uint8_t>(DataType::kResolvableSetIdentifier);
    buffer->Write(resolvable_set_identifier_->data(),
                  resolvable_set_identifier_->size(),
                  pos);
    pos += resolvable_set_identifier_->size();
  }

  if (broadcast_name_) {
    (*buffer)[pos++] =
        1 + static_cast<uint8_t>(broadcast_name_->size());  // 1 for type
    (*buffer)[pos++] = static_cast<uint8_t>(DataType::kBroadcastName);
    buffer->Write(reinterpret_cast<const uint8_t*>(broadcast_name_->c_str()),
                  broadcast_name_->size(),
                  pos);
    pos += broadcast_name_->size();
  }

  return true;
}

bool AdvertisingData::operator==(const AdvertisingData& other) const {
  if ((local_name_ != other.local_name_) || (tx_power_ != other.tx_power_) ||
      (appearance_ != other.appearance_) ||
      (service_uuids_ != other.service_uuids_) ||
      (solicitation_uuids_ != other.solicitation_uuids_) ||
      (uris_ != other.uris_) || (flags_ != other.flags_) ||
      (resolvable_set_identifier_ != other.resolvable_set_identifier_) ||
      (broadcast_name_ != other.broadcast_name_)) {
    return false;
  }

  if (manufacturer_data_.size() != other.manufacturer_data_.size()) {
    return false;
  }

  for (const auto& it : manufacturer_data_) {
    auto that = other.manufacturer_data_.find(it.first);
    if (that == other.manufacturer_data_.end()) {
      return false;
    }
    size_t bytes = it.second.size();
    if (bytes != that->second.size()) {
      return false;
    }
    if (std::memcmp(it.second.data(), that->second.data(), bytes) != 0) {
      return false;
    }
  }

  if (service_data_.size() != other.service_data_.size()) {
    return false;
  }

  for (const auto& it : service_data_) {
    auto that = other.service_data_.find(it.first);
    if (that == other.service_data_.end()) {
      return false;
    }
    size_t bytes = it.second.size();
    if (bytes != that->second.size()) {
      return false;
    }
    if (std::memcmp(it.second.data(), that->second.data(), bytes) != 0) {
      return false;
    }
  }

  return true;
}

bool AdvertisingData::operator!=(const AdvertisingData& other) const {
  return !(*this == other);
}

bool AdvertisingData::BoundedUuids::AddUuid(UUID uuid) {
  PW_CHECK(set_.size() <= bound_);
  if (set_.size() < bound_) {
    if (!set_.insert(uuid).second) {
      bt_log(DEBUG,
             "gap-le",
             "Skipping addition of duplicate UUID %s to AD",
             bt_str(uuid));
    }
    return true;
  }
  if (set_.find(uuid) != set_.end()) {
    bt_log(DEBUG,
           "gap-le",
           "Skipping addition of duplicate UUID %s to AD",
           bt_str(uuid));
    return true;
  }
  bt_log(WARN,
         "gap-le",
         "Failed to add UUID %s to AD - no space left",
         bt_str(uuid));
  return false;
}
}  // namespace bt
