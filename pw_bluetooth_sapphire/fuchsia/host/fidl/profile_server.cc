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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/profile_server.h"

#include <pw_assert/check.h>
#include <string.h>
#include <zircon/status.h>
#include <zircon/syscalls/clock.h>

#include <cstddef>
#include <memory>

#include "fuchsia/bluetooth/bredr/cpp/fidl.h"
#include "lib/fidl/cpp/binding.h"
#include "lib/fidl/cpp/interface_ptr.h"
#include "lib/fpromise/result.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"
#include "pw_bluetooth_sapphire/internal/host/common/host_error.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"
#include "zircon/errors.h"

namespace fidlbredr = fuchsia::bluetooth::bredr;
namespace fbt = fuchsia::bluetooth;
namespace android_emb = pw::bluetooth::vendor::android_hci;
using fidlbredr::DataElement;
using fidlbredr::Profile;
using pw::bluetooth::AclPriority;
using FeaturesBits = pw::bluetooth::Controller::FeaturesBits;

namespace bthost {

namespace {

bt::l2cap::ChannelParameters FidlToChannelParameters(
    const fbt::ChannelParameters& fidl) {
  bt::l2cap::ChannelParameters params;
  if (fidl.has_channel_mode()) {
    switch (fidl.channel_mode()) {
      case fbt::ChannelMode::BASIC:
        params.mode = bt::l2cap::RetransmissionAndFlowControlMode::kBasic;
        break;
      case fbt::ChannelMode::ENHANCED_RETRANSMISSION:
        params.mode = bt::l2cap::RetransmissionAndFlowControlMode::
            kEnhancedRetransmission;
        break;
      default:
        PW_CRASH("FIDL channel parameter contains invalid mode");
    }
  }
  if (fidl.has_max_rx_packet_size()) {
    params.max_rx_sdu_size = fidl.max_rx_packet_size();
  }
  if (fidl.has_flush_timeout()) {
    params.flush_timeout = std::chrono::nanoseconds(fidl.flush_timeout());
  }
  return params;
}

fbt::ChannelMode ChannelModeToFidl(const bt::l2cap::AnyChannelMode& mode) {
  if (auto* flow_control_mode =
          std::get_if<bt::l2cap::RetransmissionAndFlowControlMode>(&mode)) {
    switch (*flow_control_mode) {
      case bt::l2cap::RetransmissionAndFlowControlMode::kBasic:
        return fbt::ChannelMode::BASIC;
        break;
      case bt::l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission:
        return fbt::ChannelMode::ENHANCED_RETRANSMISSION;
        break;
      default:
        // Intentionally unhandled, fall through to PANIC.
        break;
    }
  }
  PW_CRASH("Could not convert channel parameter mode to unsupported FIDL mode");
}

fbt::ChannelParameters ChannelInfoToFidlChannelParameters(
    const bt::l2cap::ChannelInfo& info) {
  fbt::ChannelParameters params;
  params.set_channel_mode(ChannelModeToFidl(info.mode));
  params.set_max_rx_packet_size(info.max_rx_sdu_size);
  if (info.flush_timeout) {
    params.set_flush_timeout(info.flush_timeout->count());
  }
  return params;
}

// NOLINTNEXTLINE(misc-no-recursion)
fidlbredr::DataElementPtr DataElementToFidl(const bt::sdp::DataElement* in) {
  auto elem = std::make_unique<fidlbredr::DataElement>();
  bt_log(TRACE, "fidl", "DataElementToFidl: %s", in->ToString().c_str());
  PW_DCHECK(in);
  switch (in->type()) {
    case bt::sdp::DataElement::Type::kUnsignedInt: {
      switch (in->size()) {
        case bt::sdp::DataElement::Size::kOneByte:
          elem->set_uint8(*in->Get<uint8_t>());
          break;
        case bt::sdp::DataElement::Size::kTwoBytes:
          elem->set_uint16(*in->Get<uint16_t>());
          break;
        case bt::sdp::DataElement::Size::kFourBytes:
          elem->set_uint32(*in->Get<uint32_t>());
          break;
        case bt::sdp::DataElement::Size::kEightBytes:
          elem->set_uint64(*in->Get<uint64_t>());
          break;
        default:
          bt_log(INFO, "fidl", "no 128-bit integer support in FIDL yet");
          return nullptr;
      }
      return elem;
    }
    case bt::sdp::DataElement::Type::kSignedInt: {
      switch (in->size()) {
        case bt::sdp::DataElement::Size::kOneByte:
          elem->set_int8(*in->Get<int8_t>());
          break;
        case bt::sdp::DataElement::Size::kTwoBytes:
          elem->set_int16(*in->Get<int16_t>());
          break;
        case bt::sdp::DataElement::Size::kFourBytes:
          elem->set_int32(*in->Get<int32_t>());
          break;
        case bt::sdp::DataElement::Size::kEightBytes:
          elem->set_int64(*in->Get<int64_t>());
          break;
        default:
          bt_log(INFO, "fidl", "no 128-bit integer support in FIDL yet");
          return nullptr;
      }
      return elem;
    }
    case bt::sdp::DataElement::Type::kUuid: {
      auto uuid = in->Get<bt::UUID>();
      PW_DCHECK(uuid);
      elem->set_uuid(fidl_helpers::UuidToFidl(*uuid));
      return elem;
    }
    case bt::sdp::DataElement::Type::kString: {
      auto bytes = in->Get<bt::DynamicByteBuffer>();
      PW_DCHECK(bytes);
      std::vector<uint8_t> data(bytes->cbegin(), bytes->cend());
      elem->set_str(data);
      return elem;
    }
    case bt::sdp::DataElement::Type::kBoolean: {
      elem->set_b(*in->Get<bool>());
      return elem;
    }
    case bt::sdp::DataElement::Type::kSequence: {
      std::vector<fidlbredr::DataElementPtr> elems;
      const bt::sdp::DataElement* it;
      for (size_t idx = 0; (it = in->At(idx)); ++idx) {
        elems.emplace_back(DataElementToFidl(it));
      }
      elem->set_sequence(std::move(elems));
      return elem;
    }
    case bt::sdp::DataElement::Type::kAlternative: {
      std::vector<fidlbredr::DataElementPtr> elems;
      const bt::sdp::DataElement* it;
      for (size_t idx = 0; (it = in->At(idx)); ++idx) {
        elems.emplace_back(DataElementToFidl(it));
      }
      elem->set_alternatives(std::move(elems));
      return elem;
    }
    case bt::sdp::DataElement::Type::kUrl: {
      elem->set_url(*in->GetUrl());
      return elem;
    }
    case bt::sdp::DataElement::Type::kNull: {
      bt_log(INFO, "fidl", "no support for null DataElement types in FIDL");
      return nullptr;
    }
  }
}

fidlbredr::ProtocolDescriptorPtr DataElementToProtocolDescriptor(
    const bt::sdp::DataElement* in) {
  auto desc = std::make_unique<fidlbredr::ProtocolDescriptor>();
  if (in->type() != bt::sdp::DataElement::Type::kSequence) {
    bt_log(DEBUG,
           "fidl",
           "DataElement type is not kSequence (in: %s)",
           bt_str(*in));
    return nullptr;
  }
  const auto protocol_uuid = in->At(0)->Get<bt::UUID>();
  if (!protocol_uuid) {
    bt_log(DEBUG,
           "fidl",
           "first DataElement in sequence is not type kUUID (in: %s)",
           bt_str(*in));
    return nullptr;
  }
  desc->set_protocol(
      static_cast<fidlbredr::ProtocolIdentifier>(*protocol_uuid->As16Bit()));
  const bt::sdp::DataElement* it;
  std::vector<fidlbredr::DataElement> params;
  for (size_t idx = 1; (it = in->At(idx)); ++idx) {
    params.push_back(std::move(*DataElementToFidl(it)));
  }
  desc->set_params(std::move(params));

  return desc;
}

AclPriority FidlToAclPriority(fidlbredr::A2dpDirectionPriority in) {
  switch (in) {
    case fidlbredr::A2dpDirectionPriority::SOURCE:
      return AclPriority::kSource;
    case fidlbredr::A2dpDirectionPriority::SINK:
      return AclPriority::kSink;
    default:
      return AclPriority::kNormal;
  }
}

}  // namespace

ProfileServer::ProfileServer(
    bt::gap::Adapter::WeakPtr adapter,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
    uint8_t sco_offload_index,
    fidl::InterfaceRequest<Profile> request)
    : ServerBase(this, std::move(request)),
      advertised_total_(0),
      searches_total_(0),
      adapter_(std::move(adapter)),
      wake_lease_provider_(wake_lease_provider),
      sco_offload_index_(sco_offload_index),
      weak_self_(this) {}

ProfileServer::~ProfileServer() {
  sco_connection_servers_.clear();

  if (adapter().is_alive()) {
    // Unregister anything that we have registered.
    for (const auto& it : current_advertised_) {
      adapter()->bredr()->UnregisterService(it.second.registration_handle);
    }
    for (const auto& it : searches_) {
      adapter()->bredr()->RemoveServiceSearch(it.second.search_id);
    }
  }
}

void ProfileServer::L2capParametersExt::RequestParameters(
    fuchsia::bluetooth::ChannelParameters requested,
    RequestParametersCallback callback) {
  PW_CHECK(channel_.is_alive());

  if (requested.has_flush_timeout()) {
    channel_->SetBrEdrAutomaticFlushTimeout(
        std::chrono::nanoseconds(requested.flush_timeout()),
        [chan = channel_, cb = std::move(callback)](auto result) {
          if (result.is_ok()) {
            bt_log(DEBUG,
                   "fidl",
                   "L2capParametersExt::RequestParameters: setting flush "
                   "timeout succeeded");
          } else {
            bt_log(INFO,
                   "fidl",
                   "L2capParametersExt::RequestParameters: setting flush "
                   "timeout failed");
          }
          // Return the current parameters even if the request failed.
          // TODO(fxbug.dev/42152567): set current security requirements in
          // returned channel parameters
          if (chan.is_alive()) {
            cb(fidlbredr::L2capParametersExt_RequestParameters_Result::
                   WithResponse(
                       fidlbredr::L2capParametersExt_RequestParameters_Response(
                           ChannelInfoToFidlChannelParameters(chan->info()))));
          }
        });
    return;
  }

  // No other channel parameters are  supported, so just return the current
  // parameters.
  // TODO(fxbug.dev/42152567): set current security requirements in returned
  // channel parameters
  callback(fidlbredr::L2capParametersExt_RequestParameters_Result::WithResponse(
      fidlbredr::L2capParametersExt_RequestParameters_Response(
          ChannelInfoToFidlChannelParameters(channel_->info()))));
}

void ProfileServer::L2capParametersExt::handle_unknown_method(
    uint64_t ordinal, bool method_has_response) {
  bt_log(WARN, "fidl", "L2capParametersExt: unknown method received");
}

void ProfileServer::AudioOffloadExt::GetSupportedFeatures(
    GetSupportedFeaturesCallback callback) {
  fidlbredr::AudioOffloadExt_GetSupportedFeatures_Response response;
  std::vector<fidlbredr::AudioOffloadFeatures>* mutable_audio_offload_features =
      response.mutable_audio_offload_features();
  const bt::gap::AdapterState& adapter_state = adapter_->state();

  if (!adapter_state.IsControllerFeatureSupported(
          FeaturesBits::kAndroidVendorExtensions)) {
    callback(
        fidlbredr::AudioOffloadExt_GetSupportedFeatures_Result::WithResponse(
            std::move(response)));
    return;
  }

  const uint32_t a2dp_offload_capabilities =
      adapter_state.android_vendor_capabilities
          ->a2dp_source_offload_capability_mask();
  const uint32_t sbc_capability =
      static_cast<uint32_t>(android_emb::A2dpCodecType::SBC);
  const uint32_t aac_capability =
      static_cast<uint32_t>(android_emb::A2dpCodecType::AAC);

  if (a2dp_offload_capabilities & sbc_capability) {
    fidlbredr::AudioSbcSupport audio_sbc_support;
    mutable_audio_offload_features->push_back(
        fidlbredr::AudioOffloadFeatures::WithSbc(std::move(audio_sbc_support)));
  }
  if (a2dp_offload_capabilities & aac_capability) {
    fidlbredr::AudioAacSupport audio_aac_support;
    mutable_audio_offload_features->push_back(
        fidlbredr::AudioOffloadFeatures::WithAac(std::move(audio_aac_support)));
  }

  callback(fidlbredr::AudioOffloadExt_GetSupportedFeatures_Result::WithResponse(
      std::move(response)));
}

void ProfileServer::AudioOffloadExt::StartAudioOffload(
    fidlbredr::AudioOffloadConfiguration audio_offload_configuration,
    fidl::InterfaceRequest<fidlbredr::AudioOffloadController> controller) {
  auto audio_offload_controller_server =
      std::make_unique<AudioOffloadController>(std::move(controller), channel_);
  WeakPtr<AudioOffloadController> server_ptr =
      audio_offload_controller_server->GetWeakPtr();

  std::unique_ptr<bt::l2cap::A2dpOffloadManager::Configuration> config =
      AudioOffloadConfigFromFidl(audio_offload_configuration);
  if (!config) {
    bt_log(ERROR, "fidl", "%s: invalid config received", __FUNCTION__);
    server_ptr->Close(/*epitaph_value=*/ZX_ERR_NOT_SUPPORTED);
    return;
  }

  auto error_handler = [this, server_ptr](zx_status_t status) {
    if (!server_ptr.is_alive()) {
      bt_log(ERROR, "fidl", "audio offload controller server was destroyed");
      return;
    }

    bt_log(DEBUG,
           "fidl",
           "audio offload controller server closed (reason: %s)",
           zx_status_get_string(status));
    if (!profile_server_.audio_offload_controller_server_) {
      bt_log(WARN,
             "fidl",
             "could not find controller server in audio offload controller "
             "error callback");
    }

    bt::hci::ResultCallback<> stop_cb =
        [server_ptr](
            fit::result<bt::Error<pw::bluetooth::emboss::StatusCode>> result) {
          if (result.is_error()) {
            bt_log(ERROR,
                   "fidl",
                   "stopping audio offload failed in error handler: %s",
                   bt_str(result));
            server_ptr->Close(/*epitaph_value=*/ZX_ERR_UNAVAILABLE);
            return;
          }
          bt_log(ERROR,
                 "fidl",
                 "stopping audio offload complete: %s",
                 bt_str(result));
        };
    channel_->StopA2dpOffload(std::move(stop_cb));
  };
  audio_offload_controller_server->set_error_handler(error_handler);
  profile_server_.audio_offload_controller_server_ =
      std::move(audio_offload_controller_server);

  auto callback =
      [this, server_ptr](
          fit::result<bt::Error<pw::bluetooth::emboss::StatusCode>> result) {
        if (!server_ptr.is_alive()) {
          bt_log(
              ERROR, "fidl", "audio offload controller server was destroyed");
          return;
        }
        if (result.is_error()) {
          bt_log(ERROR, "fidl", "StartAudioOffload failed: %s", bt_str(result));

          auto host_error = result.error_value().host_error();
          if (host_error == bt::HostError::kInProgress) {
            server_ptr->Close(/*epitaph_value=*/ZX_ERR_ALREADY_BOUND);
          } else if (host_error == bt::HostError::kFailed) {
            server_ptr->Close(/*epitaph_value=*/ZX_ERR_INTERNAL);
          } else {
            server_ptr->Close(/*epitaph_value=*/ZX_ERR_UNAVAILABLE);
          }
          profile_server_.audio_offload_controller_server_ = nullptr;
          return;
        }
        // Send OnStarted event to tell Rust Profiles that we've finished
        // offloading
        server_ptr->SendOnStartedEvent();
      };
  channel_->StartA2dpOffload(*config, std::move(callback));
}

void ProfileServer::AudioOffloadController::Stop(
    AudioOffloadController::StopCallback callback) {
  if (!channel_.is_alive()) {
    bt_log(ERROR, "fidl", "Audio offload controller server was destroyed");
    return;
  }

  channel_->StopA2dpOffload(
      [stop_callback = std::move(callback),
       this](fit::result<bt::Error<pw::bluetooth::emboss::StatusCode>> result) {
        if (result.is_error()) {
          bt_log(
              ERROR,
              "fidl",
              "Stop a2dp offload failed with error %s. Closing with "
              "ZX_ERR_UNAVAILABLE",
              bt::HostErrorToString(result.error_value().host_error()).c_str());
          Close(/*epitaph_value=*/ZX_ERR_UNAVAILABLE);
          return;
        }
        stop_callback(
            fidlbredr::AudioOffloadController_Stop_Result::WithResponse({}));
      });
}

std::unique_ptr<bt::l2cap::A2dpOffloadManager::Configuration>
ProfileServer::AudioOffloadExt::AudioOffloadConfigFromFidl(
    fidlbredr::AudioOffloadConfiguration& audio_offload_configuration) {
  auto codec =
      fidl_helpers::FidlToCodecType(audio_offload_configuration.codec());
  if (!codec.has_value()) {
    bt_log(WARN, "fidl", "%s: invalid codec", __FUNCTION__);
    return nullptr;
  }

  std::unique_ptr<bt::l2cap::A2dpOffloadManager::Configuration> config =
      std::make_unique<bt::l2cap::A2dpOffloadManager::Configuration>();

  std::optional<android_emb::A2dpSamplingFrequency> sampling_frequency =
      fidl_helpers::FidlToSamplingFrequency(
          audio_offload_configuration.sampling_frequency());
  if (!sampling_frequency.has_value()) {
    bt_log(WARN, "fidl", "Invalid sampling frequency");
    return nullptr;
  }

  std::optional<android_emb::A2dpBitsPerSample> audio_bits_per_sample =
      fidl_helpers::FidlToBitsPerSample(
          audio_offload_configuration.bits_per_sample());
  if (!audio_bits_per_sample.has_value()) {
    bt_log(WARN, "fidl", "Invalid audio bits per sample");
    return nullptr;
  }

  std::optional<android_emb::A2dpChannelMode> audio_channel_mode =
      fidl_helpers::FidlToChannelMode(
          audio_offload_configuration.channel_mode());
  if (!audio_channel_mode.has_value()) {
    bt_log(WARN, "fidl", "Invalid channel mode");
    return nullptr;
  }

  config->codec = codec.value();
  config->max_latency = audio_offload_configuration.max_latency();
  config->scms_t_enable = fidl_helpers::FidlToScmsTEnable(
      audio_offload_configuration.scms_t_enable());
  config->sampling_frequency = sampling_frequency.value();
  config->bits_per_sample = audio_bits_per_sample.value();
  config->channel_mode = audio_channel_mode.value();
  config->encoded_audio_bit_rate =
      audio_offload_configuration.encoded_bit_rate();

  if (audio_offload_configuration.encoder_settings().is_sbc()) {
    if (audio_offload_configuration.sampling_frequency() ==
            fuchsia::bluetooth::bredr::AudioSamplingFrequency::HZ_88200 ||
        audio_offload_configuration.sampling_frequency() ==
            fuchsia::bluetooth::bredr::AudioSamplingFrequency::HZ_96000) {
      bt_log(WARN,
             "fidl",
             "%s: sbc encoder cannot use sampling frequency %hhu",
             __FUNCTION__,
             static_cast<uint8_t>(
                 audio_offload_configuration.sampling_frequency()));
      return nullptr;
    }

    config->sbc_configuration = fidl_helpers::FidlToEncoderSettingsSbc(
        audio_offload_configuration.encoder_settings(),
        audio_offload_configuration.sampling_frequency(),
        audio_offload_configuration.channel_mode());
  } else if (audio_offload_configuration.encoder_settings().is_aac()) {
    config->aac_configuration = fidl_helpers::FidlToEncoderSettingsAac(
        audio_offload_configuration.encoder_settings(),
        audio_offload_configuration.sampling_frequency(),
        audio_offload_configuration.channel_mode());
  }

  return config;
}

void ProfileServer::AudioOffloadExt::handle_unknown_method(
    uint64_t ordinal, bool method_has_response) {
  bt_log(WARN, "fidl", "AudioOffloadExt: unknown method received");
}

void ProfileServer::AudioOffloadController::handle_unknown_method(
    uint64_t ordinal, bool method_has_response) {
  bt_log(WARN, "fidl", "AudioOffloadController: unknown method received");
}

ProfileServer::ScoConnectionServer::ScoConnectionServer(
    fidl::InterfaceRequest<fuchsia::bluetooth::bredr::ScoConnection> request,
    ProfileServer* profile_server)
    : ServerBase(this, std::move(request)),
      profile_server_(profile_server),
      weak_self_(this) {
  binding()->set_error_handler([this](zx_status_t) { Close(ZX_ERR_CANCELED); });
}

ProfileServer::ScoConnectionServer::~ScoConnectionServer() {
  if (connection_.is_alive()) {
    connection_->Deactivate();
  }
}

void ProfileServer::ScoConnectionServer::Activate() {
  auto rx_callback = [this] { TryRead(); };
  auto closed_cb = [this] { Close(ZX_ERR_PEER_CLOSED); };
  bool activated =
      connection_->Activate(std::move(rx_callback), std::move(closed_cb));
  if (!activated) {
    OnError(fidlbredr::ScoErrorCode::FAILURE);
  }
}

void ProfileServer::ScoConnectionServer::OnError(
    ::fuchsia::bluetooth::bredr::ScoErrorCode error) {
  OnConnectionComplete(
      ::fuchsia::bluetooth::bredr::ScoConnectionOnConnectionCompleteRequest::
          WithError(std::move(error)));  // NOLINT(performance-move-const-arg)
  Close(ZX_ERR_PEER_CLOSED);
}

void ProfileServer::ScoConnectionServer::Read(ReadCallback callback) {
  if (!connection_.is_alive()) {
    Close(ZX_ERR_IO_REFUSED);
    return;
  }

  if (connection_->parameters().view().input_data_path().Read() !=
      pw::bluetooth::emboss::ScoDataPath::HCI) {
    bt_log(WARN, "fidl", "%s called for an offloaded SCO connection", __func__);
    Close(ZX_ERR_IO_NOT_PRESENT);
    return;
  }

  if (read_cb_) {
    bt_log(WARN,
           "fidl",
           "%s called when a read callback was already present",
           __func__);
    Close(ZX_ERR_BAD_STATE);
    return;
  }
  read_cb_ = std::move(callback);
  TryRead();
}

void ProfileServer::ScoConnectionServer::Write(
    fidlbredr::ScoConnectionWriteRequest request, WriteCallback callback) {
  if (!connection_.is_alive()) {
    Close(ZX_ERR_IO_REFUSED);
    return;
  }

  if (connection_->parameters().view().output_data_path().Read() !=
      pw::bluetooth::emboss::ScoDataPath::HCI) {
    bt_log(WARN, "fidl", "%s called for a non-HCI SCO connection", __func__);
    Close(ZX_ERR_IO_NOT_PRESENT);
    return;
  }

  if (!request.has_data()) {
    Close(ZX_ERR_INVALID_ARGS);
    return;
  }
  std::vector<uint8_t> data = std::move(*request.mutable_data());

  auto buffer = std::make_unique<bt::DynamicByteBuffer>(data.size());
  buffer->Write(data.data(), data.size());
  if (!connection_->Send(std::move(buffer))) {
    bt_log(WARN, "fidl", "%s: failed to send SCO packet", __func__);
    Close(ZX_ERR_IO);
    return;
  }
  callback(fidlbredr::ScoConnection_Write_Result::WithResponse(
      fidlbredr::ScoConnection_Write_Response()));
}

void ProfileServer::ScoConnectionServer::handle_unknown_method(
    uint64_t ordinal, bool method_has_response) {
  bt_log(WARN,
         "fidl",
         "ScoConnectionServer received unknown method with ordinal %lu",
         ordinal);
}

void ProfileServer::ScoConnectionServer::TryRead() {
  if (!read_cb_) {
    return;
  }
  std::unique_ptr<bt::hci::ScoDataPacket> packet = connection_->Read();
  if (!packet) {
    return;
  }
  std::vector<uint8_t> payload;
  fidlbredr::RxPacketStatus status =
      fidl_helpers::ScoPacketStatusToFidl(packet->packet_status_flag());
  if (packet->packet_status_flag() !=
      bt::hci_spec::SynchronousDataPacketStatusFlag::kNoDataReceived) {
    payload = packet->view().payload_data().ToVector();
  }
  fidlbredr::ScoConnection_Read_Response response;
  response.set_data(std::move(payload));
  response.set_status_flag(status);
  read_cb_(
      fidlbredr::ScoConnection_Read_Result::WithResponse(std::move(response)));
}

void ProfileServer::ScoConnectionServer::Close(zx_status_t epitaph) {
  if (connection_.is_alive()) {
    connection_->Deactivate();
  }
  binding()->Close(epitaph);
  profile_server_->sco_connection_servers_.erase(this);
}

void ProfileServer::Advertise(
    fuchsia::bluetooth::bredr::ProfileAdvertiseRequest request,
    AdvertiseCallback callback) {
  const bool has_receiver =
      request.has_receiver() || request.has_connection_receiver();
  if (!request.has_services() || !has_receiver) {
    callback(fidlbredr::Profile_Advertise_Result::WithErr(
        fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS));
    return;
  }
  if (!request.has_parameters()) {
    request.set_parameters(fbt::ChannelParameters());
  }
  std::vector<bt::sdp::ServiceRecord> registering;

  for (auto& definition : request.services()) {
    auto rec = fidl_helpers::ServiceDefinitionToServiceRecord(definition);
    // Drop the receiver on error.
    if (rec.is_error()) {
      bt_log(WARN,
             "fidl",
             "%s: Failed to create service record from service defintion",
             __FUNCTION__);
      callback(
          fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS));
      return;
    }
    registering.emplace_back(std::move(rec.value()));
  }

