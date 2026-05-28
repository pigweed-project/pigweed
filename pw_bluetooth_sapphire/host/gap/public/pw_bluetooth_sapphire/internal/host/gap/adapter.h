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

#include <lib/fit/function.h>
#include <pw_status/status.h>

#include <memory>
#include <string>

#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/gap/adapter_state.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bonding_data.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/bredr_discovery_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_advertising_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_discovery_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"
#include "pw_bluetooth_sapphire/internal/host/gap/periodic_advertising_sync_manager.h"
#include "pw_bluetooth_sapphire/internal/host/gap/types.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_common.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_group.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/server.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/service_discoverer.h"

namespace bt {

namespace hci {
class LowEnergyAdvertiser;
class LowEnergyConnector;
class LowEnergyScanner;
class SequentialCommandRunner;
class Transport;
}  // namespace hci

namespace gap {

class BrEdrConnectionManager;
class BrEdrDiscoveryManager;
class PairingDelegate;
class LowEnergyAddressManager;

// TODO(fxbug.dev/42082764): Consider removing this identifier from the bt-host
// layer.
class AdapterId : public Identifier<uint64_t> {
 public:
  constexpr explicit AdapterId(uint64_t value) : Identifier<uint64_t>(value) {}
  AdapterId() = default;
};

// Represents the host-subsystem state for a Bluetooth controller.
//
// This class is not guaranteed to be thread-safe and it is intended to be
// created, deleted, and accessed on the same event loop. No internal locking is
// provided.
//
// NOTE: We currently only support primary controllers. AMP controllers are not
// supported.
class Adapter {
 public:
  struct Config {
    // When True, BR/EDR pairing may attempt to use legacy pairing if the peer
    // does not support SSP.
    bool legacy_pairing_enabled = false;
    // When non-zero force the AndroidVendorCapabilites to be parsed as if the
    // version bytes were set to these.
    // TODO(b/450278813): remove this once we have a long term solution
    uint16_t override_vendor_capabilites_version = 0;

    // LE Advertising intervals
    uint16_t le_slow_adv_interval_min = kLEAdvertisingSlowIntervalMin;
    uint16_t le_slow_adv_interval_max = kLEAdvertisingSlowIntervalMax;
    uint16_t le_fast_adv_interval_min = kLEAdvertisingFastIntervalMin2;
    uint16_t le_fast_adv_interval_max = kLEAdvertisingFastIntervalMax2;
    uint16_t le_very_fast_adv_interval_min = kLEAdvertisingFastIntervalMin1;
    uint16_t le_very_fast_adv_interval_max = kLEAdvertisingFastIntervalMax1;

    int8_t le_slow_adv_max_tx_power =
        hci_spec::kLEExtendedAdvertisingTxPowerNoPreference;
    int8_t le_fast_adv_max_tx_power =
        hci_spec::kLEExtendedAdvertisingTxPowerNoPreference;
    int8_t le_very_fast_adv_max_tx_power =
        hci_spec::kLEExtendedAdvertisingTxPowerNoPreference;

    // LE Scanning parameters
    uint16_t le_active_scan_interval = kLEScanFastInterval;
    uint16_t le_active_scan_window = kLEScanFastWindow;
  };

  static constexpr const char* kMetricsInspectNodeName = "metrics";

  // Optionally, a FakeL2cap  may be passed for testing purposes as |l2cap|. If
  // nullptr is passed, then the Adapter will create and initialize its own
  // L2cap.
  // |wake_lease_provider| will be used to acquire wake leases and must outlive
  // the returned Adapter.
  static std::unique_ptr<Adapter> Create(
      pw::async::Dispatcher& pw_dispatcher,
      hci::Transport::WeakPtr hci,
      gatt::GATT::WeakPtr gatt,
      Config config,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
      std::unique_ptr<l2cap::ChannelManager> l2cap = nullptr);
  virtual ~Adapter() = default;

  // Returns a uniquely identifier for this adapter on the current system.
  virtual AdapterId identifier() const = 0;

