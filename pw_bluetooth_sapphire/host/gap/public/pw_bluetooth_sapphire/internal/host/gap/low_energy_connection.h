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
#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/inspectable.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/gap/generic_access_client.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_handle.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_connection_request.h"
#include "pw_bluetooth_sapphire/internal/host/gap/low_energy_state.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/iso/iso_stream_manager.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_manager.h"
#include "pw_bluetooth_sapphire/internal/host/sm/delegate.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"

namespace bt::sm {
class SecurityManager;
}

namespace bt::gap {

namespace internal {

// LowEnergyConnector constructs LowEnergyConnection instances immediately upon
// successful completion of the link layer connection procedure (to hook up HCI
// event callbacks). However, LowEnergyConnections aren't exposed to the rest of
// the stack (including the LowEnergyConnectionManager) until fully
// interrogated, as completion of the link-layer connection process is
// insufficient to guarantee a working connection. Thus this class represents
// the state of an active *AND* (outside of LowEnergyConnector) known-functional
// connection.
//
// Instances are kept alive as long as there is at least one
// LowEnergyConnectionHandle that references them. Instances are expected to be
// destroyed immediately after a peer disconnect event is received (as indicated
// by peer_disconnect_cb).
class LowEnergyConnection final : public sm::Delegate {
 public:
  // |peer| is the peer that this connection is connected to.
  // |link| is the underlying LE HCI connection that this connection corresponds
  // to. |peer_disconnect_cb| will be called when the peer disconnects. It will
  // not be called before this method returns. |error_cb| will be called when a
  // fatal connection error occurs and the connection should be closed (e.g.
  // when L2CAP reports an error). It will not be called before this method
  // returns. |conn_mgr| is the LowEnergyConnectionManager that owns this
  // connection. |l2cap|, |gatt|, and |hci| are pointers to the interfaces of
  // the corresponding layers. Returns nullptr if connection initialization
  // fails.
  using PeerDisconnectCallback =
      fit::callback<void(pw::bluetooth::emboss::StatusCode)>;
  using ErrorCallback = fit::callback<void()>;
  static std::unique_ptr<LowEnergyConnection> Create(
      Peer::WeakPtr peer,
      std::unique_ptr<hci::LowEnergyConnection> link,
      LowEnergyConnectionOptions connection_options,
      PeerDisconnectCallback peer_disconnect_cb,
      ErrorCallback error_cb,
      WeakSelf<LowEnergyConnectionManager>::WeakPtr conn_mgr,
      l2cap::ChannelManager* l2cap,
      gatt::GATT::WeakPtr gatt,
      hci::Transport::WeakPtr hci,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
      pw::async::Dispatcher& dispatcher,
      const LowEnergyState& low_energy_state);

  // Notifies request callbacks and connection refs of the disconnection.
  ~LowEnergyConnection() override;

  // Create a reference to this connection. When the last reference is dropped,
  // this connection will be disconnected.
  std::unique_ptr<LowEnergyConnectionHandle> AddRef();

  // Decrements the ref count. Must be called when a LowEnergyConnectionHandle
  // is released/destroyed.
  void DropRef(LowEnergyConnectionHandle* ref);

  // Used to respond to protocol/service requests for increased security.
  void OnSecurityRequest(sm::SecurityLevel level, sm::ResultFunction<> cb);

  // Handles a pairing request (i.e. security upgrade) received from "higher
  // levels", likely initiated from GAP. This will only be used by pairing
  // requests that are initiated in the context of testing. May only be called
  // on an already-established connection.
  void UpgradeSecurity(sm::SecurityLevel level,
                       sm::BondableMode bondable_mode,
                       sm::ResultFunction<> cb);

  // Cancels any on-going pairing procedures and sets up SMP to use the provided
  // new I/O capabilities for future pairing procedures.
  void ResetSecurityManager(sm::IOCapability ioc);

  // Must be called when interrogation has completed. May update connection
  // parameters if all initialization procedures have completed.
  void OnInterrogationComplete();

  // Opens an L2CAP channel using the parameters |params|. Otherwise, calls |cb|
  // with a nullptr.
  void OpenL2capChannel(l2cap::Psm psm,
                        l2cap::ChannelParameters params,
                        l2cap::ChannelCallback cb);