  PW_CHECK(adapter().is_alive());
  PW_CHECK(adapter()->bredr());

  uint64_t next = advertised_total_ + 1;

  auto registration_handle = adapter()->bredr()->RegisterService(
      std::move(registering),
      FidlToChannelParameters(request.parameters()),
      [this, next](auto channel, const auto& protocol_list) {
        OnChannelConnected(next, std::move(channel), std::move(protocol_list));
      });

  if (!registration_handle) {
    bt_log(WARN, "fidl", "%s: Failed to register service", __FUNCTION__);
    callback(fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS));
    return;
  };

  const auto registered_records =
      adapter()->bredr()->GetRegisteredServices(registration_handle);
  std::vector<fuchsia::bluetooth::bredr::ServiceDefinition>
      registered_definitions;
  for (auto& record : registered_records) {
    auto def = fidl_helpers::ServiceRecordToServiceDefinition(record);
    // Shouldn't fail in practice; the records are all well-formed and validated
    // earlier in this function.
    if (def.is_error()) {
      bt_log(WARN,
             "fidl",
             "Failed to construct service definition from record: %lu",
             def.error());
      continue;
    }
    registered_definitions.emplace_back(std::move(def.value()));
  }

  std::optional<ConnectionReceiverVariant> receiver_var;
  if (request.has_receiver()) {
    receiver_var = request.mutable_receiver()->Bind();
  } else if (request.has_connection_receiver()) {
    receiver_var = request.mutable_connection_receiver()->Bind();
  } else {
    // This is checked above, so it should never happen.
    ZX_PANIC("Missing ConnectionReceiver parameter");
  }
  std::visit(
      [this, next](auto&& receiver) {
        // Monitor events on the `ConnectionReceiver`. Remove the service if the
        // FIDL client revokes the service registration.
        receiver.events().OnRevoke = [this, ad_id = next]() {
          bt_log(
              DEBUG,
              "fidl",
              "Connection receiver revoked. Ending service advertisement %lu",
              ad_id);
          OnConnectionReceiverClosed(ad_id);
        };
        // Errors on the `ConnectionReceiver` will result in service
        // unregistration.
        receiver.set_error_handler([this, ad_id = next](zx_status_t status) {
          bt_log(DEBUG,
                 "fidl",
                 "Connection receiver closed with error: %s. Ending service "
                 "advertisement %lu",
                 zx_status_get_string(status),
                 ad_id);
          OnConnectionReceiverClosed(ad_id);
        });
      },
      *receiver_var);

  current_advertised_.try_emplace(
      next, std::move(*receiver_var), registration_handle);
  advertised_total_ = next;
  fuchsia::bluetooth::bredr::Profile_Advertise_Response result;
  result.set_services(std::move(registered_definitions));
  callback(fuchsia::bluetooth::bredr::Profile_Advertise_Result::WithResponse(
      std::move(result)));
}

