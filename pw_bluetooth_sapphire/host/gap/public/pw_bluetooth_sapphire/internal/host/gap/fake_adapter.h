// Copyright 2023 The Pigweed Authors
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
#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/gap/adapter.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"

namespace bt::gap::testing {

// FakeAdapter is a fake implementation of Adapter that can be used in higher
// layer unit tests (e.g. FIDL tests).
class FakeAdapter final : public Adapter {
 public:
  explicit FakeAdapter(pw::async::Dispatcher& pw_dispatcher);
  ~FakeAdapter() override = default;

  AdapterState& mutable_state() { return state_; }

  // Adapter overrides:

  AdapterId identifier() const override { return AdapterId(0); }

  bool Initialize(InitializeCallback callback,
                  fit::closure transport_closed_callback) override;

  void ShutDown() override;

  bool IsInitializing() const override {
    return init_state_ == InitState::kInitializing;
  }

  bool IsInitialized() const override {
    return init_state_ == InitState::kInitialized;
  }

  const AdapterState& state() const override { return state_; }

  class FakeLowEnergy final : public LowEnergy {
   public:
    struct RegisteredAdvertisement {
      AdvertisingData data;
      AdvertisingData scan_response;
      bool include_tx_power_level;
      DeviceAddress::Type addr_type;
      bool extended_pdu;
      bool anonymous;
      std::optional<ConnectableAdvertisingParameters> connectable;
    };

    struct Connection {
      PeerId peer_id;
      LowEnergyConnectionOptions options;
      LowEnergyConnectionHandle* handle;
    };

    struct PeriodicAdvertisementSync {
      PeerId peer_id;
      uint8_t advertising_sid;
      SyncOptions options;
      PeriodicAdvertisingSyncDelegate& delegate;
    };

    explicit FakeLowEnergy(FakeAdapter* adapter)
        : adapter_(adapter), fake_address_delegate_(adapter->pw_dispatcher_) {}
    ~FakeLowEnergy() override = default;

    const std::unordered_map<AdvertisementId, RegisteredAdvertisement>&
    registered_advertisements() {
      return advertisements_;
    }

    const std::unordered_map<PeerId, Connection>& connections() const {
      return connections_;
    }

    const std::unordered_map<hci::SyncId, PeriodicAdvertisementSync>&
    periodic_advertisement_syncs() const {
      return periodic_advertisement_syncs_;
    }

    // Update the LE random address of the adapter.
    void UpdateRandomAddress(DeviceAddress& address);

    // Overrides the result returned to StartAdvertising() callback.
    void set_advertising_result(hci::Result<> result);

    void set_sync_to_periodic_advertisement_error(bt::hci::Error error) {
      sync_to_periodic_advertisement_error_ = error;
    }

    // Notify all discovery sessions of a scan result.
    // Make sure to set the advertising data in Peer first!
    void NotifyScanResult(const Peer& peer);

    // Add scan result that discovery result callbacks will be notified of when
    // the result callback is set.
    void AddCachedScanResult(bt::PeerId peer_id) {
      cached_scan_results_.insert(peer_id);
    }

    const std::unordered_set<LowEnergyDiscoverySession*>& discovery_sessions()
        const {
      return discovery_sessions_;
    }

    // LowEnergy overrides:

    // If Connect is called multiple times, only the connection options of the
    // last call will be reported in connections().
    void Connect(PeerId peer_id,
                 ConnectionResultCallback callback,
                 LowEnergyConnectionOptions connection_options) override;

    bool Disconnect(PeerId peer_id) override;

    void OpenL2capChannel(PeerId peer_id,
                          l2cap::Psm,
                          l2cap::ChannelParameters,
                          sm::SecurityLevel security_level,
                          l2cap::ChannelCallback) override;

    void Pair(PeerId,
              sm::SecurityLevel,
              sm::BondableMode,
              sm::ResultFunction<>) override {}

    void SetLESecurityMode(LESecurityMode) override {}

    LESecurityMode security_mode() const override {
      return adapter_->le_security_mode_;
    }

    void StartAdvertising(
        AdvertisingData data,
        AdvertisingData scan_rsp,
        AdvertisingInterval interval,
        bool extended_pdu,
        bool anonymous,
        bool include_tx_power_level,
        std::optional<ConnectableAdvertisingParameters> connectable,
        std::optional<DeviceAddress::Type> address_type,
        AdvertisingStatusCallback status_callback) override;

    void StartDiscovery(bool active,
                        std::vector<hci::DiscoveryFilter> filters,
                        SessionCallback callback) override;

