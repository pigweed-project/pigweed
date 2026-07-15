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

#include <deque>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/defaults.h"
#include "pw_bluetooth_sapphire/internal/host/hci/advertising_packet_filter.h"
#include "pw_bluetooth_sapphire/internal/host/hci/discovery_filter.h"
#include "pw_bluetooth_sapphire/internal/host/hci/local_address_delegate.h"

namespace bt::hci {

class SequentialCommandRunner;

// Represents a discovered Bluetooth Low Energy peer.
class LowEnergyScanResult {
 public:
  LowEnergyScanResult() = default;
  LowEnergyScanResult(const DeviceAddress& address,
                      bool resolved,
                      bool connectable)
      : address_(address), resolved_(resolved), connectable_(connectable) {}

  LowEnergyScanResult& operator=(const LowEnergyScanResult& other);
  LowEnergyScanResult(const LowEnergyScanResult& other) { *this = other; }

  const DeviceAddress& address() const { return address_; }
  bool connectable() const { return connectable_; }

  bool resolved() const { return resolved_; }
  void set_resolved(bool value) { resolved_ = value; }

  int8_t rssi() const { return rssi_; }
  void set_rssi(int8_t value) { rssi_ = value; }

  int8_t tx_power() const { return tx_power_; }
  void set_tx_power(int8_t value) { tx_power_ = value; }

  uint8_t advertising_sid() const { return advertising_sid_; }
  void set_advertising_sid(uint8_t value) { advertising_sid_ = value; }

  uint16_t periodic_advertising_interval() const {
    return periodic_advertising_interval_;
  }
  void set_periodic_advertising_interval(uint16_t value) {
    periodic_advertising_interval_ = value;
  }

  BufferView data() const { return buffer_.view(0, data_size_); }
  void AppendData(const ByteBuffer& data);

 private:
  // The device address of the remote peer.
  DeviceAddress address_;

  // True if |address| is a static or random identity address resolved by the
  // controller.
  bool resolved_ = false;

  // True if this peer accepts connections. This is the case if this peer sent a
  // connectable advertising PDU.
  bool connectable_ = false;

  // The received signal strength of the advertisement packet corresponding to
  // this peer.
  int8_t rssi_ = hci_spec::kRSSIInvalid;

  // The transmitted signal strength of this packet, according to the advertiser
  int8_t tx_power_ = hci_spec::kLEExtendedAdvertisingTxPowerNoPreference;

  // Matches the advertising SID subfield in the ADI field of the received
  // advertisement, used to synchronize against a periodic advertising train
  uint8_t advertising_sid_ = hci_spec::kAdvertisingSidInvalid;

  // Interval of periodic advertisements, in increments of 1.25 ms, specified in
  // the LE extended advertising report.
  uint16_t periodic_advertising_interval_ =
      hci_spec::kPeriodicAdvertisingIntervalInvalid;

  // The size of the data so far accumulated in |buffer_|.
  size_t data_size_ = 0u;

  // Stores both advertising and scan response payloads.
  DynamicByteBuffer buffer_;
};

// LowEnergyScanner manages Low Energy scan procedures that are used during
// general and limited discovery and connection establishment procedures. This
// is an abstract class that provides a common interface over 5.0 Extended
// Advertising and Legacy Advertising features.
//
// Instances of this class are expected to each as a singleton on a
// per-transport basis as multiple instances cannot accurately reflect the state
// of the controller while allowing simultaneous scan operations.
class LowEnergyScanner : public LocalAddressClient {
 public:
  // Value that can be passed to StartScan() to scan indefinitely.
  static constexpr pw::chrono::SystemClock::duration kPeriodInfinite =
      pw::chrono::SystemClock::duration::zero();

  // Maximum number of scan results that will be cached.
  static constexpr size_t kMaxCachedResults = 64;

  enum class State {
    // No scan is currently being performed.
    kIdle,

    // A previously running scan is being stopped.
    kStopping,

    // A scan is being initiated.
    kInitiating,

    // An active scan is currently being performed.
    kActiveScanning,

    // A passive scan is currently being performed.
    kPassiveScanning,
  };

  enum class ScanStatus {
    // Reported when the scan could not be started.
    kFailed,

    // Reported when an active scan was started and is currently in progress.
    kActive,

    // Reported when a passive scan was started and is currently in progress.
    kPassive,

    // Called when the scan was terminated naturally at the end of the scan
    // period.
    kComplete,

    // Called when the scan was terminated due to a call to StopScan().
    kStopped,
  };

  struct ScanOptions {
    // Perform an active scan if true. During an active scan, scannable
    // advertisements are reported alongside their corresponding scan response.
    bool active = false;

    // When enabled, the controller will filter out duplicate advertising
    // reports. This means that Delegate::OnPeerFound will be called only once
    // per device address during the scan period.
    //
    // When disabled, Delegate::OnPeerFound will get called once for every
    // observed advertisement (depending on |filter_policy|).
    bool filter_duplicates = false;

