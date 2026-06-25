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

#include <lib/async/dispatcher.h>
#include <lib/inspect/component/cpp/component.h>
#include <pw_async_fuchsia/dispatcher.h>

#include "fidl/fuchsia.bluetooth.host/cpp/fidl.h"
#include "fidl/fuchsia.hardware.bluetooth/cpp/fidl.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/activity_governor_lease_provider.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/wake_alarm_provider.h"
#include "pw_bluetooth_sapphire/internal/host/gap/adapter.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt.h"
#include "pw_bluetooth_sapphire/null_lease_provider.h"
#include "pw_random_fuchsia/zircon_random_generator.h"

namespace bthost {

class HostServer;

class BtHostComponent {
 public:
  // Creates a new Host.
  static std::unique_ptr<BtHostComponent> Create(
      async_dispatcher_t* dispatcher,
      const std::string& device_path,
      std::unique_ptr<ActivityGovernorLeaseProvider> activity_governor,
      std::unique_ptr<FuchsiaWakeAlarmProvider> wake_alarm_provider);

  // Does not override RNG
  static std::unique_ptr<BtHostComponent> CreateForTesting(
      async_dispatcher_t* dispatcher, const std::string& device_path);

  ~BtHostComponent();

  // Initializes the system and reports the status to the |init_cb| in
  // |success|. |error_cb| will be called if a transport error occurs in the
  // Host after initialization. Returns false if initialization fails, otherwise
  // returns true.
  using InitCallback = fit::callback<void(bool success)>;
  using ErrorCallback = fit::callback<void()>;

  struct Config {
    bool legacy_pairing_enabled = false;
    uint16_t override_vendor_capabilites_version = 0;

    uint16_t le_slow_adv_interval_min = 0;
    uint16_t le_slow_adv_interval_max = 0;
    uint16_t le_fast_adv_interval_min = 0;
    uint16_t le_fast_adv_interval_max = 0;
    uint16_t le_very_fast_adv_interval_min = 0;
    uint16_t le_very_fast_adv_interval_max = 0;

    // -127 to +20; 127 (0x7F) means no preference.
    int8_t le_slow_adv_max_tx_power = 127;
    int8_t le_fast_adv_max_tx_power = 127;
    int8_t le_very_fast_adv_max_tx_power = 127;

    uint16_t le_active_scan_interval = 0;
    uint16_t le_active_scan_window = 0;

    uint16_t hci_command_timeout_seconds = 10;
  };

  [[nodiscard]] bool Initialize(
      fidl::ClientEnd<fuchsia_hardware_bluetooth::Vendor> vendor_client_end,
      InitCallback init_cb,
      ErrorCallback error_cb,
      Config config);

  // Shuts down all systems.
  void ShutDown();

  // Binds the given |host_client| to a Host FIDL interface server.
  void BindToHostInterface(
      fidl::ServerEnd<fuchsia_bluetooth_host::Host> host_client,
      uint8_t sco_offload_index);

  std::string device_path() { return device_path_; }

  using WeakPtr = WeakSelf<BtHostComponent>::WeakPtr;
  WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

 private:
  BtHostComponent(
      async_dispatcher_t* dispatcher,
      const std::string& device_path,
      bool initialize_rng,
      std::unique_ptr<ActivityGovernorLeaseProvider> activity_governor,
      std::unique_ptr<FuchsiaWakeAlarmProvider> wake_alarm_provider);

  pw::bluetooth_sapphire::LeaseProvider& lease_provider();

  std::optional<
      std::reference_wrapper<pw::bluetooth_sapphire::WakeAlarmProvider>>
  wake_alarm_provider();

  pw::async_fuchsia::FuchsiaDispatcher pw_dispatcher_;

  std::variant<pw::bluetooth_sapphire::NullLeaseProvider,
               std::unique_ptr<bthost::ActivityGovernorLeaseProvider>>
      lease_provider_;

  std::unique_ptr<bthost::FuchsiaWakeAlarmProvider> wake_alarm_provider_;

  // Path of bt-hci device the component supports
  std::string device_path_;

  bool initialize_rng_;

  pw::random_fuchsia::ZirconRandomGenerator random_generator_;

  std::unique_ptr<bt::hci::Transport> hci_;

  std::unique_ptr<bt::gap::Adapter> gap_;

  // The GATT profile layer and bus.
  std::unique_ptr<bt::gatt::GATT> gatt_;

  // Currently connected Host interface handle.
  // A Host allows only one of these to be connected at a time.
  std::unique_ptr<HostServer> host_server_;

  // Inspector for component inspect tree. This object is thread-safe.
  inspect::ComponentInspector inspector_;

  WeakSelf<BtHostComponent> weak_self_{this};

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(BtHostComponent);
};

}  // namespace bthost
