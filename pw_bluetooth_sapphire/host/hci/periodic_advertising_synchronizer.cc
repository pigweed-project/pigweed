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

#include "pw_bluetooth_sapphire/internal/host/hci/periodic_advertising_synchronizer.h"

#include <pw_assert/check.h>

#include <algorithm>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/util.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::hci {

using pw::bluetooth::emboss::StatusCode;

// 163.84s, the maximum supported timeout. This is twice the maximum periodic
// advertising interval of 81.91875s.
constexpr uint16_t kDefaultSyncTimeout = 0x4000;

namespace {

// Helper to build the LE_Periodic_Advertising_Create_Sync command.
std::unique_ptr<CommandPacket> BuildCreateSyncCommand(
    const PeriodicAdvertisingSynchronizer::SyncOptions& options) {
  auto command = CommandPacket::New<
      pw::bluetooth::emboss::LEPeriodicAdvertisingCreateSyncCommandWriter>(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_CREATE_SYNC);
  auto view = command.view_t();
  view.options().use_periodic_advertiser_list().Write(true);
  view.options().disable_reporting().Write(false);
  view.options().enable_duplicate_filtering().Write(options.filter_duplicates);
  view.skip().Write(0);
  view.sync_timeout().Write(kDefaultSyncTimeout);
  // The presence of a Constant Tone Extension is irrelevant.
  view.sync_cte_type().BackingStorage().WriteUInt(0);
  return std::make_unique<CommandPacket>(std::move(command));
}

// Helper to parse both V1 and V2 of the LE Periodic Advertising Sync
// Established Subevent.
struct ParsedSyncEstablishedSubevent {
  StatusCode status;
  hci_spec::SyncHandle sync_handle;
  uint8_t advertising_sid;
  DeviceAddress address;
  pw::bluetooth::emboss::LEPhy phy;
  uint16_t interval;
  uint8_t subevents_count = 0;
};

template <typename T>
ParsedSyncEstablishedSubevent ParseSyncEstablishedSubeventHelper(
    const EventPacket& event) {
  auto view = event.view<T>();
  ParsedSyncEstablishedSubevent result;
  result.status = view.status().Read();
  result.sync_handle = view.sync_handle().Read();
  result.advertising_sid = view.advertising_sid().Read();
  result.address = DeviceAddress(
      DeviceAddress::LeAddrToDeviceAddr(view.advertiser_address_type().Read()),
      DeviceAddressBytes(view.advertiser_address()));
  result.phy = view.advertiser_phy().Read();
  result.interval = view.periodic_advertising_interval().Read();

  if constexpr (std::is_same_v<
                    T,
                    pw::bluetooth::emboss::
                        LEPeriodicAdvertisingSyncEstablishedSubeventV2View>) {
    result.subevents_count = view.num_subevents().Read();
  }

  return result;
}

std::optional<ParsedSyncEstablishedSubevent> ParseSyncEstablishedSubevent(
    const EventPacket& event) {
  auto meta_event_view = event.view<pw::bluetooth::emboss::LEMetaEventView>();
  auto subevent_code = meta_event_view.subevent_code_enum().Read();

  if (subevent_code == pw::bluetooth::emboss::LeSubEventCode::
                           PERIODIC_ADVERTISING_SYNC_ESTABLISHED) {
    return ParseSyncEstablishedSubeventHelper<
        pw::bluetooth::emboss::
            LEPeriodicAdvertisingSyncEstablishedSubeventV1View>(event);
  }

  if (subevent_code == pw::bluetooth::emboss::LeSubEventCode::
                           PERIODIC_ADVERTISING_SYNC_ESTABLISHED_V2) {
    return ParseSyncEstablishedSubeventHelper<
        pw::bluetooth::emboss::
            LEPeriodicAdvertisingSyncEstablishedSubeventV2View>(event);
  }

  bt_log(WARN,
         "hci",
         "unsupported subevent code for sync established: 0x%02x",
         static_cast<uint8_t>(subevent_code));
  return std::nullopt;
}

// Helper to parse both V1 and V2 of the LE Periodic Advertising Report
// Subevent.
struct ParsedAdvertisingReportSubevent {
  hci_spec::SyncHandle sync_handle;
  pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus data_status;
  int8_t rssi;
  std::optional<uint16_t> event_counter;
  pw::span<const uint8_t> data;
};

template <typename T>
std::optional<ParsedAdvertisingReportSubevent>
ParseAdvertisingReportSubeventHelper(const EventPacket& event) {
  auto view = event.view<T>();
  ParsedAdvertisingReportSubevent result;
  result.sync_handle = view.sync_handle().Read();
  result.data_status = view.data_status().Read();
  result.rssi = view.rssi().Read();
  result.data = pw::span(view.data().BackingStorage().data(),
                         view.data().BackingStorage().SizeInBytes());

  if constexpr (std::is_same_v<T,
                               pw::bluetooth::emboss::
                                   LEPeriodicAdvertisingReportSubeventV2View>) {
    result.event_counter = view.periodic_event_counter().Read();
  }

  return result;
}

std::optional<ParsedAdvertisingReportSubevent> ParseAdvertisingReportSubevent(
    const EventPacket& event) {
  auto meta_event_view = event.view<pw::bluetooth::emboss::LEMetaEventView>();
  auto subevent_code = meta_event_view.subevent_code_enum().Read();

  if (subevent_code ==
      pw::bluetooth::emboss::LeSubEventCode::PERIODIC_ADVERTISING_REPORT) {
    return ParseAdvertisingReportSubeventHelper<
        pw::bluetooth::emboss::LEPeriodicAdvertisingReportSubeventV1View>(
        event);
  }

  if (subevent_code ==
      pw::bluetooth::emboss::LeSubEventCode::PERIODIC_ADVERTISING_REPORT_V2) {
    return ParseAdvertisingReportSubeventHelper<
        pw::bluetooth::emboss::LEPeriodicAdvertisingReportSubeventV2View>(
        event);
  }

  bt_log(WARN,
         "hci",
         "unsupported subevent code for advertising report: 0x%02x",
         static_cast<uint8_t>(subevent_code));
  return std::nullopt;
}

}  // namespace

