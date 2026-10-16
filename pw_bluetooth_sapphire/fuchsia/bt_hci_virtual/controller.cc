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

#include "controller.h"

#include <lib/driver/component/cpp/driver_export2.h>
#include <lib/driver/logging/cpp/logger.h>
#include <pw_assert/check.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bt_hci_virtual {

VirtualController::VirtualController()
    : DriverBase2("bt_hci_virtual"),
      devfs_connector_(fit::bind_member<&VirtualController::Connect>(this)) {}

zx::result<> VirtualController::Start(fdf::DriverContext context) {
  node_ = fidl::WireClient(take_node(), dispatcher());
  pw::log_fuchsia::InitializeLogging(dispatcher());
  zx::result connector = devfs_connector_.Bind(dispatcher());
  if (connector.is_error()) {
    fdf::error("Failed to bind devfs connecter to dispatcher: {}", connector);
    return connector.take_error();
  }

  fidl::Arena args_arena;
  // TODO: https://pwbug.dev/303503457 - Access virtual device via
  // "/dev/class/bt-hci-virtual"
  auto devfs = fuchsia_driver_framework::wire::DevfsAddArgs::Builder(args_arena)
                   .connector(std::move(connector.value()))
                   .class_name("sys/platform/bt-hci-emulator")
                   .Build();
  auto args = fuchsia_driver_framework::wire::NodeAddArgs::Builder(args_arena)
                  .name(fidl::StringView::FromExternal(kNodeName))
                  .devfs_args(devfs)
                  .Build();

  // Add bt_hci_virtual child node
  AddVirtualControllerChildNode(args);

  return zx::ok();
}

void VirtualController::CreateEmulator(
    CreateEmulatorCompleter::Sync& completer) {
  // Every call creates a new, independently owned emulator. The unique name
  // identifies it as the devfs node name, the vendor service instance name,
  // and the value returned to the caller, which uses it to find its emulator
  // (and nobody else's).
  std::string name =
      std::string(kEmulatorNamePrefix) + std::to_string(next_emulator_id_++);
  auto [it, inserted] = emulators_.try_emplace(name);
  PW_CHECK(inserted);
  Emulator& emulator = it->second;
  emulator.device = std::make_unique<EmulatorDevice>();

  auto add_child_cb = [this, name](auto args) {
    auto entry = emulators_.find(name);
    PW_CHECK(entry != emulators_.end(),
             "AddChildCallback ran for unknown emulator %s",
             name.c_str());
    fdf::info("EmulatorDevice successfully initialized");
    return AddEmulatorChildNode(args, entry->second);
  };
  auto shutdown_cb = [this, name]() {
    auto entry = emulators_.find(name);
    PW_CHECK(entry != emulators_.end(),
             "ShutdownCallback ran for unknown emulator %s",
             name.c_str());
    fdf::info("Releasing EmulatorDevice {}", name);
    // Remove the emulator's node. When the shutdown was triggered by the
    // owning Emulator connection closing, this is what tears the node down.
    // When it was triggered by the framework removing the node (devfs unbind)
    // the peer is already gone, which one-way calls report as success, or as
    // canceled if the client had already observed that and unbound; during
    // driver stop the client is being torn down with the dispatcher.
    fidl::WireClient<fuchsia_driver_framework::NodeController>&
        node_controller = entry->second.node_controller;
    if (node_controller.is_valid()) {
      fidl::OneWayStatus result = node_controller->Remove();
      if (!result.ok() && !result.is_canceled() &&
          !result.is_dispatcher_shutdown()) {
        fdf::warn("Failed to remove emulator node {}: {}",
                  name,
                  result.status_string());
      }
    }
    emulators_.erase(entry);
  };

  // A failure to add the node is reported too: without one the caller could
  // never find the emulator, so it must not get a name that will never appear.
  zx_status_t status = emulator.device->Initialize(name,
                                                   std::move(add_child_cb),
                                                   std::move(shutdown_cb),
                                                   outgoing().get(),
                                                   name);
  if (status != ZX_OK) {
    fdf::error(
        "Failed to create emulator {}: {}", name, zx_status_get_string(status));
    // Shutdown() invokes `shutdown_cb`, which removes the node if one was
    // added and erases the entry (destroying the device).
    emulator.device->Shutdown();
    completer.ReplyError(status);
    return;
  }
  completer.ReplySuccess(
      fidl::StringView::FromExternal(name.data(), name.size()));
}