  // Initializes the host-subsystem state for the HCI device this was created
  // for. This performs the initial HCI transport set up. Returns false if an
  // immediate error occurs. Otherwise this returns true and asynchronously
  // notifies the caller on the initialization status via |callback|.
  //
  // After successful initialization, |transport_closed_callback| will be
  // invoked when the underlying HCI transport closed for any reason (e.g. the
  // device disappeared or the transport channels were closed for an unknown
  // reason). The implementation is responsible for cleaning up this adapter by
  // calling ShutDown().
  using InitializeCallback = fit::callback<void(bool success)>;
  virtual bool Initialize(InitializeCallback callback,
                          fit::closure transport_error_callback) = 0;

  // Shuts down this Adapter. Invokes |callback| when shut down has completed.
  // TODO(armansito): This needs to do several things to potentially preserve
  // the state of various sub-protocols. For now we keep the interface pretty
  // simple.
  virtual void ShutDown() = 0;

  // Returns true if the Initialize() sequence has started but not completed yet
  // (i.e. the InitializeCallback that was passed to Initialize() has not yet
  // been called).
  virtual bool IsInitializing() const = 0;

  // Returns true if this Adapter has been fully initialized.
  virtual bool IsInitialized() const = 0;

  // Returns the global adapter setting parameters.
  virtual const AdapterState& state() const = 0;

  // Interface to the LE features of the adapter.
  class LowEnergy {
   public:
    using SyncOptions = PeriodicAdvertisingSyncManager::SyncOptions;
    using PeriodicAdvertisingSyncDelegate =
        PeriodicAdvertisingSyncManager::Delegate;

    virtual ~LowEnergy() = default;

    // Allows a caller to claim shared ownership over a connection to the
    // requested remote LE peer identified by |peer_id|.
    //
    //   * If the requested peer is already connected, |callback| is called with
    //   a
    //     LowEnergyConnectionHandle.
    //
    //   * If the requested peer is NOT connected, then this method initiates a
    //     connection to the requested peer. A LowEnergyConnectionHandle is
    //     asynchronously returned to the caller once the connection has been
    //     set up.
    //
    //     The status of the procedure is reported in |callback| in the case of
    //     an error.
    using ConnectionResult = gap::LowEnergyConnectionManager::ConnectionResult;
    using ConnectionResultCallback =
        gap::LowEnergyConnectionManager::ConnectionResultCallback;
    virtual void Connect(PeerId peer_id,
                         ConnectionResultCallback callback,
                         LowEnergyConnectionOptions connection_options) = 0;

    // Disconnects any existing LE connection to |peer_id|, invalidating all
    // active LowEnergyConnectionHandles. Returns false if the peer can not be
    // disconnected.
    virtual bool Disconnect(PeerId peer_id) = 0;

    // Opens a new L2CAP channel to service |psm| on |peer_id| using the
    // preferred parameters |params|.
    //
    // |cb| will be called with the channel created to the peer, or nullptr if
    // the channel creation resulted in an error.
    virtual void OpenL2capChannel(PeerId peer_id,
                                  l2cap::Psm psm,
                                  l2cap::ChannelParameters params,
                                  sm::SecurityLevel security_level,
                                  l2cap::ChannelCallback cb) = 0;

    // Initiates the pairing process. Expected to only be called during
    // higher-level testing.
    //   |peer_id|: the peer to pair to - if the peer is not connected, |cb| is
    //   called with an error. |pairing_level|: determines the security level of
    //   the pairing. **Note**: If the
    //                    security level of the link is already >= |pairing
    //                    level|, no pairing takes place.
    //   |bondable_mode|: sets the bonding mode of this connection. A device in
    //   bondable mode forms
    //                    a bond to the peer upon pairing, assuming the peer is
    //                    also in bondable mode.
    //   |cb|: callback called upon completion of this function, whether pairing
    //   takes place or not.
    virtual void Pair(PeerId peer_id,
                      sm::SecurityLevel pairing_level,
                      sm::BondableMode bondable_mode,
                      sm::ResultFunction<> cb) = 0;

