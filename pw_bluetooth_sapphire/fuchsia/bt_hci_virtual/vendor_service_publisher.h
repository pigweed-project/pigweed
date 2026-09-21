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
#include <lib/driver/logging/cpp/logger.h>
#include <lib/driver/outgoing/cpp/outgoing_directory.h>
#include <lib/fit/function.h>

#include <string>
#include <string_view>

namespace bt_hci_virtual {

// RAII helper to manage publishing and unpublishing
// `fuchsia.hardware.bluetooth.Service` in a driver's outgoing directory.
class VendorServicePublisher {
 public:
  using ConnectHandler =
      fit::function<void(fidl::ServerEnd<fuchsia_hardware_bluetooth::Vendor>)>;

  VendorServicePublisher() = default;

  ~VendorServicePublisher() { Unpublish(); }

  VendorServicePublisher(const VendorServicePublisher&) = delete;
  VendorServicePublisher& operator=(const VendorServicePublisher&) = delete;
  VendorServicePublisher(VendorServicePublisher&&) = delete;
  VendorServicePublisher& operator=(VendorServicePublisher&&) = delete;

  zx_status_t Publish(fdf::OutgoingDirectory* outgoing,
                      std::string_view instance_name,
                      ConnectHandler connect_handler) {
    if (!outgoing) {
      return ZX_ERR_INVALID_ARGS;
    }
    if (published_) {
      Unpublish();
    }

    fuchsia_hardware_bluetooth::Service::InstanceHandler handler({
        .vendor = std::move(connect_handler),
    });

    zx::result result =
        outgoing->AddService<fuchsia_hardware_bluetooth::Service>(
            std::move(handler), instance_name);
    if (result.is_error()) {
      fdf::error("Failed to add fuchsia_hardware_bluetooth::Service {}: {}",
                 instance_name,
                 result.status_string());
      return result.error_value();
    }

    outgoing_ = outgoing;
    instance_name_ = std::string(instance_name);
    published_ = true;
    return ZX_OK;
  }

  void Unpublish() {
    if (outgoing_ != nullptr && published_) {
      auto status =
          outgoing_->RemoveService<fuchsia_hardware_bluetooth::Service>(
              instance_name_);
      if (status.is_error()) {
        fdf::warn("Failed to remove fuchsia_hardware_bluetooth::Service {}: {}",
                  instance_name_,
                  status.status_string());
      }
      published_ = false;
      outgoing_ = nullptr;
      instance_name_.clear();
    }
  }

  bool is_published() const { return published_; }

 private:
  fdf::OutgoingDirectory* outgoing_ = nullptr;
  std::string instance_name_;
  bool published_ = false;
};

}  // namespace bt_hci_virtual
