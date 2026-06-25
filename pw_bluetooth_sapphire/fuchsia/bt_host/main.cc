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

#include <fuchsia/process/lifecycle/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/component_context.h>
#include <pw_assert/check.h>
#include <zircon/processargs.h>

#include "fidl/fuchsia.bluetooth.host/cpp/fidl.h"
#include "fidl/fuchsia.hardware.bluetooth/cpp/fidl.h"
#include "fidl/fuchsia.power.system/cpp/fidl.h"
#include "fidl/fuchsia.scheduler/cpp/fidl.h"
#include "fidl/fuchsia.time.alarms/cpp/fidl.h"
#include "host.h"
#include "lib/component/incoming/cpp/protocol.h"
#include "pw_bluetooth_sapphire/fuchsia/bt_host/bt_host_config.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/activity_governor_lease_provider.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/wake_alarm_provider.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_log/log.h"
#include "util.h"

using InitCallback = fit::callback<void(bool success)>;
using ErrorCallback = fit::callback<void()>;

const std::string OUTGOING_SERVICE_NAME = "fuchsia.bluetooth.host.Host";
const std::string THREAD_ROLE_NAME = "fuchsia.bluetooth.host.thread";
const std::string VMAR_ROLE_NAME = "fuchsia.bluetooth.host.memory";

class LifecycleHandler
    : public fuchsia::process::lifecycle::Lifecycle,
      public fidl::AsyncEventHandler<fuchsia_bluetooth_host::Receiver> {
 public:
  using WeakPtr = WeakSelf<bthost::BtHostComponent>::WeakPtr;

  explicit LifecycleHandler(async::Loop* loop, WeakPtr host)
      : loop_(loop), host_(std::move(host)) {
    // Get the PA_LIFECYCLE handle, and instantiate the channel with it
    zx::channel channel = zx::channel(zx_take_startup_handle(PA_LIFECYCLE));
    // Bind to the channel and start listening for events
    bindings_.AddBinding(
        this,
        fidl::InterfaceRequest<fuchsia::process::lifecycle::Lifecycle>(
            std::move(channel)),
        loop_->dispatcher());
  }

  // Schedule a shut down
  void PostStopTask() {
    // Verify we don't already have a Stop task scheduled
    if (shutting_down_) {
      return;
    }
    shutting_down_ = true;

    async::PostTask(loop_->dispatcher(), [this]() { Stop(); });
  }

  // Shut down immediately
  void Stop() override {
    host_->ShutDown();
    loop_->Shutdown();
    bindings_.CloseAll();
  }

  // AsyncEventHandler overrides
  void on_fidl_error(fidl::UnbindInfo error) override {
    bt_log(WARN, "bt-host", "Receiver interface disconnected");
    Stop();
  }

  void handle_unknown_event(
      fidl::UnknownEventMetadata<fuchsia_bluetooth_host::Receiver> metadata)
      override {
    bt_log(WARN,
           "bt-host",
           "Received an unknown event with ordinal %lu",
           metadata.event_ordinal);
  }

 private:
  async::Loop* loop_;
  WeakPtr host_;
  fidl::BindingSet<fuchsia::process::lifecycle::Lifecycle> bindings_;
  bool shutting_down_ = false;
};

void SetHandleRole(
    fidl::SyncClient<fuchsia_scheduler::RoleManager>& role_manager,
    fuchsia_scheduler::RoleTarget target,
    const std::string& role_name) {
  bt_log(DEBUG, "bt-host", "Setting role %s", role_name.c_str());
  fuchsia_scheduler::RoleManagerSetRoleRequest request;
  request.target(std::move(target))
      .role(fuchsia_scheduler::RoleName(role_name));
  auto set_role_res = role_manager->SetRole(std::move(request));
  if (!set_role_res.is_ok()) {
    std::string err_str = set_role_res.error_value().FormatDescription();
    bt_log(WARN,
           "bt-host",
           "Couldn't set role %s: %s",
           role_name.c_str(),
           err_str.c_str());
    return;
  }
  bt_log(INFO, "bt-host", "Set role %s successfully.", role_name.c_str());
}