PeriodicAdvertisingSync::PeriodicAdvertisingSync(
    SyncId id,
    const WeakSelf<PeriodicAdvertisingSynchronizer>::WeakPtr& synchronizer)
    : id_(id), synchronizer_(synchronizer) {}

PeriodicAdvertisingSync::PeriodicAdvertisingSync(
    PeriodicAdvertisingSync&& other)
    : id_(kInvalidSyncId) {
  Move(other);
}

PeriodicAdvertisingSync& PeriodicAdvertisingSync::operator=(
    PeriodicAdvertisingSync&& other) {
  if (this != &other) {
    Cancel();
    Move(other);
  }
  return *this;
}

PeriodicAdvertisingSync::~PeriodicAdvertisingSync() { Cancel(); }

void PeriodicAdvertisingSync::Cancel() {
  if (id_ != kInvalidSyncId && synchronizer_.is_alive()) {
    synchronizer_->CancelSync(id_);
  }
  id_ = kInvalidSyncId;
}

void PeriodicAdvertisingSync::Move(PeriodicAdvertisingSync& other) {
  id_ = other.id_;
  synchronizer_ = other.synchronizer_;
  other.id_ = kInvalidSyncId;
}

PeriodicAdvertisingSynchronizer::PeriodicAdvertisingSynchronizer(
    const WeakSelf<Transport>::WeakPtr& transport)
    : transport_(transport), weak_self_(this) {
  // Register event handlers for both V1 and V2 events where applicable.
  sync_established_v1_handler_id_ =
      transport_->command_channel()->AddLEMetaEventHandler(
          pw::bluetooth::emboss::LeSubEventCode::
              PERIODIC_ADVERTISING_SYNC_ESTABLISHED,
          [this](const auto& event) {
            OnSyncEstablished(event);
            return CommandChannel::EventCallbackResult::kContinue;
          });
  sync_established_v2_handler_id_ =
      transport_->command_channel()->AddLEMetaEventHandler(
          pw::bluetooth::emboss::LeSubEventCode::
              PERIODIC_ADVERTISING_SYNC_ESTABLISHED_V2,
          [this](const auto& event) {
            OnSyncEstablished(event);
            return CommandChannel::EventCallbackResult::kContinue;
          });
  sync_lost_event_handler_id_ =
      transport_->command_channel()->AddLEMetaEventHandler(
          pw::bluetooth::emboss::LeSubEventCode::PERIODIC_ADVERTISING_SYNC_LOST,
          [this](const auto& event) {
            OnSyncLost(event);
            return CommandChannel::EventCallbackResult::kContinue;
          });
  advertising_report_v1_event_handler_id_ =
      transport_->command_channel()->AddLEMetaEventHandler(
          pw::bluetooth::emboss::LeSubEventCode::PERIODIC_ADVERTISING_REPORT,
          [this](const auto& event) {
            OnPeriodicAdvertisingReport(event);
            return CommandChannel::EventCallbackResult::kContinue;
          });
  advertising_report_v2_event_handler_id_ =
      transport_->command_channel()->AddLEMetaEventHandler(
          pw::bluetooth::emboss::LeSubEventCode::PERIODIC_ADVERTISING_REPORT_V2,
          [this](const auto& event) {
            OnPeriodicAdvertisingReport(event);
            return CommandChannel::EventCallbackResult::kContinue;
          });
  biginfo_report_event_handler_id_ =
      transport_->command_channel()->AddLEMetaEventHandler(
          pw::bluetooth::emboss::LeSubEventCode::BIG_INFO_ADVERTISING_REPORT,
          [this](const auto& event) {
            OnBigInfoReport(event);
            return CommandChannel::EventCallbackResult::kContinue;
          });
}