    // Sets the LE security mode of the local device (see v5.2 Vol. 3 Part C
    // Section 10.2). If set to SecureConnectionsOnly, any currently encrypted
    // links not meeting the requirements of Security Mode 1 Level 4 will be
    // disconnected.
    virtual void SetLESecurityMode(LESecurityMode mode) = 0;

    // Returns the current LE security mode.
    virtual LESecurityMode security_mode() const = 0;

    // Asynchronously attempts to start advertising a set of |data| with
    // additional scan response data |scan_rsp|.
    //
    // If |connectable| is provided, the advertisement will be connectable.
    // The |connectable.connection_cb| will be called with the returned
    // advertisement ID and a connection result when a peer attempts to connect
    // to the advertisement, at which point the advertisement will have been
    // stopped. |connectable.bondable_mode| indicates the bondable mode to
    // initialize connections with.
    //
    // Returns false if the parameters represent an invalid advertisement:
    //  * if |anonymous| is true but |callback| is set
    //
    // |address_type| is used to determine whether to perform LE advertising
    // using a public or random address, depending on whether privacy has been
    // enabled or not and the value of |address_type| if given.
    //
    // |status_callback| may be called synchronously within this function.
    // |status_callback| provides one of:
    //  - an |advertisement_id|, which can be used to stop advertising
    //    or disambiguate calls to |callback|, and a success |status|.
    //  - kInvalidAdvertisementId and an error indication in |status|:
    //    * HostError::kInvalidParameters if the advertising parameters
    //      are invalid (e.g. |data| is too large).
    //    * HostError::kNotSupported if another set cannot be advertised
    //      or if the requested parameters are not supported by the hardware.
    //    * HostError::kProtocolError with a HCI error reported from
    //      the controller, otherwise.
    using ConnectionCallback =
        fit::function<void(AdvertisementId, ConnectionResult)>;
    using AdvertisingStatusCallback =
        LowEnergyAdvertisingManager::AdvertisingStatusCallback;
    struct ConnectableAdvertisingParameters {
      ConnectionCallback connection_cb;
      sm::BondableMode bondable_mode;
    };
    virtual void StartAdvertising(
        AdvertisingData data,
        AdvertisingData scan_rsp,
        AdvertisingInterval interval,
        bool extended_pdu,
        bool anonymous,
        bool include_tx_power_level,
        std::optional<ConnectableAdvertisingParameters> connectable,
        std::optional<DeviceAddress::Type> address_type,
        AdvertisingStatusCallback status_callback) = 0;

    // Starts a new discovery session and reports the result via |callback|. If
    // a session has been successfully started the caller will receive a new
    // LowEnergyDiscoverySession instance via |callback| which it uniquely owns.
    // |active| indicates whether active or passive discovery should occur.
    // On failure a nullptr will be returned via |callback|.
    using SessionCallback = LowEnergyDiscoveryManager::SessionCallback;
    virtual void StartDiscovery(bool active,
                                std::vector<hci::DiscoveryFilter> filters,
                                SessionCallback callback) = 0;

    // Synchronizes to the periodic advertising train identified by |peer| and
    // |advertising_sid| with the configuration specified by |options|.
    // Advertising events are reported via |delegate|.
    // Synchronous errors are returned immediately.
    // On success, returns a handle that stops synchronization when destroyed.
    virtual hci::Result<PeriodicAdvertisingSyncHandle>
    SyncToPeriodicAdvertisement(PeerId peer,
                                uint8_t advertising_sid,
                                SyncOptions options,
                                PeriodicAdvertisingSyncDelegate& delegate) = 0;

    // Enable or disable the privacy feature. When enabled, the controller
    // will be configured to use a new random address if it is currently
    // allowed to do so.
    virtual void EnablePrivacy(bool enabled) = 0;
    // Returns true if the privacy feature is currently enabled.
    virtual bool PrivacyEnabled() const = 0;
    // Returns the current LE address.
    virtual const DeviceAddress CurrentAddress() const = 0;

