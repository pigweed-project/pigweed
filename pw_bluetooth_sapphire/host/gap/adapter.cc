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

#include "pw_bluetooth_sapphire/internal/host/gap/adapter.h"

#include <pw_assert/check.h>
#include <pw_async/dispatcher.h>
#include <pw_bluetooth/hci_commands.emb.h>
#include <pw_bluetooth/hci_events.emb.h>
#include <pw_bytes/endian.h>

#include <cinttypes>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/metrics.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_discovery_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/event_masks.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_address_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_advertising_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_discovery_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/advertising_packet_filter.h"
#include "pw_bluetooth_sapphire/internal/host/hci/android_extended_low_energy_advertiser.h"
#include "pw_bluetooth_sapphire/internal/host/hci/discovery_filter.h"
#include "pw_bluetooth_sapphire/internal/host/hci/extended_low_energy_advertiser.h"
#include "pw_bluetooth_sapphire/internal/host/hci/extended_low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/hci/legacy_low_energy_advertiser.h"
#include "pw_bluetooth_sapphire/internal/host/hci/legacy_low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connector.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_scanner.h"
#include "pw_bluetooth_sapphire/internal/host/hci/sequential_command_runner.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/security_manager.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"
#include "pw_bluetooth_sapphire/lease.h"

namespace bt::gap {

namespace android_hci = hci_spec::vendor::android;
namespace android_emb = pw::bluetooth::vendor::android_hci;

static constexpr const char* kInspectLowEnergyDiscoveryManagerNodeName =
    "low_energy_discovery_manager";
static constexpr const char* kInspectLowEnergyConnectionManagerNodeName =
    "low_energy_connection_manager";
static constexpr const char* kInspectBrEdrConnectionManagerNodeName =
    "bredr_connection_manager";
static constexpr const char* kInspectBrEdrDiscoveryManagerNodeName =
    "bredr_discovery_manager";
static constexpr const char* kGattNodeName = "gatt";

// All asynchronous callbacks are posted on the Loop on which this Adapter
// instance is created.
class AdapterImpl final : public Adapter {
 public:
  explicit AdapterImpl(
      pw::async::Dispatcher& pw_dispatcher,
      hci::Transport::WeakPtr hci,
      gatt::GATT::WeakPtr gatt,
      Config config,
      std::unique_ptr<l2cap::ChannelManager> l2cap,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider);
  ~AdapterImpl() override;

  AdapterId identifier() const override { return identifier_; }

  bool Initialize(InitializeCallback callback,
                  fit::closure transport_error_cb) override;

  void ShutDown() override;

  bool IsInitializing() const override {
    return init_state_ == State::kInitializing;
  }

  bool IsInitialized() const override {
    return init_state_ == State::kInitialized;
  }

  const AdapterState& state() const override { return state_; }

  class LowEnergyImpl final : public LowEnergy {
   public:
    explicit LowEnergyImpl(AdapterImpl* adapter) : adapter_(adapter) {}

    void Connect(PeerId peer_id,
                 ConnectionResultCallback callback,
                 LowEnergyConnectionOptions connection_options) override {
      adapter_->le_connection_manager_->Connect(
          peer_id, std::move(callback), connection_options);
      adapter_->metrics_.le.outgoing_connection_requests.Add();
    }

    bool Disconnect(PeerId peer_id) override {
      return adapter_->le_connection_manager_->Disconnect(peer_id);
    }

    void OpenL2capChannel(PeerId peer_id,
                          l2cap::Psm psm,
                          l2cap::ChannelParameters params,
                          sm::SecurityLevel security_level,
                          l2cap::ChannelCallback cb) override {
      adapter_->metrics_.le.open_l2cap_channel_requests.Add();
      adapter_->le_connection_manager_->OpenL2capChannel(
          peer_id, psm, params, security_level, std::move(cb));
    }

    void Pair(PeerId peer_id,
              sm::SecurityLevel pairing_level,
              sm::BondableMode bondable_mode,
              sm::ResultFunction<> cb) override {
      adapter_->le_connection_manager_->Pair(
          peer_id, pairing_level, bondable_mode, std::move(cb));
      adapter_->metrics_.le.pair_requests.Add();
    }

    void SetLESecurityMode(LESecurityMode mode) override {
      adapter_->le_connection_manager_->SetSecurityMode(mode);
    }

    LESecurityMode security_mode() const override {
      return adapter_->le_connection_manager_->security_mode();
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
        AdvertisingStatusCallback status_callback) override {
      LowEnergyAdvertisingManager::ConnectionCallback advertisement_connect_cb =
          nullptr;
      if (connectable) {
        PW_CHECK(connectable->connection_cb);

        // All advertisement connections are first registered with
        // LowEnergyConnectionManager before being reported to higher layers.
        advertisement_connect_cb =
            [this, connectable_params = std::move(connectable)](
                AdvertisementId advertisement_id,
                std::unique_ptr<hci::LowEnergyConnection> link) mutable {
              auto register_link_cb = [advertisement_id,
                                       connection_callback = std::move(
                                           connectable_params->connection_cb)](
                                          ConnectionResult result) {
                connection_callback(advertisement_id, std::move(result));
              };

              adapter_->le_connection_manager_->RegisterRemoteInitiatedLink(
                  std::move(link),
                  connectable_params->bondable_mode,
                  std::move(register_link_cb));
            };
      }

      adapter_->le_advertising_manager_->StartAdvertising(
          std::move(data),
          std::move(scan_rsp),
          std::move(advertisement_connect_cb),
          interval,
          extended_pdu,
          anonymous,
          include_tx_power_level,
          address_type,
          std::move(status_callback));
      adapter_->metrics_.le.start_advertising_events.Add();
    }

    void StartDiscovery(bool active,
                        std::vector<hci::DiscoveryFilter> discovery_filters,
                        SessionCallback callback) override {
      adapter_->le_discovery_manager_->StartDiscovery(
          active, std::move(discovery_filters), std::move(callback));
      adapter_->metrics_.le.start_discovery_events.Add();
    }

    hci::Result<PeriodicAdvertisingSyncHandle> SyncToPeriodicAdvertisement(
        PeerId peer,
        uint8_t advertising_sid,
        SyncOptions options,
        PeriodicAdvertisingSyncDelegate& delegate) override {
      if (!adapter_->periodic_advertising_sync_manager_) {
        return fit::error(hci::Error(HostError::kNotSupported));
      }
      return adapter_->periodic_advertising_sync_manager_->CreateSync(
          peer, advertising_sid, options, delegate);
    }

    void EnablePrivacy(bool enabled) override {
      adapter_->le_address_manager_->EnablePrivacy(enabled);
    }

    bool PrivacyEnabled() const override {
      return adapter_->le_address_manager_->PrivacyEnabled();
    }

    const DeviceAddress CurrentAddress() const override {
      return adapter_->le_address_manager_->current_address();
    }

    void register_address_changed_callback(fit::closure callback) override {
      auto cb = [addr_changed_cb = std::move(callback)](auto) {
        addr_changed_cb();
      };
      adapter_->le_address_manager_->register_address_changed_callback(
          std::move(cb));
    }

    void set_irk(const std::optional<UInt128>& irk) override {
      adapter_->le_address_manager_->set_irk(irk);
    }

    std::optional<UInt128> irk() const override {
      return adapter_->le_address_manager_->irk();
    }

    void set_request_timeout_for_testing(
        pw::chrono::SystemClock::duration value) override {
      adapter_->le_connection_manager_->set_request_timeout_for_testing(value);
    }

    void set_scan_period_for_testing(
        pw::chrono::SystemClock::duration period) override {
      adapter_->le_discovery_manager_->set_scan_period(period);
    }

   private:
    AdapterImpl* adapter_;
  };

  LowEnergy* le() const override { return low_energy_.get(); }

  class BrEdrImpl final : public BrEdr {
   public:
    explicit BrEdrImpl(AdapterImpl* adapter) : adapter_(adapter) {}

    bool Connect(PeerId peer_id, ConnectResultCallback callback) override {
      return adapter_->bredr_connection_manager_->Connect(peer_id,
                                                          std::move(callback));
      adapter_->metrics_.bredr.outgoing_connection_requests.Add();
    }

    bool Disconnect(PeerId peer_id, DisconnectReason reason) override {
      return adapter_->bredr_connection_manager_->Disconnect(peer_id, reason);
    }

    void OpenL2capChannel(PeerId peer_id,
                          l2cap::Psm psm,
                          BrEdrSecurityRequirements security_requirements,
                          l2cap::ChannelParameters params,
                          l2cap::ChannelCallback cb) override {
      adapter_->metrics_.bredr.open_l2cap_channel_requests.Add();
      adapter_->bredr_connection_manager_->OpenL2capChannel(
          peer_id, psm, security_requirements, params, std::move(cb));
    }

    PeerId GetPeerId(hci_spec::ConnectionHandle handle) const override {
      return adapter_->bredr_connection_manager_->GetPeerId(handle);
    }

    SearchId AddServiceSearch(const UUID& uuid,
                              std::unordered_set<sdp::AttributeId> attributes,
                              SearchCallback callback) override {
      return adapter_->bredr_connection_manager_->AddServiceSearch(
          uuid, std::move(attributes), std::move(callback));
    }