    // Determines the type of filtering the controller should perform to limit
    // the number of advertising reports.
    pw::bluetooth::emboss::LEScanFilterPolicy filter_policy =
        pw::bluetooth::emboss::LEScanFilterPolicy::BASIC_UNFILTERED;

    // Determines the length of the software defined scan period. If the value
    // is kPeriodInfinite, then the scan will remain enabled until StopScan()
    // gets called. For all other values, the scan will be disabled after the
    // duration expires.
    pw::chrono::SystemClock::duration period = kPeriodInfinite;

    // Maximum time duration during an active scan for which a scannable
    // advertisement will be stored and not reported to clients until a
    // corresponding scan response is received.
    pw::chrono::SystemClock::duration scan_response_timeout =
        std::chrono::seconds(2);

    // Scan parameters.
    uint16_t interval = hci_spec::defaults::kLEScanInterval;
    uint16_t window = hci_spec::defaults::kLEScanWindow;
  };

  // This represents the data obtained for a scannable advertisement for which a
  // scan response has not yet been received. Clients are notified for scannable
  // advertisement either when the corresponding scan response is received or,
  // otherwise, a timeout expires.
  class PendingScanResult {
   public:
    PendingScanResult(LowEnergyScanResult&& result,
                      pw::async::Dispatcher& dispatcher,
                      pw::chrono::SystemClock::duration timeout,
                      fit::closure timeout_handler);
    ~PendingScanResult() { CancelTimeout(); }

    LowEnergyScanResult& result() { return result_; }

    void StartTimer() {
      CancelTimeout();
      timeout_task_.PostAfter(timeout_);
    }

    bool CancelTimeout() { return timeout_task_.Cancel(); }

   private:
    LowEnergyScanResult result_;

    // The duration which we will wait for a pending scan result to receive more
    // data before reporting the pending result to the delegate.
    pw::chrono::SystemClock::duration timeout_;

    // Since not all scannable advertisements are always followed by a scan
    // response, we report a pending result if a scan response is not received
    // within a timeout.
    SmartTask timeout_task_;
  };

  // Interface for receiving events related to Low Energy scan.
  class Delegate {
   public:
    virtual ~Delegate() = default;

    // Called when a peer is found. During a passive scan |result| contains the
    // advertising data. During an active scan |result| contains the combined
    // advertising and scan response data (if the peer is scannable).
    virtual void OnPeerFound(const std::unordered_set<uint16_t>& /*scan_id*/,
                             const LowEnergyScanResult& /*result*/) {}

    // Called when a directed advertising report is received from the peer with
    // the given address.
    virtual void OnDirectedAdvertisement(
        const LowEnergyScanResult& /*result*/) {}
  };

  LowEnergyScanner(LocalAddressDelegate* local_addr_delegate,
                   const AdvertisingPacketFilter::Config& packet_filter_config,
                   Transport::WeakPtr hci,
                   pw::async::Dispatcher& pw_dispatcher);
  ~LowEnergyScanner() override = default;

  // Returns the current Scan state.
  State state() const { return state_; }

  bool IsActiveScanning() const { return state() == State::kActiveScanning; }
  bool IsPassiveScanning() const { return state() == State::kPassiveScanning; }
  bool IsScanning() const { return IsActiveScanning() || IsPassiveScanning(); }
  bool IsInitiating() const { return state() == State::kInitiating; }
  bool IsUsingOffloadedFiltering() const {
    return packet_filter_.IsUsingOffloadedFiltering();
  }

  virtual bool IsExtendedScanner() const { return false; }

  // LocalAddressClient override:
  bool AllowsRandomAddressChange() const override {
    return !IsScanning() && hci_cmd_runner_->IsReady();
  }

  // True if no scan procedure is currently enabled.
  bool IsIdle() const { return state() == State::kIdle; }

  // Associate a set of packet filters with a particular upper layer scan
  // session with a given scan id.
  void SetPacketFilters(uint16_t scan_id,
                        const std::vector<DiscoveryFilter>& filters);

  // Unassociate all packet filters with a particular upper layer scan session
  // with a given scan id.
  void UnsetPacketFilters(uint16_t scan_id);

  // Initiates a scan. This is an asynchronous operation that abides by the
  // following rules:
  //
  //   - This method synchronously returns false if the procedure could not be
  //   started, e.g. because discovery is already in progress, or it is in the
  //   process of being stopped, or the controller does not support discovery,
  //   etc.
  //
  //   - Synchronously returns true if the procedure was initiated but the it is
  //   unknown whether or not the procedure has succeeded.
  //
  //   - |callback| is invoked asynchronously to report the status of the
  //   procedure. In the case of failure, |callback| will be invoked once to
  //   report the end of the procedure. In the case of success, |callback| will
  //   be invoked twice: the first time to report that the procedure has
  //   started, and a second time to report when the procedure ends, either due
  //   to a timeout or cancellation.
  //
  //   - |period| specifies (in milliseconds) the duration of the scan. If the
  //   special value of kPeriodInfinite is passed then scanning will continue
  //   indefinitely and must be explicitly stopped by calling StopScan().
  //   Otherwise, the value must be non-zero.
  //
  // Once started, a scan can be terminated at any time by calling the
  // StopScan() method. Otherwise, an ongoing scan will terminate at the end of
  // the scan period if a finite value for |period| was provided.
  //
  // During an active scan, scannable advertisements are reported alongside
  // their corresponding scan response. Every scannable advertisement is stored
  // and not reported until either
  //
  //   a) a scan response is received
  //   b) an implementation determined timeout period expires
  //   c) for periodic scans, when the scan period expires
  //
  // Since a passive scan involves no scan request/response, all advertisements
  // are reported immediately without waiting for a scan response.
  //
  // (For more information about passive and active scanning, see Core Spec.
  // v5.2, Vol 6, Part B, 4.4.3.1 and 4.4.3.2).
  using ScanStatusCallback = fit::function<void(ScanStatus)>;
  virtual bool StartScan(const ScanOptions& options,
                         ScanStatusCallback callback);

