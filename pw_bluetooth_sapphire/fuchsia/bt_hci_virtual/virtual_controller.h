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
#include <lib/zx/result.h>

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
  // Prefix of the unique name `CreateEmulator` gives each emulator.
  static constexpr std::string_view kEmulatorNamePrefix = "emulator-";

  // Name of the vendor service instance `CreateLoopbackDevice` publishes.
  static constexpr std::string_view kLoopbackName = "loopback";

  // Returns the name `CreateEmulator` gives the first emulator of a new
  // controller.
  static std::string FirstEmulatorName();

  // All FIDL servers and the devices run on `dispatcher`. If `outgoing` is
  // non-null, the devices publish their services to it, which must already be
  // served; otherwise `PublishService()` fails. Both must outlive this object.
  VirtualController(async_dispatcher_t* dispatcher,
                    component::OutgoingDirectory* outgoing);
  ~VirtualController() override;

  VirtualController(const VirtualController&) = delete;
  VirtualController& operator=(const VirtualController&) = delete;
  VirtualController(VirtualController&&) = delete;
  VirtualController& operator=(VirtualController&&) = delete;

  // Serves the `VirtualController` protocol on `request`.
  void Connect(
      fidl::ServerEnd<fuchsia_hardware_bluetooth::VirtualController> request);

  // Publishes the default instance of
  // `fuchsia.hardware.bluetooth.VirtualControllerService` to the outgoing
  // directory until this object is destroyed. Fails with `ZX_ERR_BAD_STATE`
  // without an outgoing directory and with `ZX_ERR_ALREADY_EXISTS` when called
  // twice.
  zx::result<> PublishService();

  // Creates the directories of the services the devices publish
  // (fuchsia.hardware.bluetooth.EmulatorService and .Service) in the outgoing
  // directory, so that clients can open and watch them before the first
  // emulator or loopback device exists. The outgoing directory must already be
  // served. Fails with `ZX_ERR_BAD_STATE` without an outgoing directory.
  zx::result<> CreateDeviceServiceDirectories();

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

  async_dispatcher_t* dispatcher_;
  component::OutgoingDirectory* outgoing_;
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