    bool RemoveServiceSearch(SearchId id) override {
      return adapter_->bredr_connection_manager_->RemoveServiceSearch(id);
    }

    void Pair(PeerId peer_id,
              BrEdrSecurityRequirements security,
              hci::ResultFunction<> callback) override {
      adapter_->bredr_connection_manager_->Pair(
          peer_id, security, std::move(callback));
      adapter_->metrics_.bredr.pair_requests.Add();
    }

    void SetBrEdrSecurityMode(BrEdrSecurityMode mode) override {
      adapter_->bredr_connection_manager_->SetSecurityMode(mode);
    }

    BrEdrSecurityMode security_mode() const override {
      return adapter_->bredr_connection_manager_->security_mode();
    }

    void SetConnectable(bool connectable,
                        hci::ResultFunction<> status_cb) override {
      adapter_->bredr_connection_manager_->SetConnectable(connectable,
                                                          std::move(status_cb));
      if (connectable) {
        adapter_->metrics_.bredr.set_connectable_true_events.Add();
      } else {
        adapter_->metrics_.bredr.set_connectable_false_events.Add();
      }
    }

    void RequestDiscovery(DiscoveryCallback callback) override {
      adapter_->bredr_discovery_manager_->RequestDiscovery(std::move(callback));
    }

    void RequestDiscoverable(DiscoverableCallback callback) override {
      adapter_->bredr_discovery_manager_->RequestDiscoverable(
          std::move(callback));
    }

    RegistrationHandle RegisterService(
        std::vector<sdp::ServiceRecord> records,
        l2cap::ChannelParameters chan_params,
        ServiceConnectCallback conn_cb) override {
      return adapter_->sdp_server_->RegisterService(
          std::move(records), chan_params, std::move(conn_cb));
    }

    bool UnregisterService(RegistrationHandle handle) override {
      return adapter_->sdp_server_->UnregisterService(handle);
    }

    std::vector<sdp::ServiceRecord> GetRegisteredServices(
        RegistrationHandle handle) const override {
      return adapter_->sdp_server_->GetRegisteredServices(handle);
    }

    std::optional<ScoRequestHandle> OpenScoConnection(
        PeerId peer_id,
        const bt::StaticPacket<
            pw::bluetooth::emboss::SynchronousConnectionParametersWriter>&
            parameters,
        sco::ScoConnectionManager::OpenConnectionCallback callback) override {
      return adapter_->bredr_connection_manager_->OpenScoConnection(
          peer_id, parameters, std::move(callback));
    }
    std::optional<ScoRequestHandle> AcceptScoConnection(
        PeerId peer_id,
        const std::vector<bt::StaticPacket<
            pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
            parameters,
        sco::ScoConnectionManager::AcceptConnectionCallback callback) override {
      return adapter_->bredr_connection_manager_->AcceptScoConnection(
          peer_id, std::move(parameters), std::move(callback));
    }

   private:
    AdapterImpl* adapter_;
  };

  BrEdr* bredr() const override { return bredr_.get(); }

  PeerCache* peer_cache() override { return &peer_cache_; }

  bool AddBondedPeer(BondingData bonding_data) override;

  void SetPairingDelegate(PairingDelegate::WeakPtr delegate) override;

  bool IsDiscoverable() const override;

  bool IsDiscovering() const override;

  void SetLocalName(std::string name, hci::ResultFunction<> callback) override;

  std::string local_name() const override {
    return bredr_discovery_manager_->local_name();
  }

  void SetDeviceClass(DeviceClass dev_class,
                      hci::ResultFunction<> callback) override;

  void GetSupportedDelayRange(
      const bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter>& codec_id,
      pw::bluetooth::emboss::LogicalTransportType logical_transport_type,
      pw::bluetooth::emboss::DataPathDirection direction,
      const std::optional<std::vector<uint8_t>>& codec_configuration,
      GetSupportedDelayRangeCallback cb) override;

  void set_auto_connect_callback(AutoConnectCallback callback) override {
    auto_conn_cb_ = std::move(callback);
  }

  void AttachInspect(inspect::Node& parent, std::string name) override;

  WeakSelf<Adapter>::WeakPtr AsWeakPtr() override {
    return weak_self_adapter_.GetWeakPtr();
  }

 private:
  // Called by Initialize() after Transport is initialized.
  void InitializeStep1();

  // Second step of the initialization sequence. Called by InitializeStep1()
  // when the first batch of HCI commands have been sent.
  void InitializeStep2();

  // Third step of the initialization sequence. Called by InitializeStep2() when
  // the second batch of HCI commands have been sent.
  void InitializeStep3();

  // Fourth step of the initialization sequence. Called by InitializeStep3()
  // when the third batch of HCI commands have been sent.
  void InitializeStep4();

  // Returns true if initialization was completed, or false if initialization is
  // not in progress.
  bool CompleteInitialization(bool success);

  // Initializes the ISO data channels on devices that support it.
  void PerformIsoInitialization();

  // Reads LMP feature mask's bits from |page|
  void InitQueueReadLMPFeatureMaskPage(uint8_t page);

  // Assigns properties to |adapter_node_| using values discovered during other
  // initialization steps.
  void UpdateInspectProperties();

  // Called by ShutDown() and during Initialize() in case of failure. This
  // synchronously cleans up the transports and resets initialization state.
  void CleanUp();

  // Called by Transport after it experiences a fatal error.
  void OnTransportError();

  // Called when a directed connectable advertisement is received from a bonded
  // LE device. This amounts to a connection request from a bonded peripheral
  // which is handled by routing the request to |le_connection_manager_| to
  // initiate a Direct Connection Establishment procedure (Vol 3, Part C,
  // 9.3.8).
  void OnLeAutoConnectRequest(Peer* peer);

  // Called by |le_address_manager_| to query whether it is currently allowed to
  // reconfigure the LE random address.
  bool IsLeRandomAddressChangeAllowed();

  // Called when we receive an LE Get Vendor Capabilities Command Complete from
  // the Controller
  void ParseLEGetVendorCapabilitiesCommandComplete(
      const hci::EventPacket& event);

  hci::AdvertisingPacketFilter::Config GetPacketFilterConfig() const {
    using PeerDeliveryMode = hci::AdvertisingPacketFilter::Config::DeliveryMode;

    bool offloading_enabled = false;
    uint8_t max_filters = 0;
    PeerDeliveryMode peer_delivery_mode = PeerDeliveryMode::kImmediate;

    // TODO(b/448475405): We suspect there is a bug with advertising packet
    // filtering where we don't get scan results on time from the Controller. So
    // as not to affect others who use Bluetooth scanning, disable advertising
    // packet filtering for now while we investigate.
    // constexpr pw::bluetooth::Controller::FeaturesBits feature =
    //     pw::bluetooth::Controller::FeaturesBits::kAndroidVendorExtensions;
    // if (state().IsControllerFeatureSupported(feature) &&
    //     state().android_vendor_capabilities.has_value() &&
    //     state().android_vendor_capabilities->supports_filtering()) {
    //   offloading_enabled = true;
    //   max_filters = state().android_vendor_capabilities->max_filters();
    // }

    bt_log(INFO,
           "gap",
           "controller support for offloaded packet filtering: %s, "
           "max_filters: %d",
           offloading_enabled ? "yes" : "no",
           max_filters);

    return hci::AdvertisingPacketFilter::Config(
        offloading_enabled, max_filters, peer_delivery_mode);
  }

  std::unique_ptr<hci::LowEnergyAdvertiser> CreateAdvertiser(bool extended) {
    constexpr auto kAndroidVendorExtensions =
        pw::bluetooth::Controller::FeaturesBits::kAndroidVendorExtensions;
    std::unique_ptr<hci::LowEnergyAdvertiser> advertiser;
    if (extended) {
      advertiser = std::make_unique<hci::ExtendedLowEnergyAdvertiser>(
          hci_, state_.low_energy_state.max_advertising_data_length_);
    } else if (state().IsControllerFeatureSupported(kAndroidVendorExtensions) &&
               state().android_vendor_capabilities.has_value()) {
      uint8_t max_advt =
          state()
              .android_vendor_capabilities->max_simultaneous_advertisements();
      bt_log(INFO,
             "gap",
             "controller support for extended advertising via android vendor "
             "extensions: yes, max simultaneous advertisements: %d",
             max_advt);
      advertiser = std::make_unique<hci::AndroidExtendedLowEnergyAdvertiser>(
          hci_, max_advt);
    } else {
      advertiser = std::make_unique<hci::LegacyLowEnergyAdvertiser>(hci_);
    }

    advertiser->AttachInspect(adapter_node_);
    return advertiser;
  }

  std::unique_ptr<hci::LowEnergyConnector> CreateConnector(
      bool extended) const {
    return std::make_unique<hci::LowEnergyConnector>(
        hci_,
        le_address_manager_.get(),
        dispatcher_,
        fit::bind_member<&hci::LowEnergyAdvertiser::OnIncomingConnection>(
            hci_le_advertiser_.get()),
        extended);
  }