void ProfileServer::Search(
    ::fuchsia::bluetooth::bredr::ProfileSearchRequest request) {
  if (!request.has_results()) {
    bt_log(WARN, "fidl", "%s: missing search results client", __FUNCTION__);
    return;
  }

  bt::UUID search_uuid;
  if (request.has_full_uuid() && request.has_service_uuid()) {
    bt_log(WARN,
           "fidl",
           "%s: Cannot request both full and service UUID",
           __FUNCTION__);
    return;
  } else if (request.has_service_uuid()) {
    search_uuid = bt::UUID(static_cast<uint32_t>(request.service_uuid()));
  } else if (request.has_full_uuid()) {
    search_uuid = fidl_helpers::UuidFromFidl(request.full_uuid());
  } else {
    bt_log(WARN, "fidl", "%s: missing service or full UUID", __FUNCTION__);
    return;
  }

  std::unordered_set<bt::sdp::AttributeId> attributes;
  if (request.has_attr_ids() && !request.attr_ids().empty()) {
    attributes.insert(request.attr_ids().begin(), request.attr_ids().end());
    // Always request the ProfileDescriptor for the event
    attributes.insert(bt::sdp::kBluetoothProfileDescriptorList);
  }

  PW_DCHECK(adapter().is_alive());

  auto next = searches_total_ + 1;

  auto search_id = adapter()->bredr()->AddServiceSearch(
      search_uuid,
      std::move(attributes),
      [this, next](auto id, const auto& attrs) {
        OnServiceFound(next, id, attrs);
      });

  if (!search_id) {
    return;
  }

  auto results_ptr = request.mutable_results()->Bind();
  results_ptr.set_error_handler(
      [this, next](zx_status_t status) { OnSearchResultError(next, status); });

  searches_.try_emplace(next, std::move(results_ptr), search_id);
  searches_total_ = next;
}

