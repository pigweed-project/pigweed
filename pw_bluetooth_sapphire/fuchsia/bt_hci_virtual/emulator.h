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

#include <fidl/fuchsia.driver.framework/cpp/fidl.h>
#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <lib/async/cpp/wait.h>
#include <lib/driver/devfs/cpp/connector.h>
#include <pw_async_fuchsia/dispatcher.h>

#include <queue>
#include <unordered_map>

#include "emulated_peer.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_random_fuchsia/zircon_random_generator.h"

namespace bt_hci_virtual {

enum class ChannelType : uint8_t { ACL, COMMAND, EMULATOR, ISO, SNOOP };

using AddChildCallback =
    fit::function<void(fuchsia_driver_framework::wire::NodeAddArgs)>;
using ShutdownCallback = fit::function<void()>;

class EmulatorDevice
    : public fidl::WireAsyncEventHandler<
          fuchsia_driver_framework::NodeController>,
      public fidl::WireAsyncEventHandler<fuchsia_driver_framework::Node>,
      public fidl::Server<fuchsia_hardware_bluetooth::Emulator>,
      public fidl::Server<fuchsia_hardware_bluetooth::HciTransport>,
      public fidl::WireServer<fuchsia_hardware_bluetooth::Vendor> {
 public:
  explicit EmulatorDevice();
  ~EmulatorDevice();

  // This error handler is called when the EmulatorDevice is shut down by DFv2.
  // We call |Shutdown()| to manually delete the heap-allocated EmulatorDevice.
  void on_fidl_error(fidl::UnbindInfo error) override { Shutdown(); }

  void handle_unknown_event(
      fidl::UnknownEventMetadata<fuchsia_driver_framework::Node> metadata)
      override {}
  void handle_unknown_event(
      fidl::UnknownEventMetadata<fuchsia_driver_framework::NodeController>
          metadata) override {}

  // Methods used by the VirtualController to control the EmulatorDevice's
  // lifecycle
  zx_status_t Initialize(std::string_view name,
                         AddChildCallback callback,
                         ShutdownCallback shutdown);
  void Shutdown();

  void set_emulator_ptr(std::unique_ptr<EmulatorDevice> ptr) {
    emulator_ptr_ = std::move(ptr);
  }

  fidl::WireClient<fuchsia_driver_framework::Node>* emulator_child_node() {
    return &emulator_child_node_;
  }
  void set_emulator_child_node(
      fidl::WireClient<fuchsia_driver_framework::Node> node) {
    emulator_child_node_ = std::move(node);
  }

 private:
  // fuchsia_hardware_bluetooth::Emulator overrides:
  void Publish(PublishRequest& request,
               PublishCompleter::Sync& completer) override;
  void AddLowEnergyPeer(AddLowEnergyPeerRequest& request,
                        AddLowEnergyPeerCompleter::Sync& completer) override;
  void AddBredrPeer(AddBredrPeerRequest& request,
                    AddBredrPeerCompleter::Sync& completer) override;
  void WatchControllerParameters(
      WatchControllerParametersCompleter::Sync& completer) override;
  void WatchLeScanStates(WatchLeScanStatesCompleter::Sync& completer) override;
  void WatchLegacyAdvertisingStates(
      WatchLegacyAdvertisingStatesCompleter::Sync& completer) override;
  void handle_unknown_method(
      fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::Emulator>
          metadata,
      fidl::UnknownMethodCompleter::Sync& completer) override;

  // fuchsia_hardware_bluetooth::Vendor overrides:
  void GetFeatures(GetFeaturesCompleter::Sync& completer) override;
  void EncodeCommand(EncodeCommandRequestView request,
                     EncodeCommandCompleter::Sync& completer) override;
  void OpenHci(OpenHciCompleter::Sync& completer) override {}
  void OpenHciTransport(OpenHciTransportCompleter::Sync& completer) override;
  void OpenSnoop(OpenSnoopCompleter::Sync& completer) override;
  void GetCrashParameters(
      GetCrashParametersCompleter::Sync& completer) override;
  void handle_unknown_method(
      fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::Vendor> metadata,
      fidl::UnknownMethodCompleter::Sync& completer) override;

  // Server<HciTransport> overrides:
  void Send(SendRequest& request, SendCompleter::Sync& completer) override;
  void AckReceive(AckReceiveCompleter::Sync& completer) override {}
  void ConfigureSco(ConfigureScoRequest& request,
                    fidl::Server<fuchsia_hardware_bluetooth::HciTransport>::
                        ConfigureScoCompleter::Sync& completer) override;
  void handle_unknown_method(
      ::fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::HciTransport>
          metadata,
      ::fidl::UnknownMethodCompleter::Sync& completer) override;

  void ConnectEmulator(
      fidl::ServerEnd<fuchsia_hardware_bluetooth::Emulator> request);
  void ConnectVendor(
      fidl::ServerEnd<fuchsia_hardware_bluetooth::Vendor> request);

  // Helper function for fuchsia.hardware.bluetooth.Emulator.Publish that adds
  // bt-hci-device as a child of EmulatorDevice device node
  zx_status_t AddHciDeviceChildNode();

  // Helper function used to initialize BR/EDR and LE peers
  void AddPeer(std::unique_ptr<EmulatedPeer> peer);

  // Event handlers
  void OnControllerParametersChanged();
  void MaybeUpdateControllerParametersChanged();
  void OnLegacyAdvertisingStateChanged();
  void MaybeUpdateLegacyAdvertisingStates();

  // Remove bt-hci-device node
  void UnpublishHci();

  void OnPeerConnectionStateChanged(const bt::DeviceAddress& address,
                                    bt::hci_spec::ConnectionHandle handle,
                                    bool connected,
                                    bool canceled);

  void SendEventToHost(pw::span<const std::byte> buffer);
  void SendAclPacketToHost(pw::span<const std::byte> buffer);
  void SendIsoPacketToHost(pw::span<const std::byte> buffer);

  pw::random_fuchsia::ZirconRandomGenerator rng_;

  // Responsible for running the thread-hostile |fake_device_|
  pw::async_fuchsia::FuchsiaDispatcher pw_dispatcher_;

  bt::testing::FakeController fake_device_;

  // List of active peers that have been registered with us
  std::unordered_map<bt::DeviceAddress, std::unique_ptr<EmulatedPeer>> peers_;

  ShutdownCallback shutdown_cb_;

  std::optional<fuchsia_hardware_bluetooth::ControllerParameters>
      controller_parameters_;
  std::optional<WatchControllerParametersCompleter::Async>
      controller_parameters_completer_;

  std::vector<fuchsia_hardware_bluetooth::LegacyAdvertisingState>
      legacy_adv_states_;
  std::queue<WatchLegacyAdvertisingStatesCompleter::Async>
      legacy_adv_states_completers_;

  // HciTransport protocol
  fidl::ServerBindingGroup<fuchsia_hardware_bluetooth::HciTransport>
      hci_transport_bindings_;

  // EmulatorDevice
  fidl::WireClient<fuchsia_driver_framework::Node> emulator_child_node_;
  driver_devfs::Connector<fuchsia_hardware_bluetooth::Emulator>
      emulator_devfs_connector_;
  fidl::ServerBindingGroup<fuchsia_hardware_bluetooth::Emulator>
      emulator_binding_group_;
  driver_devfs::Connector<fuchsia_hardware_bluetooth::Vendor>
      vendor_devfs_connector_;
  fidl::ServerBindingGroup<fuchsia_hardware_bluetooth::Vendor>
      vendor_binding_group_;
  std::unique_ptr<EmulatorDevice> emulator_ptr_;

  // bt-hci-device
  fidl::WireClient<fuchsia_driver_framework::NodeController>
      hci_node_controller_;
  fidl::WireClient<fuchsia_driver_framework::Node> hci_child_node_;
};

}  // namespace bt_hci_virtual