  std::unique_ptr<hci::LowEnergyScanner> CreateScanner(
      bool extended,
      const hci::AdvertisingPacketFilter::Config& packet_filter_config) const {
    if (extended) {
      return std::make_unique<hci::ExtendedLowEnergyScanner>(
          le_address_manager_.get(), packet_filter_config, hci_, dispatcher_);
    }

    return std::make_unique<hci::LegacyLowEnergyScanner>(
        le_address_manager_.get(), packet_filter_config, hci_, dispatcher_);
  }

  // Must be initialized first so that child nodes can be passed to other
  // constructors.
  inspect::Node adapter_node_;
  struct InspectProperties {
    inspect::StringProperty adapter_id;
    inspect::StringProperty hci_version;
    inspect::UintProperty bredr_max_num_packets;
    inspect::UintProperty bredr_max_data_length;
    inspect::UintProperty le_max_num_packets;
    inspect::UintProperty le_max_data_length;
    inspect::UintProperty sco_max_num_packets;
    inspect::UintProperty sco_max_data_length;
    inspect::StringProperty lmp_features;
    inspect::StringProperty le_features;
  };
  InspectProperties inspect_properties_;

  // Metrics properties
  inspect::Node metrics_node_;
  inspect::Node metrics_bredr_node_;
  inspect::Node metrics_le_node_;
  struct AdapterMetrics {
    struct LeMetrics {
      UintMetricCounter open_l2cap_channel_requests;
      UintMetricCounter outgoing_connection_requests;
      UintMetricCounter pair_requests;
      UintMetricCounter start_advertising_events;
      UintMetricCounter stop_advertising_events;
      UintMetricCounter start_discovery_events;
    } le;
    struct BrEdrMetrics {
      UintMetricCounter outgoing_connection_requests;
      UintMetricCounter pair_requests;
      UintMetricCounter set_connectable_true_events;
      UintMetricCounter set_connectable_false_events;
      UintMetricCounter open_l2cap_channel_requests;
    } bredr;
  };
  AdapterMetrics metrics_;

  // Uniquely identifies this adapter on the current system.
  AdapterId identifier_;

  pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider_;

  hci::Transport::WeakPtr hci_;

  // Callback invoked to notify clients when the underlying transport is
  // closed.
  fit::closure transport_error_cb_;

  // Parameters relevant to the initialization sequence.
  // TODO(armansito): The Initialize()/ShutDown() pattern has become common
  // enough in this project that it might be worth considering moving the
  // init-state-keeping into an abstract base.
  enum State {
    kNotInitialized = 0,
    kInitializing,
    kInitialized,
  };
  std::atomic<State> init_state_;
  std::unique_ptr<hci::SequentialCommandRunner> init_seq_runner_;

  // The callback passed to Initialize(). Null after initialization completes.
  InitializeCallback init_cb_;

  // Contains the global adapter state.
  AdapterState state_;

  // The maximum LMP feature page that we will read.
  std::optional<size_t> max_lmp_feature_page_index_;

  // Provides access to discovered, connected, and/or bonded remote Bluetooth
  // devices.
  PeerCache peer_cache_;

  // L2CAP layer used by GAP. This must be destroyed after the following
  // members because they raw pointers to this member.
  std::unique_ptr<l2cap::ChannelManager> l2cap_;

  // The GATT profile. We use this reference to add and remove data bearers
  // and for service discovery.
  gatt::GATT::WeakPtr gatt_;

  // Contains feature flags based on the product's configuration
  Config config_;

  // Objects that abstract the controller for connection and advertising
  // procedures.
  std::unique_ptr<hci::LowEnergyAdvertiser> hci_le_advertiser_;
  std::unique_ptr<hci::LowEnergyConnector> hci_le_connector_;
  std::unique_ptr<hci::LowEnergyScanner> hci_le_scanner_;

  // Objects that perform LE procedures.
  std::unique_ptr<LowEnergyAddressManager> le_address_manager_;
  std::unique_ptr<LowEnergyDiscoveryManager> le_discovery_manager_;
  std::optional<PeriodicAdvertisingSyncManager>
      periodic_advertising_sync_manager_;
  std::unique_ptr<LowEnergyConnectionManager> le_connection_manager_;
  std::unique_ptr<LowEnergyAdvertisingManager> le_advertising_manager_;
  std::unique_ptr<LowEnergyImpl> low_energy_;

  // Objects that perform BR/EDR procedures.
  std::unique_ptr<BrEdrConnectionManager> bredr_connection_manager_;
  std::unique_ptr<BrEdrDiscoveryManager> bredr_discovery_manager_;
  std::unique_ptr<sdp::Server> sdp_server_;
  std::unique_ptr<BrEdrImpl> bredr_;

  // Callback to propagate ownership of an auto-connected LE link.
  AutoConnectCallback auto_conn_cb_;

  pw::async::Dispatcher& dispatcher_;

  // This must remain the last member to make sure that all weak pointers are
  // invalidating before other members are destroyed.
  WeakSelf<AdapterImpl> weak_self_;
  WeakSelf<Adapter> weak_self_adapter_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AdapterImpl);
};

AdapterImpl::AdapterImpl(
    pw::async::Dispatcher& pw_dispatcher,
    hci::Transport::WeakPtr hci,
    gatt::GATT::WeakPtr gatt,
    Config config,
    std::unique_ptr<l2cap::ChannelManager> l2cap,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider)
    : identifier_(Random<AdapterId>()),
      wake_lease_provider_(wake_lease_provider),
      hci_(std::move(hci)),
      init_state_(State::kNotInitialized),
      peer_cache_(pw_dispatcher),
      l2cap_(std::move(l2cap)),
      gatt_(std::move(gatt)),
      config_(config),
      dispatcher_(pw_dispatcher),
      weak_self_(this),
      weak_self_adapter_(this) {
  PW_DCHECK(hci_.is_alive());
  PW_DCHECK(gatt_.is_alive());

  auto self = weak_self_.GetWeakPtr();
  hci_->SetTransportErrorCallback([self] {
    if (self.is_alive()) {
      self->OnTransportError();
    }
  });

  gatt_->SetPersistServiceChangedCCCCallback(
      [this](PeerId peer_id, gatt::ServiceChangedCCCPersistedData gatt_data) {
        Peer* peer = peer_cache_.FindById(peer_id);
        if (!peer) {
          bt_log(WARN,
                 "gap",
                 "Unable to find peer %s when storing persisted GATT data.",
                 bt_str(peer_id));
        } else if (!peer->le()) {
          bt_log(WARN,
                 "gap",
                 "Tried to store persisted GATT data for non-LE peer %s.",
                 bt_str(peer_id));
        } else {
          peer->MutLe().set_service_changed_gatt_data(gatt_data);
        }
      });

  gatt_->SetRetrieveServiceChangedCCCCallback([this](PeerId peer_id) {
    Peer* peer = peer_cache_.FindById(peer_id);
    if (!peer) {
      bt_log(WARN,
             "gap",
             "Unable to find peer %s when retrieving persisted GATT data.",
             peer_id.ToString().c_str());
      return std::optional<gatt::ServiceChangedCCCPersistedData>();
    }

    if (!peer->le()) {
      bt_log(WARN,
             "gap",
             "Tried to retrieve persisted GATT data for non-LE peer %s.",
             peer_id.ToString().c_str());
      return std::optional<gatt::ServiceChangedCCCPersistedData>();
    }

    return std::optional(peer->le()->get_service_changed_gatt_data());
  });
}

AdapterImpl::~AdapterImpl() {
  if (IsInitialized()) {
    ShutDown();
  }
}

bool AdapterImpl::Initialize(InitializeCallback callback,
                             fit::closure transport_error_cb) {
  PW_DCHECK(callback);
  PW_DCHECK(transport_error_cb);

  if (IsInitialized()) {
    bt_log(WARN, "gap", "Adapter already initialized");
    return false;
  }

  PW_DCHECK(!IsInitializing());
  PW_DCHECK(!init_seq_runner_);

  init_state_ = State::kInitializing;
  init_cb_ = std::move(callback);
  transport_error_cb_ = std::move(transport_error_cb);

  hci_->Initialize([this](bool success) {
    if (!success) {
      bt_log(ERROR, "gap", "Failed to initialize Transport");
      CompleteInitialization(/*success=*/false);
      return;
    }
    init_seq_runner_ = std::make_unique<hci::SequentialCommandRunner>(
        hci_->command_channel()->AsWeakPtr());

    InitializeStep1();
  });

  return true;
}

void AdapterImpl::ShutDown() {
  bt_log(DEBUG, "gap", "adapter shutting down");

  if (IsInitializing()) {
    PW_DCHECK(!init_seq_runner_->IsReady());
    init_seq_runner_->Cancel();
  }

  CleanUp();
}

bool AdapterImpl::AddBondedPeer(BondingData bonding_data) {
  return peer_cache()->AddBondedPeer(bonding_data);
}

void AdapterImpl::SetPairingDelegate(PairingDelegate::WeakPtr delegate) {
  le_connection_manager_->SetPairingDelegate(delegate);
  bredr_connection_manager_->SetPairingDelegate(delegate);
}

bool AdapterImpl::IsDiscoverable() const {
  if (bredr_discovery_manager_ && bredr_discovery_manager_->discoverable()) {
    return true;
  }

  // If LE Privacy is enabled, then we are not discoverable.
  // TODO(fxbug.dev/42060496): Make this dependent on whether the LE Public
  // advertisement is active or not.
  if (le_address_manager_ && le_address_manager_->PrivacyEnabled()) {
    return false;
  }

  return (le_advertising_manager_ && le_advertising_manager_->advertising());
}

