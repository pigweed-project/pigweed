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

#include <pw_assert/check.h>
#include <pw_bluetooth/hci_android.emb.h>

#include "pw_allocator/allocator.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"

namespace bt::hci {

namespace android_hci = hci_spec::vendor::android;
namespace android_emb = pw::bluetooth::vendor::android_hci;

CommandPacket::CommandPacket(pw::bluetooth::emboss::OpCode opcode,
                             size_t packet_size,
                             pw::Allocator& allocator)
    : DynamicPacket(packet_size, allocator) {
  PW_CHECK(
      packet_size >=
          pw::bluetooth::emboss::CommandHeader::IntrinsicSizeInBytes(),
      "command packet size must be at least 3 bytes to accommodate header");
  auto header = view<pw::bluetooth::emboss::CommandHeaderWriter>();
  header.opcode().Write(opcode);
  header.parameter_total_size().Write(
      packet_size -
      pw::bluetooth::emboss::CommandHeader::IntrinsicSizeInBytes());
}

CommandPacket::CommandPacket(hci_spec::OpCode opcode,
                             size_t packet_size,
                             pw::Allocator& allocator)
    : DynamicPacket(packet_size, allocator) {
  PW_CHECK(packet_size >=
               pw::bluetooth::emboss::CommandHeader::IntrinsicSizeInBytes(),
           "command packet size must be at least 3 bytes to accomodate header");
  auto header = view<pw::bluetooth::emboss::CommandHeaderWriter>();
  header.opcode_bits().BackingStorage().WriteUInt(opcode);
  header.parameter_total_size().Write(
      packet_size -
      pw::bluetooth::emboss::CommandHeader::IntrinsicSizeInBytes());
}

hci_spec::OpCode CommandPacket::opcode() const {
  return header_view().opcode_bits().BackingStorage().ReadUInt();
}

uint8_t CommandPacket::ogf() const {
  return header_view().opcode_bits().ogf().Read();
}

uint16_t CommandPacket::ocf() const {
  return header_view().opcode_bits().ocf().Read();
}

pw::bluetooth::emboss::CommandHeaderView CommandPacket::header_view() const {
  return view<pw::bluetooth::emboss::CommandHeaderView>();
}

EventPacket::EventPacket(size_t packet_size, pw::Allocator& allocator)
    : DynamicPacket(packet_size, allocator) {
  PW_CHECK(
      packet_size >= pw::bluetooth::emboss::EventHeader::IntrinsicSizeInBytes(),
      "event packet size must be at least 2 bytes to accomodate header");
}

hci_spec::EventCode EventPacket::event_code() const {
  return view<pw::bluetooth::emboss::EventHeaderView>()
      .event_code_uint()
      .Read();
}

std::optional<pw::bluetooth::emboss::StatusCode> EventPacket::StatusCode()
    const {
  switch (event_code()) {
    case hci_spec::kInquiryCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::InquiryCompleteEventView>();
    case hci_spec::kConnectionCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::ConnectionCompleteEventView>();
    case hci_spec::kDisconnectionCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::DisconnectionCompleteEventView>();
    case hci_spec::kAuthenticationCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::AuthenticationCompleteEventView>();
    case hci_spec::kRemoteNameRequestCompleteEventCode: {
      // Tests expect that a kPacketMalformed status is returned for incomplete
      // events, even if they contain the status field.
      pw::bluetooth::emboss::RemoteNameRequestCompleteEventView event_view(
          data().data(), size());
      if (!event_view.IsComplete()) {
        return std::nullopt;
      }
      return event_view.status().UncheckedRead();
    }
    case hci_spec::kEncryptionChangeEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::EncryptionChangeEventV1View>();
    case hci_spec::kChangeConnectionLinkKeyCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::ChangeConnectionLinkKeyCompleteEventView>();
    case hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::
              ReadRemoteSupportedFeaturesCompleteEventView>();
    case hci_spec::kReadRemoteVersionInfoCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::ReadRemoteVersionInfoCompleteEventView>();
    case hci_spec::kCommandCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::SimpleCommandCompleteEventView>();
    case hci_spec::kCommandStatusEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::CommandStatusEventView>();
    case hci_spec::kRoleChangeEventCode:
      return StatusCodeFromView<pw::bluetooth::emboss::RoleChangeEventView>();
    case hci_spec::kModeChangeEventCode:
      return StatusCodeFromView<pw::bluetooth::emboss::ModeChangeEventView>();
    case hci_spec::kReadRemoteExtendedFeaturesCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::ReadRemoteExtendedFeaturesCompleteEventView>();
    case hci_spec::kSynchronousConnectionCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::SynchronousConnectionCompleteEventView>();
    case hci_spec::kEncryptionKeyRefreshCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::EncryptionKeyRefreshCompleteEventView>();
    case hci_spec::kSimplePairingCompleteEventCode:
      return StatusCodeFromView<
          pw::bluetooth::emboss::SimplePairingCompleteEventView>();
    case hci_spec::kLEMetaEventCode: {
      hci_spec::EventCode subevent_code =
          view<pw::bluetooth::emboss::LEMetaEventView>().subevent_code().Read();

      switch (subevent_code) {
        case hci_spec::kLEConnectionCompleteSubeventCode: {
          return StatusCodeFromView<
              pw::bluetooth::emboss::LEConnectionCompleteSubeventView>();
        }
        case hci_spec::kLEConnectionUpdateCompleteSubeventCode: {
          return StatusCodeFromView<
              pw::bluetooth::emboss::LEConnectionUpdateCompleteSubeventView>();
        }
        case hci_spec::kLEReadRemoteFeaturesCompleteSubeventCode: {
          return StatusCodeFromView<
              pw::bluetooth::emboss::
                  LEReadRemoteFeaturesCompleteSubeventView>();
        }
        case hci_spec::kLEEnhancedConnectionCompleteSubeventCode: {
          return StatusCodeFromView<
              pw::bluetooth::emboss::
                  LEEnhancedConnectionCompleteSubeventV1View>();
        }
        case hci_spec::kLEAdvertisingSetTerminatedSubeventCode: {
          return StatusCodeFromView<
              pw::bluetooth::emboss::LEAdvertisingSetTerminatedSubeventView>();
        }
        case hci_spec::kLECISEstablishedSubeventCode: {
          return StatusCodeFromView<
              pw::bluetooth::emboss::LECISEstablishedSubeventView>();
        }
        case hci_spec::kLERequestPeerSCACompleteSubeventCode: {
          return StatusCodeFromView<
              pw::bluetooth::emboss::LERequestPeerSCACompleteSubeventView>();
        }
        default: {
          PW_CRASH("Emboss LE meta subevent (%#.2x) not implemented",
                   subevent_code);
          break;
        }
      }

      break;
    }
    case hci_spec::kVendorDebugEventCode: {
      hci_spec::EventCode subevent_code =
          view<pw::bluetooth::emboss::VendorDebugEventView>()
              .subevent_code()
              .Read();

      switch (subevent_code) {
        case android_hci::kLEMultiAdvtStateChangeSubeventCode: {
          return StatusCodeFromView<
              android_emb::LEMultiAdvtStateChangeSubeventView>();
        }

        default: {
          PW_CRASH("Emboss vendor subevent (%#.2x) not implemented",
                   subevent_code);
          break;
        }
      }

      break;
    }
    default: {
      PW_CRASH("Emboss event (%#.2x) not implemented", event_code());
      break;
    }
  }

  return std::nullopt;
}

hci::Result<> EventPacket::ToResult() const {
  std::optional<pw::bluetooth::emboss::StatusCode> maybe_status_code =
      StatusCode();
  if (!maybe_status_code.has_value()) {
    return bt::ToResult(HostError::kPacketMalformed);
  }
  return bt::ToResult(*maybe_status_code);
}

}  // namespace bt::hci