    // Create a Connected Isochronous Group (CIG) with the supplied parameters.
    // Once the CIG is created and configured with the controller, |callback|
    // will be invoked with the result.
    //
    // |on_closed_callback| is invoked when the CIG is shut down or closed.
    //
    // |expected_peers| is the list of peer IDs that are expected to connect to
    // this CIG. If provided, the local host will use the sleep clock accuracy
    // (SCA) values of these cached peers to compute the worst-case SCA for the
    // CIG parameters.
    void CreateCig(iso::CigParams cig_params,
                   std::vector<iso::CigCisParams> cis_params,
                   iso::IsoGroupManager::CreateCigCompleteCallback callback,
                   iso::IsoGroup::OnClosedCallback on_closed_callback,
                   std::vector<PeerId> expected_peers = {}) {
      DoCreateCig(std::move(cig_params),
                  std::move(cis_params),
                  std::move(callback),
                  std::move(on_closed_callback),
                  std::move(expected_peers));
    }

    virtual std::unique_ptr<LowEnergyConnectionHandle> AddConnectionRef(
        PeerId peer_id) = 0;

    // Register a callback to be notified any time the LE address changes.
    virtual void register_address_changed_callback(fit::closure callback) = 0;

    // Assigns the IRK to generate a RPA for the next address refresh when
    // privacy is enabled.
    virtual void set_irk(const std::optional<UInt128>& irk) = 0;

    // Returns the currently assigned Identity Resolving Key, if any.
    virtual std::optional<UInt128> irk() const = 0;

    // Sets the timeout interval to be used on future connect requests. The
    // default value is kLECreateConnectionTimeout.
    virtual void set_request_timeout_for_testing(
        pw::chrono::SystemClock::duration value) = 0;

    // Sets a new scan period to any future and ongoing discovery procedures.
    virtual void set_scan_period_for_testing(
        pw::chrono::SystemClock::duration period) = 0;

   private:
    virtual void DoCreateCig(
        iso::CigParams cig_params,
        std::vector<iso::CigCisParams> cis_params,
        iso::IsoGroupManager::CreateCigCompleteCallback callback,
        iso::IsoGroup::OnClosedCallback on_closed_callback,
        std::vector<PeerId> expected_peers) = 0;
  };

  virtual LowEnergy* le() const = 0;

  // Interface to the classic features of the adapter.
  class BrEdr {
   public:
    virtual ~BrEdr() = default;

    // Initiates an outgoing Create Connection Request to attempt to connect to
    // the peer identified by |peer_id|. Returns false if the connection
    // request was invalid, otherwise returns true and |callback| will be called
    // with the result of the procedure, whether successful or not
    using ConnectResultCallback = BrEdrConnectionManager::ConnectResultCallback;
    [[nodiscard]] virtual bool Connect(PeerId peer_id,
                                       ConnectResultCallback callback) = 0;

    // Disconnects any existing BR/EDR connection to |peer_id|. Returns true if
    // the peer is disconnected, false if the peer can not be disconnected.
    virtual bool Disconnect(PeerId peer_id, DisconnectReason reason) = 0;

    // Opens a new L2CAP channel to service |psm| on |peer_id| using the
    // preferred parameters |params|. If the current connection doesn't meet
    // |security_requirements|, attempt to upgrade the link key and report an
    // error via |cb| if the upgrade fails.
    //
    // |cb| will be called with the channel created to the peer, or nullptr if
    // the channel creation resulted in an error.
    virtual void OpenL2capChannel(
        PeerId peer_id,
        l2cap::Psm psm,
        BrEdrSecurityRequirements security_requirements,
        l2cap::ChannelParameters params,
        l2cap::ChannelCallback cb) = 0;

    // Retrieves the peer id that is connected to the connection |handle|.
    // Returns kInvalidPeerId if no such peer exists.
    virtual PeerId GetPeerId(hci_spec::ConnectionHandle handle) const = 0;