void SetRoles() {
  bt_log(DEBUG, "bt-host", "Connecting to RoleManager");
  auto client_end_res = component::Connect<fuchsia_scheduler::RoleManager>();
  if (!client_end_res.is_ok()) {
    bt_log(WARN,
           "bt-host",
           "Couldn't connect to RoleManager: %s",
           client_end_res.status_string());
    return;
  }
  fidl::SyncClient<fuchsia_scheduler::RoleManager> role_manager(
      std::move(*client_end_res));

  bt_log(DEBUG, "bt-host", "Cloning self thread");
  zx::thread thread_self;
  zx_status_t thread_status =
      zx::thread::self()->duplicate(ZX_RIGHT_SAME_RIGHTS, &thread_self);
  if (thread_status == ZX_OK) {
    SetHandleRole(
        role_manager,
        fuchsia_scheduler::RoleTarget::WithThread(std::move(thread_self)),
        THREAD_ROLE_NAME);
  } else {
    zx::result<> err = zx::error(thread_status);
    bt_log(ERROR,
           "bt-host",
           "Couldn't clone self thread for profile: %s",
           err.status_string());
  }

  bt_log(DEBUG, "bt-host", "Cloning root vmar");
  zx::vmar root_vmar;
  zx_status_t vmar_status =
      zx::vmar::root_self()->duplicate(ZX_RIGHT_SAME_RIGHTS, &root_vmar);
  if (vmar_status == ZX_OK) {
    SetHandleRole(role_manager,
                  fuchsia_scheduler::RoleTarget::WithVmar(std::move(root_vmar)),
                  VMAR_ROLE_NAME);
  } else {
    zx::result<> err = zx::error(vmar_status);
    bt_log(ERROR,
           "bt-host",
           "Couldn't clone self thread for profile: %s",
           err.status_string());
  }
}