PeriodicAdvertisingSynchronizer::~PeriodicAdvertisingSynchronizer() {
  if (transport_.is_alive() && transport_->command_channel()) {
    transport_->command_channel()->RemoveEventHandler(
        sync_established_v1_handler_id_);
    transport_->command_channel()->RemoveEventHandler(
        sync_established_v2_handler_id_);
    transport_->command_channel()->RemoveEventHandler(
        sync_lost_event_handler_id_);
    transport_->command_channel()->RemoveEventHandler(
        advertising_report_v1_event_handler_id_);
    transport_->command_channel()->RemoveEventHandler(
        advertising_report_v2_event_handler_id_);
    transport_->command_channel()->RemoveEventHandler(
        biginfo_report_event_handler_id_);
  }
}

Result<PeriodicAdvertisingSync> PeriodicAdvertisingSynchronizer::CreateSync(
    DeviceAddress advertiser_address,
    uint8_t advertising_sid,
    SyncOptions options,
    Delegate& delegate) {
  if (state_ == State::kBadState) {
    return fit::error(Error(HostError::kFailed));
  }

  if (advertiser_address.type() != DeviceAddress::Type::kLEPublic &&
      advertiser_address.type() != DeviceAddress::Type::kLERandom) {
    return fit::error(Error(HostError::kInvalidParameters));
  }

  for (auto& [_, sync] : syncs_) {
    if (sync.address == advertiser_address && sync.adv_sid == advertising_sid) {
      return fit::error(Error(HostError::kInProgress));
    }
  }

  AdvertiserListEntry entry{.address = advertiser_address,
                            .advertising_sid = advertising_sid};

  if (pending_requests_.count(entry)) {
    return fit::error(Error(HostError::kInProgress));
  }

  SyncId sync_id = next_sync_id_++;
  pending_requests_[entry] = {sync_id, options, &delegate};
  MaybeUpdateAdvertiserList();

  return fit::ok(PeriodicAdvertisingSync(sync_id, weak_self_.GetWeakPtr()));
}

void PeriodicAdvertisingSynchronizer::TransferSync(
    SyncId id,
    hci_spec::ConnectionHandle connection,
    uint16_t service_data,
    pw::Callback<void(Result<>)> callback) {
  auto sync_it = std::find_if(syncs_.begin(), syncs_.end(), [&id](auto& s) {
    return s.second.id == id;
  });
  if (sync_it == syncs_.end()) {
    bt_log(WARN, "hci", "Transfer called with invalid sync_id: %s", bt_str(id));
    callback(fit::error(Error(HostError::kInvalidParameters)));
    return;
  }
  hci_spec::SyncHandle sync_handle = sync_it->first;

  auto command = hci::CommandPacket::New<
      pw::bluetooth::emboss::LEPeriodicAdvertisingSyncTransferCommandWriter>(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_SYNC_TRANSFER);
  auto view = command.view_t();
  view.connection_handle().Write(connection);
  view.service_data().Write(service_data);
  view.sync_handle().Write(sync_handle);

  auto self = weak_self_.GetWeakPtr();
  auto event_cb = [self, cb = std::move(callback), sync_handle](
                      auto, const EventPacket& event) mutable {
    if (!self.is_alive()) {
      return;
    }

    Result<> result = event.ToResult();
    if (result.is_error()) {
      bt_log(
          WARN,
          "hci",
          "Failed to transfer periodic advertising sync (sync_handle: %" PRIu16
          ", %s)",
          sync_handle,
          bt_str(result));
    }
    cb(result);
  };

  transport_->command_channel()
      ->SendCommand(std::move(command),
                    std::move(event_cb),
                    hci_spec::kCommandCompleteEventCode)
      .IgnoreError();
}