    // Add a service search to be performed on new connected remote peers.
    // This search will happen on every peer connection.
    // |callback| will be called with the |attributes| that exist in the service
    // entry on the peer's SDP server. If |attributes| is empty, all attributes
    // on the server will be returned. Returns a SearchId which can be used to
    // remove the search later. Identical searches will perform the same search
    // for each search added. Results of added service searches will be added to
    // each Peer's BrEdrData.
    using SearchCallback = sdp::ServiceDiscoverer::ResultCallback;
    using SearchId = sdp::ServiceDiscoverer::SearchId;
    virtual SearchId AddServiceSearch(
        const UUID& uuid,
        std::unordered_set<sdp::AttributeId> attributes,
        SearchCallback callback) = 0;

    // Remove a search previously added with AddServiceSearch()
    // Returns true if a search was removed.
    virtual bool RemoveServiceSearch(SearchId id) = 0;

    // Initiate pairing to the peer with |peer_id| using the bondable
    // preference. Pairing will only be initiated if the current link key does
    // not meet the |security| requirements. |callback| will be called with the
    // result of the procedure, successful or not.
    virtual void Pair(PeerId peer_id,
                      BrEdrSecurityRequirements security,
                      hci::ResultFunction<> callback) = 0;

    // Sets the BR/EDR security mode of the local device (Core Spec v5.4, Vol 3,
    // Part C, 5.2.2). If set to SecureConnectionsOnly, any currently encrypted
    // links not meeting the requirements of Security Mode 4 Level 4 will be
    // disconnected.
    virtual void SetBrEdrSecurityMode(BrEdrSecurityMode mode) = 0;

    // Returns the current BR/EDR security mode.
    virtual BrEdrSecurityMode security_mode() const = 0;

    // Set whether this host is connectable.
    virtual void SetConnectable(bool connectable,
                                hci::ResultFunction<> status_cb) = 0;

    // Starts discovery and reports the status via |callback|. If discovery has
    // been successfully started, the callback will receive a session object
    // that it owns. If no sessions are owned, peer discovery is stopped.
    using DiscoveryCallback = BrEdrDiscoveryManager::DiscoveryCallback;
    virtual void RequestDiscovery(DiscoveryCallback callback) = 0;

    // Requests this device be discoverable. We are discoverable as long as
    // anyone holds a discoverable session.
    using DiscoverableCallback = BrEdrDiscoveryManager::DiscoverableCallback;
    virtual void RequestDiscoverable(DiscoverableCallback callback) = 0;

    // Given incomplete ServiceRecords, register services that will be made
    // available over SDP. Takes ownership of |records|. Channels created for
    // this service will be configured using the preferred parameters in
    // |chan_params|.
    //
    // A non-zero RegistrationHandle will be returned if the service was
    // successfully registered.
    //
    // If any record in |records| fails registration checks, none of the
    // services will be registered.
    //
    // |conn_cb| will be called for any connections made to any of the services
    // in |records| with a connected channel and the descriptor list for the
    // endpoint which was connected.
    using ServiceConnectCallback = sdp::Server::ConnectCallback;
    using RegistrationHandle = sdp::Server::RegistrationHandle;
    virtual RegistrationHandle RegisterService(
        std::vector<sdp::ServiceRecord> records,
        l2cap::ChannelParameters chan_params,
        ServiceConnectCallback conn_cb) = 0;

    // Unregister services previously registered with RegisterService.
    // Idempotent. Returns |true| if any records were removed.
    virtual bool UnregisterService(RegistrationHandle handle) = 0;

    // Return the set of registered services that were previously registered
    // with RegisterService, identified by |handle|.
    virtual std::vector<sdp::ServiceRecord> GetRegisteredServices(
        RegistrationHandle handle) const = 0;

    // Initiate an outbound connection. A request will be queued if a
    // connection is already in progress. On error, |callback| will be called
    // with an error result. The error will be |kCanceled| if a connection was
    // never attempted, or |kFailed| if establishing a connection failed.
    // Returns a handle that will cancel the request when dropped (if connection
    // establishment has not started). If a BR/EDR connection with the peer does
    // not exist, returns nullopt.
    using ScoRequestHandle = BrEdrConnection::ScoRequestHandle;
    virtual std::optional<ScoRequestHandle> OpenScoConnection(
        PeerId peer_id,
        const bt::StaticPacket<
            pw::bluetooth::emboss::SynchronousConnectionParametersWriter>&
            parameters,
        sco::ScoConnectionManager::OpenConnectionCallback callback) = 0;