bool AdapterImpl::IsDiscovering() const {
  return (le_discovery_manager_ && le_discovery_manager_->discovering()) ||
         (bredr_discovery_manager_ && bredr_discovery_manager_->discovering());
}

void AdapterImpl::SetLocalName(std::string name,
                               hci::ResultFunction<> callback) {
  // TODO(fxbug.dev/42116852): set the public LE advertisement name from
  // |name| If BrEdr is not supported, skip the name update.
  if (!bredr_discovery_manager_) {
    callback(ToResult(bt::HostError::kNotSupported));
    return;
  }

  // Make a copy of |name| to move separately into the lambda.
  std::string name_copy(name);
  bredr_discovery_manager_->UpdateLocalName(
      std::move(name),
      [this, cb = std::move(callback), local_name = std::move(name_copy)](
          auto status) {
        if (!bt_is_error(status, WARN, "gap", "set local name failed")) {
          state_.local_name = local_name;
        }
        cb(status);
      });
}

void AdapterImpl::SetDeviceClass(DeviceClass dev_class,
                                 hci::ResultFunction<> callback) {
  auto write_dev_class = hci::CommandPacket::New<
      pw::bluetooth::emboss::WriteClassOfDeviceCommandWriter>(
      hci_spec::kWriteClassOfDevice);
  write_dev_class.view_t().class_of_device().BackingStorage().WriteUInt(
      dev_class.to_int());
  hci_->command_channel()
      ->SendCommand(
          std::move(write_dev_class),
          [cb = std::move(callback)](auto, const hci::EventPacket& event) {
            HCI_IS_ERROR(event, WARN, "gap", "set device class failed");
            cb(event.ToResult());
          })
      .IgnoreError();
}

void AdapterImpl::GetSupportedDelayRange(
    const bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter>& codec_id,
    pw::bluetooth::emboss::LogicalTransportType logical_transport_type,
    pw::bluetooth::emboss::DataPathDirection direction,
    const std::optional<std::vector<uint8_t>>& codec_configuration,
    GetSupportedDelayRangeCallback cb) {
  if (!state_.SupportedCommands()
           .read_local_supported_controller_delay()
           .Read()) {
    bt_log(WARN,
           "gap",
           "read local supported controller delay command not supported");
    cb(PW_STATUS_UNIMPLEMENTED, /*min=*/0, /*max=*/0);
    return;
  }
  bt_log(INFO, "gap", "retrieving controller codec delay");
  size_t codec_configuration_size = 0;
  if (codec_configuration.has_value()) {
    codec_configuration_size = codec_configuration->size();
  }
  size_t packet_size =
      pw::bluetooth::emboss::ReadLocalSupportedControllerDelayCommand::
          MinSizeInBytes() +
      codec_configuration_size;
  auto cmd_packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::ReadLocalSupportedControllerDelayCommandWriter>(
      hci_spec::kReadLocalSupportedControllerDelay, packet_size);
  auto cmd_view = cmd_packet.view_t();
  cmd_view.codec_id().CopyFrom(
      const_cast<bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter>&>(
          codec_id)
          .view());
  cmd_view.logical_transport_type().Write(logical_transport_type);
  cmd_view.direction().Write(direction);
  cmd_view.codec_configuration_length().Write(codec_configuration_size);
  if (codec_configuration.has_value()) {
    std::memcpy(cmd_view.codec_configuration().BackingStorage().data(),
                codec_configuration->data(),
                codec_configuration_size);
  }

  hci_->command_channel()
      ->SendCommand(
          std::move(cmd_packet),
          [callback = std::move(cb)](auto /*id*/,
                                     const hci::EventPacket& event) {
            auto view = event.view<
                pw::bluetooth::emboss::
                    ReadLocalSupportedControllerDelayCommandCompleteEventView>();
            if (HCI_IS_ERROR(event,
                             WARN,
                             "gap",
                             "read local supported controller delay failed")) {
              callback(PW_STATUS_UNKNOWN, /*min=*/0, /*max=*/0);
              return;
            }
            bt_log(INFO, "gap", "controller delay read successfully");
            callback(PW_STATUS_OK,
                     view.min_controller_delay().Read(),
                     view.max_controller_delay().Read());
          })
      .IgnoreError();
}

void AdapterImpl::AttachInspect(inspect::Node& parent, std::string name) {
  adapter_node_ = parent.CreateChild(name);
  UpdateInspectProperties();

  peer_cache_.AttachInspect(adapter_node_);

  metrics_node_ = adapter_node_.CreateChild(kMetricsInspectNodeName);

  metrics_le_node_ = metrics_node_.CreateChild("le");
  metrics_.le.open_l2cap_channel_requests.AttachInspect(
      metrics_le_node_, "open_l2cap_channel_requests");
  metrics_.le.outgoing_connection_requests.AttachInspect(
      metrics_le_node_, "outgoing_connection_requests");
  metrics_.le.pair_requests.AttachInspect(metrics_le_node_, "pair_requests");
  metrics_.le.start_advertising_events.AttachInspect(
      metrics_le_node_, "start_advertising_events");
  metrics_.le.stop_advertising_events.AttachInspect(metrics_le_node_,
                                                    "stop_advertising_events");
  metrics_.le.start_discovery_events.AttachInspect(metrics_le_node_,
                                                   "start_discovery_events");

  metrics_bredr_node_ = metrics_node_.CreateChild("bredr");
  metrics_.bredr.outgoing_connection_requests.AttachInspect(
      metrics_bredr_node_, "outgoing_connection_requests");
  metrics_.bredr.pair_requests.AttachInspect(metrics_bredr_node_,
                                             "pair_requests");
  metrics_.bredr.set_connectable_true_events.AttachInspect(
      metrics_bredr_node_, "set_connectable_true_events");
  metrics_.bredr.set_connectable_false_events.AttachInspect(
      metrics_bredr_node_, "set_connectable_false_events");
  metrics_.bredr.open_l2cap_channel_requests.AttachInspect(
      metrics_bredr_node_, "open_l2cap_channel_requests");
}

void AdapterImpl::ParseLEGetVendorCapabilitiesCommandComplete(
    const hci::EventPacket& event) {
  // NOTE: There can be various versions of this command complete event
  // sent by the Controller. As fields are added, the version_supported
  // field is incremented to signify which fields are available. In a previous
  // undertaking (pwrev.dev/203950, fxrev.dev/1029396), we attempted to use
  // Emboss' conditional fields feature to define fields based on the version
  // they are included in. However, in practice, we've found vendors sometimes
  // send the wrong number of bytes required for the version they claim to
  // send. To tolerate these types of errors, we simply define all the fields
  // in Emboss. If we receive a response smaller than what we expect, we use
  // what the vendor sends, and fill the rest with zero to disable the
  // feature. If we receive a response larger than what we expect, we read up
  // to what we support and drop the rest of the data.
  StaticPacket<android_emb::LEGetVendorCapabilitiesCommandCompleteEventView>
      packet;
  packet.SetToZeros();
  size_t copy_size = std::min(packet.data().size(), event.size());
  packet.mutable_data().Write(event.data().data(), copy_size);

  auto params = packet.view();
  state_.android_vendor_capabilities = AndroidVendorCapabilities::New(
      params, config_.override_vendor_capabilites_version);

  uint8_t major = params.version_supported().major_number().Read();
  uint8_t minor = params.version_supported().minor_number().Read();

  std::optional<size_t> expected_size;
  if (major == 0 && minor == 0) {
    // The version_supported field was only introduced into the command in
    // Version 0.95. Controllers that use the base version, Version 0.55,
    // don't have the version_supported field.
    expected_size = android_emb::LEGetVendorCapabilitiesCommandCompleteEvent::
        version_0_55_size();
  } else if (major == 0 && minor == 95) {
    expected_size = android_emb::LEGetVendorCapabilitiesCommandCompleteEvent::
        version_0_95_size();
  } else if (major == 0 && minor == 96) {
    expected_size = android_emb::LEGetVendorCapabilitiesCommandCompleteEvent::
        version_0_96_size();
  } else if (major == 0 && minor == 98) {
    expected_size = android_emb::LEGetVendorCapabilitiesCommandCompleteEvent::
        version_0_98_size();
  } else if (major == 1 && minor == 03) {
    expected_size = android_emb::LEGetVendorCapabilitiesCommandCompleteEvent::
        version_1_03_size();
  } else if (major == 1 && minor == 04) {
    expected_size = android_emb::LEGetVendorCapabilitiesCommandCompleteEvent::
        version_1_04_size();
  } else if (major == 1 && minor == 05) {
    expected_size = android_emb::LEGetVendorCapabilitiesCommandCompleteEvent::
        version_1_05_size();
  }

  if (!expected_size.has_value()) {
    bt_log(INFO,
           "gap",
           "received LE Get Vendor Capabilities version %d.%d (%zu bytes), we "
           "don't support this version yet, reading the fields we do support",
           major,
           minor,
           event.size());
    return;
  }

  if (event.size() != expected_size) {
    bt_log(WARN,
           "gap",
           "received LE Get Vendor Capabilities version %d.%d (received %zu "
           "bytes, expected %zu bytes)",
           major,
           minor,
           event.size(),
           expected_size.value_or(-1));
  }
}

