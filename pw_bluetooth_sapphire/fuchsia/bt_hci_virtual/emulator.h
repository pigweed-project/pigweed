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

#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <lib/async/cpp/wait.h>
#include <lib/async/dispatcher.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <pw_async/task.h>
#include <pw_async_fuchsia/dispatcher.h>

#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>

#include "emulated_peer.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "vendor_service_publisher.h"

namespace bt_hci_virtual {

enum class ChannelType : uint8_t { ACL, COMMAND, EMULATOR, ISO, SNOOP };

using ShutdownCallback = fit::function<void()>;

class EmulatorDevice
    : public fidl::Server<fuchsia_hardware_bluetooth::Emulator>,
      public fidl::Server<fuchsia_hardware_bluetooth::HciTransport>,
      public fidl::WireServer<fuchsia_hardware_bluetooth::Vendor> {
 public:
  // All FIDL servers and the emulated controller run on `dispatcher`.
  explicit EmulatorDevice(async_dispatcher_t* dispatcher);
  ~EmulatorDevice();

  EmulatorDevice(const EmulatorDevice&) = delete;
  EmulatorDevice& operator=(const EmulatorDevice&) = delete;
  EmulatorDevice(EmulatorDevice&&) = delete;
  EmulatorDevice& operator=(EmulatorDevice&&) = delete;

  // Methods used by the VirtualController to control the EmulatorDevice's
  // lifecycle.
  //
  // `name` identifies this emulator: if `outgoing` is non-null, `Initialize()`
  // publishes the fuchsia.hardware.bluetooth.EmulatorService instance `name`
  // to it, and `Publish` later publishes the fuchsia.hardware.bluetooth.Service
  // instance `name`. `outgoing` must already be served and must outlive this
  // object. `shutdown` is invoked by `Shutdown()`.
  zx_status_t Initialize(std::string_view name,
                         ShutdownCallback shutdown,
                         component::OutgoingDirectory* outgoing = nullptr);
  void Shutdown();

  // Serves the fuchsia.hardware.bluetooth.Emulator protocol on `request`.
  //
  // The first connection accepted by an EmulatorDevice is its *owner*: the
  // lifetime of the emulator is tied to that connection. When the owner
  // disconnects, the EmulatorDevice calls `Shutdown()`, which unpublishes the
  // HCI vendor service, closes all other Emulator connections, and invokes the
  // ShutdownCallback passed to `Initialize()` (in the VirtualController, that
  // callback destroys this object). This gives clients a way to destroy an
  // emulator that does not depend on devfs. Later connections are observers
  // and may come and go without affecting the emulator's lifetime.
  //
  // NOTE: ownership is inferred from connection order. A client that merely
  // opens and closes the Emulator protocol before the intended owner connects
  // becomes the owner and tears the emulator down when it disconnects.
  void ConnectEmulator(
      fidl::ServerEnd<fuchsia_hardware_bluetooth::Emulator> request);

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

  void ConnectVendor(
      fidl::ServerEnd<fuchsia_hardware_bluetooth::Vendor> request);

  // Called when the owning Emulator connection (see `ConnectEmulator`) closes.
  void OnEmulatorOwnerClosed(fidl::UnbindInfo info);

  // Helper function used to initialize BR/EDR and LE peers
  void AddPeer(std::unique_ptr<EmulatedPeer> peer);

  // Event handlers
  void OnControllerParametersChanged();
  void MaybeUpdateControllerParametersChanged();
  void OnLegacyAdvertisingStateChanged();
  void MaybeUpdateLegacyAdvertisingStates();

  // Unpublish fuchsia.hardware.bluetooth.Service
  void UnpublishHci();

  zx_status_t PublishEmulatorService();
  void UnpublishEmulatorService();

  void OnPeerConnectionStateChanged(const bt::DeviceAddress& address,
                                    bt::hci_spec::ConnectionHandle handle,
                                    bool connected,
                                    bool canceled);

  void SendEventToHost(pw::span<const std::byte> buffer);
  void SendAclPacketToHost(pw::span<const std::byte> buffer);
  void SendIsoPacketToHost(pw::span<const std::byte> buffer);

  // Responsible for running the thread-hostile `fake_device_`
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
  fidl::ServerBindingGroup<fuchsia_hardware_bluetooth::Emulator>
      emulator_binding_group_;
  // True once the owning Emulator connection has been accepted (see
  // `ConnectEmulator`).
  bool emulator_owner_connected_ = false;
  // Set by `Shutdown()` so that a second call is a no-op when the
  // ShutdownCallback does not destroy this object (e.g. none was provided).
  bool shut_down_ = false;
  // Shutdown triggered by the owner disconnecting is deferred to a separate
  // dispatcher task: `Shutdown()` destroys this object (and with it
  // `emulator_binding_group_`), which must not happen from within a binding's
  // close handler. The task is cancelled in `~EmulatorDevice()` if this object
  // is destroyed first.
  pw::async::Task shutdown_task_{
      [this](pw::async::Context&, pw::Status status) {
        if (status.ok()) {
          Shutdown();
        }
      }};
  fidl::ServerBindingGroup<fuchsia_hardware_bluetooth::Vendor>
      vendor_binding_group_;

  component::OutgoingDirectory* outgoing_ = nullptr;
  std::string name_;
  // True while the EmulatorService instance `name_` is in `outgoing_`.
  bool emulator_service_published_ = false;
  VendorServicePublisher vendor_service_;
};

}  // namespace bt_hci_virtual