void PeriodicAdvertisingSynchronizer::MaybeUpdateAdvertiserList(
    bool advertiser_list_full) {
  if (state_ != State::kIdle && state_ != State::kCreateSyncPending) {
    return;
  }

  if (pending_requests_.empty() && advertiser_list_.empty()) {
    if (state_ == State::kCreateSyncPending) {
      bt_log(DEBUG, "hci", "canceling Create Sync due to no sync requests");
      SendCreateSyncCancelCommand();
      return;
    }
    return;
  }

  // All entries in the list must have the same filter_duplicates setting, so
  // we must sort them into 2 lists.
  std::set<AdvertiserListEntry> duplicate_filtering_entries;
  std::set<AdvertiserListEntry> no_duplicate_filtering_entries;
  for (auto& [entry, req] : pending_requests_) {
    if (req.options.filter_duplicates) {
      duplicate_filtering_entries.emplace(entry);
    } else {
      no_duplicate_filtering_entries.emplace(entry);
    }
  }

  // Use the longer list to maximize parallel synchronization, preferring
  // duplicate filtering to break a tie.
  std::set<AdvertiserListEntry>& next_advertiser_list =
      duplicate_filtering_entries;
  bool filter_duplicates = true;
  if (no_duplicate_filtering_entries.size() >
      duplicate_filtering_entries.size()) {
    next_advertiser_list = no_duplicate_filtering_entries;
    filter_duplicates = false;
  }

  // Once the list is optimal, send a Create Sync command.
  if (advertiser_list_ == next_advertiser_list) {
    // If Create Sync is pending and the list is optimal, there is nothing to
    // do.
    if (state_ == State::kCreateSyncPending) {
      return;
    }
    SendCreateSyncCommand(SyncOptions{.filter_duplicates = filter_duplicates});
    return;
  }

  std::vector<AdvertiserListEntry> entries_to_remove;
  std::set_difference(advertiser_list_.begin(),
                      advertiser_list_.end(),
                      next_advertiser_list.begin(),
                      next_advertiser_list.end(),
                      std::back_inserter(entries_to_remove));

  // If the list is full, Create Sync instead of trying to add entries. This
  // prevents an infinite failure loop.
  if (advertiser_list_full && entries_to_remove.empty()) {
    if (state_ == State::kCreateSyncPending) {
      return;
    }
    SendCreateSyncCommand(SyncOptions{.filter_duplicates = filter_duplicates},
                          advertiser_list_full);
    return;
  }

  // Before updating the list, Create Sync must be canceled.
  if (state_ == State::kCreateSyncPending) {
    SendCreateSyncCancelCommand();
    return;
  }

  // Remove entries before attempting to add more.
  if (!entries_to_remove.empty()) {
    SendRemoveDeviceFromListCommand(entries_to_remove.front());
    return;
  }

  std::vector<AdvertiserListEntry> entries_to_add;
  std::set_difference(next_advertiser_list.begin(),
                      next_advertiser_list.end(),
                      advertiser_list_.begin(),
                      advertiser_list_.end(),
                      std::back_inserter(entries_to_add));
  PW_CHECK(!entries_to_add.empty());
  SendAddDeviceToListCommand(entries_to_add.front());
}