void AdapterImpl::InitializeStep1() {
  state_.controller_features = hci_->GetFeatures();

  // Start by resetting the controller to a clean state and then send
  // informational parameter commands that are not specific to LE or BR/EDR.
  // The commands sent here are mandatory for all LE controllers.
  //
  // NOTE: It's safe to pass capture |this| directly in the callbacks as
  // |init_seq_runner_| will internally invalidate the callbacks if it ever
  // gets deleted.

  // HCI_Reset
  auto reset_command =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  init_seq_runner_->QueueCommand(std::move(reset_command));

  // HCI_Read_Local_Version_Information
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New<
          pw::bluetooth::emboss::ReadLocalVersionInformationCommandView>(
          hci_spec::kReadLocalVersionInfo),
      [this](const hci::EventPacket& cmd_complete) {
        if (HCI_IS_ERROR(
                cmd_complete, WARN, "gap", "read local version info failed")) {
          return;
        }
        auto params =
            cmd_complete
                .view<pw::bluetooth::emboss::
                          ReadLocalVersionInfoCommandCompleteEventView>();
        state_.hci_version = params.hci_version().Read();
      });

  // HCI_Read_Local_Supported_Commands
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New<
          pw::bluetooth::emboss::ReadLocalSupportedCommandsCommandView>(
          hci_spec::kReadLocalSupportedCommands),
      [this](const hci::EventPacket& cmd_complete) {
        if (HCI_IS_ERROR(cmd_complete,
                         WARN,
                         "gap",
                         "read local supported commands failed")) {
          return;
        }
        auto view =
            cmd_complete
                .view<pw::bluetooth::emboss::
                          ReadLocalSupportedCommandsCommandCompleteEventView>();
        std::copy(view.supported_commands().BackingStorage().begin(),
                  view.supported_commands().BackingStorage().end(),
                  state_.supported_commands);
      });

  // HCI_Read_Local_Supported_Features
  InitQueueReadLMPFeatureMaskPage(0);

  // HCI_Read_BD_ADDR
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New<pw::bluetooth::emboss::ReadBdAddrCommandView>(
          hci_spec::kReadBDADDR),
      [this](const hci::EventPacket& cmd_complete) {
        if (HCI_IS_ERROR(cmd_complete, WARN, "gap", "read BR_ADDR failed")) {
          return;
        }
        auto packet = cmd_complete.view<
            pw::bluetooth::emboss::ReadBdAddrCommandCompleteEventView>();
        state_.controller_address = DeviceAddressBytes(packet.bd_addr());
      });

  bool android_vendor_extension_support = state().IsControllerFeatureSupported(
      pw::bluetooth::Controller::FeaturesBits::kAndroidVendorExtensions);
  bt_log(INFO,
         "gap",
         "controller support for android hci vendor extensions: %s",
         android_vendor_extension_support ? "yes" : "no");
  if (android_vendor_extension_support) {
    init_seq_runner_->QueueCommand(
        hci::CommandPacket::New<
            android_emb::LEGetVendorCapabilitiesCommandView>(
            android_hci::kLEGetVendorCapabilities),
        [this](const hci::EventPacket& event) {
          if (HCI_IS_ERROR(
                  event,
                  WARN,
                  "gap",
                  "Failed to query android hci extension capabilities")) {
            return;
          }

          ParseLEGetVendorCapabilitiesCommandComplete(event);
        });
  }

  init_seq_runner_->RunCommands([this](hci::Result<> status) mutable {
    if (bt_is_error(status,
                    ERROR,
                    "gap",
                    "Failed to obtain initial controller information: %s",
                    bt_str(status))) {
      CompleteInitialization(/*success=*/false);
      return;
    }

    InitializeStep2();
  });
}

