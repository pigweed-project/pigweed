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

#include <string>
#include <string_view>
#include <utility>

#include "pw_log_fuchsia/log_backend.h"

namespace bt_hci_virtual {

VirtualControllerDriver::VirtualControllerDriver()
    : DriverBase2("bt_hci_virtual"), devfs_connector_([this](auto request) {
        controller_->Connect(std::move(request));
      }) {}

zx::result<> VirtualControllerDriver::Start(fdf::DriverContext context) {
  node_ = fidl::WireClient(take_node(), dispatcher());
  pw::log_fuchsia::InitializeLogging(dispatcher());
  controller_.emplace(
      dispatcher(),
      &outgoing()->component(),
      VirtualController::NodeHooks{
          .add_emulator_node =
              fit::bind_member<&VirtualControllerDriver::AddEmulatorNode>(this),
          .remove_emulator_node =
              fit::bind_member<&VirtualControllerDriver::RemoveEmulatorNode>(
                  this),
          .add_loopback_node =
              fit::bind_member<&VirtualControllerDriver::AddLoopbackNode>(this),
          .remove_loopback_node =
              fit::bind_member<&VirtualControllerDriver::RemoveLoopbackNode>(
                  this),
      });

  zx::result connector = devfs_connector_.Bind(dispatcher());
  if (connector.is_error()) {
    fdf::error("Failed to bind devfs connector to dispatcher: {}", connector);
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
  zx_status_t status = AddChildNode(args,
                                    "bt_hci_virtual child node",
                                    virtual_controller_child_node_,
                                    node_controller_,
                                    this);
  if (status != ZX_OK) {
    return zx::error(status);
  }

  return zx::ok();
}

zx_status_t VirtualControllerDriver::AddEmulatorNode(const std::string& name,
                                                     EmulatorDevice& device) {
  auto [it, inserted] = emulator_nodes_.try_emplace(name, device);
  PW_CHECK(inserted, "Node for emulator %s already exists", name.c_str());
  EmulatorNode& emulator = it->second;

  zx::result connector = emulator.connector.Bind(dispatcher());
  if (connector.is_error()) {
    fdf::error("Failed to bind devfs connector to dispatcher: {}", connector);
    emulator_nodes_.erase(it);
    return connector.error_value();
  }

  fidl::Arena args_arena;
  auto devfs =
      fuchsia_driver_framework::wire::DevfsAddArgs::Builder(args_arena)
          .connector(std::move(connector.value()))
          .connector_supports(fuchsia_device_fs::ConnectionType::kController)
          .class_name("bt-emulator")
          .Build();
  auto args = fuchsia_driver_framework::wire::NodeAddArgs::Builder(args_arena)
                  .name(fidl::StringView(args_arena, name))
                  .devfs_args(devfs)
                  .Build();

  zx_status_t status = AddChildNode(args,
                                    "emulator device node",
                                    emulator.node,
                                    emulator.node_controller,
                                    &emulator);
  if (status != ZX_OK) {
    emulator_nodes_.erase(it);
    return status;
  }
  fdf::info("EmulatorDevice {} successfully initialized", name);
  return ZX_OK;
}

void VirtualControllerDriver::RemoveEmulatorNode(const std::string& name) {
  auto entry = emulator_nodes_.find(name);
  if (entry == emulator_nodes_.end()) {
    // The node was never added (`AddEmulatorNode` failed).
    return;
  }
  fdf::info("Removing emulator node {}", name);
  // When the shutdown was triggered by the owning Emulator connection closing,
  // this is what tears the node down.
  RemoveNode(entry->second.node, entry->second.node_controller);
  emulator_nodes_.erase(entry);
}

zx_status_t VirtualControllerDriver::AddLoopbackNode(const std::string& name,
                                                     LoopbackDevice&) {
  fidl::Arena args_arena;
  auto args = fuchsia_driver_framework::wire::NodeAddArgs::Builder(args_arena)
                  .name(fidl::StringView(args_arena, name))
                  .Build();
  // The loopback node has no children, so it needs no event handler.
  zx_status_t status = AddChildNode(args,
                                    "loopback device node",
                                    loopback_child_node_,
                                    loopback_node_controller_,
                                    nullptr);
  if (status != ZX_OK) {
    return status;
  }
  fdf::info("Loopback node {} added", name);
  return ZX_OK;
}

void VirtualControllerDriver::RemoveLoopbackNode(const std::string& name) {
  if (loopback_node_controller_.is_valid()) {
    fdf::info("Removing loopback node {}", name);
  }
  RemoveNode(loopback_child_node_, loopback_node_controller_);
}

zx_status_t VirtualControllerDriver::AddChildNode(
    fuchsia_driver_framework::wire::NodeAddArgs args,
    std::string_view log_name,
    fidl::WireClient<fuchsia_driver_framework::Node>& child_node,
    fidl::WireClient<fuchsia_driver_framework::NodeController>& node_controller,
    fidl::WireAsyncEventHandler<fuchsia_driver_framework::Node>*
        node_event_handler) {
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

  auto child_result =
      node_.sync()->AddChild(std::move(args),
                             std::move(controller_endpoints->server),
                             std::move(child_node_endpoints->server));
  if (!child_result.ok()) {
    fdf::error("Failed to add {}, FIDL error: {}",
               log_name,
               child_result.status_string());
    return child_result.status();
  }
  if (child_result->is_error()) {
    fdf::error("Failed to add {}: {}",
               log_name,
               static_cast<uint32_t>(child_result->error_value()));
    return ZX_ERR_INTERNAL;
  }

  child_node.Bind(std::move(child_node_endpoints->client),
                  dispatcher(),
                  node_event_handler);
  node_controller.Bind(
      std::move(controller_endpoints->client), dispatcher(), this);

  return ZX_OK;
}

void VirtualControllerDriver::RemoveNode(
    fidl::WireClient<fuchsia_driver_framework::Node>& child_node,
    fidl::WireClient<fuchsia_driver_framework::NodeController>&
        node_controller) {
  if (node_controller.is_valid()) {
    fidl::OneWayStatus result = node_controller->Remove();

    // If the framework already removed the node (e.g. devfs unbind) and the
    // client has already processed the peer-closed event and unbound,
    // `Remove()` returns `is_canceled()`. (If the client hasn't unbound yet, a
    // one-way write to a closed peer channel still reports `ok()`.)
    bool already_unbound = result.is_canceled();
    // If we're tearing down during driver stop, the dispatcher is shutting
    // down.
    bool driver_stopping = result.is_dispatcher_shutdown();

    if (!result.ok() && !already_unbound && !driver_stopping) {
      fdf::warn("Failed to remove child node: {}", result.status_string());
    }
  }

  // Drop the clients so that the next node can be bound; `Bind()` on an
  // already-bound client asserts.
  node_controller = {};
  child_node = {};
}

}  // namespace bt_hci_virtual

FUCHSIA_DRIVER_EXPORT2(bt_hci_virtual::VirtualControllerDriver);