  // Accept a future incoming request to establish an Isochronous stream on this
  // LE connection. |id| specifies the CIG/CIS pair that identify the stream.
  // |cb| will be called after the request is received to indicate success of
  // establishing a stream, and the associated parameters.
  iso::AcceptCisStatus AcceptCis(iso::CigCisIdentifier id,
                                 iso::CisEstablishedCallback cb);

  // Attach connection as child node of |parent| with specified |name|.
  void AttachInspect(inspect::Node& parent, std::string name);

  void set_security_mode(LESecurityMode mode);

  // Sets a callback that will be called when the peer disconnects.
  void set_peer_disconnect_callback(PeerDisconnectCallback cb) {
    PW_CHECK(cb);
    peer_disconnect_callback_ = std::move(cb);
  }

  // |peer_conn_token| is a token generated by the connected Peer, and is used
  // to synchronize connection state.
  void set_peer_conn_token(Peer::ConnectionToken peer_conn_token) {
    PW_CHECK(interrogation_completed_);
    PW_CHECK(!peer_conn_token_);
    peer_conn_token_ = std::move(peer_conn_token);
  }

  // Sets a callback that will be called when a fatal connection error occurs.
  void set_error_callback(ErrorCallback cb) {
    PW_CHECK(cb);
    error_callback_ = std::move(cb);
  }

  size_t ref_count() const { return refs_->size(); }

  PeerId peer_id() const { return peer_->identifier(); }
  hci_spec::ConnectionHandle handle() const { return link_->handle(); }
  hci::LowEnergyConnection* link() const { return link_.get(); }
  sm::BondableMode bondable_mode() const;

  pw::chrono::SystemClock::time_point create_time() const {
    return create_time_;
  }

  sm::SecurityProperties security() const;

  pw::bluetooth::emboss::ConnectionRole role() const { return link()->role(); }

  using WeakPtr = WeakSelf<LowEnergyConnection>::WeakPtr;
  LowEnergyConnection::WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

 private:
  LowEnergyConnection(
      Peer::WeakPtr peer,
      std::unique_ptr<hci::LowEnergyConnection> link,
      LowEnergyConnectionOptions connection_options,
      PeerDisconnectCallback peer_disconnect_cb,
      ErrorCallback error_cb,
      WeakSelf<LowEnergyConnectionManager>::WeakPtr conn_mgr,
      l2cap::ChannelManager* l2cap,
      gatt::GATT::WeakPtr gatt,
      hci::Transport::WeakPtr hci,
      pw::async::Dispatcher& dispatcher,
      const LowEnergyState& low_energy_state,
      pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider);

  // Registers this connection with L2CAP and initializes the fixed channel
  // protocols. Return true on success, false on failure.
  [[nodiscard]] bool InitializeFixedChannels();

  // Register handlers for HCI events that correspond to this connection.
  void RegisterEventHandlers();

  // Start kLEConnectionPauseCentral/Peripheral timeout that will update
  // connection parameters. Should be called as soon as this GAP connection is
  // established.
  void StartConnectionPauseTimeout();

  // Start kLEConnectionPausePeripheral timeout that will send a connection
  // parameter update request. Should be called as soon as connection is
  // established.
  void StartConnectionPausePeripheralTimeout();

  // Start kLEConnectionPauseCentral timeout that will update connection
  // parameters. Should be called as soon as connection is established.
  void StartConnectionPauseCentralTimeout();

  // Initializes SecurityManager and GATT.
  // Called by the L2CAP layer once the link has been registered and the fixed
  // channels have been opened. Returns false if GATT initialization fails.
  [[nodiscard]] bool OnL2capFixedChannelsOpened(
      l2cap::Channel::WeakPtr att,
      l2cap::Channel::WeakPtr smp,
      LowEnergyConnectionOptions connection_options);

