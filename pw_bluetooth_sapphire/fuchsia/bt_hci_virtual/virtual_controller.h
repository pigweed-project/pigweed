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

#pragma once

#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/fit/function.h>
#include <lib/zx/result.h>
#include <zircon/types.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/emulator.h"
#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/loopback.h"

namespace bt_hci_virtual {

// Serves `fuchsia.hardware.bluetooth.VirtualController`, creating any number
// of `EmulatorDevice`s (one per `CreateEmulator` call, each with a unique name)
// and at most one `LoopbackDevice`. The devices publish their services to the
// outgoing directory given to the constructor. For more details, refer to
// go/bluetooth-virtual-driver-doc.
class VirtualController
    : public fidl::WireServer<fuchsia_hardware_bluetooth::VirtualController> {
 public:
  // Lets the DFv2 driver attach driver-framework nodes to the devices this
  // controller creates. A host without such nodes passes none of them.
  struct NodeHooks {
    // Called after `device` is initialized; a failure is reported to the
    // `CreateEmulator` caller and the device is shut down.
    fit::function<zx_status_t(const std::string& name, EmulatorDevice& device)>
        add_emulator_node;
    // Called from the device's `ShutdownCallback`, before the device is
    // destroyed, including after `add_emulator_node` failed or was not called.
    // Not called when this controller is destroyed.
    fit::function<void(const std::string& name)> remove_emulator_node;
    // Called after `device` is initialized; a failure destroys the device.
    fit::function<zx_status_t(const std::string& name, LoopbackDevice& device)>
        add_loopback_node;
    // Called before the device is destroyed, when `CreateLoopbackDevice`
    // replaces it or creating it failed, including after `add_loopback_node`
    // failed or was not called. Not called when this controller is destroyed.
    fit::function<void(const std::string& name)> remove_loopback_node;
  };

  // Prefix of the unique name `CreateEmulator` gives each emulator.
  static constexpr std::string_view kEmulatorNamePrefix = "emulator-";

  // Name of the vendor service instance `CreateLoopbackDevice` publishes.
  static constexpr std::string_view kLoopbackName = "loopback";

  // The service `PublishService()` publishes, as its default instance.
  static constexpr std::string_view kServiceName =
      "fuchsia.hardware.bluetooth.VirtualControllerService";
  // The member of the `kServiceName` instance that serves the
  // `VirtualController` protocol.
  static constexpr std::string_view kServiceMemberName = "device";

  // Returns the path of the instance `PublishService()` publishes, relative to
  // the outgoing directory.
  static std::string ServiceInstancePath();

  // Returns the name `CreateEmulator` gives the first emulator of a new
  // controller.
  static std::string FirstEmulatorName();

  // All FIDL servers and the devices run on `dispatcher`. If `outgoing` is
  // non-null, the devices publish their services to it, which must already be
  // served; otherwise `PublishService()` fails. Both must outlive this object.
  VirtualController(async_dispatcher_t* dispatcher,
                    component::OutgoingDirectory* outgoing,
                    NodeHooks&& hooks = {});
  ~VirtualController() override;

  VirtualController(const VirtualController&) = delete;
  VirtualController& operator=(const VirtualController&) = delete;
  VirtualController(VirtualController&&) = delete;
  VirtualController& operator=(VirtualController&&) = delete;

  // Serves the `VirtualController` protocol on `request`.
  void Connect(
      fidl::ServerEnd<fuchsia_hardware_bluetooth::VirtualController> request);

  // Publishes the default instance of `kServiceName` to the outgoing directory
  // until this object is destroyed. Fails with `ZX_ERR_BAD_STATE` without an
  // outgoing directory and with `ZX_ERR_ALREADY_EXISTS` when called twice.
  zx::result<> PublishService();

 private:
  // fuchsia_hardware_bluetooth::VirtualController overrides:
  void CreateEmulator(CreateEmulatorCompleter::Sync& completer) override;
  void CreateLoopbackDevice(
      CreateLoopbackDeviceRequestView request,
      CreateLoopbackDeviceCompleter::Sync& completer) override;
  void handle_unknown_method(
      fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::VirtualController>
          metadata,
      fidl::UnknownMethodCompleter::Sync& completer) override;

  // Removes the loopback device's node, if any, and destroys the device.
  void DestroyLoopbackDevice();

  async_dispatcher_t* dispatcher_;
  component::OutgoingDirectory* outgoing_;
  NodeHooks hooks_;
  bool service_published_ = false;

  // Live emulators keyed by the unique name returned from `CreateEmulator`.
  // Each entry is erased by its own ShutdownCallback (see `CreateEmulator`).
  std::map<std::string, std::unique_ptr<EmulatorDevice>> emulators_;
  uint64_t next_emulator_id_;

  std::unique_ptr<LoopbackDevice> loopback_device_;

  fidl::ServerBindingGroup<fuchsia_hardware_bluetooth::VirtualController>
      bindings_;
};

}  // namespace bt_hci_virtual