void AdapterImpl::InitializeStep2() {
  PW_DCHECK(IsInitializing());

  // Low Energy MUST be supported. We don't support BR/EDR-only controllers.
  if (!state_.IsLowEnergySupported()) {
    bt_log(ERROR, "gap", "Bluetooth LE not supported by controller");
    CompleteInitialization(/*success=*/false);
    return;
  }

  // Check the HCI version. We officially only support 4.2+ only but for now
  // we just log a warning message if the version is legacy.
  if (state_.hci_version <
      pw::bluetooth::emboss::CoreSpecificationVersion::V4_2) {
    bt_log(WARN,
           "gap",
           "controller is using legacy HCI version %s",
           hci_spec::HCIVersionToString(state_.hci_version).c_str());
  }

  PW_DCHECK(init_seq_runner_->IsReady());

  if (state_.SupportedCommands().write_default_link_policy_settings().Read()) {
    // HCI_Write_Default_Link_Policy_Settings
    auto write_default_link_policy_cmd = hci::CommandPacket::New<
        pw::bluetooth::emboss::WriteDefaultLinkPolicySettingsCommandWriter>(
        hci_spec::kWriteDefaultLinkPolicySettings);
    auto view = write_default_link_policy_cmd.view_t();
    view.default_link_policy_settings().enable_role_switch().Write(1);
    view.default_link_policy_settings().enable_hold_mode().Write(0);
    view.default_link_policy_settings().enable_sniff_mode().Write(1);
    init_seq_runner_->QueueCommand(
        write_default_link_policy_cmd,
        [](const hci::EventPacket& cmd_complete) {
          if (cmd_complete.ToResult().is_error()) {
            bt_log(WARN, "gap", "Set Default Link Policy command FAILED");
          } else {
            bt_log(INFO, "gap", "Set Default Link Policy succeeded");
          }
        });
  }

  // If the controller supports the Read Buffer Size command then send it.
  // Otherwise we'll default to 0 when initializing the ACLDataChannel.
  if (state_.SupportedCommands().read_buffer_size().Read()) {
    // HCI_Read_Buffer_Size
    init_seq_runner_->QueueCommand(
        hci::CommandPacket::New<
            pw::bluetooth::emboss::ReadBufferSizeCommandView>(
            hci_spec::kReadBufferSize),
        [this](const hci::EventPacket& cmd_complete) {
          if (HCI_IS_ERROR(
                  cmd_complete, WARN, "gap", "read buffer size failed")) {
            return;
          }
          auto packet = cmd_complete.view<
              pw::bluetooth::emboss::ReadBufferSizeCommandCompleteEventView>();
          uint16_t acl_mtu = packet.acl_data_packet_length().Read();
          uint16_t acl_max_count = packet.total_num_acl_data_packets().Read();
          if (acl_mtu && acl_max_count) {
            state_.bredr_data_buffer_info =
                hci::DataBufferInfo(acl_mtu, acl_max_count);
          }
          // Use UncheckedRead because this field is supposed to
          // be 0x01-0xFF, but it is possible and harmless for controllers to
          // set to 0x00 if not supported.
          uint16_t sco_mtu =
              packet.synchronous_data_packet_length().UncheckedRead();
          uint16_t sco_max_count =
              packet.total_num_synchronous_data_packets().Read();
          if (sco_mtu && sco_max_count) {
            state_.sco_buffer_info =
                hci::DataBufferInfo(sco_mtu, sco_max_count);
          }
        });
  }

  // HCI_LE_Read_Local_Supported_Features
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New<
          pw::bluetooth::emboss::LEReadLocalSupportedFeaturesCommandView>(
          hci_spec::kLEReadLocalSupportedFeatures),
      [this](const hci::EventPacket& cmd_complete) {
        if (HCI_IS_ERROR(cmd_complete,
                         WARN,
                         "gap",
                         "LE read local supported features failed")) {
          return;
        }
        auto packet = cmd_complete.view<
            pw::bluetooth::emboss::
                LEReadLocalSupportedFeaturesCommandCompleteEventView>();
        state_.low_energy_state.supported_features_ =
            packet.le_features().BackingStorage().ReadUInt();
      });

  // HCI_LE_Read_Supported_States
  init_seq_runner_->QueueCommand(
      hci::CommandPacket::New<
          pw::bluetooth::emboss::LEReadSupportedStatesCommandView>(
          hci_spec::kLEReadSupportedStates),
      [this](const hci::EventPacket& cmd_complete) {
        if (HCI_IS_ERROR(cmd_complete,
                         WARN,
                         "gap",
                         "LE read local supported states failed")) {
          return;
        }
        auto packet =
            cmd_complete
                .view<pw::bluetooth::emboss::
                          LEReadSupportedStatesCommandCompleteEventView>();
        state_.low_energy_state.supported_states_ =
            packet.le_states().BackingStorage().ReadLittleEndianUInt<64>();
      });

  if (state_.SupportedCommands()
          .le_read_maximum_advertising_data_length()
          .Read()) {
    // HCI_LE_Read_Maximum_Advertising_Data_Length
    init_seq_runner_->QueueCommand(
        hci::CommandPacket::New<
            pw::bluetooth::emboss::LEReadMaxAdvertisingDataLengthCommandView>(
            hci_spec::kLEReadMaximumAdvertisingDataLength),
        [this](const hci::EventPacket& cmd_complete) {
          if (HCI_IS_ERROR(cmd_complete,
                           WARN,
                           "gap",
                           "LE read maximum advertising data length failed")) {
            return;
          }

          auto params = cmd_complete.view<
              pw::bluetooth::emboss::
                  LEReadMaximumAdvertisingDataLengthCommandCompleteEventView>();
          state_.low_energy_state.max_advertising_data_length_ =
              params.max_advertising_data_length().Read();
          bt_log(INFO,
                 "gap",
                 "maximum advertising data length: %d",
                 state_.low_energy_state.max_advertising_data_length_);
        });
  } else {
    bt_log(INFO,
           "gap",
           "LE read maximum advertising data command not supported, "
           "defaulting to legacy maximum: %zu",
           hci_spec::kMaxLEAdvertisingDataLength);
    state_.low_energy_state.max_advertising_data_length_ =
        hci_spec::kMaxLEAdvertisingDataLength;
  }

  if (state_.SupportedCommands().le_read_buffer_size_v2().Read()) {
    // HCI_LE_Read_Buffer_Size [v2]
    init_seq_runner_->QueueCommand(
        hci::CommandPacket::New<
            pw::bluetooth::emboss::LEReadBufferSizeCommandV2View>(
            hci_spec::kLEReadBufferSizeV2),
        [this](const hci::EventPacket& cmd_complete) {
          if (HCI_IS_ERROR(cmd_complete,
                           WARN,
                           "gap",
                           "LE read buffer size [v2] failed")) {
            return;
          }
          auto params =
              cmd_complete
                  .view<pw::bluetooth::emboss::
                            LEReadBufferSizeV2CommandCompleteEventView>();
          uint16_t acl_mtu = params.le_acl_data_packet_length().Read();
          uint8_t acl_max_count = params.total_num_le_acl_data_packets().Read();
          if (acl_mtu && acl_max_count) {
            state_.low_energy_state.acl_data_buffer_info_ =
                hci::DataBufferInfo(acl_mtu, acl_max_count);
          }
          uint16_t iso_mtu = params.iso_data_packet_length().Read();
          uint8_t iso_max_count = params.total_num_iso_data_packets().Read();
          if (iso_mtu && iso_max_count) {
            state_.low_energy_state.iso_data_buffer_info_ =
                hci::DataBufferInfo(iso_mtu, iso_max_count);
          }
        });
  } else {
    // HCI_LE_Read_Buffer_Size [v1]
    init_seq_runner_->QueueCommand(
        hci::CommandPacket::New<
            pw::bluetooth::emboss::LEReadBufferSizeCommandV1View>(
            hci_spec::kLEReadBufferSizeV1),
        [this](const hci::EventPacket& cmd_complete) {
          if (HCI_IS_ERROR(
                  cmd_complete, WARN, "gap", "LE read buffer size failed")) {
            return;
          }
          auto params =
              cmd_complete
                  .view<pw::bluetooth::emboss::
                            LEReadBufferSizeV1CommandCompleteEventView>();
          uint16_t mtu = params.le_acl_data_packet_length().Read();
          uint8_t max_count = params.total_num_le_acl_data_packets().Read();
          if (mtu && max_count) {
            state_.low_energy_state.acl_data_buffer_info_ =
                hci::DataBufferInfo(mtu, max_count);
          }
        });
  }

  if (state_.features.HasBit(
          /*page=*/0u,
          hci_spec::LMPFeature::kSecureSimplePairingControllerSupport)) {
    // HCI_Write_Simple_Pairing_Mode
    auto write_spm = hci::CommandPacket::New<
        pw::bluetooth::emboss::WriteSimplePairingModeCommandWriter>(
        hci_spec::kWriteSimplePairingMode);
    auto write_ssp_params = write_spm.view_t();
    write_ssp_params.simple_pairing_mode().Write(
        pw::bluetooth::emboss::GenericEnableParam::ENABLE);
    init_seq_runner_->QueueCommand(
        std::move(write_spm), [](const hci::EventPacket& event) {
          // Warn if the command failed
          HCI_IS_ERROR(event, WARN, "gap", "write simple pairing mode failed");
        });
  }

  // If there are extended features then try to read the first page of the
  // extended features.
  if (state_.features.HasBit(/*page=*/0u,
                             hci_spec::LMPFeature::kExtendedFeatures)) {
    // HCI_Write_LE_Host_Support
    if (!state_.SupportedCommands().write_le_host_support().Read()) {
      bt_log(INFO, "gap", "LE Host is not supported");
    } else {
      bt_log(INFO, "gap", "LE Host is supported. Enabling LE Host mode");
      auto cmd_packet = hci::CommandPacket::New<
          pw::bluetooth::emboss::WriteLEHostSupportCommandWriter>(
          hci_spec::kWriteLEHostSupport);
      auto params = cmd_packet.view_t();
      params.le_supported_host().Write(
          pw::bluetooth::emboss::GenericEnableParam::ENABLE);
      init_seq_runner_->QueueCommand(
          std::move(cmd_packet), [](const hci::EventPacket& event) {
            HCI_IS_ERROR(event, WARN, "gap", "Write LE Host support failed");
          });
    }

    // HCI_Write_Secure_Connections_Host_Support
    if (!state_.SupportedCommands()
             .write_secure_connections_host_support()
             .Read()) {
      bt_log(INFO, "gap", "Secure Connections (Host Support) is not supported");
    } else {
      bt_log(INFO,
             "gap",
             "Secure Connections (Host Support) is supported. "
             "Enabling Secure Connections (Host Support) mode");
      auto cmd_packet = hci::CommandPacket::New<
          pw::bluetooth::emboss::
              WriteSecureConnectionsHostSupportCommandWriter>(
          hci_spec::kWriteSecureConnectionsHostSupport);
      auto params = cmd_packet.view_t();
      params.secure_connections_host_support().Write(
          pw::bluetooth::emboss::GenericEnableParam::ENABLE);
      init_seq_runner_->QueueCommand(
          std::move(cmd_packet), [](const hci::EventPacket& event) {
            HCI_IS_ERROR(event,
                         WARN,
                         "gap",
                         "Write Secure Connections (Host Support) failed");
          });
    }

    // Read updated page 1 after host support bits enabled.
    InitQueueReadLMPFeatureMaskPage(1);
  }

  init_seq_runner_->RunCommands([this](hci::Result<> status) mutable {
    if (bt_is_error(
            status,
            ERROR,
            "gap",
            "failed to obtain initial controller information (step 2)")) {
      CompleteInitialization(/*success=*/false);
      return;
    }
    InitializeStep3();
  });
}

void AdapterImpl::PerformIsoInitialization() {
  const hci::DataBufferInfo iso_data_buffer_info =
      state_.low_energy_state.iso_data_buffer_info();
  if (!iso_data_buffer_info.IsAvailable()) {
    bt_log(WARN, "gap", "Unable to read ISO data buffer information");
    return;
  }

  bt_log(INFO,
         "gap",
         "ISO data buffer information available (size: %zu, count: %zu)",
         iso_data_buffer_info.max_data_length(),
         iso_data_buffer_info.max_num_packets());

  if (!hci_->InitializeIsoDataChannel(iso_data_buffer_info)) {
    bt_log(WARN,
           "gap",
           "Failed to initialize IsoDataChannel, proceeding without HCI ISO "
           "support");
    return;
  }

  bt_log(INFO, "gap", "IsoDataChannel initialized successfully");
  bt_log(INFO, "gap", "Enabling ConnectedIsochronousStream (Host Support)");
  auto cmd_packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::LESetHostFeatureCommandWriter>(
      hci_spec::kLESetHostFeature);
  auto params = cmd_packet.view_t();
  params.bit_number().Write(
      static_cast<uint8_t>(hci_spec::LESupportedFeatureBitPos::
                               kConnectedIsochronousStreamHostSupport));
  params.bit_value().Write(pw::bluetooth::emboss::GenericEnableParam::ENABLE);
  init_seq_runner_->QueueCommand(
      std::move(cmd_packet), [](const hci::EventPacket& event) {
        HCI_IS_ERROR(
            event, WARN, "gap", "Set Host Feature (ISO support) failed");
      });
}