  // Called when the preferred connection parameters have been received for a LE
  // peripheral. This can happen in the form of:
  //
  //   1. <<Peripheral Connection Interval Range>> advertising data field
  //   2. "Peripheral Preferred Connection Parameters" GATT characteristic
  //      (under "GAP" service)
  //   3. HCI LE Remote Connection Parameter Request Event
  //   4. L2CAP Connection Parameter Update request
  //
  // TODO(fxbug.dev/42147867): Support #1 above.
  // TODO(fxbug.dev/42147868): Support #3 above.
  //
  // This method caches |params| for later connection attempts and sends the
  // parameters to the controller if the initializing procedures are complete
  // (since we use more agressing initial parameters for pairing and service
  // discovery, as recommended by the specification in v5.0, Vol 3, Part C,
  // Section 9.3.12.1).
  //
  // |peer_id| uniquely identifies the peer. |handle| represents
  // the logical link that |params| should be applied to.
  void OnNewLEConnectionParams(
      const hci_spec::LEPreferredConnectionParameters& params);

  // As an LE peripheral, request that the connection parameters |params| be
  // used on the given connection |conn| with peer |peer_id|. This may send an
  // HCI LE Connection Update command or an L2CAP Connection Parameter Update
  // Request depending on what the local and remote controllers support.
  //
  // Interrogation must have completed before this may be called.
  void RequestConnectionParameterUpdate(
      const hci_spec::LEPreferredConnectionParameters& params);

  // Handler for connection parameter update command sent when an update is
  // requested by RequestConnectionParameterUpdate.
  //
  // If the HCI LE Connection Update command fails with status
  // kUnsupportedRemoteFeature, the update will be retried with an L2CAP
  // Connection Parameter Update Request.
  void HandleRequestConnectionParameterUpdateCommandStatus(
      hci_spec::LEPreferredConnectionParameters params, hci::Result<> status);

  // As an LE peripheral, send an L2CAP Connection Parameter Update Request
  // requesting |params| on the LE signaling channel of the given logical link
  // |handle|.
  //
  // NOTE: This should only be used if the LE peripheral and/or LE central do
  // not support the Connection Parameters Request Link Layer Control Procedure
  // (Core Spec v5.2  Vol 3, Part A, Sec 4.20). If they do,
  // UpdateConnectionParams(...) should be used instead.
  void L2capRequestConnectionParameterUpdate(
      const hci_spec::LEPreferredConnectionParameters& params);

  // Requests that the controller use the given connection |params| by sending
  // an HCI LE Connection Update command. This may be issued on both the LE
  // peripheral and the LE central.
  //
  // The link layer may modify the preferred parameters |params| before
  // initiating the Connection Parameters Request Link Layer Control Procedure
  // (Core Spec v5.2, Vol 6, Part B, Sec 5.1.7).
  //
  // If non-null, |status_cb| will be called when the HCI Command Status event
  // is received.
  //
  // The HCI LE Connection Update Complete event will be generated after the
  // parameters have been applied or if the update fails, and will indicate the
  // (possibly modified) parameter values.
  //
  // NOTE: If the local host is an LE peripheral, then the local controller and
  // the remote LE central must have indicated support for this procedure in the
  // LE feature mask. Otherwise, L2capRequestConnectionParameterUpdate(...)
  // should be used instead.
  using StatusCallback = hci::ResultCallback<>;
  void UpdateConnectionParams(
      const hci_spec::LEPreferredConnectionParameters& params,
      StatusCallback status_cb = nullptr);

  // This event may be generated without host interaction by the Link Layer, or
  // as the result of a Connection Update Command sent by either device, which
  // is why it is not simply handled by the command handler. (See Core Spec
  // v5.2, Vol 6, Part B, Sec 5.1.7.1).
  void OnLEConnectionUpdateComplete(const hci::EventPacket& event);

  // Updates or requests an update of the connection parameters, for central and
  // peripheral roles respectively, if interrogation has completed.
  // TODO(fxbug.dev/42159733): Wait to update connection parameters until all
  // initialization procedures have completed.
  void MaybeUpdateConnectionParameters();

  // Registers the peer with GATT and initiates service discovery. If
  // |service_uuid| is specified, only discover the indicated service and the
  // GAP service. Returns true on success, false on failure.
  bool InitializeGatt(l2cap::Channel::WeakPtr att,
                      std::optional<UUID> service_uuid);

  // Called when service discovery completes. |services| will only include
  // services with the GAP UUID (there should only be one, but this is not
  // guaranteed).
  void OnGattServicesResult(att::Result<> status, gatt::ServiceList services);