int main() {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  pw::log_fuchsia::InitializeLogging(loop.dispatcher());

  bt_log(INFO, "bt-host", "Starting bt-host");
  SetRoles();

  bt_host_config::Config config =
      bt_host_config::Config::TakeFromStartupHandle();
  if (config.device_path().empty()) {
    bt_log(ERROR, "bt-host", "device_path is empty! Can't open. Quitting.");
    return 1;
  }
  bt_log(INFO, "bt-host", "device_path: %s", config.device_path().c_str());

  std::unique_ptr<bthost::ActivityGovernorLeaseProvider> lease_provider;
  if (config.enable_suspend()) {
    zx::result client_end_res =
        component::Connect<fuchsia_power_system::ActivityGovernor>();
    if (!client_end_res.is_ok()) {
      bt_log(ERROR,
             "bt-host",
             "Couldn't connect to ActivityGovernor: %s",
             client_end_res.status_string());
      return 1;
    }
    lease_provider = bthost::ActivityGovernorLeaseProvider::Create(
        std::move(client_end_res.value()), loop.dispatcher());
    if (!lease_provider) {
      bt_log(ERROR, "bt-host", "Couldn't create ActivityGovernorLeaseProvider");
      return 1;
    }
  }

  std::unique_ptr<bthost::FuchsiaWakeAlarmProvider> wake_alarm_provider;
  if (config.enable_suspend()) {
    zx::result client_end_res =
        component::Connect<fuchsia_time_alarms::WakeAlarms>();
    if (!client_end_res.is_ok()) {
      bt_log(ERROR,
             "bt-host",
             "Couldn't connect to WakeAlarms: %s",
             client_end_res.status_string());
      return 1;
    }
    wake_alarm_provider = bthost::FuchsiaWakeAlarmProvider::Create(
        std::move(client_end_res.value()), *lease_provider, loop.dispatcher());
    if (!wake_alarm_provider) {
      bt_log(ERROR, "bt-host", "Couldn't create FuchsiaWakeAlarmProvider");
      return 1;
    }
  }

  std::unique_ptr<bthost::BtHostComponent> host =
      bthost::BtHostComponent::Create(loop.dispatcher(),
                                      config.device_path(),
                                      std::move(lease_provider),
                                      std::move(wake_alarm_provider));

  LifecycleHandler lifecycle_handler(&loop, host->GetWeakPtr());

  auto init_cb = [&host, &lifecycle_handler, &loop, &config](bool success) {
    PW_DCHECK(host);
    if (!success) {
      bt_log(
          ERROR, "bt-host", "Failed to initialize bt-host; shutting down...");
      lifecycle_handler.Stop();
      return;
    }
    bt_log(DEBUG, "bt-host", "bt-host initialized; starting FIDL servers...");

    // Bind current host to Host protocol interface
    auto endpoints = fidl::CreateEndpoints<fuchsia_bluetooth_host::Host>();
    if (endpoints.is_error()) {
      bt_log(ERROR,
             "bt-host",
             "Couldn't create endpoints: %d",
             endpoints.error_value());
      lifecycle_handler.Stop();
      return;
    }
    host->BindToHostInterface(std::move(endpoints->server),
                              config.sco_offload_path_index());

    // Add Host device and protocol to bt-gap via Receiver
    zx::result receiver_client =
        component::Connect<fuchsia_bluetooth_host::Receiver>();
    if (!receiver_client.is_ok()) {
      bt_log(ERROR,
             "bt-host",
             "Error connecting to the Receiver protocol: %s",
             receiver_client.status_string());
      lifecycle_handler.Stop();
      return;
    }
    fidl::Client client(
        std::move(*receiver_client), loop.dispatcher(), &lifecycle_handler);
    fit::result<fidl::Error> result =
        client->AddHost(fuchsia_bluetooth_host::ReceiverAddHostRequest(
            std::move(endpoints->client)));
    if (!result.is_ok()) {
      bt_log(ERROR,
             "bt-host",
             "Failed to add host: %s",
             result.error_value().FormatDescription().c_str());
      lifecycle_handler.Stop();
      return;
    }
  };

  auto error_cb = [&lifecycle_handler]() {
    // The controller has reported an error. Shut down after the currently
    // scheduled tasks finish executing.
    bt_log(WARN, "bt-host", "Error in bt-host; shutting down...");
    lifecycle_handler.PostStopTask();
  };

  zx::result<fidl::ClientEnd<fuchsia_hardware_bluetooth::Vendor>>
      vendor_client_end_result =
          bthost::CreateVendorHandle(config.device_path());
  if (!vendor_client_end_result.is_ok()) {
    bt_log(ERROR,
           "bt-host",
           "Failed to create VendorHandle; cannot initialize bt-host: %s",
           vendor_client_end_result.status_string());
    return 1;
  }

  bthost::BtHostComponent::Config host_config = {
      .legacy_pairing_enabled = config.legacy_pairing_enabled(),
      .override_vendor_capabilites_version =
          config.override_vendor_capabilites_version(),
      .le_slow_adv_interval_min = config.le_slow_adv_interval_min(),
      .le_slow_adv_interval_max = config.le_slow_adv_interval_max(),
      .le_fast_adv_interval_min = config.le_fast_adv_interval_min(),
      .le_fast_adv_interval_max = config.le_fast_adv_interval_max(),
      .le_very_fast_adv_interval_min = config.le_very_fast_adv_interval_min(),
      .le_very_fast_adv_interval_max = config.le_very_fast_adv_interval_max(),
      .le_slow_adv_max_tx_power = config.le_slow_adv_max_tx_power(),
      .le_fast_adv_max_tx_power = config.le_fast_adv_max_tx_power(),
      .le_very_fast_adv_max_tx_power = config.le_very_fast_adv_max_tx_power(),
      .le_active_scan_interval = config.le_active_scan_interval(),
      .le_active_scan_window = config.le_active_scan_window(),
      .hci_command_timeout_seconds = config.hci_command_timeout_seconds(),
  };

  bool initialize_res =
      host->Initialize(std::move(vendor_client_end_result.value()),
                       init_cb,
                       error_cb,
                       host_config);
  if (!initialize_res) {
    bt_log(ERROR, "bt-host", "Error initializing bt-host; shutting down...");
    return 1;
  }

  loop.Run();
  return 0;
}