void AdapterImpl::InitializeStep3() {
  PW_CHECK(IsInitializing());
  PW_CHECK(init_seq_runner_->IsReady());
  PW_CHECK(!init_seq_runner_->HasQueuedCommands());

  if (!state_.bredr_data_buffer_info.IsAvailable() &&
      !state_.low_energy_state.acl_data_buffer_info().IsAvailable()) {
    bt_log(ERROR, "gap", "Both BR/EDR and LE buffers are unavailable");
    CompleteInitialization(/*success=*/false);
    return;
  }

  // Now that we have all the ACL data buffer information it's time to
  // initialize the ACLDataChannel.
  if (!hci_->InitializeACLDataChannel(
          state_.bredr_data_buffer_info,
          state_.low_energy_state.acl_data_buffer_info())) {
    bt_log(ERROR, "gap", "Failed to initialize ACLDataChannel (step 3)");
    CompleteInitialization(/*success=*/false);
    return;
  }

  // The controller may not support SCO flow control (as implied by not
  // supporting HCI_Write_Synchronous_Flow_Control_Enable), in which case we
  // don't support HCI SCO on this controller yet.
  // TODO(fxbug.dev/42171056): Support controllers that don't support
  // SCO flow control.
  bool sco_flow_control_supported =
      state_.SupportedCommands().write_synchronous_flow_control_enable().Read();
  if (state_.sco_buffer_info.IsAvailable() && sco_flow_control_supported) {
    // Enable SCO flow control.
    auto sync_flow_control = hci::CommandPacket::New<
        pw::bluetooth::emboss::WriteSynchronousFlowControlEnableCommandWriter>(
        hci_spec::kWriteSynchronousFlowControlEnable);
    auto flow_control_params = sync_flow_control.view_t();
    flow_control_params.synchronous_flow_control_enable().Write(
        pw::bluetooth::emboss::GenericEnableParam::ENABLE);
    init_seq_runner_->QueueCommand(
        std::move(sync_flow_control), [this](const hci::EventPacket& event) {
          if (HCI_IS_ERROR(event,
                           ERROR,
                           "gap",
                           "Write synchronous flow control enable failed, "
                           "proceeding without HCI "
                           "SCO support")) {
            return;
          }

          if (!hci_->InitializeScoDataChannel(state_.sco_buffer_info)) {
            bt_log(WARN,
                   "gap",
                   "Failed to initialize ScoDataChannel, proceeding without "
                   "HCI SCO support");
            return;
          }
          bt_log(DEBUG, "gap", "ScoDataChannel initialized successfully");
        });
  } else {
    bt_log(INFO,
           "gap",
           "HCI SCO not supported (SCO buffer available: %d, SCO flow control "
           "supported: %d)",
           state_.sco_buffer_info.IsAvailable(),
           sco_flow_control_supported);
  }

  bool supports_iso_peripheral = state_.low_energy_state.IsFeatureSupported(
      hci_spec::LESupportedFeature::kConnectedIsochronousStreamPeripheral);
  bool supports_iso_central = state_.low_energy_state.IsFeatureSupported(
      hci_spec::LESupportedFeature::kConnectedIsochronousStreamCentral);

  if (supports_iso_peripheral || supports_iso_central) {
    // Configure support for in-band ISO channels.
    PerformIsoInitialization();
  }

  hci_->AttachInspect(adapter_node_);

  // Create ChannelManager, if we haven't been provided one for testing. Doing
  // so here lets us guarantee that AclDataChannel's lifetime is a superset of
  // ChannelManager's lifetime.
  if (!l2cap_) {
    // Initialize ChannelManager to make it available for the next
    // initialization step. The AclDataChannel must be initialized before
    // creating ChannelManager.
    l2cap_ = l2cap::ChannelManager::Create(hci_->acl_data_channel(),
                                           hci_->command_channel(),
                                           /*random_channel_ids=*/true,
                                           dispatcher_,
                                           wake_lease_provider_);
    l2cap_->AttachInspect(adapter_node_,
                          l2cap::ChannelManager::kInspectNodeName);
  }

  // HCI_Set_Event_Mask
  {
    uint64_t event_mask = BuildEventMask();
    auto set_event = hci::CommandPacket::New<
        pw::bluetooth::emboss::SetEventMaskCommandWriter>(
        hci_spec::kSetEventMask);
    auto set_event_params = set_event.view_t();
    set_event_params.event_mask().Write(event_mask);
    init_seq_runner_->QueueCommand(
        std::move(set_event), [](const hci::EventPacket& event) {
          HCI_IS_ERROR(event, WARN, "gap", "set event mask failed");
        });
  }

  // HCI_LE_Set_Event_Mask
  {
    uint64_t event_mask = BuildLEEventMask();
    auto cmd_packet = hci::CommandPacket::New<
        pw::bluetooth::emboss::LESetEventMaskCommandWriter>(
        hci_spec::kLESetEventMask);
    cmd_packet.view_t().le_event_mask().BackingStorage().WriteUInt(event_mask);
    init_seq_runner_->QueueCommand(
        std::move(cmd_packet), [](const hci::EventPacket& event) {
          HCI_IS_ERROR(event, WARN, "gap", "LE set event mask failed");
        });
  }

  // If page 2 of the extended features bitfield is available, read it
  if (max_lmp_feature_page_index_.has_value() &&
      max_lmp_feature_page_index_.value() > 1) {
    InitQueueReadLMPFeatureMaskPage(2);
  }

  init_seq_runner_->RunCommands([this](hci::Result<> status) mutable {
    if (bt_is_error(
            status,
            ERROR,
            "gap",
            "failed to obtain initial controller information (step 3)")) {
      CompleteInitialization(/*success=*/false);
      return;
    }
    InitializeStep4();
  });
}

void AdapterImpl::InitializeStep4() {
  // Initialize the scan manager and low energy adapters based on current
  // feature support
  PW_DCHECK(IsInitializing());

  // We use the public controller address as the local LE identity address.
  DeviceAddress adapter_identity(DeviceAddress::Type::kLEPublic,
                                 state_.controller_address);

  // Initialize the LE local address manager.
  le_address_manager_ = std::make_unique<LowEnergyAddressManager>(
      adapter_identity,
      fit::bind_member<&AdapterImpl::IsLeRandomAddressChangeAllowed>(this),
      hci_->command_channel()->AsWeakPtr(),
      dispatcher_);

  // Initialize the HCI adapters. Note: feature support for extended
  // scanning, connections, etc are all grouped under the extended advertising
  // feature flag.
  bool extended = state().low_energy_state.IsFeatureSupported(
      hci_spec::LESupportedFeature::kLEExtendedAdvertising);
  bt_log(INFO,
         "gap",
         "controller support for extended operations: %s",
         extended ? "yes" : "no");
  hci_le_advertiser_ = CreateAdvertiser(extended);
  hci_le_connector_ = CreateConnector(extended);

  hci::AdvertisingPacketFilter::Config packet_filter_config =
      GetPacketFilterConfig();
  hci_le_scanner_ = CreateScanner(extended, packet_filter_config);

  // Initialize the LE manager objects
  le_discovery_manager_ = std::make_unique<LowEnergyDiscoveryManager>(
      hci_le_scanner_.get(), &peer_cache_, packet_filter_config, dispatcher_);
  le_discovery_manager_->AttachInspect(
      adapter_node_, kInspectLowEnergyDiscoveryManagerNodeName);
  le_discovery_manager_->set_peer_connectable_callback(
      fit::bind_member<&AdapterImpl::OnLeAutoConnectRequest>(this));

  le_connection_manager_ = std::make_unique<LowEnergyConnectionManager>(
      hci_,
      le_address_manager_.get(),
      hci_le_connector_.get(),
      &peer_cache_,
      l2cap_.get(),
      gatt_,
      le_discovery_manager_->GetWeakPtr(),
      sm::SecurityManager::CreateLE,
      state(),
      dispatcher_,
      wake_lease_provider_);
  le_connection_manager_->AttachInspect(
      adapter_node_, kInspectLowEnergyConnectionManagerNodeName);

  gatt_->AttachInspect(adapter_node_, kGattNodeName);

  le_advertising_manager_ = std::make_unique<LowEnergyAdvertisingManager>(
      hci_le_advertiser_.get(), le_address_manager_.get());

  if (state().low_energy_state.IsFeatureSupported(
          hci_spec::LESupportedFeature::kSynchronizedReceiver)) {
    periodic_advertising_sync_manager_.emplace(
        hci_, peer_cache_, le_discovery_manager_->GetWeakPtr(), dispatcher_);
  }

  low_energy_ = std::make_unique<LowEnergyImpl>(this);

  // Initialize the BR/EDR manager objects if the controller supports BR/EDR.
  if (state_.IsBREDRSupported()) {
    DeviceAddress local_bredr_address(DeviceAddress::Type::kBREDR,
                                      state_.controller_address);

    bredr_connection_manager_ = std::make_unique<BrEdrConnectionManager>(
        hci_,
        &peer_cache_,
        local_bredr_address,
        le_address_manager_.get(),
        l2cap_.get(),
        state_.features.HasBit(/*page=*/0,
                               hci_spec::LMPFeature::kInterlacedPageScan),
        state_.IsLocalSecureConnectionsSupported(),
        config_.legacy_pairing_enabled,
        state_.IsControllerRemotePublicKeyValidationSupported(),
        sm::SecurityManager::CreateBrEdr,
        dispatcher_);
    bredr_connection_manager_->AttachInspect(
        adapter_node_, kInspectBrEdrConnectionManagerNodeName);

    pw::bluetooth::emboss::InquiryMode mode =
        pw::bluetooth::emboss::InquiryMode::STANDARD;
    if (state_.features.HasBit(
            /*page=*/0, hci_spec::LMPFeature::kExtendedInquiryResponse)) {
      mode = pw::bluetooth::emboss::InquiryMode::EXTENDED;
    } else if (state_.features.HasBit(
                   /*page=*/0, hci_spec::LMPFeature::kRSSIwithInquiryResults)) {
      mode = pw::bluetooth::emboss::InquiryMode::RSSI;
    }

    bredr_discovery_manager_ = std::make_unique<BrEdrDiscoveryManager>(
        dispatcher_, hci_->command_channel()->AsWeakPtr(), mode, &peer_cache_);
    bredr_discovery_manager_->AttachInspect(
        adapter_node_, kInspectBrEdrDiscoveryManagerNodeName);

    sdp_server_ = std::make_unique<sdp::Server>(l2cap_.get());
    sdp_server_->AttachInspect(adapter_node_);

    bredr_ = std::make_unique<BrEdrImpl>(this);
  }

  // Override the current privacy setting and always use the local stable
  // identity address (i.e. not a RPA) when initiating connections. This
  // improves interoperability with certain Bluetooth peripherals that fail to
  // authenticate following a RPA rotation.
  //
  // The implication here is that the public address is revealed in LL
  // connection request PDUs. LE central privacy is still preserved during an
  // active scan, i.e. in LL scan request PDUs.
  //
  // TODO(fxbug.dev/42141593): Remove this temporary fix once we determine the
  // root cause for authentication failures.
  hci_le_connector_->UseLocalIdentityAddress();

  // Update properties before callback called so properties can be verified in
  // unit tests.
  UpdateInspectProperties();

  // Assign a default name and device class before notifying completion.
  auto self = weak_self_.GetWeakPtr();
  SetLocalName(kDefaultLocalName, [self](auto) mutable {
    // Set the default device class - a computer with audio.
    // TODO(fxbug.dev/42074312): set this from a platform configuration file
    DeviceClass dev_class(DeviceClass::MajorClass::kComputer);
    dev_class.SetServiceClasses({DeviceClass::ServiceClass::kAudio});
    self->SetDeviceClass(dev_class, [self](const auto&) {
      self->CompleteInitialization(/*success=*/true);
    });
  });
}

