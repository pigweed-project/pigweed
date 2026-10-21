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

#include "emulator.h"

#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <zircon/status.h>

#include <cinttypes>
#include <string>
#include <utility>

#include "emulated_peer.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"
#include "pw_random_fuchsia/zircon_random_generator.h"

namespace fhbt = fuchsia_hardware_bluetooth;

using bt::DeviceAddress;
using bt::testing::FakeController;

namespace bt_hci_virtual {

namespace {

FakeController::Settings SettingsFromFidl(const fhbt::EmulatorSettings& input) {
  FakeController::Settings settings;
  if (input.hci_config().has_value() &&
      input.hci_config().value() == fhbt::HciConfig::kLeOnly) {
    settings.ApplyLEOnlyDefaults();
  } else {
    settings.ApplyDualModeDefaults();
  }

  if (input.address().has_value()) {
    settings.bd_addr =
        DeviceAddress(DeviceAddress::Type::kBREDR, input.address()->bytes());
  }

  // TODO(armansito): Don't ignore "extended_advertising" setting when
  // supported.
  if (input.acl_buffer_settings().has_value()) {
    fhbt::AclBufferSettings acl_settings = input.acl_buffer_settings().value();

    if (acl_settings.data_packet_length().has_value()) {
      settings.acl_data_packet_length =
          acl_settings.data_packet_length().value();
    }

    if (acl_settings.total_num_data_packets().has_value()) {
      settings.total_num_acl_data_packets =
          acl_settings.total_num_data_packets().value();
    }
  }

  if (input.le_acl_buffer_settings().has_value()) {
    fhbt::AclBufferSettings le_acl_settings =
        input.le_acl_buffer_settings().value();

    if (le_acl_settings.data_packet_length().has_value()) {
      settings.le_acl_data_packet_length =
          le_acl_settings.data_packet_length().value();
    }

    if (le_acl_settings.total_num_data_packets().has_value()) {
      settings.le_total_num_acl_data_packets =
          le_acl_settings.total_num_data_packets().value();
    }
  }

  return settings;
}

std::optional<fuchsia_bluetooth::AddressType> LeOwnAddressTypeToFidl(
    pw::bluetooth::emboss::LEOwnAddressType type) {
  std::optional<fuchsia_bluetooth::AddressType> res;

  switch (type) {
    case pw::bluetooth::emboss::LEOwnAddressType::PUBLIC:
    case pw::bluetooth::emboss::LEOwnAddressType::PRIVATE_DEFAULT_TO_PUBLIC:
      res.emplace(fuchsia_bluetooth::AddressType::kPublic);
      return res;
    case pw::bluetooth::emboss::LEOwnAddressType::RANDOM:
    case pw::bluetooth::emboss::LEOwnAddressType::PRIVATE_DEFAULT_TO_RANDOM:
      res.emplace(fuchsia_bluetooth::AddressType::kRandom);
      return res;
  }

  ZX_PANIC("unsupported own address type");
  return res;
}

}  // namespace

EmulatorDevice::EmulatorDevice(async_dispatcher_t* dispatcher)
    : pw_dispatcher_(dispatcher), fake_device_(pw_dispatcher_) {}

EmulatorDevice::~EmulatorDevice() {
  pw_dispatcher_.Cancel(shutdown_task_);
  UnpublishHci();
  fake_device_.Stop();
}

zx_status_t EmulatorDevice::Initialize(std::string_view name,
                                       ShutdownCallback shutdown,
                                       component::OutgoingDirectory* outgoing) {
  shutdown_cb_ = std::move(shutdown);
  outgoing_ = outgoing;
  name_ = std::string(name);
  if (outgoing_ == nullptr) {
    bt_log(WARN,
           "bt-hci-virtual",
           "No outgoing directory provided; fuchsia.hardware.bluetooth.Service "
           "cannot be published");
  }

  // The generator is process-wide and may only be installed once.
  if (bt::random_generator() == nullptr) {
    static pw::random_fuchsia::ZirconRandomGenerator generator;
    bt::set_random_generator(&generator);
  }

  // Initialize `fake_device_`
  auto init_complete_cb = [](pw::Status status) {
    if (!status.ok()) {
      bt_log(WARN,
             "bt-hci-virtual",
             "FakeController failed to initialize: %s",
             status.str());
    }
  };
  auto error_cb = [this](pw::Status status) {
    bt_log(WARN, "bt-hci-virtual", "FakeController error: %s", status.str());
    UnpublishHci();
  };
  fake_device_.Initialize(init_complete_cb, error_cb);

  fake_device_.set_controller_parameters_callback(
      fit::bind_member<&EmulatorDevice::OnControllerParametersChanged>(this));
  fake_device_.set_advertising_state_callback(
      fit::bind_member<&EmulatorDevice::OnLegacyAdvertisingStateChanged>(this));
  fake_device_.set_connection_state_callback(
      fit::bind_member<&EmulatorDevice::OnPeerConnectionStateChanged>(this));

  return ZX_OK;
}

void EmulatorDevice::Shutdown() {
  if (shut_down_) {
    return;
  }
  shut_down_ = true;

  pw_dispatcher_.Cancel(shutdown_task_);
  UnpublishHci();
  fake_device_.Stop();
  peers_.clear();
  controller_parameters_completer_.reset();
  while (!legacy_adv_states_completers_.empty()) {
    legacy_adv_states_completers_.pop();
  }
  // Close any remaining (observer) Emulator connections and any active Vendor
  // or HciTransport connections. RemoveAll() tears the bindings down without
  // running their close handlers, so this cannot re-enter
  // `OnEmulatorOwnerClosed`.
  emulator_binding_group_.RemoveAll();
  vendor_binding_group_.RemoveAll();
  hci_transport_bindings_.RemoveAll();

  // The callback typically destroys `this`, so move it out first (the
  // fit::function must not be destroyed while it is executing) and touch
  // nothing afterwards.
  ShutdownCallback shutdown_cb = std::move(shutdown_cb_);
  if (shutdown_cb) {
    shutdown_cb();
  }
}

void EmulatorDevice::Publish(PublishRequest& request,
                             PublishCompleter::Sync& completer) {
  if (vendor_service_.is_published()) {
    bt_log(INFO, "bt-hci-virtual", "bt-hci-device is already published");
    completer.Reply(fit::error(fhbt::EmulatorError::kHciAlreadyPublished));
    return;
  }

  FakeController::Settings settings = SettingsFromFidl(request);
  fake_device_.set_settings(settings);

  zx_status_t status = vendor_service_.Publish(
      outgoing_, name_, fit::bind_member<&EmulatorDevice::ConnectVendor>(this));
  if (status != ZX_OK) {
    bt_log(WARN, "bt-hci-virtual", "Failed to start vendor service");
    completer.Reply(fit::error(fhbt::EmulatorError::kFailed));
    return;
  }
  bt_log(INFO,
         "bt-hci-virtual",
         "Successfully started vendor service %s",
         name_.c_str());
  completer.Reply(fit::success());
}

void EmulatorDevice::AddLowEnergyPeer(
    AddLowEnergyPeerRequest& request,
    AddLowEnergyPeerCompleter::Sync& completer) {
  auto result = EmulatedPeer::NewLowEnergy(
      std::move(request), &fake_device_, pw_dispatcher_.native());
  if (result.is_error()) {
    completer.Reply(fit::error(result.error()));
    return;
  }

  AddPeer(result.take_value());
  completer.Reply(fit::success());
}

void EmulatorDevice::AddBredrPeer(AddBredrPeerRequest& request,
                                  AddBredrPeerCompleter::Sync& completer) {
  auto result = EmulatedPeer::NewBredr(
      std::move(request), &fake_device_, pw_dispatcher_.native());
  if (result.is_error()) {
    completer.Reply(fit::error(result.error()));
    return;
  }

  AddPeer(result.take_value());
  completer.Reply(fit::success());
}

void EmulatorDevice::WatchControllerParameters(
    WatchControllerParametersCompleter::Sync& completer) {
  controller_parameters_completer_.emplace(completer.ToAsync());
  MaybeUpdateControllerParametersChanged();
}

void EmulatorDevice::WatchLeScanStates(
    WatchLeScanStatesCompleter::Sync& completer) {}

void EmulatorDevice::WatchLegacyAdvertisingStates(
    WatchLegacyAdvertisingStatesCompleter::Sync& completer) {
  legacy_adv_states_completers_.emplace(completer.ToAsync());
  MaybeUpdateLegacyAdvertisingStates();
}

void EmulatorDevice::handle_unknown_method(
    fidl::UnknownMethodMetadata<fhbt::Emulator> metadata,
    fidl::UnknownMethodCompleter::Sync& completer) {
  bt_log(
      ERROR,
      "bt-hci-virtual",
      "Unknown method in Emulator request, closing with ZX_ERR_NOT_SUPPORTED");
  completer.Close(ZX_ERR_NOT_SUPPORTED);
}

void EmulatorDevice::GetFeatures(GetFeaturesCompleter::Sync& completer) {
  completer.Reply(fuchsia_hardware_bluetooth::wire::VendorFeatures());
}

void EmulatorDevice::EncodeCommand(EncodeCommandRequestView request,
                                   EncodeCommandCompleter::Sync& completer) {
  completer.ReplyError(ZX_ERR_INVALID_ARGS);
}

void EmulatorDevice::OpenHciTransport(
    OpenHciTransportCompleter::Sync& completer) {
  // Sets FakeController to respond with event, ACL, and ISO packet types when
  // it receives an incoming packet of the appropriate type
  fake_device_.SetEventFunction(
      fit::bind_member<&EmulatorDevice::SendEventToHost>(this));
  fake_device_.SetReceiveAclFunction(
      fit::bind_member<&EmulatorDevice::SendAclPacketToHost>(this));
  fake_device_.SetReceiveIsoFunction(
      fit::bind_member<&EmulatorDevice::SendIsoPacketToHost>(this));

  auto endpoints = fidl::CreateEndpoints<fhbt::HciTransport>();
  if (endpoints.is_error()) {
    bt_log(ERROR,
           "bt-hci-virtual",
           "Failed to create endpoints: %s",
           endpoints.status_string());
    completer.ReplyError(endpoints.error_value());
    return;
  }

  hci_transport_bindings_.AddBinding(pw_dispatcher_.native(),
                                     std::move(endpoints->server),
                                     this,
                                     fidl::kIgnoreBindingClosure);
  completer.ReplySuccess(std::move(endpoints->client));
}

void EmulatorDevice::OpenSnoop(OpenSnoopCompleter::Sync& completer) {
  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}

void EmulatorDevice::GetCrashParameters(
    GetCrashParametersCompleter::Sync& completer) {
  completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
}

void EmulatorDevice::handle_unknown_method(
    fidl::UnknownMethodMetadata<fhbt::Vendor> metadata,
    fidl::UnknownMethodCompleter::Sync& completer) {
  bt_log(ERROR,
         "bt-hci-virtual",
         "Unknown method in Vendor request, closing with ZX_ERR_NOT_SUPPORTED");
  completer.Close(ZX_ERR_NOT_SUPPORTED);
}

void EmulatorDevice::Send(SendRequest& request,
                          SendCompleter::Sync& completer) {
  switch (request.Which()) {
    case fhbt::SentPacket::Tag::kCommand: {
      std::vector<uint8_t> command_data = request.command().value();
      pw::span<const std::byte> buffer = bt::BufferView(command_data).subspan();
      fake_device_.SendCommand(buffer);
      completer.Reply();
      return;
    }
    case fhbt::SentPacket::Tag::kAcl: {
      std::vector<uint8_t> acl_data = request.acl().value();
      pw::span<const std::byte> buffer = bt::BufferView(acl_data).subspan();
      fake_device_.SendAclData(buffer);
      completer.Reply();
      return;
    }
    case fhbt::SentPacket::Tag::kIso: {
      std::vector<uint8_t> iso_data = request.iso().value();
      pw::span<const std::byte> buffer = bt::BufferView(iso_data).subspan();
      fake_device_.SendIsoData(buffer);
      completer.Reply();
      return;
    }
    default: {
      bt_log(ERROR,
             "bt-hci-virtual",
             "Received unknown packet type %" PRIu64,
             static_cast<uint64_t>(request.Which()));
      return;
    }
  }
}

void EmulatorDevice::ConfigureSco(
    ConfigureScoRequest& request,
    fidl::Server<fuchsia_hardware_bluetooth::HciTransport>::
        ConfigureScoCompleter::Sync& completer) {
  // This interface is not implemented.
  completer.Close(ZX_ERR_NOT_SUPPORTED);
}

void EmulatorDevice::handle_unknown_method(
    ::fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::HciTransport>
        metadata,
    ::fidl::UnknownMethodCompleter::Sync& completer) {
  bt_log(ERROR,
         "bt-hci-virtual",
         "Unknown method in HciTransport request, closing with "
         "ZX_ERR_NOT_SUPPORTED");
  completer.Close(ZX_ERR_NOT_SUPPORTED);
}

void EmulatorDevice::ConnectEmulator(fidl::ServerEnd<fhbt::Emulator> request) {
  if (!emulator_owner_connected_) {
    // The first connection owns the emulator's lifetime; see the declaration
    // of `ConnectEmulator` for the full contract.
    emulator_owner_connected_ = true;
    emulator_binding_group_.AddBinding(
        pw_dispatcher_.native(),
        std::move(request),
        this,
        fit::bind_member<&EmulatorDevice::OnEmulatorOwnerClosed>(this));
    return;
  }
  emulator_binding_group_.AddBinding(pw_dispatcher_.native(),
                                     std::move(request),
                                     this,
                                     fidl::kIgnoreBindingClosure);
}

void EmulatorDevice::OnEmulatorOwnerClosed(fidl::UnbindInfo info) {
  if (info.is_dispatcher_shutdown()) {
    // The dispatcher is stopping; the owner of this device tears it down.
    // Posting to a shutting-down dispatcher would assert.
    return;
  }
  bt_log(INFO,
         "bt-hci-virtual",
         "Emulator owner disconnected (%s); shutting down",
         info.FormatDescription().c_str());
  // Defer the shutdown: it destroys this object, which is not allowed from
  // within the binding group's close handler.
  pw_dispatcher_.Post(shutdown_task_);
}

void EmulatorDevice::ConnectVendor(fidl::ServerEnd<fhbt::Vendor> request) {
  vendor_binding_group_.AddBinding(pw_dispatcher_.native(),
                                   std::move(request),
                                   this,
                                   fidl::kIgnoreBindingClosure);
}

void EmulatorDevice::AddPeer(std::unique_ptr<EmulatedPeer> peer) {
  auto address = peer->address();
  peer->set_closed_callback([this, address] { peers_.erase(address); });
  peers_[address] = std::move(peer);
}

void EmulatorDevice::OnControllerParametersChanged() {
  fhbt::ControllerParameters fidl_value;
  fidl_value.local_name(fake_device_.local_name());

  const auto& device_class_bytes = fake_device_.device_class().bytes();
  uint32_t device_class = 0;
  device_class |= device_class_bytes[0];
  device_class |= static_cast<uint32_t>(device_class_bytes[1]) << 8;
  device_class |= static_cast<uint32_t>(device_class_bytes[2]) << 16;

  std::optional<fuchsia_bluetooth::DeviceClass> device_class_option =
      fuchsia_bluetooth::DeviceClass{device_class};
  fidl_value.device_class(device_class_option);

  controller_parameters_.emplace(fidl_value);
  MaybeUpdateControllerParametersChanged();
}

void EmulatorDevice::MaybeUpdateControllerParametersChanged() {
  if (!controller_parameters_.has_value() ||
      !controller_parameters_completer_.has_value()) {
    return;
  }
  controller_parameters_completer_->Reply(
      std::move(controller_parameters_.value()));
  controller_parameters_.reset();
  controller_parameters_completer_.reset();
}

void EmulatorDevice::OnLegacyAdvertisingStateChanged() {
  // We have requests to resolve. Construct the FIDL table for the current
  // state.
  fhbt::LegacyAdvertisingState fidl_state;
  const FakeController::LEAdvertisingState& adv_state =
      fake_device_.legacy_advertising_state();
  fidl_state.enabled(adv_state.enabled);

  // Populate the rest only if advertising is enabled.
  fidl_state.type(static_cast<fhbt::LegacyAdvertisingType>(
      bt::hci::LowEnergyAdvertiser::
          AdvertisingEventPropertiesToLEAdvertisingType(adv_state.properties)));
  fidl_state.address_type(LeOwnAddressTypeToFidl(adv_state.own_address_type));

  if (adv_state.interval_min) {
    fidl_state.interval_min(adv_state.interval_min);
  }
  if (adv_state.interval_max) {
    fidl_state.interval_max(adv_state.interval_max);
  }

  if (adv_state.data_length) {
    std::vector<uint8_t> output(adv_state.data,
                                adv_state.data + adv_state.data_length);

    if (!fidl_state.advertising_data().has_value()) {
      fidl_state.advertising_data() = fhbt::AdvertisingData();
    }
    fidl_state.advertising_data()->data(output);
  }
  if (adv_state.scan_rsp_length) {
    std::vector<uint8_t> output(
        adv_state.scan_rsp_data,
        adv_state.scan_rsp_data + adv_state.scan_rsp_length);
    if (!fidl_state.scan_response().has_value()) {
      fidl_state.scan_response() = fhbt::AdvertisingData();
    }
    fidl_state.scan_response()->data(output);
  }

  legacy_adv_states_.emplace_back(fidl_state);
  MaybeUpdateLegacyAdvertisingStates();
}

void EmulatorDevice::MaybeUpdateLegacyAdvertisingStates() {
  if (legacy_adv_states_.empty() || legacy_adv_states_completers_.empty()) {
    return;
  }
  while (!legacy_adv_states_completers_.empty()) {
    legacy_adv_states_completers_.front().Reply(legacy_adv_states_);
    legacy_adv_states_completers_.pop();
  }
  legacy_adv_states_.clear();
}

void EmulatorDevice::OnPeerConnectionStateChanged(
    const DeviceAddress& address,
    bt::hci_spec::ConnectionHandle handle,
    bool connected,
    bool canceled) {
  bt_log(TRACE,
         "bt-hci-virtual",
         "Peer connection state changed: %s (handle: %#06x) (connected: %d) "
         "(canceled: %d)",
         address.ToString().c_str(),
         handle,
         connected,
         canceled);

  auto iter = peers_.find(address);
  if (iter != peers_.end()) {
    iter->second->UpdateConnectionState(connected);
  }
}

void EmulatorDevice::UnpublishHci() { vendor_service_.Unpublish(); }

void EmulatorDevice::SendEventToHost(pw::span<const std::byte> buffer) {
  if (hci_transport_bindings_.size() == 0) {
    bt_log(ERROR, "bt-hci-virtual", "No HciTransport bindings");
    return;
  }
  // Using HciTransport protocol (i.e. |cmd_channel_| is not set)
  hci_transport_bindings_.ForEachBinding(
      [buffer](
          const fidl::ServerBinding<fuchsia_hardware_bluetooth::HciTransport>&
              binding) {
        // Send the event to bt-host
        std::vector<uint8_t> data = bt::BufferView(buffer).ToVector();
        fhbt::ReceivedPacket event = fhbt::ReceivedPacket::WithEvent(data);
        fit::result<::fidl::OneWayError> status =
            fidl::SendEvent(binding)->OnReceive(event);
        if (!status.is_ok()) {
          bt_log(ERROR,
                 "bt-hci-virtual",
                 "Failed to send OnReceive event to bt-host: %s",
                 status.error_value().status_string());
        }
      });
}

void EmulatorDevice::SendAclPacketToHost(pw::span<const std::byte> buffer) {
  if (hci_transport_bindings_.size() == 0) {
    bt_log(ERROR, "bt-hci-virtual", "No HciTransport bindings");
    return;
  }
  // Using HciTransport protocol (i.e. |acl_channel_| is not set)
  hci_transport_bindings_.ForEachBinding(
      [buffer](
          const fidl::ServerBinding<fuchsia_hardware_bluetooth::HciTransport>&
              binding) {
        // Send the event to bt-host
        std::vector<uint8_t> data = bt::BufferView(buffer).ToVector();
        fhbt::ReceivedPacket event = fhbt::ReceivedPacket::WithAcl(data);
        fit::result<::fidl::OneWayError> status =
            fidl::SendEvent(binding)->OnReceive(event);
        if (!status.is_ok()) {
          bt_log(ERROR,
                 "bt-hci-virtual",
                 "Failed to send OnReceive event to bt-host: %s",
                 status.error_value().status_string());
        }
      });
}

void EmulatorDevice::SendIsoPacketToHost(pw::span<const std::byte> buffer) {
  if (hci_transport_bindings_.size() == 0) {
    bt_log(ERROR, "bt-hci-virtual", "No HciTransport bindings");
    return;
  }
  // Using HciTransport protocol (i.e. |iso_channel_| is not set)
  hci_transport_bindings_.ForEachBinding(
      [buffer](
          const fidl::ServerBinding<fuchsia_hardware_bluetooth::HciTransport>&
              binding) {
        // Send the event to bt-host
        std::vector<uint8_t> data = bt::BufferView(buffer).ToVector();
        fhbt::ReceivedPacket event = fhbt::ReceivedPacket::WithIso(data);
        fit::result<::fidl::OneWayError> status =
            fidl::SendEvent(binding)->OnReceive(event);
        if (!status.is_ok()) {
          bt_log(ERROR,
                 "bt-hci-virtual",
                 "Failed to send OnReceive event to bt-host: %s",
                 status.error_value().status_string());
        }
      });
}

}  // namespace bt_hci_virtual