  // Stops a previously started scan. Returns false if a scan is not in
  // progress. Otherwise, cancels any in progress scan procedure and returns
  // true.
  virtual bool StopScan();

  // Call the Delegate::OnPeerFound method for all cached peers. This method is
  // useful for relaying peers to an upper layer scan session that joins in the
  // middle of a real Controller scan.
  void NotifyCachedPeers(uint16_t scan_id);

  // Assigns the delegate for scan events.
  void set_delegate(Delegate* delegate) { delegate_ = delegate; }

 protected:
  bool AnyFiltersPass(const std::vector<DiscoveryFilter>& filters,
                      const AdvertisingData::ParseResult& ad,
                      bool connectable,
                      int8_t rssi) const;

  void NotifyPeerFound(const LowEnergyScanResult& result);

  void NotifyDirectedAdvertisement(const LowEnergyScanResult& result) const {
    delegate()->OnDirectedAdvertisement(result);
  }

  // Build the HCI command packet to set the scan parameters for the flavor of
  // low energy scanning being implemented.
  virtual CommandPacket BuildSetScanParametersPacket(
      const DeviceAddress& local_address, const ScanOptions& options) const = 0;

  // Build the HCI command packet to enable scanning for the flavor of low
  // energy scanning being implemented.
  virtual CommandPacket BuildEnablePacket(
      const ScanOptions& options,
      pw::bluetooth::emboss::GenericEnableParam enable) const = 0;

  void AddPendingResult(LowEnergyScanResult&& scan_result);

  bool HasPendingResult(const DeviceAddress& address) {
    return pending_results_.count(address) != 0;
  }

  std::unique_ptr<PendingScanResult> RemovePendingResult(
      const DeviceAddress& address);

  void set_state(State state) { state_ = state; }
  pw::async::Dispatcher& dispatcher() const { return pw_dispatcher_; }
  Transport::WeakPtr hci() const { return hci_; }
  Delegate* delegate() const { return delegate_; }

 private:
  // Called by StartScan() after the local peer address has been obtained.
  void StartScanInternal(const DeviceAddress& local_address,
                         const ScanOptions& options,
                         ScanStatusCallback callback);

  // Called by StopScan() and by the scan timeout handler set up by StartScan().
  void StopScanInternal(bool stopped);

  State state_ = State::kIdle;
  pw::async::Dispatcher& pw_dispatcher_;
  Delegate* delegate_ = nullptr;  // weak

  // Callback passed in to the most recently accepted call to StartScan();
  ScanStatusCallback scan_cb_;

  // The scan period timeout handler for the currently active scan session.
  SmartTask scan_timeout_task_;

  // Maximum time duration for which a scannable advertisement will be stored
  // and not reported to clients until a corresponding scan response is
  // received.
  pw::chrono::SystemClock::duration scan_response_timeout_;

  // Cached scan results for the current scan period during discovery. The
  // minimum (and default) scan period is 10.24 seconds when performing LE
  // discovery. This can cause a long wait for a discovery session that joined
  // in the middle of a scan period and duplicate filtering is enabled. We
  // maintain this cache to immediately notify new sessions of the currently
  // cached results for this period.
  std::deque<LowEnergyScanResult> cached_scan_results_;

  // Scannable advertising events for which a Scan Response PDU has not been
  // received. This is accumulated during a discovery procedure and always
  // cleared at the end of the scan period.
  std::unordered_map<DeviceAddress, std::unique_ptr<PendingScanResult>>
      pending_results_;

  // Manages all filtering related tasks for LowEnergyScanner (e.g. Host level
  // filtering, Controller offloaded filtering, and the transition between the
  // two, etc).
  AdvertisingPacketFilter packet_filter_;

  // Used to obtain the local peer address type to use during scanning.
  LocalAddressDelegate* local_addr_delegate_;  // weak

  // The HCI transport.
  Transport::WeakPtr hci_;

  // Command runner for all HCI commands sent out by implementations.
  std::unique_ptr<SequentialCommandRunner> hci_cmd_runner_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(LowEnergyScanner);
};

}  // namespace bt::hci