void ProfileServer::Connect(fuchsia::bluetooth::PeerId peer_id,
                            fidlbredr::ConnectParameters connection,
                            ConnectCallback callback) {
  bt::PeerId id{peer_id.value};

  // Anything other than L2CAP is not supported by this server.
  if (!connection.is_l2cap()) {
    bt_log(
        WARN,
        "fidl",
        "%s: non-l2cap connections are not supported (is_rfcomm: %d, peer: %s)",
        __FUNCTION__,
        connection.is_rfcomm(),
        bt_str(id));
    callback(fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS));
    return;
  }

  // The L2CAP parameters must include a PSM. ChannelParameters are optional.
  auto l2cap_params = std::move(connection.l2cap());
  if (!l2cap_params.has_psm()) {
    bt_log(WARN,
           "fidl",
           "%s: missing l2cap psm (peer: %s)",
           __FUNCTION__,
           bt_str(id));
    callback(fpromise::error(fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS));
    return;
  }
  uint16_t psm = l2cap_params.psm();

  fbt::ChannelParameters parameters =
      std::move(*l2cap_params.mutable_parameters());

  auto connected_cb = [self = weak_self_.GetWeakPtr(),
                       cb = callback.share(),
                       id](bt::l2cap::Channel::WeakPtr chan) {
    if (!chan.is_alive()) {
      bt_log(INFO,
             "fidl",
             "Connect: Channel socket is empty, returning failed. (peer: %s)",
             bt_str(id));
      cb(fpromise::error(fuchsia::bluetooth::ErrorCode::FAILED));
      return;
    }

    if (!self.is_alive()) {
      cb(fpromise::error(fuchsia::bluetooth::ErrorCode::FAILED));
      return;
    }

    std::optional<fuchsia::bluetooth::bredr::Channel> fidl_chan =
        self->ChannelToFidl(std::move(chan));
    if (!fidl_chan) {
      cb(fpromise::error(fuchsia::bluetooth::ErrorCode::FAILED));
      return;
    }

    cb(fpromise::ok(std::move(fidl_chan.value())));
  };
  PW_DCHECK(adapter().is_alive());

  adapter()->bredr()->OpenL2capChannel(
      id,
      psm,
      fidl_helpers::FidlToBrEdrSecurityRequirements(parameters),
      FidlToChannelParameters(parameters),
      std::move(connected_cb));
}

