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
#include <memory>

#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/le_connection_parameters.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/local_address_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::hci {

class LocalAddressDelegate;

// A LowEnergyConnector abstracts over the HCI commands and events involved in
// initiating a direct link-layer connection with a peer device. This class also
// provides a way for a delegate to be notified when a connection is initiated
// by a remote.
//
// This class vends Connection objects for LE link layer connections.
//
// Instances of this class are expected to each exist as a singleton on a
// per-transport basis as multiple instances cannot accurately reflect the state
// of the controller while allowing simultaneous operations.
class LowEnergyConnector : public LocalAddressClient {
 public:
  // The IncomingConnectionDelegate defines the interface that
  // LowEnergyConnector will use to callback on an incoming connection.
  //
  //  - |handle|: Data Connection Handle used for ACL and SCO logical link
  //  connections.
  //
  //  - |role|: The role that this device is operating in for this connection.
  //
  //  - |peer_address|: The address of the remote peer.
  //
  //  - |conn_params|: Connection related parameters.
  using IncomingConnectionDelegate =
      fit::function<void(hci_spec::ConnectionHandle handle,
                         pw::bluetooth::emboss::ConnectionRole role,
                         const DeviceAddress& peer_address,
                         const hci_spec::LEConnectionParameters& conn_params)>;

  // The constructor expects the following arguments:
  //   - |hci|: The HCI transport this should operate on.
  //
  //   - |local_addr_delegate|: The delegate used to obtain the current public
  //     or random device address to use in locally initiated requests.
  //
  //   - |dispatcher|: The dispatcher that will be used to run all
  //     asynchronous operations. This must be bound to the thread on which the
  //     LowEnergyConnector is created.
  //
  //   - |delegate|: The delegate that will be notified when a new logical link
  //     is established due to an incoming request (remote initiated).
  //
  //   - |use_extended_operations|: If true, send LE Extended Create Connection
  //     to the Controller instead of LE Create Connection.
  LowEnergyConnector(Transport::WeakPtr hci,
                     LocalAddressDelegate* local_addr_delegate,
                     pw::async::Dispatcher& dispatcher,
                     IncomingConnectionDelegate delegate,
                     bool use_extended_operations = false);

  // Deleting an instance cancels any pending connection request.
  ~LowEnergyConnector() override;

  // Creates a LE link layer connection to the remote device identified by
  // |peer_address| with initial connection parameters |initial_parameters|.
  // Returns false, if a create connection request is currently pending.
  //
  // If |use_accept_list| is true, then the controller filter accept list is
  // used to determine which advertiser to connect to. Otherwise, the controller
  // will connect to |peer_address|.
  //
  // |status_callback| is called asynchronously to notify the status of the
  // operation. A valid |link| will be provided on success.
  //
  // |timeout_ms| specifies a time period after which the request will time out.
  // When a request to create connection times out, |status_callback| will be
  // called with a null |link| and a |status| with error Host::Error::kTimedOut.
  using StatusCallback = fit::function<void(
      Result<> status, std::unique_ptr<LowEnergyConnection> link)>;
  bool CreateConnection(
      bool use_accept_list,
      const DeviceAddress& peer_address,
      uint16_t scan_interval,
      uint16_t scan_window,
      const hci_spec::LEPreferredConnectionParameters& initial_parameters,
      StatusCallback status_callback,
      pw::chrono::SystemClock::duration timeout);

  // Cancels the currently pending connection attempt.
  void Cancel() { CancelInternal(false); }

  // Returns true if a connection request is currently pending.
  bool request_pending() const { return pending_request_.has_value(); }

  // Returns the peer address of a connection request if a connection request is
  // currently pending.
  std::optional<DeviceAddress> pending_peer_address() const;

  // Returns true if a connection timeout has been posted. Returns false if it
  // was not posted or was canceled. This is intended for unit tests.
  bool timeout_posted() const { return request_timeout_task_.is_pending(); }

