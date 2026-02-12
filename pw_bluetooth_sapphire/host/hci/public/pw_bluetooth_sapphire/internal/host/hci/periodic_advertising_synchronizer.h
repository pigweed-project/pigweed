// Copyright 2025 The Pigweed Authors
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

#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/identifier.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/big_info.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::hci {

class PeriodicAdvertisingSynchronizer;
class Transport;

// A unique identifier for a periodic advertising synchronization.
using SyncId = Identifier<uint64_t>;
constexpr static SyncId kInvalidSyncId = SyncId(0);

// An RAII handle that represents a periodic advertising synchronization.
// Destroying this handle will cancel the synchronization.
class PeriodicAdvertisingSync final {
 public:
  PeriodicAdvertisingSync(PeriodicAdvertisingSync&& other);
  PeriodicAdvertisingSync& operator=(PeriodicAdvertisingSync&& other);
  ~PeriodicAdvertisingSync();

  // Explicitly cancels the synchronization. This is equivalent to destroying
  // the handle.
  void Cancel();

  SyncId id() const { return id_; }

 private:
  friend class PeriodicAdvertisingSynchronizer;
  PeriodicAdvertisingSync(
      SyncId id,
      const WeakSelf<PeriodicAdvertisingSynchronizer>::WeakPtr& synchronizer);

  void Move(PeriodicAdvertisingSync& other);

  SyncId id_;
  WeakSelf<PeriodicAdvertisingSynchronizer>::WeakPtr synchronizer_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(PeriodicAdvertisingSync);
};

// This class is responsible for managing the HCI state for synchronizing to
// periodic advertising trains. It handles sending the necessary HCI commands
// and processing the corresponding events.
//
// This class is intended to be used by the GAP layer, which will provide a
// more abstract and user-friendly API.
class PeriodicAdvertisingSynchronizer final {
 public:
  explicit PeriodicAdvertisingSynchronizer(
      const WeakSelf<Transport>::WeakPtr& transport);
  ~PeriodicAdvertisingSynchronizer();

  constexpr static SyncId kInvalidSyncId = bt::hci::kInvalidSyncId;

  // Represents a received BIGInfo report.
  using BroadcastIsochronousGroupInfo = hci_spec::BroadcastIsochronousGroupInfo;

  // Represents a received periodic advertising report.
  struct PeriodicAdvertisingReport {
    // The data from the advertising report.
    DynamicByteBuffer data;

    // The RSSI of the received packet.
    int8_t rssi;

    // The periodic event counter of the received packet. Only available in v2
    // reports.
    std::optional<uint16_t> event_counter;
  };

  // Parameters for a periodic advertising synchronization.
  struct SyncParameters {
    // The address of the advertiser.
    DeviceAddress address;

    // The advertising SID of the periodic advertising train.
    uint8_t advertising_sid;

    // The interval of the periodic advertising.
    uint16_t interval;

    // The PHY used by the advertiser for periodic advertising.
    pw::bluetooth::emboss::LEPhy phy;

    // The number of subevents in the periodic advertising train.
    uint8_t subevents_count;

    bool operator==(const SyncParameters& other) const {
      return address == other.address &&
             advertising_sid == other.advertising_sid &&
             interval == other.interval && phy == other.phy &&
             subevents_count == other.subevents_count;
    }
  };

  // Options for a periodic advertising synchronization.
  struct SyncOptions {
    // Whether to filter duplicate advertising reports.
    bool filter_duplicates;
  };

  // Delegate for receiving events from the PeriodicAdvertisingSynchronizer.
  class Delegate {
   public:
    virtual ~Delegate() = default;

    // Called when a periodic advertising synchronization has been established.
    virtual void OnSyncEstablished(SyncId id, SyncParameters parameters) = 0;

    // Called when a periodic advertising synchronization has been lost.
    virtual void OnSyncLost(SyncId id, Error error) = 0;

    // Called when a periodic advertising report is received.
    virtual void OnAdvertisingReport(SyncId id,
                                     PeriodicAdvertisingReport&& report) = 0;

    // Called when a BIGInfo report is received.
    virtual void OnBigInfoReport(SyncId id,
                                 BroadcastIsochronousGroupInfo&& report) = 0;
  };