    // Accept inbound connection requests using the parameters given in order.
    // The parameters will be tried in order until either a connection is
    // successful, all parameters have been rejected, or the procedure is
    // canceled. On success, |callback| will be called with the connection
    // object and the index of the parameters used to establish the connection.
    // On error, |callback| will be called with an error result. If another
    // Open/Accept request is made before a connection request is received, this
    // request will be canceled (with error |kCanceled|). Returns a handle that
    // will cancel the request when destroyed (if connection establishment has
    // not started). If a BR/EDR connection with the peer does not exist,
    // returns nullopt.
    virtual std::optional<ScoRequestHandle> AcceptScoConnection(
        PeerId peer_id,
        std::vector<bt::StaticPacket<
            pw::bluetooth::emboss::SynchronousConnectionParametersWriter>>
            parameters,
        sco::ScoConnectionManager::AcceptConnectionCallback callback) = 0;
  };

  //  Returns nullptr if the controller does not support classic.
  virtual BrEdr* bredr() const = 0;

  // Returns this Adapter's peer cache.
  virtual PeerCache* peer_cache() = 0;

  // Add a previously bonded device to the peer cache and set it up for
  // auto-connect procedures.
  virtual bool AddBondedPeer(BondingData bonding_data) = 0;

  // Assigns a pairing delegate to this adapter. This PairingDelegate and its
  // I/O capabilities will be used for all future pairing procedures. Setting a
  // new PairingDelegate cancels all ongoing pairing procedures.
  virtual void SetPairingDelegate(PairingDelegate::WeakPtr delegate) = 0;

  // Returns true if this adapter is currently in discoverable mode on the LE or
  // BR/EDR transports.
  virtual bool IsDiscoverable() const = 0;

  // Returns true if any discovery process (LE or BR/EDR) is running on this
  // adapter.
  virtual bool IsDiscovering() const = 0;

  // Sets the Local Name of this adapter, for both BR/EDR discoverability and
  // public LE services.
  virtual void SetLocalName(std::string name,
                            hci::ResultFunction<> callback) = 0;

  virtual std::string local_name() const = 0;

  // Sets the Device Class of this adapter.
  virtual void SetDeviceClass(DeviceClass dev_class,
                              hci::ResultFunction<> callback) = 0;

  // If the operation is successful, specifies the minimum and maximum local
  // delay (in microseconds) supported by the controller for the codec
  // specified.
  // Returns PW_STATUS_UNIMPLEMENTED if the operation is not supported on this
  // controller. Returns PW_STATUS_UNKNOWN if the operation fails to complete
  // successfully.
  using GetSupportedDelayRangeCallback = fit::function<void(
      pw::Status status, uint32_t min_delay_us, uint32_t max_delay_us)>;
  virtual void GetSupportedDelayRange(
      const bt::StaticPacket<pw::bluetooth::emboss::CodecIdWriter>& codec_id,
      pw::bluetooth::emboss::LogicalTransportType logical_transport_type,
      pw::bluetooth::emboss::DataPathDirection direction,
      const std::optional<std::vector<uint8_t>>& codec_configuration,
      GetSupportedDelayRangeCallback cb) = 0;

  // Assign a callback to be notified when a connection is automatically
  // established to a bonded LE peer in the directed connectable mode (Vol 3,
  // Part C, 9.3.3).
  using AutoConnectCallback =
      fit::function<void(std::unique_ptr<bt::gap::LowEnergyConnectionHandle>)>;
  virtual void set_auto_connect_callback(AutoConnectCallback callback) = 0;

  // Attach Adapter's inspect node as a child node under |parent| with the given
  // |name|.
  virtual void AttachInspect(inspect::Node& parent, std::string name) = 0;

  // Returns a weak pointer to this adapter.
  using WeakPtr = WeakSelf<Adapter>::WeakPtr;
  virtual Adapter::WeakPtr AsWeakPtr() = 0;
};

}  // namespace gap
}  // namespace bt
