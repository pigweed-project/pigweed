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
#include <lib/fit/defer.h>
#include <pw_async/heap_dispatcher.h>

#include <memory>
#include <unordered_set>

#include "pw_bluetooth_sapphire/internal/host/common/inspectable.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/hci/discovery_filter.h"
#include "pw_bluetooth_sapphire/internal/host/hci/low_energy_scanner.h"

namespace bt {

namespace hci {
class LowEnergyScanner;
class Transport;
}  // namespace hci

namespace gap {

class Peer;
class PeerCache;

// LowEnergyDiscoveryManager implements GAP LE central/observer role
// discovery procedures. This class provides mechanisms for multiple clients to
// simultaneously scan for nearby peers filtered by adveritising data
// contents. This class also provides hooks for other layers to manage the
// Adapter's scan state for other procedures that require it (e.g. connection
// establishment, pairing procedures, and other scan and advertising
// procedures).
//
// An instance of LowEnergyDiscoveryManager can be initialized in either
// "legacy" or "extended" mode. The legacy mode is intended for Bluetooth
// controllers that only support the pre-5.0 HCI scan command set. The extended
// mode is intended for Bluetooth controllers that claim to support the "LE
// Extended Advertising" feature.
//
// Only one instance of LowEnergyDiscoveryManager should be created per
// hci::Transport object as multiple instances cannot correctly maintain state
// if they operate concurrently.
//
// To request a session, a client calls StartDiscovery() and asynchronously
// obtains a LowEnergyDiscoverySession that it uniquely owns. The session object
// can be configured with a callback to receive scan results. The session
// maintains an internal filter that may be modified to restrict the scan
// results based on properties of received advertisements.
//
// PROCEDURE:
//
// Starting the first discovery session initiates a periodic scan procedure, in
// which the scan is stopped and restarted for a given scan period (10.24
// seconds by default). This continues until all sessions have been removed.
//
// By default duplicate filtering is used which means that a new advertising
// report will be generated for each discovered advertiser only once per scan
// period. Scan results for each scan period are cached so that sessions added
// during a scan period can receive previously processed results.
//
// EXAMPLE:
//     bt::gap::LowEnergyDiscoveryManager discovery_manager(
//         bt::gap::LowEnergyDiscoveryManager::Mode::kLegacy,
//         transport, dispatcher);
//     ...
//
//     // Only scan for peers advertising the "Heart Rate" GATT Service.
//     uint16_t uuid = 0x180d;
//     bt::hci::DiscoveryFilter discovery_filter;
//     discovery_filter.set_service_uuids({bt::UUID(uuid)});
//
//     std::vector<bt::hci::DiscoveryFilter> discovery_filters;
//     discovery_filters.push_back(discovery_filter);
//
//     std::unique_ptr<bt::gap::LowEnergyDiscoverySession> session;
//     discovery_manager.StartDiscovery(/*active=*/true, discovery_filters,
//       [&session](auto new_session) {
//         // Take ownership of the session to make sure it isn't terminated
//         // when this callback returns.
//         session = std::move(new_session);
//
//         session->SetResultCallback([](
//           const bt::hci::LowEnergyScanResult& result) {
//             // Do stuff with |result|
//           });
//       });
//
// NOTE: These classes are not thread-safe. An instance of
// LowEnergyDiscoveryManager is bound to its creation thread and the associated
// dispatcher and must be accessed and destroyed on the same thread.

// Represents a LE discovery session initiated via
// LowEnergyDiscoveryManager::StartDiscovery(). Instances cannot be created
// directly; instead they are handed to callers by LowEnergyDiscoveryManager.
//
// The discovery classes are not thread-safe. A LowEnergyDiscoverySession MUST
// be accessed and destroyed on the thread that it was created on.

class LowEnergyDiscoverySession;
using LowEnergyDiscoverySessionPtr = std::unique_ptr<LowEnergyDiscoverySession>;

// See comments above.
class LowEnergyDiscoveryManager final
    : public hci::LowEnergyScanner::Delegate,
      public WeakSelf<LowEnergyDiscoveryManager> {
 public:
  // |peer_cache| and |scanner| MUST out-live this LowEnergyDiscoveryManager.
  LowEnergyDiscoveryManager(
      hci::LowEnergyScanner* scanner,
      PeerCache* peer_cache,
      const hci::AdvertisingPacketFilter::Config& packet_filter_config,
      pw::async::Dispatcher& dispatcher);
  ~LowEnergyDiscoveryManager() override;

  // Starts a new discovery session and reports the result via |callback|. If a
  // session has been successfully started the caller will receive a new
  // LowEnergyDiscoverySession instance via |callback| which it uniquely owns.
  // |active| indicates whether active or passive discovery should occur.
  // On failure a nullptr will be returned via |callback|.
  //
  // TODO(armansito): Implement option to disable duplicate filtering. Would
  // this require software filtering for clients that did not request it?
  using SessionCallback = fit::function<void(LowEnergyDiscoverySessionPtr)>;
  void StartDiscovery(bool active,
                      std::vector<hci::DiscoveryFilter> filters,
                      SessionCallback callback);

  // Pause current and future discovery sessions until the returned PauseToken
  // is destroyed. If PauseDiscovery is called multiple times, discovery will be
  // paused until all returned PauseTokens are destroyed. NOTE:
  // deferred_action::cancel() must not be called, or else discovery will never
  // resume.
  using PauseToken = fit::deferred_action<fit::callback<void()>>;
  [[nodiscard]] PauseToken PauseDiscovery();

  // Sets a new scan period to any future and ongoing discovery procedures.
  void set_scan_period(pw::chrono::SystemClock::duration period) {
    scan_period_ = period;
  }

  // Returns whether there is an active scan in progress.
  bool discovering() const;

  // Returns true if discovery is paused.
  bool paused() const { return *paused_count_ != 0; }

  // Registers a callback which runs when a connectable advertisement is
  // received from known peer which was previously observed to be connectable
  // during general discovery. The |peer| argument is guaranteed to be valid
  // until the callback returns. The callback can also assume that LE transport
  // information (i.e. |peer->le()|) will be present and accessible.
  using PeerConnectableCallback = fit::function<void(Peer* peer)>;
  void set_peer_connectable_callback(PeerConnectableCallback callback) {
    connectable_cb_ = std::move(callback);
  }

  void AttachInspect(inspect::Node& parent, std::string name);

  void set_active_scan_parameters(uint16_t interval, uint16_t window);

 private:
  enum class State {
    kIdle,
    kStarting,
    kActive,
    kPassive,
    kStopping,
  };
  static std::string StateToString(State state);

  struct InspectProperties {
    inspect::Node node;
    inspect::UintProperty failed_count;
    inspect::DoubleProperty active_scan_interval_ms;
    inspect::DoubleProperty active_scan_window_ms;
  };

  const PeerCache* peer_cache() const { return peer_cache_; }

  // Creates and stores a new session object and returns it.
  std::unique_ptr<LowEnergyDiscoverySession> AddSession(
      bool active, std::vector<hci::DiscoveryFilter> discovery_filters);

  // Called by LowEnergyDiscoverySession to stop a session that it was assigned
  // to.
  void RemoveSession(LowEnergyDiscoverySession* session);

  // hci::LowEnergyScanner::Delegate override:
  void OnPeerFound(const std::unordered_set<uint16_t>& scan_ids,
                   const hci::LowEnergyScanResult& result) override;
  void OnDirectedAdvertisement(const hci::LowEnergyScanResult& result) override;

  // Called by hci::LowEnergyScanner
  void OnScanStatus(hci::LowEnergyScanner::ScanStatus status);

  // Handlers for scan status updates.
  void OnScanFailed();
  void OnPassiveScanStarted();
  void OnActiveScanStarted();
  void OnScanStopped();
  void OnScanComplete();

  // Create sessions for all pending requests and pass the sessions to the
  // request callbacks.
  void NotifyPending();

  // Tells the scanner to start scanning. Aliases are provided for improved
  // readability.
  void StartScan(bool active);
  inline void StartActiveScan() { StartScan(true); }
  inline void StartPassiveScan() { StartScan(false); }

  // Tells the scanner to stop scanning.
  void StopScan();

  // If there are any pending requests or valid sessions, start discovery.
  // Discovery must not be paused.
  // Called when discovery is unpaused or the scan period ends and needs to be
  // restarted.
  void ResumeDiscovery();

  // Used by destructor to handle all sessions
  void DeactivateAndNotifySessions();

  // The dispatcher that we use for invoking callbacks asynchronously.
  pw::async::Dispatcher& dispatcher_;
  pw::async::HeapDispatcher heap_dispatcher_{dispatcher_};

  InspectProperties inspect_;

  StringInspectable<State> state_;

  // The peer cache that we use for storing and looking up scan results. We
  // hold a raw pointer as we expect this to out-live us.
  PeerCache* const peer_cache_;

  uint16_t next_scan_id_ = 0;
  hci::AdvertisingPacketFilter::Config packet_filter_config_;

  // Called when a directed connectable advertisement is received during an
  // active or passive scan.
  PeerConnectableCallback connectable_cb_;

  // The list of currently pending calls to start discovery.
  struct DiscoveryRequest {
    bool active;
    std::vector<hci::DiscoveryFilter> filters;
    SessionCallback callback;
  };
  std::vector<DiscoveryRequest> pending_;

  // The currently active/known sessions. The number of elements in |sessions_|
  // acts as our scan reference count. When |sessions_| becomes empty scanning
  // is stopped. Similarly, scanning is started on the insertion of the first
  // element.
  //
  // We store raw (weak) pointers here because, while we don't actually own the
  // session objects they will always notify us before destruction so we can
  // remove them from this list.
  //
  using ScanId = uint16_t;
  std::unordered_map<ScanId, LowEnergyDiscoverySession*> sessions_;

  // The value (in ms) that we use for the duration of each scan period.
  pw::chrono::SystemClock::duration scan_period_ = kLEGeneralDiscoveryScanMin;

  uint16_t active_scan_interval_ = kLEScanFastInterval;
  uint16_t active_scan_window_ = kLEScanFastWindow;

  // Count of the number of outstanding PauseTokens. When |paused_count_| is 0,
  // discovery is unpaused.
  IntInspectable<int> paused_count_;

  // The scanner that performs the HCI procedures. |scanner_| must out-live this
  // discovery manager.
  hci::LowEnergyScanner* scanner_;  // weak

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyDiscoveryManager);
};

class LowEnergyDiscoverySession final
    : public WeakSelf<LowEnergyDiscoverySession> {
 public:
  LowEnergyDiscoverySession(
      uint16_t scan_id,
      bool active,
      pw::async::Dispatcher& dispatcher,
      fit::function<void(LowEnergyDiscoverySession*)> notify_cached_peers_cb,
      fit::function<void(LowEnergyDiscoverySession*)> on_stop_cb);

  // Destroying a session instance automatically ends the session.
  ~LowEnergyDiscoverySession();

  // Sets a callback for receiving notifications on discovered peers.
  // |data| contains advertising and scan response data (if any) obtained during
  // discovery.
  //
  // When this callback is set, it will immediately receive notifications for
  // the cached results from the most recent scan period. If a filter was
  // assigned earlier, then the callback will only receive results that match
  // the filter.
  //
  // Passive discovery sessions will call this callback for both directed and
  // undirected advertisements from known peers, while active discovery sessions
  // will ignore directed advertisements (as they are not from new peers).
  using PeerFoundFunction = fit::function<void(const Peer& peer)>;
  void SetResultCallback(PeerFoundFunction callback);

  // Called to deliver scan results.
  void NotifyDiscoveryResult(const Peer& peer) const;

  // Sets a callback to get notified when the session becomes inactive due to an
  // internal error.
  void set_error_callback(fit::closure callback) {
    error_cb_ = std::move(callback);
  }

  // Marks this session as inactive and notifies the error handler.
  void NotifyError();

  // Ends this session. This instance will stop receiving notifications for
  // peers.
  void Stop();

  // Returns true if this session has not been stopped and has not errored.
  bool alive() const { return alive_; }

  uint16_t scan_id() const { return scan_id_; }

  // Returns true if this is an active discovery session, or false if this is a
  // passive discovery session.
  bool active() const { return active_; }

 private:
  uint16_t scan_id_;
  bool alive_ = true;
  bool active_;
  pw::async::HeapDispatcher heap_dispatcher_;
  fit::callback<void()> error_cb_;
  PeerFoundFunction peer_found_fn_;
  fit::function<void(LowEnergyDiscoverySession*)> notify_cached_peers_cb_;
  fit::callback<void(LowEnergyDiscoverySession*)> on_stop_cb_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyDiscoverySession);
};

}  // namespace gap
}  // namespace bt
