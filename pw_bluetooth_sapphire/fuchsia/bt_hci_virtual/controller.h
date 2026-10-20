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
#include <lib/driver/component/cpp/driver_base2.h>
#include <lib/driver/devfs/cpp/connector.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/emulator.h"
#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/loopback.h"

namespace bt_hci_virtual {

// The VirtualController class implements the
// fuchsia.hardware.bluetooth.VirtualController API. It is used to create two
// kinds of devices: any number of EmulatorDevices (one per `CreateEmulator`
// call, each with a unique name and independently owned by its first Emulator
// connection) and/or a LoopbackDevice. The EmulatorDevice is used for Bluetooth
// integration tests, and the LoopbackDevice is used by RootCanal for PTS-bot.
//
// VirtualController publishes itself as a DFv2 driver and adds a child node
// (`bt_hci_virtual`) that exposes the VirtualController protocol via devfs.
// Each EmulatorDevice/LoopbackDevice it creates gets its own child node of the
// driver's node (`node_`), a sibling of `bt_hci_virtual`. Emulator nodes also
// expose the Emulator protocol via devfs.
//
// EmulatorDevice/LoopbackDevice then implements and serves the FIDL protocols
// that their client needs. For more details, refer to
// go/bluetooth-virtual-driver-doc.
class VirtualController
    : public fdf::DriverBase2,
      public fidl::WireAsyncEventHandler<
          fuchsia_driver_framework::NodeController>,
      public fidl::WireAsyncEventHandler<fuchsia_driver_framework::Node>,
      public fidl::WireServer<fuchsia_hardware_bluetooth::VirtualController> {
 public:
  // Name of the devfs node through which VirtualController is reached.
  static constexpr std::string_view kNodeName = "bt_hci_virtual";

  // Prefix of the unique name `CreateEmulator` gives each emulator.
  static constexpr std::string_view kEmulatorNamePrefix = "emulator-";

  // Name `CreateLoopbackDevice` gives the loopback device.
  static constexpr std::string_view kLoopbackName = "loopback";

  VirtualController();

  // fdf::DriverBase overrides:
  zx::result<> Start(fdf::DriverContext context) override;

  void handle_unknown_event(
      fidl::UnknownEventMetadata<fuchsia_driver_framework::Node> metadata)
      override {}
  void handle_unknown_event(
      fidl::UnknownEventMetadata<fuchsia_driver_framework::NodeController>
          metadata) override {}

 private:
  // One live emulator: its device and the driver-framework state of its node.
  struct Emulator final
      : public fidl::WireAsyncEventHandler<fuchsia_driver_framework::Node> {
    explicit Emulator(std::unique_ptr<EmulatorDevice> dev)
        : device(std::move(dev)),
          connector(fit::bind_member<&EmulatorDevice::ConnectEmulator>(
              device.get())) {}

    // Shuts the emulator down when its node is removed by the driver framework
    // (e.g. via devfs `fuchsia.device.Controller/ScheduleUnbind`). This
    // destroys the node's clients (including the one calling this) and then
    // the device, so nothing may be touched afterwards.
    void on_fidl_error(fidl::UnbindInfo) override { device->Shutdown(); }
    void handle_unknown_event(
        fidl::UnknownEventMetadata<fuchsia_driver_framework::Node> metadata)
        override {}

    std::unique_ptr<EmulatorDevice> device;
    // Declared after `device`, which they refer to, so that they are destroyed
    // first.
    driver_devfs::Connector<fuchsia_hardware_bluetooth::Emulator> connector;
    // Bound with the enclosing `Emulator` as its event handler, so entries of
    // `emulators_` must never move.
    fidl::WireClient<fuchsia_driver_framework::Node> node;
    fidl::WireClient<fuchsia_driver_framework::NodeController> node_controller;
  };

  // fuchsia_hardware_bluetooth::VirtualController overrides:
  void CreateEmulator(CreateEmulatorCompleter::Sync& completer) override;
  void CreateLoopbackDevice(
      CreateLoopbackDeviceRequestView request,
      CreateLoopbackDeviceCompleter::Sync& completer) override;
  void handle_unknown_method(
      fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::VirtualController>
          metadata,
      fidl::UnknownMethodCompleter::Sync& completer) override;

  void Connect(
      fidl::ServerEnd<fuchsia_hardware_bluetooth::VirtualController> request);

  // Adds the devfs node of the emulator `name` (serving its Emulator
  // protocol).
  zx_status_t AddEmulatorNode(const std::string& name, Emulator& emulator);
  // Adds a child node of `node_` with `args` and binds `child_node` (with
  // `node_event_handler`) and `node_controller` to it. `log_name` names the
  // node in log messages.
  zx_status_t AddChildNode(
      fuchsia_driver_framework::wire::NodeAddArgs args,
      std::string_view log_name,
      fidl::WireClient<fuchsia_driver_framework::Node>& child_node,
      fidl::WireClient<fuchsia_driver_framework::NodeController>&
          node_controller,
      fidl::WireAsyncEventHandler<fuchsia_driver_framework::Node>*
          node_event_handler);
  // Removes the node `node_controller` controls, if it was ever added, and
  // resets `child_node` and `node_controller`.
  void RemoveNode(fidl::WireClient<fuchsia_driver_framework::Node>& child_node,
                  fidl::WireClient<fuchsia_driver_framework::NodeController>&
                      node_controller);

  // Live emulators keyed by the unique name returned from `CreateEmulator`.
  // Each entry is erased by its own ShutdownCallback (see `CreateEmulator`).
  // A std::map so that entries never move (see `Emulator::node`).
  std::map<std::string, Emulator> emulators_;
  uint64_t next_emulator_id_ = 0;

  std::unique_ptr<LoopbackDevice> loopback_device_;

  // VirtualController
  fidl::WireClient<fuchsia_driver_framework::Node> node_;
  fidl::WireClient<fuchsia_driver_framework::NodeController> node_controller_;
  fidl::WireClient<fuchsia_driver_framework::Node>
      virtual_controller_child_node_;
  driver_devfs::Connector<fuchsia_hardware_bluetooth::VirtualController>
      devfs_connector_;
  fidl::ServerBindingGroup<fuchsia_hardware_bluetooth::VirtualController>
      virtual_controller_binding_group_;

  // LoopbackDevice
  fidl::WireClient<fuchsia_driver_framework::NodeController>
      loopback_node_controller_;
  fidl::WireClient<fuchsia_driver_framework::Node> loopback_child_node_;
};

}  // namespace bt_hci_virtual