bool AdapterImpl::CompleteInitialization(bool success) {
  if (!init_cb_) {
    return false;
  }

  if (success) {
    init_state_ = State::kInitialized;
  } else {
    CleanUp();
  }

  init_cb_(success);
  return true;
}

void AdapterImpl::InitQueueReadLMPFeatureMaskPage(uint8_t page) {
  PW_DCHECK(init_seq_runner_);
  PW_DCHECK(init_seq_runner_->IsReady());

  if (max_lmp_feature_page_index_.has_value() &&
      page > max_lmp_feature_page_index_.value()) {
    bt_log(WARN,
           "gap",
           "Maximum value of LMP features mask page is %zu. Received page "
           "%" PRIx8,
           max_lmp_feature_page_index_.value(),
           page);
    return;
  }

  if (page == 0) {
    init_seq_runner_->QueueCommand(
        hci::CommandPacket::New<
            pw::bluetooth::emboss::ReadLocalSupportedFeaturesCommandView>(
            hci_spec::kReadLocalSupportedFeatures),
        [this, page](const hci::EventPacket& cmd_complete) {
          if (HCI_IS_ERROR(cmd_complete,
                           WARN,
                           "gap",
                           "read local supported features failed")) {
            return;
          }
          auto view = cmd_complete.view<
              pw::bluetooth::emboss::
                  ReadLocalSupportedFeaturesCommandCompleteEventView>();
          state_.features.SetPage(page, view.lmp_features().Read());
        });
    return;
  }

  if (!state_.features.HasBit(/*page=*/0u,
                              hci_spec::LMPFeature::kExtendedFeatures)) {
    bt_log(WARN, "gap", "LMP features mask does not have extended features");
    max_lmp_feature_page_index_ = 0;
    return;
  }

  if (!max_lmp_feature_page_index_.has_value() ||
      page <= max_lmp_feature_page_index_.value()) {
    // HCI_Read_Local_Extended_Features
    auto cmd_packet = hci::CommandPacket::New<
        pw::bluetooth::emboss::ReadLocalExtendedFeaturesCommandWriter>(
        hci_spec::kReadLocalExtendedFeatures);
    cmd_packet.view_t().page_number().Write(page);  // Try to read |page|

    init_seq_runner_->QueueCommand(
        std::move(cmd_packet),
        [this, page](const hci::EventPacket& cmd_complete) {
          if (HCI_IS_ERROR(cmd_complete,
                           WARN,
                           "gap",
                           "read local extended features failed")) {
            return;
          }
          auto view = cmd_complete.view<
              pw::bluetooth::emboss::
                  ReadLocalExtendedFeaturesCommandCompleteEventView>();
          state_.features.SetPage(page, view.extended_lmp_features().Read());
          max_lmp_feature_page_index_ = view.max_page_number().Read();
        });
  }
}

void AdapterImpl::UpdateInspectProperties() {
  inspect_properties_.adapter_id =
      adapter_node_.CreateString("adapter_id", identifier_.ToString());
  inspect_properties_.hci_version = adapter_node_.CreateString(
      "hci_version", hci_spec::HCIVersionToString(state_.hci_version));

  inspect_properties_.bredr_max_num_packets = adapter_node_.CreateUint(
      "bredr_max_num_packets", state_.bredr_data_buffer_info.max_num_packets());
  inspect_properties_.bredr_max_data_length = adapter_node_.CreateUint(
      "bredr_max_data_length", state_.bredr_data_buffer_info.max_data_length());

  inspect_properties_.le_max_num_packets = adapter_node_.CreateUint(
      "le_max_num_packets",
      state_.low_energy_state.acl_data_buffer_info().max_num_packets());
  inspect_properties_.le_max_data_length = adapter_node_.CreateUint(
      "le_max_data_length",
      state_.low_energy_state.acl_data_buffer_info().max_data_length());

  inspect_properties_.sco_max_num_packets = adapter_node_.CreateUint(
      "sco_max_num_packets", state_.sco_buffer_info.max_num_packets());
  inspect_properties_.sco_max_data_length = adapter_node_.CreateUint(
      "sco_max_data_length", state_.sco_buffer_info.max_data_length());

  inspect_properties_.lmp_features =
      adapter_node_.CreateString("lmp_features", state_.features.ToString());

  auto le_features = bt_lib_cpp_string::StringPrintf(
      "0x%016" PRIx64, state_.low_energy_state.supported_features());
  inspect_properties_.le_features =
      adapter_node_.CreateString("le_features", le_features);
}

void AdapterImpl::CleanUp() {
  if (init_state_ == State::kNotInitialized) {
    bt_log(DEBUG, "gap", "clean up: not initialized");
    return;
  }

  init_state_ = State::kNotInitialized;
  state_ = AdapterState();
  transport_error_cb_ = nullptr;

  // Destroy objects in reverse order of construction.
  low_energy_ = nullptr;
  bredr_ = nullptr;
  sdp_server_ = nullptr;
  bredr_discovery_manager_ = nullptr;
  le_advertising_manager_ = nullptr;
  le_connection_manager_ = nullptr;
  le_discovery_manager_ = nullptr;

  hci_le_connector_ = nullptr;
  hci_le_advertiser_ = nullptr;
  hci_le_scanner_ = nullptr;

  le_address_manager_ = nullptr;

  l2cap_ = nullptr;

  hci_.reset();
}

void AdapterImpl::OnTransportError() {
  bt_log(INFO, "gap", "HCI transport error");
  if (CompleteInitialization(/*success=*/false)) {
    return;
  }
  if (transport_error_cb_) {
    transport_error_cb_();
  }
}

void AdapterImpl::OnLeAutoConnectRequest(Peer* peer) {
  PW_DCHECK(le_connection_manager_);
  PW_DCHECK(peer);
  PW_DCHECK(peer->le());

  PeerId peer_id = peer->identifier();

  if (!peer->le()->should_auto_connect()) {
    bt_log(DEBUG,
           "gap",
           "ignoring auto-connection (peer->should_auto_connect() is false) "
           "(peer: %s)",
           bt_str(peer_id));
    return;
  }

  LowEnergyConnectionOptions options{.auto_connect = true};

  auto self = weak_self_.GetWeakPtr();
  le_connection_manager_->Connect(
      peer_id,
      [self, peer_id](auto result) {
        if (!self.is_alive()) {
          bt_log(DEBUG, "gap", "ignoring auto-connection (adapter destroyed)");
          return;
        }

        if (result.is_error()) {
          bt_log(INFO,
                 "gap",
                 "failed to auto-connect (peer: %s, error: %s)",
                 bt_str(peer_id),
                 HostErrorToString(result.error_value()).c_str());
          return;
        }

        auto conn = std::move(result).value();
        PW_CHECK(conn);
        bt_log(INFO, "gap", "peer auto-connected (peer: %s)", bt_str(peer_id));
        if (self->auto_conn_cb_) {
          self->auto_conn_cb_(std::move(conn));
        }
      },
      options);
}

bool AdapterImpl::IsLeRandomAddressChangeAllowed() {
  return hci_le_advertiser_->AllowsRandomAddressChange() &&
         hci_le_scanner_->AllowsRandomAddressChange() &&
         hci_le_connector_->AllowsRandomAddressChange();
}

std::unique_ptr<Adapter> Adapter::Create(
    pw::async::Dispatcher& pw_dispatcher,
    hci::Transport::WeakPtr hci,
    gatt::GATT::WeakPtr gatt,
    Config config,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
    std::unique_ptr<l2cap::ChannelManager> l2cap) {
  return std::make_unique<AdapterImpl>(
      pw_dispatcher, hci, gatt, config, std::move(l2cap), wake_lease_provider);
}

}  // namespace bt::gap
