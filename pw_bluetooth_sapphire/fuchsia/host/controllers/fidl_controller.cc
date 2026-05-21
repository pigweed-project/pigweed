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

#include "pw_bluetooth_sapphire/fuchsia/host/controllers/fidl_controller.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/fuchsia/host/controllers/helpers.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "zircon/status.h"

namespace bt::controllers {

namespace fhbt = fuchsia_hardware_bluetooth;
using ReceivedPacket = fhbt::ReceivedPacket;

namespace {
pw::bluetooth::Controller::FeaturesBits StandardFeatures() {
  // HCI SCO is not vendor specific and is a standard feature.
  return pw::bluetooth::Controller::FeaturesBits::kHciSco;
}

pw::bluetooth::Controller::FeaturesBits VendorFeaturesToFeaturesBits(
    fhbt::VendorFeatures features) {
  pw::bluetooth::Controller::FeaturesBits out{0};
  if (features.acl_priority_command().has_value() &&
      features.acl_priority_command()) {
    out |= pw::bluetooth::Controller::FeaturesBits::kSetAclPriorityCommand;
  }
  if (features.android_vendor_extensions().has_value()) {
    // Ignore the content of android_vendor_extension field now.
    out |= pw::bluetooth::Controller::FeaturesBits::kAndroidVendorExtensions;
  }
  return out;
}

fhbt::VendorAclPriority AclPriorityToFidl(pw::bluetooth::AclPriority priority) {
  switch (priority) {
    case pw::bluetooth::AclPriority::kNormal:
      return fhbt::VendorAclPriority::kNormal;
    case pw::bluetooth::AclPriority::kSource:
    case pw::bluetooth::AclPriority::kSink:
      return fhbt::VendorAclPriority::kHigh;
  }
}

fhbt::VendorAclDirection AclPriorityToFidlAclDirection(
    pw::bluetooth::AclPriority priority) {
  switch (priority) {
    // The direction for kNormal is arbitrary.
    case pw::bluetooth::AclPriority::kNormal:
    case pw::bluetooth::AclPriority::kSource:
      return fhbt::VendorAclDirection::kSource;
    case pw::bluetooth::AclPriority::kSink:
      return fhbt::VendorAclDirection::kSink;
  }
}

fhbt::ScoCodingFormat ScoCodingFormatToFidl(
    pw::bluetooth::Controller::ScoCodingFormat coding_format) {
  switch (coding_format) {
    case pw::bluetooth::Controller::ScoCodingFormat::kCvsd:
      return fhbt::ScoCodingFormat::kCvsd;
    case pw::bluetooth::Controller::ScoCodingFormat::kMsbc:
      return fhbt::ScoCodingFormat::kMsbc;
    default:
      PW_CRASH("invalid SCO coding format");
  }
}

fhbt::ScoEncoding ScoEncodingToFidl(
    pw::bluetooth::Controller::ScoEncoding encoding) {
  switch (encoding) {
    case pw::bluetooth::Controller::ScoEncoding::k8Bits:
      return fhbt::ScoEncoding::kBits8;
    case pw::bluetooth::Controller::ScoEncoding::k16Bits:
      return fhbt::ScoEncoding::kBits16;
    default:
      PW_CRASH("invalid SCO encoding");
  }
}

fhbt::ScoSampleRate ScoSampleRateToFidl(
    pw::bluetooth::Controller::ScoSampleRate sample_rate) {
  switch (sample_rate) {
    case pw::bluetooth::Controller::ScoSampleRate::k8Khz:
      return fhbt::ScoSampleRate::kKhz8;
    case pw::bluetooth::Controller::ScoSampleRate::k16Khz:
      return fhbt::ScoSampleRate::kKhz16;
    default:
      PW_CRASH("invalid SCO sample rate");
  }
}

}  // namespace

VendorEventHandler::VendorEventHandler(
    std::function<void(zx_status_t)> unbind_callback)
    : unbind_callback_(std::move(unbind_callback)) {}

void VendorEventHandler::handle_unknown_event(
    fidl::UnknownEventMetadata<fhbt::Vendor> metadata) {
  bt_log(WARN,
         "controllers",
         "Unknown event from Vendor server: %lu",
         metadata.event_ordinal);
}

void VendorEventHandler::on_fidl_error(fidl::UnbindInfo error) {
  bt_log(ERROR,
         "controllers",
         "Vendor protocol closed: %s",
         error.FormatDescription().c_str());
  unbind_callback_(ZX_ERR_PEER_CLOSED);
}

HciEventHandler::HciEventHandler(
    std::function<void(zx_status_t)> unbind_callback,
    std::function<void(fuchsia_hardware_bluetooth::ReceivedPacket)>
        on_receive_callback)
    : on_receive_callback_(std::move(on_receive_callback)),
      unbind_callback_(std::move(unbind_callback)) {}

void HciEventHandler::OnReceive(
    fuchsia_hardware_bluetooth::ReceivedPacket& packet) {
  on_receive_callback_(std::move(packet));
}

void HciEventHandler::handle_unknown_event(
    fidl::UnknownEventMetadata<fhbt::HciTransport> metadata) {
  bt_log(WARN,
         "controllers",
         "Unknown event from Hci server: %lu",
         metadata.event_ordinal);
}

void HciEventHandler::on_fidl_error(fidl::UnbindInfo error) {
  bt_log(ERROR,
         "controllers",
         "Hci protocol closed: %s",
         error.FormatDescription().c_str());
  unbind_callback_(ZX_ERR_PEER_CLOSED);
}

FidlController::FidlController(fidl::ClientEnd<fhbt::Vendor> vendor_client_end,
                               async_dispatcher_t* dispatcher)
    : vendor_event_handler_([this](zx_status_t status) { OnError(status); }),
      hci_event_handler_([this](zx_status_t status) { OnError(status); },
                         [this](fhbt::ReceivedPacket packet) {
                           OnReceive(std::move(packet));
                         }),
      sco_event_handler_(
          [this](zx_status_t status) { OnScoUnbind(status); },
          [this](fhbt::ScoPacket packet) { OnReceiveSco(std::move(packet)); }),
      dispatcher_(dispatcher) {
  PW_CHECK(vendor_client_end.is_valid());
  vendor_client_end_ = std::move(vendor_client_end);
}

FidlController::~FidlController() { CleanUp(); }

void FidlController::Initialize(PwStatusCallback complete_callback,
                                PwStatusCallback error_callback) {
  initialize_complete_cb_ = std::move(complete_callback);
  error_cb_ = std::move(error_callback);

  vendor_ = fidl::Client<fhbt::Vendor>(
      std::move(vendor_client_end_), dispatcher_, &vendor_event_handler_);

  // Connect to Hci protocol
  vendor_->OpenHciTransport().Then(
      [this](fidl::Result<fhbt::Vendor::OpenHciTransport>& result) {
        if (!result.is_ok()) {
          bt_log(ERROR,
                 "bt-host",
                 "Failed to open HciTransport: %s",
                 result.error_value().FormatDescription().c_str());
          if (result.error_value().is_framework_error()) {
            OnError(result.error_value().framework_error().status());
          } else {
            OnError(result.error_value().domain_error());
          }
          return;
        }

        InitializeHci(std::move(result->channel()));
      });
}

void FidlController::InitializeHci(
    fidl::ClientEnd<fhbt::HciTransport> hci_client_end) {
  hci_ = fidl::Client<fhbt::HciTransport>(
      std::move(hci_client_end), dispatcher_, &hci_event_handler_);

  initialize_complete_cb_(PW_STATUS_OK);
}

void FidlController::Close(PwStatusCallback callback) {
  CleanUp();
  callback(PW_STATUS_OK);
}

void FidlController::SendCommand(pw::span<const std::byte> command) {
  BufferView view = BufferView(command);
  fidl::VectorView vec_view = fidl::VectorView<uint8_t>::FromExternal(
      const_cast<uint8_t*>(view.data()), view.size());
  fidl::ObjectView obj_view =
      fidl::ObjectView<fidl::VectorView<uint8_t>>::FromExternal(&vec_view);
  // Use Wire bindings to avoid copying `command`.
  hci_.wire()
      ->Send(fhbt::wire::SentPacket::WithCommand(obj_view))
      .Then([this](fidl::WireUnownedResult<fhbt::HciTransport::Send>& result) {
        if (!result.ok()) {
          bt_log(ERROR,
                 "controllers",
                 "failed to send command: %s",
                 result.status_string());
          OnError(result.status());
          return;
        }
      });
}

void FidlController::SendAclData(pw::span<const std::byte> data) {
  BufferView view = BufferView(data);
  fidl::VectorView vec_view = fidl::VectorView<uint8_t>::FromExternal(
      const_cast<uint8_t*>(view.data()), view.size());
  fidl::ObjectView obj_view =
      fidl::ObjectView<fidl::VectorView<uint8_t>>::FromExternal(&vec_view);
  // Use Wire bindings to avoid copying `data`.
  hci_.wire()
      ->Send(fhbt::wire::SentPacket::WithAcl(obj_view))
      .Then([this](fidl::WireUnownedResult<fhbt::HciTransport::Send>& result) {
        if (!result.ok()) {
          bt_log(ERROR,
                 "controllers",
                 "failed to send ACL: %s",
                 result.status_string());
          OnError(result.status());
          return;
        }
      });
}

void FidlController::SendScoData(pw::span<const std::byte> data) {
  if (!sco_connection_) {
    bt_log(ERROR,
           "controllers",
           "SendScoData() called when SCO is not configured");
    OnError(ZX_ERR_BAD_STATE);
    return;
  }
  BufferView view = BufferView(data);
  fidl::VectorView vec_view = fidl::VectorView<uint8_t>::FromExternal(
      const_cast<uint8_t*>(view.data()), view.size());
  // Use Wire bindings to avoid copying `data`.
  sco_connection_.value().wire()->Send(vec_view).Then(
      [this](fidl::WireUnownedResult<fhbt::ScoConnection::Send>& result) {
        if (!result.ok()) {
          bt_log(ERROR,
                 "controllers",
                 "failed to send SCO: %s",
                 result.status_string());
          OnError(result.status());
          return;
        }
      });
}

void FidlController::SendIsoData(pw::span<const std::byte> data) {
  BufferView view = BufferView(data);
  fidl::VectorView vec_view = fidl::VectorView<uint8_t>::FromExternal(
      const_cast<uint8_t*>(view.data()), view.size());
  fidl::ObjectView obj_view =
      fidl::ObjectView<fidl::VectorView<uint8_t>>::FromExternal(&vec_view);
  // Use Wire bindings to avoid copying `data`.
  hci_.wire()
      ->Send(fhbt::wire::SentPacket::WithIso(obj_view))
      .Then([this](fidl::WireUnownedResult<fhbt::HciTransport::Send>& result) {
        if (!result.ok()) {
          bt_log(ERROR,
                 "controllers",
                 "failed to send ISO: %s",
                 result.status_string());
          OnError(result.status());
          return;
        }
      });
}

void FidlController::ConfigureSco(ScoCodingFormat coding_format,
                                  ScoEncoding encoding,
                                  ScoSampleRate sample_rate,
                                  PwStatusCallback callback) {
  if (sco_connection_) {
    callback(pw::Status::AlreadyExists());
    return;
  }

  fidl::Request<fhbt::HciTransport::ConfigureSco> request;
  request.coding_format() = ScoCodingFormatToFidl(coding_format);
  request.encoding() = ScoEncodingToFidl(encoding);
  request.sample_rate() = ScoSampleRateToFidl(sample_rate);

  auto [client_end, server_end] =
      fidl::CreateEndpoints<fhbt::ScoConnection>().value();
  request.connection() = std::move(server_end);
  sco_connection_ =
      fidl::Client(std::move(client_end), dispatcher_, &sco_event_handler_);

  fit::result<::fidl::OneWayError> result =
      hci_->ConfigureSco(std::move(request));

  if (!result.is_ok()) {
    bt_log(WARN,
           "controllers",
           "Failed to configure SCO: %s",
           result.error_value().FormatDescription().c_str());
    sco_connection_.reset();
    callback(ZxStatusToPwStatus(result.error_value().status()));
    return;
  }

  callback(ZxStatusToPwStatus(ZX_OK));
}

void FidlController::ResetSco(pw::Callback<void(pw::Status)> callback) {
  if (!sco_connection_) {
    bt_log(WARN, "controllers", "ResetSco(): no SCO connection configured");
    callback(pw::Status::FailedPrecondition());
    return;
  }
  if (reset_sco_cb_) {
    bt_log(WARN, "controllers", "ResetSco(): already pending", );
    callback(pw::Status::AlreadyExists());
    return;
  }
  reset_sco_cb_ = std::move(callback);

  fit::result<fidl::OneWayError> result = sco_connection_.value()->Stop();
  if (result.is_error()) {
    OnError(ZX_ERR_BAD_STATE);
    return;
  }
}

void FidlController::GetFeatures(
    pw::Callback<void(FidlController::FeaturesBits)> callback) {
  vendor_->GetFeatures().Then(
      [this, cb = std::move(callback)](
          fidl::Result<fhbt::Vendor::GetFeatures>& result) mutable {
        if (result.is_error()) {
          bt_log(WARN,
                 "controllers",
                 "GetFeatures(): %s",
                 result.error_value().status_string());
          OnError(ZX_ERR_BAD_STATE);
          return;
        }

        FidlController::FeaturesBits features_bits =
            VendorFeaturesToFeaturesBits(result.value()) | StandardFeatures();
        cb(features_bits);
      });
}

void FidlController::EncodeVendorCommand(
    pw::bluetooth::VendorCommandParameters parameters,
    pw::Callback<void(pw::Result<pw::span<const std::byte>>)> callback) {
  PW_CHECK(vendor_);

  if (!std::holds_alternative<pw::bluetooth::SetAclPriorityCommandParameters>(
          parameters)) {
    callback(pw::Status::Unimplemented());
    return;
  }

  pw::bluetooth::SetAclPriorityCommandParameters params =
      std::get<pw::bluetooth::SetAclPriorityCommandParameters>(parameters);

  fhbt::VendorSetAclPriorityParams priority_params;
  priority_params.connection_handle(params.connection_handle);
  priority_params.priority(AclPriorityToFidl(params.priority));
  priority_params.direction(AclPriorityToFidlAclDirection(params.priority));

  auto command = fhbt::VendorCommand::WithSetAclPriority(priority_params);

  vendor_->EncodeCommand(command).Then(
      [cb = std::move(callback)](
          fidl::Result<fhbt::Vendor::EncodeCommand>& result) mutable {
        if (!result.is_ok()) {
          bt_log(ERROR,
                 "controllers",
                 "Failed to encode vendor command: %s",
                 result.error_value().FormatDescription().c_str());
          if (result.error_value().is_framework_error()) {
            cb(ZxStatusToPwStatus(
                result.error_value().framework_error().status()));
          } else {
            cb(ZxStatusToPwStatus(result.error_value().domain_error()));
          }
          return;
        }
        auto span = pw::as_bytes(pw::span(result->encoded()));
        cb(span);
      });
}

void FidlController::ScoEventHandler::OnReceive(fhbt::ScoPacket& packet) {
  on_receive_callback_(std::move(packet));
}

void FidlController::ScoEventHandler::on_fidl_error(fidl::UnbindInfo error) {
  bt_log(DEBUG,
         "controllers",
         "SCO protocol closed: %s",
         error.FormatDescription().c_str());
  unbind_callback_(ZX_ERR_PEER_CLOSED);
}

void FidlController::ScoEventHandler::handle_unknown_event(
    fidl::UnknownEventMetadata<fhbt::ScoConnection> metadata) {
  bt_log(WARN,
         "controllers",
         "Unknown event from ScoConnection server: %lu",
         metadata.event_ordinal);
}

FidlController::ScoEventHandler::ScoEventHandler(
    pw::Function<void(zx_status_t)> unbind_callback,
    pw::Function<void(fuchsia_hardware_bluetooth::ScoPacket)>
        on_receive_callback)
    : on_receive_callback_(std::move(on_receive_callback)),
      unbind_callback_(std::move(unbind_callback)) {}

void FidlController::OnReceive(ReceivedPacket packet) {
  switch (packet.Which()) {
    case ReceivedPacket::Tag::kEvent:
      event_cb_(BufferView(packet.event().value()).subspan());
      break;
    case ReceivedPacket::Tag::kAcl:
      acl_cb_(BufferView(packet.acl().value()).subspan());
      break;
    case ReceivedPacket::Tag::kIso:
      iso_cb_(BufferView(packet.iso().value()).subspan());
      break;
    default:
      bt_log(WARN,
             "controllers",
             "OnReceive: unknown packet type %lu",
             static_cast<uint64_t>(packet.Which()));
  }
  fit::result<fidl::OneWayError> result = hci_->AckReceive();
  if (result.is_error()) {
    OnError(ZX_ERR_IO);
  }
}

void FidlController::OnReceiveSco(
    fuchsia_hardware_bluetooth::ScoPacket packet) {
  fit::result<fidl::OneWayError> result = sco_connection_.value()->AckReceive();
  if (result.is_error()) {
    OnError(ZX_ERR_IO);
    return;
  }
  sco_cb_(BufferView(packet.packet()).subspan());
}

void FidlController::OnScoUnbind(zx_status_t status) {
  // The server shouldn't close a ScoConnection on its own. It should only close
  // after the host calls Stop().
  if (!reset_sco_cb_) {
    bt_log(ERROR,
           "controllers",
           "ScoConnection closed unexpectedly (Stop() not called): %s",
           zx_status_get_string(status));
    OnError(ZX_ERR_BAD_STATE);
    return;
  }
  bt_log(DEBUG, "controllers", "ScoConnection closed");
  sco_connection_.reset();
  reset_sco_cb_(pw::OkStatus());
  reset_sco_cb_ = nullptr;
}

void FidlController::OnError(zx_status_t status) {
  CleanUp();

  // If |initialize_complete_cb_| has already been called, then initialization
  // is complete and we use |error_cb_|
  if (initialize_complete_cb_) {
    initialize_complete_cb_(ZxStatusToPwStatus(status));
    // Clean up |error_cb_| since we only need one callback to be called during
    // initialization.
    error_cb_ = nullptr;
  } else if (error_cb_) {
    error_cb_(ZxStatusToPwStatus(status));
  }
}

void FidlController::CleanUp() {
  if (shutting_down_) {
    return;
  }
  shutting_down_ = true;

  if (hci_) {
    auto hci_endpoint = hci_.UnbindMaybeGetEndpoint();
  }
  if (vendor_) {
    auto vendor_endpoint = vendor_.UnbindMaybeGetEndpoint();
  }
  if (sco_connection_) {
    auto sco_endpoint = sco_connection_->UnbindMaybeGetEndpoint();
    sco_connection_.reset();
  }
}

}  // namespace bt::controllers
