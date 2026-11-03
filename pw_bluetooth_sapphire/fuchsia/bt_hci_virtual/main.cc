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

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>

#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/virtual_controller.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_log_fuchsia/log_backend.h"

int main() {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  pw::log_fuchsia::InitializeLogging(loop.dispatcher());
  component::OutgoingDirectory outgoing(loop.dispatcher());

  // VirtualController requires an outgoing directory that is already served.
  if (zx::result result = outgoing.ServeFromStartupInfo(); result.is_error()) {
    bt_log(ERROR,
           "bt-hci-virtual",
           "Failed to serve outgoing directory: %s",
           result.status_string());
    return 1;
  }

  bt_hci_virtual::VirtualController controller(loop.dispatcher(), &outgoing);
  if (zx::result result = controller.PublishService(); result.is_error()) {
    bt_log(ERROR,
           "bt-hci-virtual",
           "Failed to publish VirtualControllerService: %s",
           result.status_string());
    return 1;
  }
  if (zx::result result = controller.CreateDeviceServiceDirectories();
      result.is_error()) {
    bt_log(ERROR,
           "bt-hci-virtual",
           "Failed to create the device service directories: %s",
           result.status_string());
    return 1;
  }

  loop.Run();
  return 0;
}