void ProfileServer::ConnectSco(
    ::fuchsia::bluetooth::bredr::ProfileConnectScoRequest request) {
  if (!request.has_connection()) {
    bt_log(WARN, "fidl", "%s missing connection", __FUNCTION__);
    return;
  }
  std::unique_ptr<ScoConnectionServer> connection =
      std::make_unique<ScoConnectionServer>(
          std::move(*request.mutable_connection()), this);
  ScoConnectionServer* connection_raw = connection.get();
  WeakPtr<ScoConnectionServer> connection_weak = connection->GetWeakPtr();

  if (!request.has_peer_id() || !request.has_initiator() ||
      !request.has_params() || request.params().empty()) {
    connection->OnError(fidlbredr::ScoErrorCode::INVALID_ARGUMENTS);
    return;
  }
  bt::PeerId peer_id(request.peer_id().value);

  if (request.initiator() && request.params().size() != 1u) {
    bt_log(WARN,
           "fidl",
           "%s: too many parameters in initiator request (peer: %s)",
           __FUNCTION__,
           bt_str(peer_id));
    connection->OnError(fidlbredr::ScoErrorCode::INVALID_ARGUMENTS);
    return;
  }

  auto params_result = fidl_helpers::FidlToScoParametersVector(
      request.params(), sco_offload_index_);
  if (params_result.is_error()) {
    bt_log(WARN,
           "fidl",
           "%s: invalid parameters (peer: %s)",
           __FUNCTION__,
           bt_str(peer_id));
    connection->OnError(fidlbredr::ScoErrorCode::INVALID_ARGUMENTS);
    return;
  }
  connection->set_parameters(std::move(*request.mutable_params()));
  auto params = params_result.value();

  sco_connection_servers_.emplace(connection_raw, std::move(connection));

  if (request.initiator()) {
    auto callback = [self = weak_self_.GetWeakPtr(), connection_weak](
                        bt::sco::ScoConnectionManager::OpenConnectionResult
                            result) mutable {
      // The connection may complete after this server is destroyed.
      if (!self.is_alive()) {
        // Prevent leaking connections.
        if (result.is_ok()) {
          result.value()->Deactivate();
        }
        return;
      }
      if (result.is_error()) {
        self->OnScoConnectionResult(connection_weak, result.take_error());
        return;
      }
      self->OnScoConnectionResult(
          connection_weak,
          fit::ok(std::make_pair(std::move(result.value()),
                                 /*parameter index=*/0u)));
    };
    // If the BR/EDR connection doesn't exist, no handle will be returned and
    // the callback will be synchronously called with an error.
    std::optional<bt::gap::Adapter::BrEdr::ScoRequestHandle> handle =
        adapter()->bredr()->OpenScoConnection(
            peer_id, params.front(), std::move(callback));
    if (handle && connection_weak.is_alive()) {
      connection_weak->set_request_handle(std::move(*handle));
    }
    return;
  }

  auto callback = [self = weak_self_.GetWeakPtr(), connection_weak](
                      bt::sco::ScoConnectionManager::AcceptConnectionResult
                          result) mutable {
    // The connection may complete after this server is destroyed.
    if (!self.is_alive()) {
      // Prevent leaking connections.
      if (result.is_ok()) {
        result.value().first->Deactivate();
      }
      return;
    }

    self->OnScoConnectionResult(connection_weak, std::move(result));
  };
  // If the BR/EDR connection doesn't exist, no handle will be returned and the
  // callback will be synchronously called with an error.
  std::optional<bt::gap::Adapter::BrEdr::ScoRequestHandle> handle =
      adapter()->bredr()->AcceptScoConnection(
          peer_id, params, std::move(callback));
  if (handle && connection_weak.is_alive()) {
    connection_weak->set_request_handle(std::move(*handle));
  }
}