  // Notifies all connection refs of disconnection.
  void CloseRefs();

  // sm::Delegate overrides:
  void OnPairingComplete(sm::Result<> status) override;
  void OnAuthenticationFailure(hci::Result<> status) override;
  void OnNewSecurityProperties(const sm::SecurityProperties& sec) override;
  std::optional<sm::IdentityInfo> OnIdentityInformationRequest() override;
  void ConfirmPairing(ConfirmCallback confirm) override;
  void DisplayPasskey(uint32_t passkey,
                      sm::Delegate::DisplayMethod method,
                      ConfirmCallback confirm) override;
  void RequestPasskey(PasskeyResponseCallback respond) override;

  pw::async::Dispatcher& dispatcher_;

  // Notifies Peer of connection destruction. This should be ordered first so
  // that it is destroyed last.
  std::optional<Peer::ConnectionToken> peer_conn_token_;

  Peer::WeakPtr peer_;
  std::unique_ptr<hci::LowEnergyConnection> link_;
  LowEnergyConnectionOptions connection_options_;
  WeakSelf<LowEnergyConnectionManager>::WeakPtr conn_mgr_;

  // Manages all Isochronous streams for this connection. If this connection is
  // operating as a Central, |iso_mgr_| is used to establish an outgoing
  // connection to a peer. When operating as a Peripheral, |iso_mgr_| is used to
  // allow incoming requests for specified CIG/CIS combinations.
  std::optional<iso::IsoStreamManager> iso_mgr_;

  struct InspectProperties {
    inspect::StringProperty peer_id;
    inspect::StringProperty peer_address;
    inspect::IntProperty connected_time;
  };
  InspectProperties inspect_properties_;
  inspect::Node inspect_node_;

  // Used to update the L2CAP layer to reflect the correct link security level.
  l2cap::ChannelManager* l2cap_;

  // Reference to the GATT profile layer is used to initiate service discovery
  // and register the link.
  gatt::GATT::WeakPtr gatt_;

  // The ATT Bearer is owned by LowEnergyConnection but weak pointers are passed
  // to the GATT layer. As such, this connection must be unregistered from the
  // GATT layer before the Bearer is destroyed. Created during initialization,
  // but if initialization fails this may be nullptr.
  std::unique_ptr<att::Bearer> att_bearer_;

  // SMP pairing manager.
  std::unique_ptr<sm::SecurityManager> sm_;

  hci::CommandChannel::WeakPtr cmd_;

  hci::Transport::WeakPtr hci_;

  // Called when the peer disconnects.
  PeerDisconnectCallback peer_disconnect_callback_;

  // Called when a fatal connection error occurs and the connection should be
  // closed (e.g. when L2CAP reports an error).
  ErrorCallback error_callback_;

  // Event handler ID for the HCI LE Connection Update Complete event.
  hci::CommandChannel::EventHandlerId conn_update_cmpl_handler_id_;

  // Called with the status of the next HCI LE Connection Update Complete event.
  // The HCI LE Connection Update command does not have its own complete event
  // handler because the HCI LE Connection Complete event can be generated for
  // other reasons.
  fit::callback<void(pw::bluetooth::emboss::StatusCode)>
      le_conn_update_complete_command_callback_;

  // Called after kLEConnectionPausePeripheral.
  std::optional<SmartTask> conn_pause_peripheral_timeout_;

  // Called after kLEConnectionPauseCentral.
  std::optional<SmartTask> conn_pause_central_timeout_;

  // Set to true when a request to update the connection parameters has been
  // sent.
  bool connection_parameters_update_requested_ = false;

  bool interrogation_completed_ = false;

  // LowEnergyConnectionManager is responsible for making sure that these
  // pointers are always valid.
  using ConnectionHandleSet = std::unordered_set<LowEnergyConnectionHandle*>;
  IntInspectable<ConnectionHandleSet> refs_;

  // Null until service discovery completes.
  std::optional<GenericAccessClient> gap_service_client_;

  WeakSelf<LowEnergyConnection> weak_self_;
  WeakSelf<sm::Delegate> weak_delegate_;

  pw::chrono::SystemClock::time_point create_time_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyConnection);
};

}  // namespace internal
}  // namespace bt::gap
