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

#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <lib/async/cpp/wait.h>
#include <lib/async/dispatcher.h>
#include <lib/zx/channel.h>

#include "pw_bluetooth/controller.h"
#include "pw_bluetooth_sapphire/lease.h"

namespace bt::controllers {

class VendorEventHandler
    : public fidl::AsyncEventHandler<fuchsia_hardware_bluetooth::Vendor> {
 public:
  explicit VendorEventHandler(std::function<void(zx_status_t)> unbind_callback);
  void on_fidl_error(fidl::UnbindInfo error) override;
  void handle_unknown_event(
      fidl::UnknownEventMetadata<fuchsia_hardware_bluetooth::Vendor> metadata)
      override;

 private:
  std::function<void(zx_status_t)> unbind_callback_;
};

class HciEventHandler
    : public fidl::AsyncEventHandler<fuchsia_hardware_bluetooth::HciTransport> {
 public:
  HciEventHandler(
      std::function<void(zx_status_t)> unbind_callback,
      std::function<void(fuchsia_hardware_bluetooth::ReceivedPacket)>
          on_receive_callback);
  void OnReceive(fuchsia_hardware_bluetooth::ReceivedPacket&) override;
  void on_fidl_error(fidl::UnbindInfo error) override;
  void handle_unknown_event(
      fidl::UnknownEventMetadata<fuchsia_hardware_bluetooth::HciTransport>
          metadata) override;

 private:
  std::function<void(fuchsia_hardware_bluetooth::ReceivedPacket)>
      on_receive_callback_;
  std::function<void(zx_status_t)> unbind_callback_;
};

class FidlController final : public pw::bluetooth::Controller {
 public:
  using PwStatusCallback = pw::Callback<void(pw::Status)>;

  // |dispatcher| must outlive this object.
  FidlController(
      fidl::ClientEnd<fuchsia_hardware_bluetooth::Vendor> vendor_client_end,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
      async_dispatcher_t* dispatcher);

  ~FidlController() override;

  // Controller overrides:
  void SetEventFunction(DataFunction func) override {
    event_cb_ = std::move(func);
  }

  void SetReceiveAclFunction(DataFunction func) override {
    acl_cb_ = std::move(func);
  }

  void SetReceiveScoFunction(DataFunction func) override {
    sco_cb_ = std::move(func);
  }

  void SetReceiveIsoFunction(DataFunction func) override {
    iso_cb_ = std::move(func);
  }

  void Initialize(PwStatusCallback complete_callback,
                  PwStatusCallback error_callback) override;

  void Close(PwStatusCallback callback) override;

  void SendCommand(pw::span<const std::byte> command) override;

  void SendAclData(pw::span<const std::byte> data) override;

  void SendScoData(pw::span<const std::byte> data) override;

  void SendIsoData(pw::span<const std::byte> data) override;

  void ConfigureSco(ScoCodingFormat coding_format,
                    ScoEncoding encoding,
                    ScoSampleRate sample_rate,
                    pw::Callback<void(pw::Status)> callback) override;

  void ResetSco(pw::Callback<void(pw::Status)> callback) override;

  void GetFeatures(pw::Callback<void(FeaturesBits)> callback) override;
  void EncodeVendorCommand(
      pw::bluetooth::VendorCommandParameters parameters,
      pw::Callback<void(pw::Result<pw::span<const std::byte>>)> callback)
      override;

 private:
  class ScoEventHandler : public fidl::AsyncEventHandler<
                              fuchsia_hardware_bluetooth::ScoConnection> {
   public:
    ScoEventHandler(pw::Function<void(zx_status_t)> unbind_callback,
                    pw::Function<void(fuchsia_hardware_bluetooth::ScoPacket)>
                        on_receive_callback);

   private:
    // AsyncEventHandler<ScoConnection> overrides:
    void OnReceive(fuchsia_hardware_bluetooth::ScoPacket& packet) override;
    void on_fidl_error(fidl::UnbindInfo error) override;
    void handle_unknown_event(
        fidl::UnknownEventMetadata<fuchsia_hardware_bluetooth::ScoConnection>
            metadata) override;

    pw::Function<void(fuchsia_hardware_bluetooth::ScoPacket)>
        on_receive_callback_;
    pw::Function<void(zx_status_t)> unbind_callback_;
  };

  void OnReceive(fuchsia_hardware_bluetooth::ReceivedPacket packet);
  void OnReceiveSco(fuchsia_hardware_bluetooth::ScoPacket packet);

  void OnScoUnbind(zx_status_t status);

  // Cleanup and call |error_cb_| with |status|
  void OnError(zx_status_t status);

  void CleanUp();

  // Initializes HCI layer by binding |hci_handle| to |hci_| and opening two-way
  // command channel and ACL data channel
  void InitializeHci(
      fidl::ClientEnd<fuchsia_hardware_bluetooth::HciTransport> hci_client_end);

  // |vendor_handle_| holds the Vendor channel until Initialize() is called, at
  // which point |vendor_| is bound to the channel. This prevents errors from
  // being lost before initialization.
  fidl::ClientEnd<fuchsia_hardware_bluetooth::Vendor> vendor_client_end_;
  fidl::Client<fuchsia_hardware_bluetooth::Vendor> vendor_;

  fidl::Client<fuchsia_hardware_bluetooth::HciTransport> hci_;

  VendorEventHandler vendor_event_handler_;
  HciEventHandler hci_event_handler_;

  // Only set after ConfigureSco() is called. Unbound on ResetSco().
  std::optional<fidl::Client<fuchsia_hardware_bluetooth::ScoConnection>>
      sco_connection_;
  // Shared across all ScoConnections.
  ScoEventHandler sco_event_handler_;
  // Valid only when a ResetSco() call is pending.
  PwStatusCallback reset_sco_cb_;

  async_dispatcher_t* dispatcher_;

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;

  DataFunction event_cb_;
  DataFunction acl_cb_;
  DataFunction sco_cb_;
  DataFunction iso_cb_;
  PwStatusCallback initialize_complete_cb_;
  PwStatusCallback error_cb_;

  bool shutting_down_ = false;
};

}  // namespace bt::controllers