void ProfileServer::handle_unknown_method(uint64_t ordinal,
                                          bool method_has_response) {
  bt_log(WARN, "fidl", "ProfileServer: unknown method received");
}

void ProfileServer::OnChannelConnected(
    uint64_t ad_id,
    bt::l2cap::Channel::WeakPtr channel,
    const bt::sdp::DataElement& protocol_list) {
  auto it = current_advertised_.find(ad_id);
  if (it == current_advertised_.end()) {
    // The receiver has disappeared, do nothing.
    return;
  }

  PW_DCHECK(adapter().is_alive());
  auto handle = channel->link_handle();
  auto id = adapter()->bredr()->GetPeerId(handle);

  // The protocol that is connected should be L2CAP, because that is the only
  // thing that we can connect. We can't say anything about what the higher
  // level protocols will be.
  auto prot_seq = protocol_list.At(0);
  PW_CHECK(prot_seq);

  fidlbredr::ProtocolDescriptorPtr desc =
      DataElementToProtocolDescriptor(prot_seq);
  PW_CHECK(desc);

  fuchsia::bluetooth::PeerId peer_id{id.value()};

  std::vector<fidlbredr::ProtocolDescriptor> list;
  list.emplace_back(std::move(*desc));

  std::optional<fuchsia::bluetooth::bredr::Channel> fidl_chan =
      ChannelToFidl(std::move(channel));
  if (!fidl_chan) {
    bt_log(INFO, "fidl", "ChannelToFidl failed. Ignoring channel.");
    return;
  }

  ConnectionReceiverVariant& receiver_var = it->second.receiver;
  if (auto* receiver =
          std::get_if<fidl::InterfacePtr<fidlbredr::ConnectionReceiver>>(
              &receiver_var)) {
    (*receiver)->Connected(
        peer_id, std::move(fidl_chan.value()), std::move(list));
  } else if (auto* receiver = std::get_if<
                 fidl::InterfacePtr<fidlbredr::ConnectionReceiver2>>(
                 &receiver_var)) {
    fidlbredr::ConnectionReceiver2ConnectedRequest request;
    request.set_peer_id(peer_id);
    request.set_channel(std::move(fidl_chan.value()));
    request.set_protocol(std::move(list));
    // Capture a wake lease until a response is received.
    pw::bluetooth_sapphire::Lease wake_lease =
        PW_SAPPHIRE_ACQUIRE_LEASE(wake_lease_provider_,
                                  "ConnectionReceiver2.Connected")
            .value_or(pw::bluetooth_sapphire::Lease());
    (*receiver)->Connected(
        std::move(request),
        [lease = std::move(wake_lease)](
            fidlbredr::ConnectionReceiver2_Connected_Result result) {
          if (result.is_framework_err()) {
            bt_log(WARN,
                   "fidl",
                   "ConnectionReceiver2.Connected error: %d",
                   fidl::ToUnderlying(result.framework_err()));
          }
        });
  }
}

void ProfileServer::OnConnectionReceiverClosed(uint64_t ad_id) {
  auto it = current_advertised_.find(ad_id);
  if (it == current_advertised_.end() || !adapter().is_alive()) {
    return;
  }

  adapter()->bredr()->UnregisterService(it->second.registration_handle);

  current_advertised_.erase(it);
}

