// Copyright 2026 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/virtual_controller.h"

#include <pw_assert/check.h>
#include <zircon/status.h>

#include <memory>
#include <string>
#include <utility>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"

namespace bt_hci_virtual {

namespace fhbt = fuchsia_hardware_bluetooth;

namespace {

// ID of the first emulator a new controller creates.
constexpr uint64_t kFirstEmulatorId = 0;

// Returns the name of the emulator with `id`.
std::string EmulatorName(uint64_t id) {
  return std::string(VirtualController::kEmulatorNamePrefix) +
         std::to_string(id);
}

}  // namespace

VirtualController::VirtualController(async_dispatcher_t* dispatcher,
                                     component::OutgoingDirectory* outgoing,
                                     NodeHooks&& hooks)
    : dispatcher_(dispatcher),
      outgoing_(outgoing),
      hooks_(std::move(hooks)),
      next_emulator_id_(kFirstEmulatorId) {}

VirtualController::~VirtualController() {
  if (!service_published_) {
    return;
  }
  zx::result result = outgoing_->RemoveService(kServiceName);
  if (result.is_error()) {
    bt_log(WARN,
           "bt-hci-virtual",
           "Failed to remove VirtualControllerService: %s",
           result.status_string());
  }
}

void VirtualController::Connect(
    fidl::ServerEnd<fhbt::VirtualController> request) {
  bindings_.AddBinding(
      dispatcher_, std::move(request), this, fidl::kIgnoreBindingClosure);
}

std::string VirtualController::ServiceInstancePath() {
  return std::string(component::OutgoingDirectory::kServiceDirectory) + "/" +
         std::string(kServiceName) + "/" +
         component::OutgoingDirectory::kDefaultServiceInstance;
}

std::string VirtualController::FirstEmulatorName() {
  return EmulatorName(kFirstEmulatorId);
}

zx::result<> VirtualController::PublishService() {
  if (outgoing_ == nullptr) {
    return zx::error(ZX_ERR_BAD_STATE);
  }
  if (service_published_) {
    return zx::error(ZX_ERR_ALREADY_EXISTS);
  }
  // TODO: b/430578502 - Use `AddService<fhbt::VirtualControllerService>` once
  // the pinned SDK ships its bindings; until then the single instance is
  // published by path.
  zx::result<> result =
      outgoing_->AddUnmanagedProtocolAt<fhbt::VirtualController>(
          ServiceInstancePath(),
          fit::bind_member<&VirtualController::Connect>(this),
          kServiceMemberName);
  if (result.is_ok()) {
    service_published_ = true;
  }
  return result;
}

void VirtualController::CreateEmulator(
    CreateEmulatorCompleter::Sync& completer) {
  // Every call creates a new, independently owned emulator. Its unique name is
  // its service instance name and is returned to the caller, which uses it to
  // find its emulator (and nobody else's).
  std::string name = EmulatorName(next_emulator_id_++);
  auto [it, inserted] = emulators_.try_emplace(
      name, std::make_unique<EmulatorDevice>(dispatcher_));
  PW_CHECK(inserted);
  EmulatorDevice& device = *it->second;

  auto shutdown_cb = [this, name]() {
    auto entry = emulators_.find(name);
    PW_CHECK(entry != emulators_.end(),
             "ShutdownCallback ran for unknown emulator %s",
             name.c_str());
    bt_log(INFO, "bt-hci-virtual", "Releasing EmulatorDevice %s", name.c_str());
    if (hooks_.remove_emulator_node) {
      hooks_.remove_emulator_node(name);
    }
    emulators_.erase(entry);
  };

  zx_status_t status =
      device.Initialize(name, std::move(shutdown_cb), outgoing_);
  // A failure to add the node is reported too: without one the caller could
  // never find the emulator, so it must not get a name that will never appear.
  if (status == ZX_OK && hooks_.add_emulator_node) {
    status = hooks_.add_emulator_node(name, device);
  }
  if (status != ZX_OK) {
    bt_log(ERROR,
           "bt-hci-virtual",
           "Failed to create emulator %s: %s",
           name.c_str(),
           zx_status_get_string(status));
    // Shutdown() invokes `shutdown_cb`, which erases the entry (destroying the
    // device).
    device.Shutdown();
    completer.ReplyError(status);
    return;
  }
  completer.ReplySuccess(
      fidl::StringView::FromExternal(name.data(), name.size()));
}

void VirtualController::CreateLoopbackDevice(
    CreateLoopbackDeviceRequestView request,
    CreateLoopbackDeviceCompleter::Sync& completer) {
  std::string name(kLoopbackName);

  if (!request->has_uart_channel()) {
    bt_log(
        ERROR, "bt-hci-virtual", "CreateLoopbackDevice without uart_channel");
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  // Any previous device is replaced.
  DestroyLoopbackDevice();
  loopback_device_ = std::make_unique<LoopbackDevice>(dispatcher_);
  zx_status_t status = loopback_device_->Initialize(
      std::move(request->uart_channel()), name, outgoing_);
  if (status == ZX_OK && hooks_.add_loopback_node) {
    status = hooks_.add_loopback_node(name, *loopback_device_);
  }
  if (status != ZX_OK) {
    bt_log(ERROR,
           "bt-hci-virtual",
           "Failed to create loopback device: %s",
           zx_status_get_string(status));
    DestroyLoopbackDevice();
    return;
  }
}

void VirtualController::DestroyLoopbackDevice() {
  if (!loopback_device_) {
    return;
  }
  if (hooks_.remove_loopback_node) {
    hooks_.remove_loopback_node(std::string(kLoopbackName));
  }
  loopback_device_.reset();
}

void VirtualController::handle_unknown_method(
    fidl::UnknownMethodMetadata<fhbt::VirtualController> metadata,
    fidl::UnknownMethodCompleter::Sync& completer) {
  bt_log(ERROR,
         "bt-hci-virtual",
         "Unknown method in VirtualController request, closing with "
         "ZX_ERR_NOT_SUPPORTED");
  completer.Close(ZX_ERR_NOT_SUPPORTED);
}

}  // namespace bt_hci_virtual
