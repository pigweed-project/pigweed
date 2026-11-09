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

#include <lib/fit/function.h>
#include <pw_assert/check.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <memory>
#include <string>
#include <string_view>
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
                                     component::OutgoingDirectory* outgoing)
    : dispatcher_(dispatcher),
      outgoing_(outgoing),
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

zx::result<> VirtualController::CreateDeviceServiceDirectories() {
  if (outgoing_ == nullptr) {
    return zx::error(ZX_ERR_BAD_STATE);
  }
  // TODO: b/578928018 - `component::OutgoingDirectory` has no way to create an
  // empty directory, so add an entry to each one and remove it again; the
  // directory it was created in stays.
  constexpr std::string_view kPlaceholderEntry = "placeholder";
  for (std::string_view service :
       {std::string_view(fhbt::EmulatorService::Name),
        std::string_view(fhbt::Service::Name)}) {
    std::string path =
        std::string(component::OutgoingDirectory::kServiceDirectory) + "/" +
        std::string(service);
    zx::result<> added = outgoing_->AddUnmanagedProtocolAt(
        [](zx::channel) {}, path, kPlaceholderEntry);
    if (added.is_error()) {
      return added;
    }
    // Note that RemoveUnmanagedProtocolAt() cannot be used here because it
    // doesn't allow the protocol to be re-added (see b/581021793).
    zx::result<> removed = outgoing_->RemoveProtocolAt(path, kPlaceholderEntry);
    if (removed.is_error()) {
      return removed;
    }
  }
  return zx::ok();
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
    emulators_.erase(entry);
  };

  zx_status_t status =
      device.Initialize(name, std::move(shutdown_cb), outgoing_);
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
  if (!request->has_uart_channel()) {
    bt_log(
        ERROR, "bt-hci-virtual", "CreateLoopbackDevice without uart_channel");
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  // Any previous device is replaced.
  loopback_device_ = std::make_unique<LoopbackDevice>(dispatcher_);
  zx_status_t status = loopback_device_->Initialize(
      std::move(request->uart_channel()), kLoopbackName, outgoing_);
  if (status != ZX_OK) {
    bt_log(ERROR,
           "bt-hci-virtual",
           "Failed to create loopback device: %s",
           zx_status_get_string(status));
    loopback_device_.reset();
  }
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