void VirtualController::CreateLoopbackDevice(
    CreateLoopbackDeviceRequestView request,
    CreateLoopbackDeviceCompleter::Sync& completer) {
  std::string name = "loopback";

  loopback_device_ = std::make_unique<LoopbackDevice>();

  PW_CHECK(request->has_uart_channel());
  zx_status_t status = loopback_device_->Initialize(
      std::move(request->uart_channel()),
      name,
      [this](auto args) {
        // Add LoopbackDevice as a child node of VirtualController
        fdf::info("LoopbackDevice successfully initialized");
        return AddLoopbackChildNode(args);
      },
      outgoing().get());
  if (status != ZX_OK) {
    fdf::error("Failed to bind: {}", zx_status_get_string(status));
    if (loopback_node_controller_.is_valid()) {
      auto _ = loopback_node_controller_->Remove();
    }
    loopback_device_.reset();
    return;
  }
}

void VirtualController::handle_unknown_method(
    fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::VirtualController>
        metadata,
    fidl::UnknownMethodCompleter::Sync& completer) {
  fdf::error(
      "Unknown method in VirtualController request, closing with "
      "ZX_ERR_NOT_SUPPORTED");
  completer.Close(ZX_ERR_NOT_SUPPORTED);
}

void VirtualController::Connect(
    fidl::ServerEnd<fuchsia_hardware_bluetooth::VirtualController> request) {
  virtual_controller_binding_group_.AddBinding(
      dispatcher(), std::move(request), this, fidl::kIgnoreBindingClosure);
}

zx_status_t VirtualController::AddVirtualControllerChildNode(
    fuchsia_driver_framework::wire::NodeAddArgs args) {
  // Create the endpoints of fuchsia_driver_framework::NodeController protocol
  auto controller_endpoints =
      fidl::CreateEndpoints<fuchsia_driver_framework::NodeController>();
  if (controller_endpoints.is_error()) {
    fdf::error("Create node controller endpoints failed: {}",
               controller_endpoints);
    return controller_endpoints.error_value();
  }

  // Create the endpoints of fuchsia_driver_framework::Node protocol for the
  // child node, and hold the client end of it, because no driver will bind to
  // the child node.
  auto child_node_endpoints =
      fidl::CreateEndpoints<fuchsia_driver_framework::Node>();
  if (child_node_endpoints.is_error()) {
    fdf::error("Create child node endpoints failed: {}", child_node_endpoints);
    return child_node_endpoints.error_value();
  }

  // Add a VirtualController child node
  auto child_result =
      node_.sync()->AddChild(std::move(args),
                             std::move(controller_endpoints->server),
                             std::move(child_node_endpoints->server));
  if (!child_result.ok()) {
    fdf::error("Failed to add bt_hci_virtual child node, FIDL error: {}",
               child_result.status_string());
    return child_result.status();
  }
  if (child_result->is_error()) {
    fdf::error("Failed to add bt_hci_virtual child node: {}",
               static_cast<uint32_t>(child_result->error_value()));
    return ZX_ERR_INTERNAL;
  }

  virtual_controller_child_node_.Bind(
      std::move(child_node_endpoints->client), dispatcher(), this);
  node_controller_.Bind(
      std::move(controller_endpoints->client), dispatcher(), this);

  return ZX_OK;
}