void ProfileServer::OnSearchResultError(uint64_t search_id,
                                        zx_status_t status) {
  bt_log(DEBUG,
         "fidl",
         "Search result closed, ending search %lu reason %s",
         search_id,
         zx_status_get_string(status));

  auto it = searches_.find(search_id);

  if (it == searches_.end() || !adapter().is_alive()) {
    return;
  }

  adapter()->bredr()->RemoveServiceSearch(it->second.search_id);

  searches_.erase(it);
}

void ProfileServer::OnServiceFound(
    uint64_t search_id,
    bt::PeerId peer_id,
    const std::map<bt::sdp::AttributeId, bt::sdp::DataElement>& attributes) {
  auto search_it = searches_.find(search_id);
  if (search_it == searches_.end()) {
    // Search was de-registered.
    return;
  }

  // Convert ProfileDescriptor Attribute
  auto it = attributes.find(bt::sdp::kProtocolDescriptorList);

  fidl::VectorPtr<fidlbredr::ProtocolDescriptor> descriptor_list;

  if (it != attributes.end()) {
    std::vector<fidlbredr::ProtocolDescriptor> list;
    size_t idx = 0;
    auto* sdp_list_element = it->second.At(idx);
    while (sdp_list_element != nullptr) {
      fidlbredr::ProtocolDescriptorPtr desc =
          DataElementToProtocolDescriptor(sdp_list_element);
      if (!desc) {
        break;
      }
      list.push_back(std::move(*desc));
      sdp_list_element = it->second.At(++idx);
    }
    descriptor_list = std::move(list);
  }

  // Add the rest of the attributes
  std::vector<fidlbredr::Attribute> fidl_attrs;

  for (const auto& it : attributes) {
    auto attr = std::make_unique<fidlbredr::Attribute>();
    attr->set_id(it.first);
    attr->set_element(std::move(*DataElementToFidl(&it.second)));
    fidl_attrs.emplace_back(std::move(*attr));
  }

  if (search_it->second.unacknowledged_search_results_count >=
          kMaxUnackedSearchResults ||
      !search_it->second.pending_search_results.empty()) {
    bt_log(ERROR,
           "fidl",
           "Unacknowledged search results limit exceeded for peer %s.",
           bt_str(peer_id));
    search_it->second.pending_search_results.push_back(PendingSearchResult(
        {peer_id.value()}, std::move(descriptor_list), std::move(fidl_attrs)));
    return;
  }

  SendServiceFound(search_it->second,
                   PendingSearchResult({peer_id.value()},
                                       std::move(descriptor_list),
                                       std::move(fidl_attrs)));
}

void ProfileServer::SendServiceFound(RegisteredSearch& search,
                                     PendingSearchResult pending_result) {
  search.unacknowledged_search_results_count++;
  if (!search.wake_lease) {
    search.wake_lease = PW_SAPPHIRE_ACQUIRE_LEASE(wake_lease_provider_,
                                                  "SearchResults.ServiceFound")
                            .value_or(pw::bluetooth_sapphire::Lease());
  }

  auto response_cb = [search_id = search.search_id, this](auto) {
    OnServiceFoundComplete(search_id);
  };

  search.results->ServiceFound(pending_result.peer_id,
                               std::move(pending_result.descriptor_list),
                               std::move(pending_result.attributes),
                               std::move(response_cb));
}

void ProfileServer::OnServiceFoundComplete(
    bt::gap::BrEdrConnectionManager::SearchId search_id) {
  auto search_it = searches_.find(search_id);
  if (search_it == searches_.end()) {
    // Search was de-registered.
    return;
  }
  auto& search = search_it->second;

  search.unacknowledged_search_results_count--;
  while (!search.pending_search_results.empty() &&
         search.unacknowledged_search_results_count <
             kMaxUnackedSearchResults) {
    auto pending_result = std::move(search.pending_search_results.front());
    search.pending_search_results.pop_front();
    SendServiceFound(search, std::move(pending_result));
  }

  if (search.unacknowledged_search_results_count == 0) {
    search.wake_lease.reset();
  }
}

void ProfileServer::OnScoConnectionResult(
    WeakPtr<ScoConnectionServer>& server,
    bt::sco::ScoConnectionManager::AcceptConnectionResult result) {
  if (result.is_error()) {
    if (!server.is_alive()) {
      return;
    }

    bt_log(INFO,
           "fidl",
           "%s: SCO connection failed (status: %s)",
           __FUNCTION__,
           bt::HostErrorToString(result.error_value()).c_str());

    fidlbredr::ScoErrorCode fidl_error = fidlbredr::ScoErrorCode::FAILURE;
    if (result.error_value() == bt::HostError::kCanceled) {
      fidl_error = fidlbredr::ScoErrorCode::CANCELLED;
    }
    if (result.error_value() == bt::HostError::kParametersRejected) {
      fidl_error = fidlbredr::ScoErrorCode::PARAMETERS_REJECTED;
    }
    server->OnError(fidl_error);
    return;
  }

  bt::sco::ScoConnection::WeakPtr connection = std::move(result.value().first);
  const uint16_t max_tx_data_size = connection->max_tx_sdu_size();

  if (!server.is_alive()) {
    connection->Deactivate();
    return;
  }
  server->set_connection(std::move(connection));

  server->Activate();
  if (!server.is_alive()) {
    return;
  }

  size_t parameter_index = result.value().second;
  PW_CHECK(parameter_index < server->parameters().size(),
           "parameter_index (%zu)  >= request->parameters.size() (%zu)",
           parameter_index,
           server->parameters().size());
  fidlbredr::ScoConnectionParameters parameters =
      fidl::Clone(server->parameters()[parameter_index]);
  parameters.set_max_tx_data_size(max_tx_data_size);
  server->OnConnectedParams(std::move(parameters));
}

void ProfileServer::OnAudioDirectionExtError(AudioDirectionExt* ext_server,
                                             zx_status_t status) {
  bt_log(DEBUG,
         "fidl",
         "audio direction ext server closed (reason: %s)",
         zx_status_get_string(status));
  auto handle = audio_direction_ext_servers_.extract(ext_server->unique_id());
  if (handle.empty()) {
    bt_log(WARN,
           "fidl",
           "could not find ext server in audio direction ext error callback");
  }
}

fidl::InterfaceHandle<fidlbredr::AudioDirectionExt>
ProfileServer::BindAudioDirectionExtServer(
    bt::l2cap::Channel::WeakPtr channel) {
  fidl::InterfaceHandle<fidlbredr::AudioDirectionExt> client;

  bt::l2cap::Channel::UniqueId unique_id = channel->unique_id();

  auto audio_direction_ext_server = std::make_unique<AudioDirectionExt>(
      client.NewRequest(), std::move(channel));
  AudioDirectionExt* server_ptr = audio_direction_ext_server.get();

  audio_direction_ext_server->set_error_handler(
      [this, server_ptr](zx_status_t status) {
        OnAudioDirectionExtError(server_ptr, status);
      });

  audio_direction_ext_servers_[unique_id] =
      std::move(audio_direction_ext_server);

  return client;
}

