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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/iso_stream_server.h"

#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/fidl/cpp/wire/channel.h>
#include <pw_assert/check.h>
#include <pw_bluetooth/hci_data.emb.h>

#include <cinttypes>

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"

namespace bthost {

IsoStreamServer::IsoStreamServer(
    fidl::InterfaceRequest<fuchsia::bluetooth::le::IsochronousStream> request,
    fit::callback<void()> on_closed_cb)
    : ServerBase(this, std::move(request)),
      on_closed_cb_(std::move(on_closed_cb)),
      weak_self_(this) {
  set_error_handler([this](zx_status_t) { OnClosed(); });
}

IsoStreamServer::~IsoStreamServer() {
  if (iso_stream_.has_value() && iso_stream_->is_alive()) {
    (*iso_stream_)->Close();
  }
}

void IsoStreamServer::OnStreamEstablished(
    bt::iso::IsoStream::WeakPtr stream_ptr,
    const bt::iso::CisEstablishedParameters& connection_params) {
  bt_log(INFO, "fidl", "CIS established");
  iso_stream_ = stream_ptr;
  fuchsia::bluetooth::le::IsochronousStreamOnEstablishedRequest request;
  request.set_result(ZX_OK);
  fuchsia::bluetooth::le::CisEstablishedParameters params =
      bthost::fidl_helpers::CisEstablishedParametersToFidl(connection_params);
  request.set_established_params(std::move(params));
  binding()->events().OnEstablished(std::move(request));

  if (establishment_cb_) {
    establishment_cb_(pw::bluetooth::emboss::StatusCode::SUCCESS);
  }
}

void IsoStreamServer::OnStreamEstablishmentFailed(
    pw::bluetooth::emboss::StatusCode status) {
  PW_CHECK(status != pw::bluetooth::emboss::StatusCode::SUCCESS);
  bt_log(WARN,
         "fidl",
         "CIS failed to be established: %u",
         static_cast<unsigned>(status));
  fuchsia::bluetooth::le::IsochronousStreamOnEstablishedRequest request;
  request.set_result(ZX_ERR_INTERNAL);
  binding()->events().OnEstablished(std::move(request));

  if (establishment_cb_) {
    establishment_cb_(status);
  }

  async::PostTask(async_get_default_dispatcher(),
                  [self = weak_self_.GetWeakPtr()]() {
                    if (self.is_alive()) {
                      self->Close(ZX_ERR_INTERNAL);
                    }
                  });
}

void IsoStreamServer::SetupDataPath(
    fuchsia::bluetooth::le::IsochronousStreamSetupDataPathRequest parameters,
    SetupDataPathCallback fidl_cb) {
  pw::bluetooth::emboss::DataPathDirection direction =
      fidl_helpers::DataPathDirectionFromFidl(parameters.data_direction());
  const char* direction_as_str =
      fidl_helpers::DataPathDirectionToString(direction);
  bt_log(INFO,
         "fidl",
         "Request received to set up data path (direction: %s)",
         direction_as_str);
  if (direction != pw::bluetooth::emboss::DataPathDirection::OUTPUT) {
    // We only support Controller => Host at the moment
    bt_log(WARN,
           "fidl",
           "Attempt to set up data path with unsupported direction: %s",
           direction_as_str);
    fidl_cb(fpromise::error(ZX_ERR_NOT_SUPPORTED));
    return;
  }

  bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter> codec_id =
      fidl_helpers::CodecIdFromFidl(parameters.codec_attributes().codec_id());
  std::optional<std::vector<uint8_t>> codec_configuration;
  if (parameters.codec_attributes().has_codec_configuration()) {
    codec_configuration = parameters.codec_attributes().codec_configuration();
  }

  zx::duration delay(parameters.controller_delay());
  uint32_t delay_in_us = delay.to_usecs();
  if (!iso_stream_.has_value()) {
    bt_log(WARN, "fidl", "data path setup failed (CIS not established)");
    fidl_cb(fpromise::error(ZX_ERR_BAD_STATE));
    return;
  }
  if (!iso_stream_->is_alive()) {
    bt_log(INFO, "fidl", "Attempt to set data path after CIS closed");
    fidl_cb(fpromise::error(ZX_ERR_BAD_STATE));
    return;
  }

  auto on_setup_complete_cb =
      [fidl_cb =
           std::move(fidl_cb)](bt::iso::IsoStream::SetupDataPathError error) {
        switch (error) {
          case bt::iso::IsoStream::kSuccess:
            bt_log(INFO, "fidl", "data path successfully setup");
            fidl_cb(fpromise::ok());
            break;
          case bt::iso::IsoStream::kStreamAlreadyExists:
            bt_log(
                WARN, "fidl", "data path setup failed (stream already setup)");
            fidl_cb(fpromise::error(ZX_ERR_ALREADY_EXISTS));
            break;
          case bt::iso::IsoStream::kCisNotEstablished:
            bt_log(
                WARN, "fidl", "data path setup failed (CIS not established)");
            fidl_cb(fpromise::error(ZX_ERR_BAD_STATE));
            break;
          case bt::iso::IsoStream::kInvalidArgs:
            bt_log(WARN, "fidl", "data path setup failed (invalid parameters)");
            fidl_cb(fpromise::error(ZX_ERR_INVALID_ARGS));
            break;
          case bt::iso::IsoStream::kStreamClosed:
            bt_log(WARN, "fidl", "data path setup failed (stream closed)");
            fidl_cb(fpromise::error(ZX_ERR_BAD_STATE));
            break;
          default:
            bt_log(ERROR,
                   "fidl",
                   "Unsupported case in SetupDataPathError: %u",
                   static_cast<unsigned>(error));
            fidl_cb(fpromise::error(ZX_ERR_INTERNAL));
            break;
        }
      };
  (*iso_stream_)
      ->SetupDataPath(
          direction,
          codec_id,
          codec_configuration,
          delay_in_us,
          std::move(on_setup_complete_cb),
          fit::bind_member<&IsoStreamServer::OnIncomingDataAvailable>(this));
}

void IsoStreamServer::Write(
    fuchsia::bluetooth::le::IsochronousStreamWriteRequest request,
    WriteCallback fidl_cb) {
  if (!iso_stream_->is_alive()) {
    bt_log(WARN, "fidl", "Attempt to write data on a closed stream");
    Close(ZX_ERR_BAD_STATE);
    return;
  }

  const std::vector<uint8_t>& data = request.data();

  pw::span<const std::byte> data_span =
      std::as_bytes(pw::span(data.data(), data.size()));

  (*iso_stream_)->Send(data_span);
  fidl_cb(fpromise::ok());
}

void IsoStreamServer::SendIncomingPacket(pw::span<const std::byte> packet) {
  auto view = pw::bluetooth::emboss::MakeIsoDataFramePacketView(packet.data(),
                                                                packet.size());
  if (!view.Ok()) {
    bt_log(ERROR, "fidl", "Failed to parse ISO data frame");
    // Hanging get will remain unfulfilled
    return;
  }
  PW_CHECK(view.header().pb_flag().Read() ==
               pw::bluetooth::emboss::IsoDataPbFlag::COMPLETE_SDU,
           "Incomplete SDU received from IsoStream");
  fuchsia::bluetooth::le::IsochronousStream_Read_Response response;

  size_t data_fragment_size = view.sdu_fragment_size().Read();
  std::vector<std::uint8_t> data_as_vector(data_fragment_size);

  std::memcpy(data_as_vector.data(),
              view.iso_sdu_fragment().BackingStorage().data(),
              data_fragment_size);
  response.set_data(data_as_vector);
  response.set_sequence_number(view.packet_sequence_number().Read());
  response.set_status_flag(fidl_helpers::EmbossIsoPacketStatusFlagToFidl(
      view.packet_status_flag().Read()));

  PW_CHECK(hanging_read_cb_);
  hanging_read_cb_(
      fuchsia::bluetooth::le::IsochronousStream_Read_Result::WithResponse(
          std::move(response)));
  hanging_read_cb_ = nullptr;
}

bool IsoStreamServer::OnIncomingDataAvailable(
    pw::span<const std::byte> packet) {
  if (!hanging_read_cb_) {
    // This is not a hard error, but it is a bit suspicious and worth noting. We
    // should not receive a notification of incoming data unless we have a
    // hanging Read() operation.
    bt_log(WARN,
           "fidl",
           "Notification of incoming data received with no outstanding read "
           "operation");
    return false;
  }
  SendIncomingPacket(packet);
  return true;
}

void IsoStreamServer::Read(ReadCallback callback) {
  // We should not have more than one outstanding Read()
  if (hanging_read_cb_) {
    Close(ZX_ERR_BAD_STATE);
    return;
  }

  hanging_read_cb_ = std::move(callback);

  if (iso_stream_.has_value() && iso_stream_->is_alive()) {
    std::optional<bt::iso::IsoDataPacket> packet =
        (*iso_stream_)->ReadNextQueuedIncomingPacket();
    if (packet) {
      SendIncomingPacket(*packet);
      return;
    }
  }
}

void IsoStreamServer::OnClosed() {
  if (iso_stream_.has_value() && iso_stream_->is_alive()) {
    (*iso_stream_)->Close();
  }
  // This may free our instance.
  if (on_closed_cb_) {
    on_closed_cb_();
  }
}

void IsoStreamServer::Close(zx_status_t epitaph) {
  if (binding()->is_bound()) {
    binding()->Close(epitaph);
    OnClosed();
  }
}

void IsoStreamServer::handle_unknown_method(uint64_t ordinal,
                                            bool has_response) {
  bt_log(WARN,
         "fidl",
         "Received unknown fidl call %#" PRIx64 " (%s responses)",
         ordinal,
         has_response ? "with" : "without");
}

}  // namespace bthost