void PeriodicAdvertisingSynchronizer::SendCreateSyncCommand(
    const SyncOptions& options, bool advertiser_list_full) {
  PW_CHECK(state_ == State::kIdle);
  auto self = weak_self_.GetWeakPtr();
  bt_log(
      DEBUG,
      "hci",
      "sending Create Sync (filter_duplicates: %d, advertiser_list_full: %d)",
      options.filter_duplicates,
      advertiser_list_full);
  auto create_cmd = BuildCreateSyncCommand(options);
  state_ = State::kCreateSyncPending;
  // Complete on the Command Status event because there is a separate event
  // handler for LE Periodic Advertising Sync Established.
  transport_->command_channel()
      ->SendCommand(
          std::move(*create_cmd),
          [self, advertiser_list_full](auto, const EventPacket& event) {
            if (!self.is_alive()) {
              return;
            }

            Result<> result = event.ToResult();
            if (result.is_error()) {
              bt_log(WARN,
                     "hci",
                     "Create Sync command failed: %s",
                     bt_str(result.error_value()));

              if (result.error_value().is(
                      StatusCode::MEMORY_CAPACITY_EXCEEDED)) {
                // The controller has insufficient resources to handle more
                // periodic advertising trains, so fail all requests.
                self->state_ = State::kIdle;
                self->FailAllRequests(Error(HostError::kFailed));
                self->MaybeUpdateAdvertiserList();
                return;
              }

              self->state_ = State::kBadState;
              self->FailAllRequests(Error(HostError::kFailed));
              return;
            }

            self->MaybeUpdateAdvertiserList(advertiser_list_full);
          },
          hci_spec::kCommandStatusEventCode)
      .IgnoreError();
}

void PeriodicAdvertisingSynchronizer::SendCreateSyncCancelCommand() {
  PW_CHECK(state_ == State::kCreateSyncPending);

  bt_log(DEBUG, "hci", "canceling Create Sync");
  auto self = weak_self_.GetWeakPtr();
  auto cancel_cmd = hci::CommandPacket::New<
      pw::bluetooth::emboss::
          LEPeriodicAdvertisingCreateSyncCancelCommandWriter>(
      pw::bluetooth::emboss::OpCode::
          LE_PERIODIC_ADVERTISING_CREATE_SYNC_CANCEL);
  state_ = State::kCreateSyncCancelPending;
  transport_->command_channel()
      ->SendCommand(std::move(cancel_cmd),
                    [self](auto, const EventPacket& event) {
                      if (!self.is_alive()) {
                        return;
                      }
                      Result<> result = event.ToResult();

                      if (result.is_error()) {
                        bt_log(WARN,
                               "hci",
                               "Create Sync Cancel command failed: %s",
                               bt_str(result.error_value()));

                        // The only specified error is Command Disallowed,
                        // which indicates that no Create Sync command was
                        // pending (possibly due to a race with the Sync
                        // Established event). Thus, we should continue to
                        // wait for Sync Established.
                        return;
                      }

                      // Create Sync will be pending until a Sync Established
                      // event is received with status "canceled by host".
                    })
      .IgnoreError();
}

void PeriodicAdvertisingSynchronizer::SendAddDeviceToListCommand(
    const AdvertiserListEntry& entry) {
  bt_log(DEBUG,
         "hci",
         "adding device to periodic advertiser list: %s",
         bt_str(entry.address));

  PW_CHECK(state_ == State::kIdle);
  state_ = State::kAddDevicePending;

  auto self = weak_self_.GetWeakPtr();
  auto add_cmd = hci::CommandPacket::New<
      pw::bluetooth::emboss::LEAddDeviceToPeriodicAdvertiserListCommandWriter>(
      pw::bluetooth::emboss::OpCode::LE_ADD_DEVICE_TO_PERIODIC_ADVERTISER_LIST);
  auto view = add_cmd.view_t();
  view.advertiser_address_type().Write(
      DeviceAddress::DeviceAddrToLePeerAddrNoAnon(entry.address.type()));
  view.advertiser_address().CopyFrom(entry.address.value().view());
  view.advertising_sid().Write(entry.advertising_sid);
  transport_->command_channel()
      ->SendCommand(
          std::move(add_cmd),
          [self, entry](auto, const EventPacket& event) {
            if (!self.is_alive()) {
              return;
            }

            Result<> result = event.ToResult();
            if (result.is_error()) {
              if (result.error_value().is(
                      StatusCode::MEMORY_CAPACITY_EXCEEDED)) {
                if (self->advertiser_list_.empty()) {
                  bt_log(WARN,
                         "hci",
                         "periodic advertiser list is full when empty");
                  self->state_ = State::kIdle;
                  self->FailAllRequests(Error(HostError::kFailed));
                  return;
                }

                bt_log(INFO, "hci", "periodic advertiser list is full");

                self->state_ = State::kIdle;
                self->MaybeUpdateAdvertiserList(
                    /*advertiser_list_full=*/true);
                return;
              }

              bt_log(
                  WARN,
                  "hci",
                  "Add Device to Periodic Advertiser List command failed: %s",
                  bt_str(result.error_value()));
              self->state_ = State::kBadState;
              self->FailAllRequests(Error(HostError::kFailed));
              return;
            }

            self->advertiser_list_.emplace(entry);
            self->state_ = State::kIdle;
            self->MaybeUpdateAdvertiserList();
          })
      .IgnoreError();
}