  // Disable central privacy and always use the local identity address as the
  // local address when initiating connections, by-passing
  // LocalAddressDelegate's current privacy setting. This policy allows better
  // interoperability with peripherals that cannot resolve the local identity
  // when a RPA is used during pairing.
  //
  // By default the address provided by the LocalAddressDelegate is used.
  //
  // TODO(fxbug.dev/42141593): Remove this temporary fix once we determine the
  // root cause for authentication failures.
  void UseLocalIdentityAddress() { use_local_identity_address_ = true; }

  // LocalAddressClient override:
  bool AllowsRandomAddressChange() const override {
    return !pending_request_ || !pending_request_->initiating;
  }

 private:
  struct PendingRequest {
    PendingRequest() = default;
    PendingRequest(const DeviceAddress& peer_address,
                   StatusCallback status_callback);

    bool initiating = false;  // True if the HCI command has been sent.
    bool canceled = false;
    bool timed_out = false;
    CommandChannel::TransactionId transaction_id = 0u;
    DeviceAddress local_address;
    DeviceAddress peer_address;
    StatusCallback status_callback;
  };

  static CommandPacket BuildExtendedCreateConnectionPacket(
      const DeviceAddress& local_address,
      const DeviceAddress& peer_address,
      const hci_spec::LEPreferredConnectionParameters& initial_params,
      bool use_accept_list,
      uint16_t scan_interval,
      uint16_t scan_window);

  static CommandPacket BuildCreateConnectionPacket(
      const DeviceAddress& local_address,
      const DeviceAddress& peer_address,
      const hci_spec::LEPreferredConnectionParameters& initial_params,
      bool use_accept_list,
      uint16_t scan_interval,
      uint16_t scan_window);

  // Event handler for either the HCI LE Enhanced Connection Complete event or
  // HCI LE Connection Complete Event
  template <typename T>
  void OnConnectionCompleteEvent(const EventPacket& event);

  Transport::WeakPtr hci() const { return hci_; }

  // Called by CreateConnection() after the local device address has been
  // obtained.
  void CreateConnectionInternal(
      const DeviceAddress& local_address,
      bool use_accept_list,
      const DeviceAddress& peer_address,
      uint16_t scan_interval,
      uint16_t scan_window,
      const hci_spec::LEPreferredConnectionParameters& initial_params,
      StatusCallback status_callback,
      pw::chrono::SystemClock::duration timeout);

  // Called by Cancel() and by OnCreateConnectionTimeout().
  void CancelInternal(bool timed_out = false);

  // Called when a LE Create Connection request has completed.
  void OnCreateConnectionComplete(Result<> result,
                                  std::unique_ptr<LowEnergyConnection> link);

  // Called when a LE Create Connection request has timed out.
  void OnCreateConnectionTimeout();

  pw::async::Dispatcher& pw_dispatcher_;

  // The HCI transport.
  Transport::WeakPtr hci_;

  // Used to obtain the local device address type to use during initiation.
  LocalAddressDelegate* local_addr_delegate_;  // weak

  // The delegate that gets notified when a new link layer connection gets
  // created.
  IncomingConnectionDelegate delegate_;

  // The currently pending request.
  std::optional<PendingRequest> pending_request_;

  // Task that runs when a request to create connection times out. We do not
  // rely on CommandChannel's timer since that request completes when we receive
  // the HCI Command Status event.
  SmartTask request_timeout_task_{pw_dispatcher_};

  // Use the local public address if true.
  // TODO(fxbug.dev/42141593): Remove this temporary fix once we determine the
  // root cause for authentication failures.
  bool use_local_identity_address_ = false;

  // send LE Extended Create Connection to the Controller instead of the legacy
  // LE Create Connection
  bool use_extended_operations_ = false;

  // Our event handle ID for the LE Enhanced Connection Complete event.
  std::unordered_set<CommandChannel::EventHandlerId> event_handler_ids_;

  // Keep this as the last member to make sure that all weak pointers are
  // invalidated before other members get destroyed.
  WeakSelf<LowEnergyConnector> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyConnector);
};

}  // namespace bt::hci