void ProfileServer::OnL2capParametersExtError(L2capParametersExt* ext_server,
                                              zx_status_t status) {
  bt_log(DEBUG,
         "fidl",
         "fidl parameters ext server closed (reason: %s)",
         zx_status_get_string(status));
  auto handle = l2cap_parameters_ext_servers_.extract(ext_server->unique_id());
  if (handle.empty()) {
    bt_log(WARN,
           "fidl",
           "could not find ext server in l2cap parameters ext error callback");
  }
}

fidl::InterfaceHandle<fidlbredr::L2capParametersExt>
ProfileServer::BindL2capParametersExtServer(
    bt::l2cap::Channel::WeakPtr channel) {
  fidl::InterfaceHandle<fidlbredr::L2capParametersExt> client;

  bt::l2cap::Channel::UniqueId unique_id = channel->unique_id();

  auto l2cap_parameters_ext_server = std::make_unique<L2capParametersExt>(
      client.NewRequest(), std::move(channel));
  L2capParametersExt* server_ptr = l2cap_parameters_ext_server.get();

  l2cap_parameters_ext_server->set_error_handler(
      [this, server_ptr](zx_status_t status) {
        OnL2capParametersExtError(server_ptr, status);
      });

  l2cap_parameters_ext_servers_[unique_id] =
      std::move(l2cap_parameters_ext_server);
  return client;
}

void ProfileServer::OnAudioOffloadExtError(AudioOffloadExt* ext_server,
                                           zx_status_t status) {
  bt_log(DEBUG,
         "fidl",
         "audio offload ext server closed (reason: %s)",
         zx_status_get_string(status));
  auto handle = audio_offload_ext_servers_.extract(ext_server->unique_id());
  if (handle.empty()) {
    bt_log(WARN,
           "fidl",
           "could not find ext server in audio offload ext error callback");
  }
}

fidl::InterfaceHandle<fidlbredr::AudioOffloadExt>
ProfileServer::BindAudioOffloadExtServer(bt::l2cap::Channel::WeakPtr channel) {
  fidl::InterfaceHandle<fidlbredr::AudioOffloadExt> client;

  bt::l2cap::Channel::UniqueId unique_id = channel->unique_id();

  std::unique_ptr<bthost::ProfileServer::AudioOffloadExt>
      audio_offload_ext_server = std::make_unique<AudioOffloadExt>(
          *this, client.NewRequest(), std::move(channel), adapter_);
  AudioOffloadExt* server_ptr = audio_offload_ext_server.get();

  audio_offload_ext_server->set_error_handler(
      [this, server_ptr](zx_status_t status) {
        OnAudioOffloadExtError(server_ptr, status);
      });

  audio_offload_ext_servers_[unique_id] = std::move(audio_offload_ext_server);

  return client;
}

std::optional<fidl::InterfaceHandle<fuchsia::bluetooth::Channel>>
ProfileServer::BindChannelServer(bt::l2cap::Channel::WeakPtr channel,
                                 fit::callback<void()> closed_callback) {
  fidl::InterfaceHandle<fbt::Channel> client;

  bt::l2cap::Channel::UniqueId unique_id = channel->unique_id();

  std::unique_ptr<bthost::ChannelServer> connection_server =
      ChannelServer::Create(client.NewRequest(),
                            std::move(channel),
                            wake_lease_provider_,
                            std::move(closed_callback));
  if (!connection_server) {
    return std::nullopt;
  }

  channel_servers_[unique_id] = std::move(connection_server);
  return client;
}

std::optional<fuchsia::bluetooth::bredr::Channel> ProfileServer::ChannelToFidl(
    bt::l2cap::Channel::WeakPtr channel) {
  PW_CHECK(channel.is_alive());
  fidlbredr::Channel fidl_chan;
  fidl_chan.set_channel_mode(ChannelModeToFidl(channel->mode()));
  fidl_chan.set_max_tx_sdu_size(channel->max_tx_sdu_size());
  if (channel->info().flush_timeout) {
    fidl_chan.set_flush_timeout(channel->info().flush_timeout->count());
  }

  auto closed_cb = [this, unique_id = channel->unique_id()]() {
    bt_log(DEBUG,
           "fidl",
           "Channel closed_cb called, destroying servers (unique_id: %d)",
           unique_id);
    channel_servers_.erase(unique_id);
    l2cap_parameters_ext_servers_.erase(unique_id);
    audio_direction_ext_servers_.erase(unique_id);
    audio_offload_ext_servers_.erase(unique_id);
    audio_offload_controller_server_ = nullptr;
  };
  if (use_sockets_) {
    auto sock = l2cap_socket_factory_.MakeSocketForChannel(
        channel, std::move(closed_cb));
    fidl_chan.set_socket(std::move(sock));
  } else {
    std::optional<fidl::InterfaceHandle<fuchsia::bluetooth::Channel>>
        connection = BindChannelServer(channel, std::move(closed_cb));
    if (!connection) {
      return std::nullopt;
    }
    fidl_chan.set_connection(std::move(connection.value()));
  }

  if (adapter()->state().IsControllerFeatureSupported(
          FeaturesBits::kSetAclPriorityCommand)) {
    fidl_chan.set_ext_direction(BindAudioDirectionExtServer(channel));
  }

  if (adapter()->state().IsControllerFeatureSupported(
          FeaturesBits::kAndroidVendorExtensions) &&
      adapter()
          ->state()
          .android_vendor_capabilities->a2dp_source_offload_capability_mask()) {
    fidl_chan.set_ext_audio_offload(BindAudioOffloadExtServer(channel));
  }

  fidl_chan.set_ext_l2cap(BindL2capParametersExtServer(channel));

  return fidl_chan;
}

void ProfileServer::AudioDirectionExt::SetPriority(
    fuchsia::bluetooth::bredr::A2dpDirectionPriority priority,
    SetPriorityCallback callback) {
  channel_->RequestAclPriority(
      FidlToAclPriority(priority), [cb = std::move(callback)](auto result) {
        if (result.is_ok()) {
          cb(fpromise::ok());
          return;
        }
        bt_log(DEBUG, "fidl", "ACL priority request failed");
        cb(fpromise::error(fuchsia::bluetooth::ErrorCode::FAILED));
      });
}

void ProfileServer::AudioDirectionExt::handle_unknown_method(
    uint64_t ordinal, bool method_has_response) {
  bt_log(WARN, "fidl", "AudioDirectionExt: unknown method received");
}

}  // namespace bthost
