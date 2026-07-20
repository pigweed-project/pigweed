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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/low_energy_connection_server.h"

#include <fuchsia/bluetooth/cpp/fidl.h>
#include <pw_assert/check.h>
#include <pw_status/status.h>
#include <pw_status/try.h>

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"

namespace fbt = fuchsia::bluetooth;
namespace fbg = fuchsia::bluetooth::gatt2;

namespace bthost {
namespace {

pw::Result<bt::l2cap::CreditBasedFlowControlMode> ConvertModeFromFidl(
    fbt::ChannelMode mode) {
  switch (mode) {
    case fbt::ChannelMode::LE_CREDIT_BASED_FLOW_CONTROL:
      return bt::l2cap::CreditBasedFlowControlMode::kLeCreditBasedFlowControl;
    case fbt::ChannelMode::ENHANCED_CREDIT_BASED_FLOW_CONTROL:
      return bt::l2cap::CreditBasedFlowControlMode::
          kEnhancedCreditBasedFlowControl;
    default:
      return pw::Status::Unimplemented();
  }
}

pw::Result<bt::l2cap::ChannelParameters> ConvertParamsFromFidl(
    const fbt::ChannelParameters& fidl) {
  bt::l2cap::ChannelParameters params;

  if (fidl.has_flush_timeout()) {
    // Request included parameters that must not be set for an LE L2CAP channel.
    return pw::Status::InvalidArgument();
  }

  if (fidl.has_channel_mode()) {
    PW_TRY_ASSIGN(params.mode, ConvertModeFromFidl(fidl.channel_mode()));
  } else {
    params.mode =
        bt::l2cap::CreditBasedFlowControlMode::kLeCreditBasedFlowControl;
  }

  if (fidl.has_max_rx_packet_size()) {
    params.max_rx_sdu_size = fidl.max_rx_packet_size();
  }

  return params;
}

bt::sm::SecurityLevel ConvertSecurityRequirementsFromFidl(
    const fbt::ChannelParameters& fidl) {
  bt::sm::SecurityLevel security_level = bt::sm::SecurityLevel::kEncrypted;

  if (!fidl.has_security_requirements()) {
    return security_level;
  }

  auto& security_reqs = fidl.security_requirements();
  if (security_reqs.has_authentication_required() &&
      security_reqs.authentication_required()) {
    security_level = bt::sm::SecurityLevel::kAuthenticated;
  }
  if (security_reqs.has_secure_connections_required() &&
      security_reqs.secure_connections_required()) {
    security_level = bt::sm::SecurityLevel::kSecureAuthenticated;
  }
  return security_level;
}

}  // namespace

LowEnergyConnectionServer::LowEnergyConnectionServer(
    bt::gap::Adapter::WeakPtr adapter,
    bt::gatt::GATT::WeakPtr gatt,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
    std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection,
    zx::channel handle,
    fit::callback<void()> closed_cb)
    : ServerBase(this, std::move(handle)),
      conn_(std::move(connection)),
      closed_handler_(std::move(closed_cb)),
      peer_id_(conn_->peer_identifier()),
      adapter_(std::move(adapter)),
      gatt_(std::move(gatt)),
      wake_lease_provider_(wake_lease_provider),
      weak_self_(this) {
  PW_DCHECK(conn_);

  set_error_handler([this](zx_status_t) { OnClosed(); });
  conn_->set_closed_callback(
      fit::bind_member<&LowEnergyConnectionServer::OnClosed>(this));
}

void LowEnergyConnectionServer::OnClosed() {
  if (closed_handler_) {
    binding()->Close(ZX_ERR_CONNECTION_RESET);
    closed_handler_();
  }
}

void LowEnergyConnectionServer::RequestGattClient(
    fidl::InterfaceRequest<fbg::Client> client) {
  if (gatt_client_server_.has_value()) {
    bt_log(INFO,
           "fidl",
           "%s: gatt client server already bound (peer: %s)",
           __FUNCTION__,
           bt_str(peer_id_));
    client.Close(ZX_ERR_ALREADY_BOUND);
    return;
  }

  fit::callback<void()> server_error_cb = [this] {
    bt_log(
        TRACE, "fidl", "gatt client server error (peer: %s)", bt_str(peer_id_));
    gatt_client_server_.reset();
  };
  gatt_client_server_.emplace(
      peer_id_, gatt_, std::move(client), std::move(server_error_cb));
}

void LowEnergyConnectionServer::AcceptCis(
    fuchsia::bluetooth::le::ConnectionAcceptCisRequest parameters) {
  if (!parameters.has_connection_stream()) {
    bt_log(WARN, "fidl", "AcceptCis invoked without a connection stream");
    return;
  }
  ::fidl::InterfaceRequest<::fuchsia::bluetooth::le::IsochronousStream>*
      connection_stream = parameters.mutable_connection_stream();
  uint8_t cig_id = parameters.cig_id();
  uint8_t cis_id = parameters.cis_id();
  bt::iso::CigCisIdentifier id(cig_id, cis_id);

  // Check for existing stream with same CIG/CIS combination
  if (iso_streams_.count(id) != 0) {
    bt_log(WARN,
           "fidl",
           "AcceptCis invoked with duplicate ID (CIG: %u, CIS: %u)",
           cig_id,
           cis_id);
    connection_stream->Close(ZX_ERR_INVALID_ARGS);
    return;
  }
  auto stream_server = std::make_unique<IsoStreamServer>(
      std::move(*connection_stream), [id, this]() { iso_streams_.erase(id); });
  auto weak_stream_server = stream_server->GetWeakPtr();
  iso_streams_[id] = std::move(stream_server);

  bt::iso::AcceptCisStatus result = conn_->AcceptCis(
      id,
      [weak_stream_server](
          pw::bluetooth::emboss::StatusCode status,
          std::optional<bt::iso::IsoStream::WeakPtr> weak_stream_ptr,
          const std::optional<bt::iso::CisEstablishedParameters>&
              connection_params) {
        if (weak_stream_server.is_alive()) {
          if (status == pw::bluetooth::emboss::StatusCode::SUCCESS) {
            PW_CHECK(weak_stream_ptr.has_value());
            PW_CHECK(connection_params.has_value());
            weak_stream_server->OnStreamEstablished(*weak_stream_ptr,
                                                    *connection_params);
          } else {
            weak_stream_server->OnStreamEstablishmentFailed(status);
          }
        }
      });

  switch (result) {
    case bt::iso::AcceptCisStatus::kSuccess:
      bt_log(INFO,
             "fidl",
             "waiting for incoming CIS connection (CIG: %u, CIS: %u)",
             cig_id,
             cis_id);
      return;
    case bt::iso::AcceptCisStatus::kNotPeripheral:
      bt_log(WARN,
             "fidl",
             "attempt to wait for incoming CIS on Central not allowed");
      iso_streams_[id]->Close(ZX_ERR_NOT_SUPPORTED);
      return;
    case bt::iso::AcceptCisStatus::kAlreadyExists:
      bt_log(WARN,
             "fidl",
             "redundant request to wait for incoming CIS (CIG: %u, CIS: %u)",
             cig_id,
             cis_id);
      iso_streams_[id]->Close(ZX_ERR_INVALID_ARGS);
      return;
    default:
      PW_CRASH("Invalid AcceptCisStatus value %d", static_cast<int>(result));
  }
}

void LowEnergyConnectionServer::GetCodecLocalDelayRange(
    ::fuchsia::bluetooth::le::CodecDelayGetCodecLocalDelayRangeRequest
        parameters,
    GetCodecLocalDelayRangeCallback callback) {
  bt_log(INFO, "fidl", "request received to read controller supported delay");

  if (!parameters.has_logical_transport_type()) {
    bt_log(WARN,
           "fidl",
           "request to read controller delay missing logical_transport_type");
    callback(fpromise::error(ZX_ERR_INVALID_ARGS));
    return;
  }

  if (!parameters.has_data_direction()) {
    bt_log(WARN,
           "fidl",
           "request to read controller delay missing data_direction");
    callback(fpromise::error(ZX_ERR_INVALID_ARGS));
    return;
  }

  if (!parameters.has_codec_attributes()) {
    bt_log(WARN,
           "fidl",
           "request to read controller delay missing codec_attributes");
    callback(fpromise::error(ZX_ERR_INVALID_ARGS));
    return;
  }

  if (!parameters.codec_attributes().has_codec_id()) {
    bt_log(WARN, "fidl", "request to read controller delay missing codec_id");
    callback(fpromise::error(ZX_ERR_INVALID_ARGS));
    return;
  }

  // Process required parameters
  pw::bluetooth::emboss::LogicalTransportType transport_type =
      fidl_helpers::LogicalTransportTypeFromFidl(
          parameters.logical_transport_type());
  pw::bluetooth::emboss::DataPathDirection direction =
      fidl_helpers::DataPathDirectionFromFidl(parameters.data_direction());
  bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter> codec_id =
      fidl_helpers::CodecIdFromFidl(parameters.codec_attributes().codec_id());

  // Codec configuration is optional
  std::optional<std::vector<uint8_t>> codec_configuration;
  if (parameters.codec_attributes().has_codec_configuration()) {
    codec_configuration = parameters.codec_attributes().codec_configuration();
  } else {
    codec_configuration = std::nullopt;
  }

  adapter_->GetSupportedDelayRange(
      codec_id,
      transport_type,
      direction,
      codec_configuration,
      [callback = std::move(callback)](
          pw::Status status, uint32_t min_delay_us, uint32_t max_delay_us) {
        if (!status.ok()) {
          bt_log(WARN, "fidl", "failed to get controller supported delay");
          callback(fpromise::error(ZX_ERR_INTERNAL));
          return;
        }
        bt_log(INFO,
               "fidl",
               "controller supported delay [%d, %d] microseconds",
               min_delay_us,
               max_delay_us);
        fuchsia::bluetooth::le::CodecDelay_GetCodecLocalDelayRange_Response
            response;
        zx::duration min_delay = zx::usec(min_delay_us);
        zx::duration max_delay = zx::usec(max_delay_us);
        response.set_min_controller_delay(min_delay.get());
        response.set_max_controller_delay(max_delay.get());
        callback(
            fuchsia::bluetooth::le::CodecDelay_GetCodecLocalDelayRange_Result::
                WithResponse(std::move(response)));
      });
}

void LowEnergyConnectionServer::ConnectL2cap(
    fuchsia::bluetooth::le::ConnectionConnectL2capRequest request) {
  // Make available in callback below.
  constexpr auto kFuncName = __FUNCTION__;

  if (!request.has_channel()) {
    bt_log(WARN,
           "fidl",
           "%s: No channel request, cannot fulfill call.",
           kFuncName);
    return;
  }

  if (!request.has_psm()) {
    bt_log(ERROR, "fidl", "%s: missing psm.", kFuncName);
    request.mutable_channel()->Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  if (!request.has_parameters()) {
    bt_log(DEBUG,
           "fidl",
           "%s: No parameters provided, using default parameters.",
           kFuncName);
  }

  auto parameters = ConvertParamsFromFidl(*request.mutable_parameters());
  if (!parameters.ok()) {
    bt_log(ERROR, "fidl", "%s: Parameters invalid.", kFuncName);
    request.mutable_channel()->Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  bt::sm::SecurityLevel security_level =
      ConvertSecurityRequirementsFromFidl(*request.mutable_parameters());
  auto cb = [self = weak_self_.GetWeakPtr(),
             request = std::move(*request.mutable_channel())](
                bt::l2cap::Channel::WeakPtr channel) mutable {
    if (!self.is_alive()) {
      bt_log(
          WARN,
          "fidl",
          "%s (cb): Connection server was destroyed before callback completed.",
          kFuncName);
      request.Close(ZX_ERR_INTERNAL);
      return;
    }
    self->ServeChannel(std::move(channel), std::move(request));
  };

  PW_CHECK(adapter_.is_alive());
  adapter_->le()->OpenL2capChannel(
      peer_id_, request.psm(), *parameters, security_level, std::move(cb));
}

void LowEnergyConnectionServer::ServeChannel(
    bt::l2cap::Channel::WeakPtr channel,
    fidl::InterfaceRequest<fuchsia::bluetooth::Channel> request) {
  if (!channel.is_alive()) {
    bt_log(WARN,
           "fidl",
           "%s: Channel was destroyed before it could be served.",
           __FUNCTION__);
    request.Close(ZX_ERR_INTERNAL);
    return;
  }

  bt::l2cap::Channel::UniqueId unique_id = channel->unique_id();

  auto on_close = [this, unique_id]() { channel_servers_.erase(unique_id); };

  auto server = ChannelServer::Create(std::move(request),
                                      std::move(channel),
                                      wake_lease_provider_,
                                      std::move(on_close));

  if (!server) {
    bt_log(ERROR,
           "fidl",
           "%s: Channel server could not be created.",
           __FUNCTION__);
    request.Close(ZX_ERR_INTERNAL);
    return;
  }

  channel_servers_[unique_id] = std::move(server);
}

}  // namespace bthost