void PeriodicAdvertisingSynchronizer::SendRemoveDeviceFromListCommand(
    const AdvertiserListEntry& entry) {
  bt_log(DEBUG,
         "hci",
         "removing device from periodic advertiser list: %s",
         bt_str(entry.address));

  PW_CHECK(state_ == State::kIdle);
  state_ = State::kRemoveDevicePending;

  auto self = weak_self_.GetWeakPtr();
  auto remove_cmd = hci::CommandPacket::New<
      pw::bluetooth::emboss::
          LERemoveDeviceFromPeriodicAdvertiserListCommandWriter>(
      pw::bluetooth::emboss::OpCode::
          LE_REMOVE_DEVICE_FROM_PERIODIC_ADVERTISER_LIST);
  auto view = remove_cmd.view_t();
  view.advertiser_address_type().Write(
      DeviceAddress::DeviceAddrToLePeerAddrNoAnon(entry.address.type()));
  view.advertiser_address().CopyFrom(entry.address.value().view());
  view.advertising_sid().Write(entry.advertising_sid);

  transport_->command_channel()
      ->SendCommand(std::move(remove_cmd),
                    [self, entry](auto, const EventPacket& event) {
                      if (!self.is_alive()) {
                        return;
                      }

                      Result<> result = event.ToResult();
                      if (result.is_error()) {
                        bt_log(WARN,
                               "hci",
                               "Remove Device from Periodic Advertiser List "
                               "command failed: %s",
                               bt_str(result.error_value()));
                        self->state_ = State::kBadState;
                        self->FailAllRequests(Error(HostError::kFailed));
                        return;
                      }

                      self->advertiser_list_.erase(entry);
                      self->state_ = State::kIdle;
                      self->MaybeUpdateAdvertiserList();
                    })
      .IgnoreError();
}

void PeriodicAdvertisingSynchronizer::OnSyncEstablished(
    const EventPacket& event) {
  PW_CHECK(state_ == State::kCreateSyncPending ||
           state_ == State::kCreateSyncCancelPending);
  state_ = State::kIdle;

  std::optional<ParsedSyncEstablishedSubevent> parsed_event =
      ParseSyncEstablishedSubevent(event);
  PW_CHECK(parsed_event);

  bt_log(DEBUG,
         "hci",
         "Sync Established event received (status: %s)",
         hci_spec::StatusCodeToString(parsed_event->status).c_str());

  if (parsed_event->status == StatusCode::OPERATION_CANCELLED_BY_HOST) {
    MaybeUpdateAdvertiserList();
    return;
  }

  if (parsed_event->status != StatusCode::SUCCESS) {
    bt_log(WARN,
           "hci",
           "Sync Established event error: %s",
           hci_spec::StatusCodeToString(parsed_event->status).c_str());
    FailRequestsWithEntriesInAdvertiserList(
        ToResult(parsed_event->status).error_value());
    MaybeUpdateAdvertiserList();
    return;
  }

  auto req_iter = pending_requests_.find(AdvertiserListEntry{
      parsed_event->address, parsed_event->advertising_sid});

  // This can happen if the request is canceled right before the event is
  // received.
  if (req_iter == pending_requests_.end()) {
    bt_log(WARN,
           "hci",
           "unexpected sync established event, terminating sync (handle: %d)",
           parsed_event->sync_handle);
    auto command = CommandPacket::New<
        pw::bluetooth::emboss::LEPeriodicAdvertisingTerminateSyncCommandWriter>(
        pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC);
    command.view_t().sync_handle().Write(parsed_event->sync_handle);
    transport_->command_channel()
        ->SendCommand(std::move(command),
                      [self = weak_self_.GetWeakPtr()](
                          auto, const EventPacket& complete_event) {
                        Result<> result = complete_event.ToResult();
                        if (result.is_error()) {
                          bt_log(WARN,
                                 "hci",
                                 "failed to terminate unexpected sync: %s",
                                 bt_str(result.error_value()));
                          return;
                        }
                      })
        .IgnoreError();
    MaybeUpdateAdvertiserList();
    return;
  }

  auto pending_req_node = pending_requests_.extract(req_iter);
  PendingRequest& pending_req = pending_req_node.mapped();

  MaybeUpdateAdvertiserList();

  if (parsed_event->status != StatusCode::SUCCESS) {
    pending_req.delegate->OnSyncLost(
        pending_req.id, ToResult(parsed_event->status).error_value());
    return;
  }

  SyncParameters params;
  params.address = parsed_event->address;
  params.advertising_sid = parsed_event->advertising_sid;
  params.interval = parsed_event->interval;
  params.phy = parsed_event->phy;
  params.subevents_count = parsed_event->subevents_count;

  syncs_[parsed_event->sync_handle] = {pending_req.id,
                                       params.address,
                                       params.advertising_sid,
                                       pending_req.delegate,
                                       {}};
  pending_req.delegate->OnSyncEstablished(pending_req.id, params);
}

