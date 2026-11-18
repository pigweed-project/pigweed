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

#include <fuchsia/bluetooth/le/cpp/fidl.h>
#include <lib/async/cpp/wait.h>
#include <lib/fidl/cpp/binding.h>

#include <memory>
#include <unordered_map>

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/low_energy_connection_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/server_base.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/gap/adapter.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_advertising_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_manager.h"
#include "pw_bluetooth_sapphire/lease.h"

namespace bthost {

// Implements the low_energy::Peripheral FIDL interface.
class LowEnergyPeripheralServer
    : public AdapterServerBase<fuchsia::bluetooth::le::Peripheral> {
 public:
  LowEnergyPeripheralServer(
      bt::gap::Adapter::WeakPtr adapter,
      bt::gatt::GATT::WeakPtr gatt,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
      fidl::InterfaceRequest<fuchsia::bluetooth::le::Peripheral> request,
      bool privileged = false);
  ~LowEnergyPeripheralServer() override;

  // fuchsia::bluetooth::le::Peripheral overrides:
  void Advertise(
      fuchsia::bluetooth::le::AdvertisingParameters parameters,
      fidl::InterfaceHandle<fuchsia::bluetooth::le::AdvertisedPeripheral>
          advertised_peripheral,
      AdvertiseCallback callback) override;
  void StartAdvertising(
      fuchsia::bluetooth::le::AdvertisingParameters parameters,
      ::fidl::InterfaceRequest<fuchsia::bluetooth::le::AdvertisingHandle> token,
      StartAdvertisingCallback callback) override;

  // fuchsia::bluetooth::le::ChannelListenerRegistry overrides:
  void ListenL2cap(
      fuchsia::bluetooth::le::ChannelListenerRegistryListenL2capRequest request,
      ListenL2capCallback callback) override;

  // Returns the connection handle associated with the given |id|, or nullptr if
  // the peer with |id| is no longer connected. Should only be used for testing.
  const bt::gap::LowEnergyConnectionHandle* FindConnectionForTesting(
      bt::PeerId id) const;

 private:
  using ConnectionRefPtr = std::unique_ptr<bt::gap::LowEnergyConnectionHandle>;
  using AdvertisementInstanceId = uint64_t;
  using ConnectionServerId = uint64_t;

  // Manages state associated with a single invocation of the
  // `Peripheral.Advertise` method.
  class AdvertisementInstance final {
   public:
    using AdvertiseCompleteCallback = fit::callback<void(
        fuchsia::bluetooth::le::Peripheral_Advertise_Result)>;

    // |complete_cb| will be called to send a Peripheral.Advertise response to
    // the client when an error occurs or this AdvertisementInstance is
    // destroyed. This is done so that the FIDL client can determine when the
    // server has terminated this AdvertisementInstance (this is useful for
    // reconfiguring an advertisement).
    AdvertisementInstance(
        LowEnergyPeripheralServer* peripheral_server,
        AdvertisementInstanceId id,
        fuchsia::bluetooth::le::AdvertisingParameters parameters,
        fidl::InterfaceHandle<fuchsia::bluetooth::le::AdvertisedPeripheral>
            handle,
        AdvertiseCompleteCallback complete_cb);
    ~AdvertisementInstance();

    // This method is separate from the constructor because HCI-level
    // advertising may be started many times over the life of this object.
    void StartAdvertising();

    // Called when a central connects to us.  When this is called, the
    // advertisement in |advertisement_id| has been stopped.
    void OnConnected(bt::gap::AdvertisementId advertisement_id,
                     bt::gap::Adapter::LowEnergy::ConnectionResult result);

   private:
    // After advertising successfully starts, the advertisement instance must be
    // registered to tie advertising to the lifetime of this object.
    void Register(bt::gap::AdvertisementInstance instance);

    // End the advertisement with a result. Idempotent.
    // This object should be destroyed immediately after calling this method.
    void CloseWith(
        fpromise::result<void, fuchsia::bluetooth::le::PeripheralError> result);

    LowEnergyPeripheralServer* peripheral_server_;
    AdvertisementInstanceId id_;
    fuchsia::bluetooth::le::AdvertisingParameters parameters_;

    // The advertising handle set by Register. When destroyed, advertising will
    // be stopped.
    std::optional<bt::gap::AdvertisementInstance> instance_;

    // The AdvertisedPeripheral protocol representing this advertisement.
    fidl::InterfacePtr<fuchsia::bluetooth::le::AdvertisedPeripheral>
        advertised_peripheral_;

    // Callback used to send a response to the Advertise request that started
    // this advertisement.
    AdvertiseCompleteCallback advertise_complete_cb_;

    WeakSelf<AdvertisementInstance> weak_self_;

    BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AdvertisementInstance);
  };

  class AdvertisementInstanceDeprecated final {
   public:
    explicit AdvertisementInstanceDeprecated(
        fidl::InterfaceRequest<fuchsia::bluetooth::le::AdvertisingHandle>
            handle);
    ~AdvertisementInstanceDeprecated();

    // Begin watching for ZX_CHANNEL_PEER_CLOSED events on the AdvertisingHandle
    // this was initialized with. The returned status will indicate an error if
    // wait cannot be initiated (e.g. because the peer closed its end of the
    // channel).
    zx_status_t Register(bt::gap::AdvertisementInstance instance);