    hci::Result<PeriodicAdvertisingSyncHandle> SyncToPeriodicAdvertisement(
        PeerId peer,
        uint8_t advertising_sid,
        SyncOptions options,
        PeriodicAdvertisingSyncDelegate& delegate) override;

    void EnablePrivacy(bool enabled) override;

    // Returns true if the privacy feature is currently enabled.
    bool PrivacyEnabled() const override {
      return fake_address_delegate_.privacy_enabled();
    }
    // Returns the current LE address.
    const DeviceAddress CurrentAddress() const override {
      return fake_address_delegate_.current_address();
    }

    void DoCreateCig(iso::CigParams /*cig_params*/,
                     std::vector<iso::CigCisParams> /*cis_params*/,
                     iso::IsoGroupManager::CreateCigCompleteCallback callback,
                     iso::IsoGroup::OnClosedCallback /*on_closed_callback*/,
                     std::vector<PeerId> /*expected_peers*/) override {
      callback(pw::unexpected(HostError::kNotSupported));
    }

    std::unique_ptr<LowEnergyConnectionHandle> AddConnectionRef(
        PeerId /*peer_id*/) override {
      return nullptr;
    }

    void register_address_changed_callback(fit::closure callback) override {
      fake_address_delegate_.register_address_changed_callback(
          std::move(callback));
    }

    void set_irk(const std::optional<UInt128>&) override {}

    std::optional<UInt128> irk() const override { return std::nullopt; }

    void set_request_timeout_for_testing(
        pw::chrono::SystemClock::duration) override {}

    void set_scan_period_for_testing(
        pw::chrono::SystemClock::duration) override {}

   private:
    uint16_t next_scan_id_ = 0;
    FakeAdapter* adapter_;
    hci::SyncId next_sync_id_ = hci::SyncId(1);
    AdvertisementId next_advertisement_id_ = AdvertisementId(1);
    std::unordered_map<AdvertisementId, RegisteredAdvertisement>
        advertisements_;
    std::unordered_map<PeerId, Connection> connections_;
    hci::FakeLocalAddressDelegate fake_address_delegate_;
    l2cap::ChannelId next_channel_id_ = l2cap::kFirstDynamicChannelId;
    std::unordered_map<l2cap::ChannelId,
                       std::unique_ptr<l2cap::testing::FakeChannel>>
        channels_;
    std::optional<hci::Result<>> advertising_result_override_;
    std::unordered_set<LowEnergyDiscoverySession*> discovery_sessions_;
    std::unordered_set<PeerId> cached_scan_results_;
    std::unordered_map<hci::SyncId, PeriodicAdvertisementSync>
        periodic_advertisement_syncs_;
    std::optional<bt::hci::Error> sync_to_periodic_advertisement_error_;
  };

  LowEnergy* le() const override { return fake_le_.get(); }
  FakeLowEnergy* fake_le() const { return fake_le_.get(); }

  class FakeBrEdr final : public BrEdr {
   public:
    struct RegisteredService {
      std::vector<sdp::ServiceRecord> records;
      l2cap::ChannelParameters channel_params;
      ServiceConnectCallback connect_callback;
    };

    struct RegisteredSearch {
      UUID uuid;
      std::unordered_set<sdp::AttributeId> attributes;
      SearchCallback callback;
    };

    FakeBrEdr() = default;
    ~FakeBrEdr() override;

    // Called with a reference to the l2cap::FakeChannel created when a channel
    // is connected with Connect().
    using ChannelCallback =
        fit::function<void(l2cap::testing::FakeChannel::WeakPtr)>;
    void set_l2cap_channel_callback(ChannelCallback cb) {
      channel_cb_ = std::move(cb);
    }

    // Destroys the channel, invaliding all weak pointers. Returns true if the
    // channel was successfully destroyed.
    bool DestroyChannel(l2cap::ChannelId channel_id) {
      return channels_.erase(channel_id);
    }

    // Notifies all registered searches associated with the provided |uuid| with
    // the peer's service |attributes|.
    void TriggerServiceFound(
        PeerId peer_id,
        UUID uuid,
        std::map<sdp::AttributeId, sdp::DataElement> attributes);

    const std::map<RegistrationHandle, RegisteredService>& registered_services()
        const {
      return registered_services_;
    }

    const std::map<RegistrationHandle, RegisteredSearch>& registered_searches()
        const {
      return registered_searches_;
    }

    // BrEdr overrides:
    [[nodiscard]] bool Connect(PeerId, ConnectResultCallback) override {
      return false;
    }

    bool Disconnect(PeerId, DisconnectReason) override { return false; }