void PeriodicAdvertisingSynchronizer::OnSyncLost(const EventPacket& event) {
  auto view = event.view<
      pw::bluetooth::emboss::LEPeriodicAdvertisingSyncLostSubeventView>();
  auto sync_handle = view.sync_handle().Read();

  auto sync_node = syncs_.extract(sync_handle);
  if (sync_node.empty()) {
    bt_log(WARN, "hci", "sync lost for unknown handle: %d", sync_handle);
    return;
  }

  EstablishedSync& sync = sync_node.mapped();
  sync.delegate->OnSyncLost(
      sync.id, ToResult(StatusCode::CONNECTION_TIMEOUT).error_value());
}

void PeriodicAdvertisingSynchronizer::OnPeriodicAdvertisingReport(
    const EventPacket& event) {
  auto parsed_event = ParseAdvertisingReportSubevent(event);
  PW_CHECK(parsed_event);

  auto sync_iter = syncs_.find(parsed_event->sync_handle);
  if (sync_iter == syncs_.end()) {
    bt_log(WARN,
           "hci",
           "advertising report for unknown handle: %d",
           parsed_event->sync_handle);
    return;
  }
  EstablishedSync& sync = sync_iter->second;

  if (parsed_event->data_status ==
      pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::
          INCOMPLETE_TRUNCATED) {
    bt_log(WARN,
           "hci",
           "truncated advertising report for handle: %d",
           parsed_event->sync_handle);
    sync.partial_report_buffer.clear();
    return;
  }

  sync.partial_report_buffer.insert(sync.partial_report_buffer.end(),
                                    parsed_event->data.begin(),
                                    parsed_event->data.end());

  if (parsed_event->data_status ==
      pw::bluetooth::emboss::LEPeriodicAdvertisingDataStatus::INCOMPLETE) {
    return;
  }

  PeriodicAdvertisingReport report;
  report.rssi = parsed_event->rssi;
  report.event_counter = parsed_event->event_counter;
  report.data = DynamicByteBuffer(BufferView(
      sync.partial_report_buffer.data(), sync.partial_report_buffer.size()));
  sync.partial_report_buffer.clear();

  sync.delegate->OnAdvertisingReport(sync.id, std::move(report));
}

void PeriodicAdvertisingSynchronizer::OnBigInfoReport(
    const EventPacket& event) {
  auto view = event.view<
      pw::bluetooth::emboss::LEBigInfoAdvertisingReportSubeventView>();
  auto sync_handle = view.sync_handle().Read();

  auto sync_iter = syncs_.find(sync_handle);
  if (sync_iter == syncs_.end()) {
    bt_log(WARN, "hci", "biginfo report for unknown handle: %d", sync_handle);
    return;
  }
  EstablishedSync& sync = sync_iter->second;

  BroadcastIsochronousGroupInfo report;
  report.num_bis = view.num_bis().Read();
  report.nse = view.nse().Read();
  report.iso_interval = view.iso_interval().Read();
  report.bn = view.bn().Read();
  report.pto = view.pto().Read();
  report.irc = view.irc().Read();
  report.max_pdu = view.max_pdu().Read();
  report.sdu_interval = view.sdu_interval().Read();
  report.max_sdu = view.max_sdu().Read();
  report.phy = view.phy().Read();
  report.framing = view.framing().Read();
  report.encryption = view.encryption().Read();

  sync.delegate->OnBigInfoReport(sync.id, std::move(report));
}