zx_status_t VirtualController::AddLoopbackChildNode(
    fuchsia_driver_framework::wire::NodeAddArgs args) {
  // Create the endpoints of fuchsia_driver_framework::NodeController protocol
  auto controller_endpoints =
      fidl::CreateEndpoints<fuchsia_driver_framework::NodeController>();
  if (controller_endpoints.is_error()) {
    fdf::error("Create node controller endpoints failed: {}",
               controller_endpoints);
    return controller_endpoints.error_value();
  }

  // Create the endpoints of fuchsia_driver_framework::Node protocol for the
  // child node, and hold the client end of it, because no driver will bind to
  // the child node.
  auto child_node_endpoints =
      fidl::CreateEndpoints<fuchsia_driver_framework::Node>();
  if (child_node_endpoints.is_error()) {
    fdf::error("Create child node endpoints failed: {}", child_node_endpoints);
    return child_node_endpoints.error_value();
  }

  // Add a loopback device as a child node of VirtualController
  auto child_result =
      node_.sync()->AddChild(std::move(args),
                             std::move(controller_endpoints->server),
                             std::move(child_node_endpoints->server));
  if (!child_result.ok()) {
    fdf::error("Failed to add loopback device node, FIDL error: {}",
               child_result.status_string());
    return child_result.status();
  }

  if (child_result->is_error()) {
    fdf::error("Failed to add loopback device node: {}",
               static_cast<uint32_t>(child_result->error_value()));
    return ZX_ERR_INTERNAL;
  }

  // |loopback_child_node_| does not need to create more child nodes so we do
  // not need an event_handler and we do not need to worry about it being
  // re-bound
  loopback_child_node_.Bind(std::move(child_node_endpoints->client),
                            dispatcher());
  loopback_node_controller_.Bind(
      std::move(controller_endpoints->client), dispatcher(), this);

  return ZX_OK;
}

zx_status_t VirtualController::AddEmulatorChildNode(
    fuchsia_driver_framework::wire::NodeAddArgs args, Emulator& emulator) {
  // Create the endpoints of fuchsia_driver_framework::NodeController protocol
  auto controller_endpoints =
      fidl::CreateEndpoints<fuchsia_driver_framework::NodeController>();
  if (controller_endpoints.is_error()) {
    fdf::error("Create node controller endpoints failed: {}",
               controller_endpoints);
    return controller_endpoints.error_value();
  }

  // Create the endpoints of fuchsia_driver_framework::Node protocol for the
  // child node, and hold the client end of it, because no driver will bind to
  // the child node.
  auto child_node_endpoints =
      fidl::CreateEndpoints<fuchsia_driver_framework::Node>();
  if (child_node_endpoints.is_error()) {
    fdf::error("Create child node endpoints failed: {}", child_node_endpoints);
    return child_node_endpoints.error_value();
  }

  // Add an emulator device as a child node of VirtualController
  auto child_result =
      node_.sync()->AddChild(std::move(args),
                             std::move(controller_endpoints->server),
                             std::move(child_node_endpoints->server));

  if (!child_result.ok()) {
    fdf::error("Failed to add emulator device node, FIDL error: {}",
               child_result.status_string());
    return child_result.status();
  }

  if (child_result->is_error()) {
    fdf::error("Failed to add emulator device node: {}",
               static_cast<uint32_t>(child_result->error_value()));
    return ZX_ERR_INTERNAL;
  }

  fidl::WireClient<fuchsia_driver_framework::Node> child_node(
      std::move(child_node_endpoints->client),
      dispatcher(),
      emulator.device.get());
  emulator.device->set_emulator_child_node(std::move(child_node));
  emulator.node_controller.Bind(
      std::move(controller_endpoints->client), dispatcher(), this);

  return ZX_OK;
}

}  // namespace bt_hci_virtual

FUCHSIA_DRIVER_EXPORT2(bt_hci_virtual::VirtualController);