    void OpenL2capChannel(PeerId peer_id,
                          l2cap::Psm psm,
                          BrEdrSecurityRequirements security_requirements,
                          l2cap::ChannelParameters params,
                          l2cap::ChannelCallback cb) override;

    PeerId GetPeerId(hci_spec::ConnectionHandle) const override {
      return PeerId();
    }

    SearchId AddServiceSearch(const UUID& uuid,
                              std::unordered_set<sdp::AttributeId> attributes,
                              SearchCallback callback) override;

    bool RemoveServiceSearch(SearchId) override { return false; }

    void Pair(PeerId,
              BrEdrSecurityRequirements,
              hci::ResultFunction<>) override {}

    void SetBrEdrSecurityMode(BrEdrSecurityMode) override {}

    BrEdrSecurityMode security_mode() const override {
      return BrEdrSecurityMode::Mode4;
    }

    void SetConnectable(bool, hci::ResultFunction<>) override {}

    void RequestDiscovery(DiscoveryCallback) override {}

    void RequestDiscoverable(DiscoverableCallback) override {}

    RegistrationHandle RegisterService(std::vector<sdp::ServiceRecord> records,
                                       l2cap::ChannelParameters chan_params,
                                       ServiceConnectCallback conn_cb) override;

    bool UnregisterService(RegistrationHandle handle) override;

    std::vector<sdp::ServiceRecord> GetRegisteredServices(
        RegistrationHandle) const override {
      return {};
    }

    std::optional<ScoRequestHandle> OpenScoConnection(
        PeerId,
        const bt::StaticPacket<
            pw::bluetooth::emboss::SynchronousConnectionParametersWriter>&,
        sco::ScoConnectionManager::OpenConnectionCallback) override {
      return std::nullopt;
    }

    std::optional<ScoRequestHandle> AcceptScoConnection(
        PeerId,
        std::vector<bt::StaticPacket<
            pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>,
        sco::ScoConnectionManager::AcceptConnectionCallback) override {
      return std::nullopt;
    }

   private:
    // Callback used by tests to get new channel refs.
    ChannelCallback channel_cb_;
    RegistrationHandle next_service_handle_ = 1;
    RegistrationHandle next_search_handle_ = 1;
    std::map<RegistrationHandle, RegisteredService> registered_services_;
    std::map<RegistrationHandle, RegisteredSearch> registered_searches_;

    l2cap::ChannelId next_channel_id_ = l2cap::kFirstDynamicChannelId;
    std::unordered_map<l2cap::ChannelId,
                       std::unique_ptr<l2cap::testing::FakeChannel>>
        channels_;
  };

  BrEdr* bredr() const override { return fake_bredr_.get(); }
  FakeBrEdr* fake_bredr() const { return fake_bredr_.get(); }

  PeerCache* peer_cache() override { return &peer_cache_; }

  bool AddBondedPeer(BondingData) override { return true; }

  void SetPairingDelegate(PairingDelegate::WeakPtr) override {}

  bool IsDiscoverable() const override { return is_discoverable_; }

  bool IsDiscovering() const override { return is_discovering_; }

  void SetLocalName(std::string name, hci::ResultFunction<> callback) override;

  std::string local_name() const override { return local_name_; }

  void SetDeviceClass(DeviceClass dev_class,
                      hci::ResultFunction<> callback) override;

  void GetSupportedDelayRange(
      const bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter>& codec_id,
      pw::bluetooth::emboss::LogicalTransportType logical_transport_type,
      pw::bluetooth::emboss::DataPathDirection direction,
      const std::optional<std::vector<uint8_t>>& codec_configuration,
      GetSupportedDelayRangeCallback cb) override;

  void set_auto_connect_callback(AutoConnectCallback) override {}

  void AttachInspect(inspect::Node&, std::string) override {}

  Adapter::WeakPtr AsWeakPtr() override { return weak_self_.GetWeakPtr(); }

 private:
  enum InitState {
    kNotInitialized = 0,
    kInitializing,
    kInitialized,
  };

  InitState init_state_;
  AdapterState state_;
  pw::async::Dispatcher& pw_dispatcher_;
  std::unique_ptr<FakeLowEnergy> fake_le_;
  std::unique_ptr<FakeBrEdr> fake_bredr_;
  bool is_discoverable_ = true;
  bool is_discovering_ = true;
  std::string local_name_;
  DeviceClass device_class_;
  LESecurityMode le_security_mode_;

  pw::async::HeapDispatcher heap_dispatcher_;
  PeerCache peer_cache_;
  WeakSelf<Adapter> weak_self_;
};

}  // namespace bt::gap::testing
