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
#include <pw_assert/check.h>

#include <cstdint>

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/fake_hci_transport_server.h"

namespace bt::fidl::testing {

class FakeVendorServer final
    : public ::fidl::Server<fuchsia_hardware_bluetooth::Vendor> {
 public:
  FakeVendorServer(
      ::fidl::ServerEnd<fuchsia_hardware_bluetooth::Vendor> server_end,
      async_dispatcher_t* dispatcher)
      : binding_(::fidl::BindServer(dispatcher, std::move(server_end), this)),
        dispatcher_(dispatcher) {}

  void Unbind() { binding_.Unbind(); }

  fidl::testing::FakeHciTransportServer* hci_server() {
    return &fake_hci_server_.value();
  }

  void set_open_hci_error(bool val) { open_hci_error_ = val; }

 private:
  void GetFeatures(GetFeaturesCompleter::Sync& completer) override {
    fuchsia_hardware_bluetooth::VendorFeatures features;
    features.acl_priority_command(true);
    completer.Reply(features);
  }

  void EncodeCommand(EncodeCommandRequest& request,
                     EncodeCommandCompleter::Sync& completer) override {
    PW_CHECK(request.set_acl_priority()->priority().has_value());
    PW_CHECK(request.set_acl_priority()->direction().has_value());
    std::vector<uint8_t> tmp{static_cast<unsigned char>(
        WhichSetAclPriority(request.set_acl_priority()->priority().value(),
                            request.set_acl_priority()->direction().value()))};
    completer.Reply(fit::success(tmp));
  }

  // Not supported
  void OpenHci(OpenHciCompleter::Sync& completer) override {
    PW_CRASH("OpenHci not supported");
  }

  void OpenSnoop(OpenSnoopCompleter::Sync& completer) override {
    PW_CRASH("OpenSnoop not supported");
  }

  void OpenHciTransport(OpenHciTransportCompleter::Sync& completer) override {
    if (open_hci_error_) {
      completer.Reply(fit::error(ZX_ERR_INTERNAL));
      return;
    }

    auto [hci_client_end, hci_server_end] =
        ::fidl::Endpoints<fuchsia_hardware_bluetooth::HciTransport>::Create();

    fake_hci_server_.emplace(std::move(hci_server_end), dispatcher_);
    completer.Reply(fit::success(std::move(hci_client_end)));
  }

  void GetCrashParameters(
      GetCrashParametersCompleter::Sync& completer) override {
    completer.Reply(fit::error(ZX_ERR_NOT_SUPPORTED));
  }

  void handle_unknown_method(
      ::fidl::UnknownMethodMetadata<fuchsia_hardware_bluetooth::Vendor>
          metadata,
      ::fidl::UnknownMethodCompleter::Sync& completer) override {
    // Not implemented
  }

  uint8_t WhichSetAclPriority(
      fuchsia_hardware_bluetooth::VendorAclPriority priority,
      fuchsia_hardware_bluetooth::VendorAclDirection direction) {
    if (priority == fuchsia_hardware_bluetooth::VendorAclPriority::kHigh) {
      if (direction ==
          fuchsia_hardware_bluetooth::VendorAclDirection::kSource) {
        return static_cast<uint8_t>(pw::bluetooth::AclPriority::kSource);
      }
      return static_cast<uint8_t>(pw::bluetooth::AclPriority::kSink);
    }
    return static_cast<uint8_t>(pw::bluetooth::AclPriority::kNormal);
  }

  // Flag for testing. |OpenHci()| returns an error when set to true
  bool open_hci_error_ = false;

  std::optional<fidl::testing::FakeHciTransportServer> fake_hci_server_;

  ::fidl::ServerBindingRef<fuchsia_hardware_bluetooth::Vendor> binding_;

  async_dispatcher_t* dispatcher_;
};

}  // namespace bt::fidl::testing