    // Returns the ID assigned to this instance, or
    // bt::gap::kInvalidAdvertisementId if one wasn't assigned.
    bt::gap::AdvertisementId id() const {
      return instance_ ? instance_->id() : bt::gap::kInvalidAdvertisementId;
    }

    bool pending() const { return pending_; }
    void set_pending(bool value) { pending_ = value; }

   private:
    // The value will be set when an advertisement is active and will not be set
    // when an advertisement is pending or after it has been stopped (e.g., by a
    // client dropping their end of the AdvertisingHandle).
    std::optional<bt::gap::AdvertisementInstance> instance_;

    fidl::InterfaceRequest<fuchsia::bluetooth::le::AdvertisingHandle> handle_;
    async::Wait handle_closed_wait_;

    // Set when the client has requested the start of an advertisement and the
    // request is still being processed (it has not yet started).
    bool pending_ = false;

    BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AdvertisementInstanceDeprecated);
  };

  // Called when a central connects to us.  When this is called, the
  // advertisement in |advertisement_id| has been stopped.
  void OnConnectedDeprecated(
      bt::gap::AdvertisementId advertisement_id,
      bt::gap::Adapter::LowEnergy::ConnectionResult result);

  // Sets up a Connection server and returns the client end.
  fidl::InterfaceHandle<fuchsia::bluetooth::le::Connection>
  CreateConnectionServer(
      std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection);

  // Common advertising initiation code shared by Peripheral.{Advertise,
  // StartAdvertising}. If advertising was initiated by `Advertise`,
  // `advertisement_instance` must be set to the identifier of the
  // `AdvertisementInstance` that connections to this advertisement should be
  // routed to. Otherwise, connections will be sent in a
  // `Peripheral.OnConnected` event.
  void StartAdvertisingInternal(
      fuchsia::bluetooth::le::AdvertisingParameters& parameters,
      bt::gap::Adapter::LowEnergy::AdvertisingStatusCallback status_cb,
      std::optional<AdvertisementInstanceId> advertisement_instance =
          std::nullopt);

  void RemoveAdvertisingInstance(AdvertisementInstanceId id) {
    advertisements_.erase(id);
  }

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;

  // Represents the current advertising instance:
  // - Contains no value if advertising was never requested.
  // - Contains a value while advertising is being (re)enabled and during
  // advertising.
  // - May correspond to an invalidated advertising instance if advertising is
  // stopped by closing the AdvertisingHandle.
  std::optional<AdvertisementInstanceDeprecated> advertisement_deprecated_;

  // Stores a queued StartAdvertising() request while waiting for the current
  // advertising request to complete.
  std::optional<std::tuple<
      fuchsia::bluetooth::le::AdvertisingParameters,
      ::fidl::InterfaceRequest<fuchsia::bluetooth::le::AdvertisingHandle>,
      StartAdvertisingCallback>>
      queued_start_advertising_;

  // Map of all active advertisement instances associated with a call to
  // `Advertise`. bt::gap::AdvertisementId cannot be used as a map key because
  // it is received asynchronously, and we need an advertisement ID to refer to
  // before advertising starts.
  AdvertisementInstanceId next_advertisement_instance_id_ = 0u;
  std::unordered_map<AdvertisementInstanceId, AdvertisementInstance>
      advertisements_;

  // Connections that were initiated to this peripheral. A single Peripheral
  // instance can hold many connections across numerous advertisements that it
  // initiates during its lifetime.
  ConnectionServerId next_connection_server_id_ = 0u;
  std::unordered_map<ConnectionServerId,
                     std::unique_ptr<LowEnergyConnectionServer>>
      connections_;

  bt::gatt::GATT::WeakPtr gatt_;

  // True if PrivilegedPeripheral created this server. Defaults to false.
  bool privileged_;

  // Keep this as the last member to make sure that all weak pointers are
  // invalidated before other members get destroyed.
  WeakSelf<LowEnergyPeripheralServer> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyPeripheralServer);
};

// Implements the fuchsia::bluetooth::le::PrivilegedPeripheral FIDL interface.
class LowEnergyPrivilegedPeripheralServer
    : public AdapterServerBase<fuchsia::bluetooth::le::PrivilegedPeripheral> {
 public:
  LowEnergyPrivilegedPeripheralServer(
      const bt::gap::Adapter::WeakPtr& adapter,
      bt::gatt::GATT::WeakPtr gatt,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
      fidl::InterfaceRequest<fuchsia::bluetooth::le::PrivilegedPeripheral>
          request);

  // fuchsia::bluetooth::le::Peripheral overrides:
  void Advertise(
      fuchsia::bluetooth::le::AdvertisingParameters parameters,
      fidl::InterfaceHandle<fuchsia::bluetooth::le::AdvertisedPeripheral>
          advertised_peripheral,
      AdvertiseCallback callback) override;
  void StartAdvertising(
      fuchsia::bluetooth::le::AdvertisingParameters parameters,
      ::fidl::InterfaceRequest<fuchsia::bluetooth::le::AdvertisingHandle> token,
      StartAdvertisingCallback callback) override;

  // fuchsia::bluetooth::le::ChannelListenerRegistry overrides:
  void ListenL2cap(
      fuchsia::bluetooth::le::ChannelListenerRegistryListenL2capRequest request,
      ListenL2capCallback callback) override;

 private:
  std::unique_ptr<LowEnergyPeripheralServer> le_peripheral_server_;

  WeakSelf<LowEnergyPrivilegedPeripheralServer> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyPrivilegedPeripheralServer);
};

}  // namespace bthost