void PeriodicAdvertisingSynchronizer::CancelSync(SyncId sync_id) {
  // Check pending CreateSync requests. This returns true if a pending request
  // was found and canceled.
  if (CancelPendingCreateSync(sync_id)) {
    return;
  }

  // Check established syncs. This returns true if an established sync was
  // found and a termination command was sent.
  if (CancelEstablishedSync(sync_id)) {
    return;
  }

  bt_log(DEBUG,
         "hci",
         "CancelSync called with invalid sync_id: %s",
         bt_str(sync_id));
}

bool PeriodicAdvertisingSynchronizer::CancelPendingCreateSync(SyncId sync_id) {
  auto iter =
      std::find_if(pending_requests_.begin(),
                   pending_requests_.end(),
                   [&sync_id](auto& req) { return req.second.id == sync_id; });
  if (iter == pending_requests_.end()) {
    return false;
  }
  auto node = pending_requests_.extract(iter);
  if (node.empty()) {
    return false;
  }

  PendingRequest& req = node.mapped();
  req.delegate->OnSyncLost(sync_id,
                           ToResult(HostError::kCanceled).error_value());

  MaybeUpdateAdvertiserList();
  return true;
}

bool PeriodicAdvertisingSynchronizer::CancelEstablishedSync(SyncId sync_id) {
  auto iter =
      std::find_if(syncs_.begin(), syncs_.end(), [&sync_id](auto& sync) {
        return sync.second.id == sync_id;
      });
  if (iter == syncs_.end()) {
    return false;
  }

  auto node = syncs_.extract(iter);
  hci_spec::SyncHandle sync_handle = node.key();
  EstablishedSync& sync = node.mapped();
  sync.delegate->OnSyncLost(sync.id, Error(HostError::kCanceled));

  auto command = CommandPacket::New<
      pw::bluetooth::emboss::LEPeriodicAdvertisingTerminateSyncCommandWriter>(
      pw::bluetooth::emboss::OpCode::LE_PERIODIC_ADVERTISING_TERMINATE_SYNC);
  command.view_t().sync_handle().Write(sync_handle);

  auto self = weak_self_.GetWeakPtr();
  auto cb = [self, sync_id](auto, const EventPacket& event) {
    if (!self.is_alive()) {
      return;
    }

    Result<> result = event.ToResult();
    if (result.is_error()) {
      // This could happen if the Sync Lost event raced with the Terminate
      // Sync command.
      bt_log(WARN,
             "hci",
             "failed to terminate periodic advertising sync %s: %s",
             bt_str(sync_id),
             bt_str(result.error_value()));
    }
  };

  transport_->command_channel()
      ->SendCommand(std::move(command), std::move(cb))
      .IgnoreError();
  return true;
}

void PeriodicAdvertisingSynchronizer::FailAllRequests(Error error) {
  // Extract requests before notifying to prevent reentrancy bugs.
  auto requests = std::move(pending_requests_);
  pending_requests_.clear();
  for (auto& [_, req] : requests) {
    req.delegate->OnSyncLost(req.id, error);
  }
}

void PeriodicAdvertisingSynchronizer::FailRequestsWithEntriesInAdvertiserList(
    Error error) {
  // Extract requests before notifying to prevent reentrancy bugs.
  std::vector<decltype(pending_requests_)::node_type> requests_to_fail;
  for (auto iter = pending_requests_.begin();
       iter != pending_requests_.end();) {
    if (advertiser_list_.count(iter->first)) {
      auto node = pending_requests_.extract(iter++);
      requests_to_fail.emplace_back(std::move(node));
      continue;
    }
    ++iter;
  }
  for (auto& req : requests_to_fail) {
    req.mapped().delegate->OnSyncLost(req.mapped().id, error);
  }
}

}  // namespace bt::hci
