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

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/emulator.h"
#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/loopback.h"
#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/virtual_controller.h"

namespace bt_hci_virtual {

// DFv2 driver that hosts a `VirtualController`, serves it via devfs at
// `kNodeName`, and gives each device the controller creates its own child node.
class VirtualControllerDriver
    : public fdf::DriverBase2,
      public fidl::WireAsyncEventHandler<
          fuchsia_driver_framework::NodeController>,
      public fidl::WireAsyncEventHandler<fuchsia_driver_framework::Node> {
 public:
  // Name of the devfs node through which `VirtualController` is reached.
  static constexpr std::string_view kNodeName = "bt_hci_virtual";

  VirtualControllerDriver();

  // fdf::DriverBase overrides:
  zx::result<> Start(fdf::DriverContext context) override;

  void handle_unknown_event(
      fidl::UnknownEventMetadata<fuchsia_driver_framework::Node> metadata)
      override {}
  void handle_unknown_event(
      fidl::UnknownEventMetadata<fuchsia_driver_framework::NodeController>
          metadata) override {}

 private:
  // The driver-framework state of one emulator's node.
  struct EmulatorNode final
      : public fidl::WireAsyncEventHandler<fuchsia_driver_framework::Node> {
    explicit EmulatorNode(EmulatorDevice& emulator_device)
        : device(emulator_device),
          connector(fit::bind_member<&EmulatorDevice::ConnectEmulator>(
              &emulator_device)) {}

    // Shuts the emulator down when its node is removed by the driver framework
    // (e.g. via devfs `fuchsia.device.Controller/ScheduleUnbind`). This
    // destroys the node's clients (including the one calling this), so nothing
    // may be touched afterwards.
    void on_fidl_error(fidl::UnbindInfo) override { device.Shutdown(); }
    void handle_unknown_event(
        fidl::UnknownEventMetadata<fuchsia_driver_framework::Node> metadata)
        override {}

    // Valid until `RemoveEmulatorNode` erases this entry (see
    // `VirtualController::NodeHooks::remove_emulator_node`).
    EmulatorDevice& device;
    driver_devfs::Connector<fuchsia_hardware_bluetooth::Emulator> connector;
    // Bound with the enclosing `EmulatorNode` as its event handler, so entries
    // of `emulator_nodes_` must never move.
    fidl::WireClient<fuchsia_driver_framework::Node> node;
    fidl::WireClient<fuchsia_driver_framework::NodeController> node_controller;
  };

  // `VirtualController::NodeHooks`:
  zx_status_t AddEmulatorNode(const std::string& name, EmulatorDevice& device);
  void RemoveEmulatorNode(const std::string& name);
  zx_status_t AddLoopbackNode(const std::string& name, LoopbackDevice&);
  void RemoveLoopbackNode(const std::string& name);
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

  // The hosted controller; emplaced by `Start()`.
  std::optional<VirtualController> controller_;

  // Nodes of the live emulators, keyed by the emulator's name. Declared after
  // `controller_` so that they are destroyed before the devices they refer to.
  // A std::map so that entries never move (see `EmulatorNode::node`).
  std::map<std::string, EmulatorNode> emulator_nodes_;

  // VirtualController
  fidl::WireClient<fuchsia_driver_framework::Node> node_;
  fidl::WireClient<fuchsia_driver_framework::NodeController> node_controller_;
  fidl::WireClient<fuchsia_driver_framework::Node>
      virtual_controller_child_node_;
  driver_devfs::Connector<fuchsia_hardware_bluetooth::VirtualController>
      devfs_connector_;

  // LoopbackDevice
  fidl::WireClient<fuchsia_driver_framework::NodeController>
      loopback_node_controller_;
  fidl::WireClient<fuchsia_driver_framework::Node> loopback_child_node_;
};

}  // namespace bt_hci_virtual