  // Starts a periodic advertising synchronization.
  //
  // @param advertiser_address The address of the advertiser to synchronize to.
  // @param advertising_sid The advertising SID of the periodic advertising
  // train.
  // @param options The options for the synchronization.
  // @param delegate The delegate to receive events.
  // @return A handle to the synchronization on success, or an error on
  // failure.
  [[nodiscard]] Result<PeriodicAdvertisingSync> CreateSync(
      DeviceAddress advertiser_address,
      uint8_t advertising_sid,
      SyncOptions options,
      Delegate& delegate);

  // Transfers a periodic advertising synchronization to a connected peer (PAST
  // Sender).
  //
  // @param id The ID of the synchronization to transfer.
  // @param connection The connection to transfer the synchronization over.
  // @param service_data Application-specific data to transfer with the sync.
  // @param callback The callback to be invoked with the result of the
  // transfer.
  void TransferSync(SyncId id,
                    hci_spec::ConnectionHandle connection,
                    uint16_t service_data,
                    pw::Callback<void(Result<>)> callback);

 private:
  friend class PeriodicAdvertisingSync;

  enum class State {
    kIdle,
    kCreateSyncPending,
    kAddDevicePending,
    kRemoveDevicePending,
    kCreateSyncCancelPending,
    kBadState
  };

  struct AdvertiserListEntry {
    DeviceAddress address;
    uint8_t advertising_sid;

    bool operator<(const AdvertiserListEntry& other) const {
      return std::tie(address, advertising_sid) <
             std::tie(other.address, other.advertising_sid);
    }

    bool operator==(const AdvertiserListEntry& other) const {
      return address == other.address &&
             advertising_sid == other.advertising_sid;
    }
  };

  // A request to create a periodic advertising synchronization that is pending
  // in the controller.
  struct PendingRequest {
    SyncId id;
    SyncOptions options;
    Delegate* delegate;
  };

  // An established periodic advertising synchronization.
  struct EstablishedSync {
    SyncId id;
    DeviceAddress address;
    uint8_t adv_sid;
    Delegate* delegate;
    std::vector<uint8_t> partial_report_buffer;
  };

  void OnSyncEstablished(const EventPacket& event);
  void OnSyncLost(const EventPacket& event);
  void OnPeriodicAdvertisingReport(const EventPacket& event);
  void OnBigInfoReport(const EventPacket& event);

  // Cancels a synchronization attempt. The result of the cancellation will be
  // reported through the Delegate::OnSyncLost event.
  void CancelSync(SyncId sync_id);

  // Helper methods for CancelSync. Each of these will return true if they found
  // and canceled a sync, and false otherwise.
  bool CancelPendingCreateSync(SyncId sync_id);
  bool CancelEstablishedSync(SyncId sync_id);

  // This function sends the next command needed to configure the advertiser
  // list. It is a no-op once the advertiser list is optimal and the Create Sync
  // command is pending. It is also a no-op if there are no pending requests and
  // there is no pending Create Sync command.
  // @param advertiser_list_full Indicates the list is full and no entries
  // should be added.
  void MaybeUpdateAdvertiserList(bool advertiser_list_full = false);

  void SendCreateSyncCommand(const SyncOptions& options,
                             bool advertiser_list_full = false);
  void SendCreateSyncCancelCommand();
  void SendAddDeviceToListCommand(const AdvertiserListEntry& entry);
  void SendRemoveDeviceFromListCommand(const AdvertiserListEntry& entry);

  void FailAllRequests(Error error);
  void FailRequestsWithEntriesInAdvertiserList(Error error);

  Transport::WeakPtr transport_;

  CommandChannel::EventHandlerId sync_established_v1_handler_id_;
  CommandChannel::EventHandlerId sync_established_v2_handler_id_;
  CommandChannel::EventHandlerId sync_lost_event_handler_id_;
  CommandChannel::EventHandlerId advertising_report_v1_event_handler_id_;
  CommandChannel::EventHandlerId advertising_report_v2_event_handler_id_;
  CommandChannel::EventHandlerId biginfo_report_event_handler_id_;

  SyncId next_sync_id_{1};

  // Requests to create a sync that have not yet received a Sync Established
  // event.
  std::map<AdvertiserListEntry, PendingRequest> pending_requests_;

  // The set of devices that have been added to the periodic advertiser list.
  std::set<AdvertiserListEntry> advertiser_list_;

  State state_ = State::kIdle;

  // Established periodic advertising synchronizations, keyed by sync handle.
  std::unordered_map<hci_spec::SyncHandle, EstablishedSync> syncs_;

  WeakSelf<PeriodicAdvertisingSynchronizer> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(PeriodicAdvertisingSynchronizer);
};

}  // namespace bt::hci
