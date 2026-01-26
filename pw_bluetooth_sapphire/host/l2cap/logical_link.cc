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

#include "pw_bluetooth_sapphire/internal/host/l2cap/logical_link.h"

#include <cpp-string/string_printf.h>
#include <lib/fit/function.h>
#include <pw_assert/check.h>

#include <cinttypes>
#include <functional>
#include <memory>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/trace.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/bredr_dynamic_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/bredr_signaling_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/le_dynamic_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/le_signaling_channel.h"
#include "pw_bluetooth_sapphire/internal/host/transport/link_type.h"
#include "pw_bluetooth_sapphire/internal/host/transport/transport.h"

namespace bt::l2cap::internal {

using pw::bluetooth::AclPriority;

namespace {

const char* kInspectHandlePropertyName = "handle";
const char* kInspectLinkTypePropertyName = "link_type";
const char* kInspectChannelsNodeName = "channels";
const char* kInspectAutosniffNodeName = "autosniff";
const char* kInspectChannelNodePrefix = "channel_";
const char* kInspectFlushTimeoutPropertyName = "flush_timeout_ms";

constexpr bool IsValidLEFixedChannel(ChannelId id) {
  switch (id) {
    case kATTChannelId:
    case kLESignalingChannelId:
    case kLESMPChannelId:
      return true;
    default:
      break;
  }
  return false;
}

constexpr bool IsValidBREDRFixedChannel(ChannelId id) {
  switch (id) {
    case kSignalingChannelId:
    case kConnectionlessChannelId:
    case kSMPChannelId:
      return true;
    default:
      break;
  }
  return false;
}

// Constants taken verbatim from CHRE (non-sco)
// TODO(fxbug.dev/421454138): Possibly make these parameters 1. configurable
// or 2. dynamically chosen per connection
constexpr SniffModeParams kDefaultSniffParams =
    SniffModeParams{.min_interval = 400,
                    .max_interval = 816,
                    .sniff_attempt = 4,
                    .sniff_timeout = 1};

FixedChannelsSupported FixedChannelsSupportedBitForChannelId(
    ChannelId channel_id) {
  switch (channel_id) {
    case kSignalingChannelId:
      return kFixedChannelsSupportedBitSignaling;
    case kConnectionlessChannelId:
      return kFixedChannelsSupportedBitConnectionless;
    case kSMPChannelId:
      return kFixedChannelsSupportedBitSM;
    default:
      return 0;
  }
}

}  // namespace

LogicalLink::LogicalLink(
    hci_spec::ConnectionHandle handle,
    bt::LinkType type,
    pw::bluetooth::emboss::ConnectionRole role,
    uint16_t max_acl_payload_size,
    QueryServiceCallback query_service_cb,
    hci::AclDataChannel* acl_data_channel,
    hci::CommandChannel* cmd_channel,
    bool random_channel_ids,
    A2dpOffloadManager& a2dp_offload_manager,
    pw::async::Dispatcher& dispatcher,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider)
    : wake_lease_provider_(wake_lease_provider),
      pw_dispatcher_(dispatcher),
      handle_(handle),
      type_(type),
      role_(role),
      max_acl_payload_size_(max_acl_payload_size),
      flush_timeout_(
          pw::chrono::SystemClock::duration::max(), /*convert=*/
          [](pw::chrono::SystemClock::duration f) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(f)
                .count();
          }),
      closed_(false),
      recombiner_(handle, wake_lease_provider),
      acl_data_channel_(acl_data_channel),
      cmd_channel_(cmd_channel),
      query_service_cb_(std::move(query_service_cb)),
      a2dp_offload_manager_(a2dp_offload_manager),
      weak_conn_interface_(this),
      weak_self_(this) {
  PW_CHECK(type_ == bt::LinkType::kLE || type_ == bt::LinkType::kACL);
  PW_CHECK(acl_data_channel_);
  PW_CHECK(cmd_channel_);
  PW_CHECK(query_service_cb_);

  // Allow packets to be sent on this link immediately.
  acl_data_channel_->RegisterConnection(weak_conn_interface_.GetWeakPtr());

  if (type == bt::LinkType::kACL) {
    autosniff_.emplace(kDefaultSniffParams,
                       cmd_channel,
                       handle,
                       &dispatcher,
                       kAutosniffTimeout);
  }

  // Set up the signaling channel and dynamic channels.
  if (type_ == bt::LinkType::kLE) {
    signaling_channel_ = std::make_unique<LESignalingChannel>(
        OpenFixedChannel(kLESignalingChannelId),
        role_,
        pw_dispatcher_,
        wake_lease_provider_);
    dynamic_registry_ = std::make_unique<LeDynamicChannelRegistry>(
        signaling_channel_.get(),
        fit::bind_member<&LogicalLink::OnChannelDisconnectRequest>(this),
        fit::bind_member<&LogicalLink::OnServiceRequest>(this),
        random_channel_ids);

    ServeConnectionParameterUpdateRequest();
    ServeFlowControlCreditInd();
  } else {
    signaling_channel_ = std::make_unique<BrEdrSignalingChannel>(
        OpenFixedChannel(kSignalingChannelId),
        role_,
        pw_dispatcher_,
        wake_lease_provider_);
    dynamic_registry_ = std::make_unique<BrEdrDynamicChannelRegistry>(
        signaling_channel_.get(),
        fit::bind_member<&LogicalLink::OnChannelDisconnectRequest>(this),
        fit::bind_member<&LogicalLink::OnServiceRequest>(this),
        random_channel_ids);

    SendFixedChannelsSupportedInformationRequest();
  }
  a2dp_offload_manager_.SetSniffSuppress(
      fit::bind_member<&LogicalLink::SuppressAutosniff>(this));
}

LogicalLink::~LogicalLink() {
  bt_log(DEBUG, "l2cap", "LogicalLink destroyed (handle: %#.4x)", handle_);
  PW_CHECK(closed_);
}

Channel::WeakPtr LogicalLink::OpenFixedChannel(ChannelId id) {
  PW_DCHECK(!closed_);

  TRACE_DURATION("bluetooth",
                 "LogicalLink::OpenFixedChannel",
                 "handle",
                 handle_,
                 "channel id",
                 id);

  // We currently only support the pre-defined fixed-channels.
  if (!AllowsFixedChannel(id)) {
    bt_log(ERROR, "l2cap", "cannot open fixed channel with id %#.4x", id);
    return Channel::WeakPtr();
  }

  auto iter = channels_.find(id);
  if (iter != channels_.end()) {
    bt_log(ERROR,
           "l2cap",
           "channel is already open! (id: %#.4x, handle: %#.4x)",
           id,
           handle_);
    return Channel::WeakPtr();
  }

  std::unique_ptr<ChannelImpl> chan =
      ChannelImpl::CreateFixedChannel(pw_dispatcher_,
                                      id,
                                      GetWeakPtr(),
                                      cmd_channel_->AsWeakPtr(),
                                      max_acl_payload_size_,
                                      a2dp_offload_manager_,
                                      wake_lease_provider_);

  auto pp_iter = pending_pdus_.find(id);
  if (pp_iter != pending_pdus_.end()) {
    for (auto& pdu : pp_iter->second) {
      TRACE_FLOW_END(
          "bluetooth", "LogicalLink::HandleRxPacket queued", pdu.trace_id());
      chan->HandleRxPdu(std::move(pdu));
    }
    pending_pdus_.erase(pp_iter);
  }

  if (inspect_properties_.channels_node) {
    chan->AttachInspect(inspect_properties_.channels_node,
                        inspect_properties_.channels_node.UniqueName(
                            kInspectChannelNodePrefix));
  }

  channels_[id] = std::move(chan);

  // Reset round robin iterator
  current_channel_ = channels_.begin();

  return channels_[id]->GetWeakPtr();
}

void LogicalLink::OpenFixedChannelAsync(ChannelId channel_id,
                                        ChannelCallback callback) {
  if (type_ == LinkType::kACL) {
    if (fixed_channels_supported_callbacks_.count(channel_id) > 0) {
      bt_log(ERROR,
             "l2cap",
             "fixed channel is already pending! (id: %#.4x, handle: %#.4x)",
             channel_id,
             handle_);
      callback(Channel::WeakPtr());
      return;
    }

    if (!fixed_channels_supported_.has_value()) {
      fixed_channels_supported_callbacks_.emplace(
          channel_id, [this, channel_id, cb = std::move(callback)]() mutable {
            OpenFixedChannelAsync(channel_id, std::move(cb));
          });
      return;
    }

    bool channel_supported = *fixed_channels_supported_ &
                             FixedChannelsSupportedBitForChannelId(channel_id);

    if (!channel_supported) {
      bt_log(DEBUG,
             "l2cap",
             "tried to open fixed channel not supported by peer (id: %#.4x, "
             "handle: %#.4x)",
             channel_id,
             handle_);
      callback(Channel::WeakPtr());
      return;
    }
  }

  callback(OpenFixedChannel(channel_id));
}

void LogicalLink::OpenChannel(Psm psm,
                              ChannelParameters params,
                              ChannelCallback callback) {
  PW_DCHECK(!closed_);
  PW_DCHECK(dynamic_registry_);

  auto create_channel =
      [this, cb = std::move(callback)](const DynamicChannel* dyn_chan) mutable {
        CompleteDynamicOpen(dyn_chan, std::move(cb));
      };
  dynamic_registry_->OpenOutbound(psm, params, std::move(create_channel));

  // Reset round robin iterator
  current_channel_ = channels_.begin();
}

void LogicalLink::HandleRxPacket(hci::ACLDataPacketPtr packet) {
  PW_DCHECK(packet);
  PW_DCHECK(!closed_);

  TRACE_DURATION("bluetooth", "LogicalLink::HandleRxPacket", "handle", handle_);

  // We do not support the Connectionless data channel, and the active broadcast
  // flag can only be used on the connectionless channel.  Drop packets that are
  // broadcast.
  if (packet->broadcast_flag() ==
      hci_spec::ACLBroadcastFlag::kActivePeripheralBroadcast) {
    bt_log(DEBUG, "l2cap", "Unsupported Broadcast Frame dropped");
    return;
  }

  if (autosniff_.has_value()) {
    autosniff_->MarkPacketRx();
  }

  auto result = recombiner_.ConsumeFragment(std::move(packet));
  if (result.frames_dropped) {
    bt_log(TRACE, "l2cap", "Frame(s) dropped due to recombination error");
  }

  if (!result.pdu) {
    // Either a partial fragment was received, which was buffered for
    // recombination, or the packet was dropped.
    return;
  }

  PW_DCHECK(result.pdu->is_valid());

  uint16_t channel_id = result.pdu->channel_id();
  auto iter = channels_.find(channel_id);
  PendingPduMap::iterator pp_iter;

  if (iter == channels_.end()) {
    // Only buffer data for fixed channels. This prevents stale data that is
    // intended for a closed dynamic channel from being delivered to a new
    // channel that recycled the former's ID. The downside is that it's possible
    // to lose any data that is received after a dynamic channel's connection
    // request and before its completed configuration. This would require tricky
    // additional state to track "pending open" channels here and it's not clear
    // if that is necessary since hosts should not send data before a channel is
    // first configured.
    if (!AllowsFixedChannel(channel_id)) {
      bt_log(WARN,
             "l2cap",
             "Dropping PDU for nonexistent dynamic channel %#.4x on link %#.4x",
             channel_id,
             handle_);
      return;
    }

    // The packet was received on a channel for which no ChannelImpl currently
    // exists. Buffer packets for the channel to receive when it gets created.
    pp_iter = pending_pdus_.emplace(channel_id, std::list<PDU>()).first;
  } else {
    // A channel exists. |pp_iter| will be valid only if the drain task has not
    // run yet (see LogicalLink::OpenFixedChannel()).
    pp_iter = pending_pdus_.find(channel_id);
  }

  if (pp_iter != pending_pdus_.end()) {
    result.pdu->set_trace_id(TRACE_NONCE());
    TRACE_FLOW_BEGIN("bluetooth",
                     "LogicalLink::HandleRxPacket queued",
                     result.pdu->trace_id());

    pp_iter->second.emplace_back(std::move(*result.pdu));

    bt_log(TRACE,
           "l2cap",
           "PDU buffered (channel: %#.4x, ll: %#.4x)",
           channel_id,
           handle_);
    return;
  }

  iter->second->HandleRxPdu(std::move(*result.pdu));
}

void LogicalLink::UpgradeSecurity(sm::SecurityLevel level,
                                  sm::ResultFunction<> callback) {
  PW_DCHECK(security_callback_);

  if (closed_) {
    bt_log(DEBUG, "l2cap", "Ignoring security request on closed link");
    return;
  }

  // Report success If the link already has the expected security level.
  if (level <= security().level()) {
    callback(fit::ok());
    return;
  }

  bt_log(DEBUG,
         "l2cap",
         "Security upgrade requested (level = %s)",
         sm::LevelToString(level));
  security_callback_(handle_, level, std::move(callback));
}

void LogicalLink::AssignSecurityProperties(
    const sm::SecurityProperties& security) {
  if (closed_) {
    bt_log(DEBUG, "l2cap", "Ignoring security request on closed link");
    return;
  }

  bt_log(DEBUG,
         "l2cap",
         "Link security updated (handle: %#.4x): %s",
         handle_,
         security.ToString().c_str());

  security_ = security;
}

bool LogicalLink::HasAvailablePacket() const {
  for (auto& [_, channel] : channels_) {
    // TODO(fxbug.dev/42074553): Check HasSDUs() after transmission engines are
    // refactored
    if (channel->HasPDUs() || channel->HasFragments()) {
      return true;
    }
  }
  return false;
}

void LogicalLink::RoundRobinChannels() {
  // Go through all channels in map
  if (next(current_channel_) == channels_.end()) {
    current_channel_ = channels_.begin();
  } else {
    current_channel_++;
  }
}

bool LogicalLink::IsNextPacketContinuingFragment() const {
  return current_pdus_channel_.is_alive() &&
         current_pdus_channel_->HasFragments();
}

std::unique_ptr<hci::ACLDataPacket> LogicalLink::GetNextOutboundPacket() {
  for (size_t i = 0; i < channels_.size(); i++) {
    if (!IsNextPacketContinuingFragment()) {
      current_pdus_channel_ = ChannelImpl::WeakPtr();

      // Go to next channel to try and get next packet to send
      RoundRobinChannels();

      if (current_channel_->second->HasPDUs()) {
        current_pdus_channel_ = current_channel_->second->GetWeakPtr();
      }
    }

    if (current_pdus_channel_.is_alive()) {
      // Next packet will either be a starting or continuing fragment
      auto pdu = current_pdus_channel_->GetNextOutboundPacket();
      if (pdu) {
        if (autosniff_.has_value()) {
          autosniff_->MarkPacketTx();
        }
      }
      return pdu;
    }
  }
  // All channels are empty
  // This should never actually return a nullptr since we only call
  // LogicalLink::GetNextOutboundPacket() when LogicalLink::HasAvailablePacket()
  // is true
  return nullptr;
}

void LogicalLink::OnOutboundPacketAvailable() {
  acl_data_channel_->OnOutboundPacketAvailable();
}

void LogicalLink::set_error_callback(fit::closure callback) {
  link_error_cb_ = std::move(callback);
}

void LogicalLink::set_security_upgrade_callback(
    SecurityUpgradeCallback callback) {
  security_callback_ = std::move(callback);
}

void LogicalLink::set_connection_parameter_update_callback(
    LEConnectionParameterUpdateCallback callback) {
  connection_parameter_update_callback_ = std::move(callback);
}

bool LogicalLink::AllowsFixedChannel(ChannelId id) {
  return (type_ == bt::LinkType::kLE) ? IsValidLEFixedChannel(id)
                                      : IsValidBREDRFixedChannel(id);
}

void LogicalLink::RemoveChannel(Channel* chan, fit::closure removed_cb) {
  PW_DCHECK(chan);

  if (closed_) {
    bt_log(DEBUG, "l2cap", "Ignore RemoveChannel() on closed link");
    removed_cb();
    return;
  }

  const ChannelId id = chan->id();
  auto iter = channels_.find(id);
  if (iter == channels_.end()) {
    removed_cb();
    return;
  }

  // Ignore if the found channel doesn't match the requested one (even though
  // their IDs are the same).
  if (iter->second.get() != chan) {
    removed_cb();
    return;
  }

  pending_pdus_.erase(id);
  channels_.erase(iter);

  // Reset round robin iterator
  current_channel_ = channels_.begin();

  // Disconnect the channel if it's a dynamic channel. This path is for local-
  // initiated closures and does not invoke callbacks back to the channel user.
  // TODO(armansito): Change this if statement into an assert when a registry
  // gets created for LE channels.
  if (dynamic_registry_) {
    dynamic_registry_->CloseChannel(id, std::move(removed_cb));
    return;
  }

  removed_cb();
}

void LogicalLink::SignalError() {
  if (closed_) {
    bt_log(DEBUG, "l2cap", "Ignore SignalError() on closed link");
    return;
  }

  bt_log(INFO,
         "l2cap",
         "Upper layer error on link %#.4x; closing all channels",
         handle());

  size_t num_channels_to_close = channels_.size();

  if (signaling_channel_) {
    PW_CHECK(channels_.count(kSignalingChannelId) ||
             channels_.count(kLESignalingChannelId));
    // There is no need to close the signaling channel.
    num_channels_to_close--;
  }

  if (num_channels_to_close == 0) {
    link_error_cb_();
    return;
  }

  // num_channels_closing is shared across all callbacks.
  fit::closure channel_removed_cb =
      [this, channels_remaining = num_channels_to_close]() mutable {
        channels_remaining--;
        if (channels_remaining != 0) {
          return;
        }
        bt_log(TRACE,
               "l2cap",
               "Channels on link %#.4x closed; passing error to lower layer",
               handle());
        // Invoking error callback may destroy this LogicalLink.
        link_error_cb_();
      };

  for (auto channel_iter = channels_.begin();
       channel_iter != channels_.end();) {
    auto& [id, channel] = *channel_iter++;

    // Do not close the signaling channel, as it is used to close the dynamic
    // channels.
    if (id == kSignalingChannelId || id == kLESignalingChannelId) {
      continue;
    }

    // Signal the channel, as it did not request the closure.
    channel->OnClosed();

    // This erases from |channel_| and invalidates any iterator pointing to
    // |channel|.
    RemoveChannel(channel.get(), channel_removed_cb.share());
  }
}

void LogicalLink::SignalCreditsAvailable(ChannelId channel, uint16_t credits) {
  if (closed_) {
    return;
  }

  CommandHandler(signaling_channel_.get()).SendCredits(channel, credits);
}

void LogicalLink::Close() {
  PW_DCHECK(!closed_);

  closed_ = true;

  acl_data_channel_->UnregisterConnection(handle_);

  for (auto& iter : channels_) {
    iter.second->OnClosed();
  }
  channels_.clear();
  dynamic_registry_.reset();
}

std::optional<DynamicChannelRegistry::ServiceInfo>
LogicalLink::OnServiceRequest(Psm psm) {
  PW_DCHECK(!closed_);

  // Query upper layer for a service handler attached to this PSM.
  auto result = query_service_cb_(handle_, psm);
  if (!result) {
    return std::nullopt;
  }

  auto channel_cb = [this, chan_cb = std::move(result->channel_cb)](
                        const DynamicChannel* dyn_chan) mutable {
    CompleteDynamicOpen(dyn_chan, std::move(chan_cb));
  };
  return DynamicChannelRegistry::ServiceInfo(result->channel_params,
                                             std::move(channel_cb));
}

void LogicalLink::OnChannelDisconnectRequest(const DynamicChannel* dyn_chan) {
  PW_DCHECK(dyn_chan);
  PW_DCHECK(!closed_);

  auto iter = channels_.find(dyn_chan->local_cid());
  if (iter == channels_.end()) {
    bt_log(WARN,
           "l2cap",
           "No ChannelImpl found for closing dynamic channel %#.4x",
           dyn_chan->local_cid());
    return;
  }

  ChannelImpl* channel = iter->second.get();
  PW_DCHECK(channel->remote_id() == dyn_chan->remote_cid());

  // Signal closure because this is a remote disconnection.
  channel->OnClosed();
  channels_.erase(iter);

  // Reset round robin iterator
  current_channel_ = channels_.begin();
}

void LogicalLink::CompleteDynamicOpen(const DynamicChannel* dyn_chan,
                                      ChannelCallback open_cb) {
  PW_DCHECK(!closed_);

  if (!dyn_chan) {
    open_cb(Channel::WeakPtr());
    return;
  }

  const ChannelId local_cid = dyn_chan->local_cid();
  const ChannelId remote_cid = dyn_chan->remote_cid();
  bt_log(DEBUG,
         "l2cap",
         "Link %#.4x: Channel opened with ID %#.4x (remote ID: %#.4x, psm: %s, "
         "mode: %s)",
         handle_,
         local_cid,
         remote_cid,
         PsmToString(dyn_chan->psm()).c_str(),
         AnyChannelModeToString(dyn_chan->info().mode).c_str());

  auto chan_info = dyn_chan->info();
  // Extract preferred flush timeout to avoid creating channel with a flush
  // timeout that hasn't been successfully configured yet.
  auto preferred_flush_timeout = chan_info.flush_timeout;
  chan_info.flush_timeout.reset();

  std::unique_ptr<ChannelImpl> chan =
      ChannelImpl::CreateDynamicChannel(pw_dispatcher_,
                                        local_cid,
                                        remote_cid,
                                        GetWeakPtr(),
                                        chan_info,
                                        cmd_channel_->AsWeakPtr(),
                                        max_acl_payload_size_,
                                        a2dp_offload_manager_,
                                        wake_lease_provider_);
  auto chan_weak = chan->GetWeakPtr();
  channels_[local_cid] = std::move(chan);

  if (inspect_properties_.channels_node) {
    chan_weak->AttachInspect(inspect_properties_.channels_node,
                             inspect_properties_.channels_node.UniqueName(
                                 kInspectChannelNodePrefix));
  }

  // If a flush timeout was requested for this channel, try to set it before
  // returning the channel to the client to ensure outbound PDUs have correct
  // flushable flag.
  if (preferred_flush_timeout.has_value()) {
    chan_weak->SetBrEdrAutomaticFlushTimeout(
        preferred_flush_timeout.value(),
        [cb = std::move(open_cb), chan_weak](auto /*result*/) {
          cb(chan_weak);
        });
    return;
  }

  open_cb(std::move(chan_weak));
}

void LogicalLink::SendFixedChannelsSupportedInformationRequest() {
  PW_CHECK(signaling_channel_);

  BrEdrCommandHandler cmd_handler(signaling_channel_.get());
  if (!cmd_handler.SendInformationRequest(
          InformationType::kFixedChannelsSupported,
          [self = GetWeakPtr()](auto& rsp) {
            if (self.is_alive()) {
              self->OnRxFixedChannelsSupportedInfoRsp(rsp);
            }
          })) {
    bt_log(ERROR,
           "l2cap",
           "Failed to send Fixed Channels Supported Information Request");
    return;
  }

  bt_log(TRACE, "l2cap", "Sent Fixed Channels Supported Information Request");
}

void LogicalLink::OnRxFixedChannelsSupportedInfoRsp(
    const BrEdrCommandHandler::InformationResponse& rsp) {
  if (rsp.status() == BrEdrCommandHandler::Status::kReject) {
    bt_log(
        TRACE,
        "l2cap",
        "Fixed Channels Supported Information Request rejected (reason %#.4hx)",
        static_cast<unsigned short>(rsp.reject_reason()));
    return;
  }

  if (rsp.result() == InformationResult::kNotSupported) {
    bt_log(TRACE,
           "l2cap",
           "Received Fixed Channels Supported Information Response (result: "
           "Not Supported)");
    return;
  }

  if (rsp.result() != InformationResult::kSuccess) {
    bt_log(TRACE,
           "l2cap",
           "Received Fixed Channels Supported Information Response (result: "
           "%.4hx)",
           static_cast<uint16_t>(rsp.result()));
    return;
  }

  if (rsp.type() != InformationType::kFixedChannelsSupported) {
    bt_log(TRACE,
           "l2cap",
           "Incorrect Fixed Channels Supported Information Response type "
           "(type: %#.4hx)",
           static_cast<unsigned short>(rsp.type()));
    return;
  }

  bt_log(TRACE,
         "l2cap",
         "Received Fixed Channels Supported Information Response (mask: "
         "%#016" PRIx64 ")",
         rsp.fixed_channels());

  fixed_channels_supported_ = rsp.fixed_channels();
  auto callbacks = std::move(fixed_channels_supported_callbacks_);
  fixed_channels_supported_callbacks_.clear();
  for (auto& [_, cb] : callbacks) {
    cb();
  }
}

void LogicalLink::SendConnectionParameterUpdateRequest(
    hci_spec::LEPreferredConnectionParameters params,
    ConnectionParameterUpdateRequestCallback request_cb) {
  PW_CHECK(signaling_channel_);
  PW_CHECK(type_ == bt::LinkType::kLE);
  PW_CHECK(role_ == pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  LowEnergyCommandHandler cmd_handler(signaling_channel_.get());
  cmd_handler.SendConnectionParameterUpdateRequest(
      params.min_interval(),
      params.max_interval(),
      params.max_latency(),
      params.supervision_timeout(),
      [cb = std::move(request_cb)](
          const LowEnergyCommandHandler::ConnectionParameterUpdateResponse&
              rsp) mutable {
        bool accepted = false;

        if (rsp.status() != LowEnergyCommandHandler::Status::kSuccess) {
          bt_log(TRACE,
                 "l2cap",
                 "LE Connection Parameter Update Request rejected (reason: "
                 "%#.4hx)",
                 static_cast<unsigned short>(rsp.reject_reason()));
        } else {
          accepted = rsp.result() == ConnectionParameterUpdateResult::kAccepted;
        }
        cb(accepted);
      });
}

void LogicalLink::RequestAclPriority(
    Channel::WeakPtr channel,
    AclPriority priority,
    fit::callback<void(fit::result<fit::failed>)> callback) {
  PW_CHECK(channel.is_alive());
  auto iter = channels_.find(channel->id());
  PW_CHECK(iter != channels_.end());
  pending_acl_requests_.push(
      PendingAclRequest{std::move(channel), priority, std::move(callback)});
  if (pending_acl_requests_.size() == 1) {
    HandleNextAclPriorityRequest();
  }
}

void LogicalLink::SetBrEdrAutomaticFlushTimeout(
    pw::chrono::SystemClock::duration flush_timeout,
    hci::ResultCallback<> callback) {
  if (type_ != bt::LinkType::kACL) {
    bt_log(
        ERROR, "l2cap", "attempt to set flush timeout on non-ACL logical link");
    callback(ToResult(
        pw::bluetooth::emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS));
    return;
  }

  auto callback_wrapper = [self = GetWeakPtr(),
                           flush_timeout,
                           cb = std::move(callback)](auto result) mutable {
    if (self.is_alive() && result.is_ok()) {
      self->flush_timeout_.Set(flush_timeout);
    }
    cb(result);
  };

  if (flush_timeout < std::chrono::milliseconds(1) ||
      (flush_timeout > hci_spec::kMaxAutomaticFlushTimeoutDuration &&
       flush_timeout != pw::chrono::SystemClock::duration::max())) {
    callback_wrapper(ToResult(
        pw::bluetooth::emboss::StatusCode::INVALID_HCI_COMMAND_PARAMETERS));
    return;
  }

  uint16_t converted_flush_timeout;
  if (flush_timeout == pw::chrono::SystemClock::duration::max()) {
    // The command treats a flush timeout of 0 as infinite.
    converted_flush_timeout = 0;
  } else {
    // Slight imprecision from casting or converting to ms is fine for the flush
    // timeout (a few ms difference from the requested value doesn't matter).
    // Overflow is not possible because of the max value check above.
    converted_flush_timeout = static_cast<uint16_t>(
        static_cast<float>(
            std::chrono::duration_cast<std::chrono::milliseconds>(flush_timeout)
                .count()) *
        hci_spec::kFlushTimeoutMsToCommandParameterConversionFactor);
    PW_CHECK(converted_flush_timeout != 0);
    PW_CHECK(converted_flush_timeout <=
             hci_spec::kMaxAutomaticFlushTimeoutCommandParameterValue);
  }

  auto write_timeout = hci::CommandPacket::New<
      pw::bluetooth::emboss::WriteAutomaticFlushTimeoutCommandWriter>(
      hci_spec::kWriteAutomaticFlushTimeout);
  auto write_timeout_view = write_timeout.view_t();
  write_timeout_view.connection_handle().Write(handle_);
  write_timeout_view.flush_timeout().Write(converted_flush_timeout);

  cmd_channel_
      ->SendCommand(
          std::move(write_timeout),
          [cb = std::move(callback_wrapper), handle = handle_, flush_timeout](
              auto, const hci::EventPacket& event) mutable {
            if (event.ToResult().is_error()) {
              bt_log(WARN,
                     "hci",
                     "WriteAutomaticFlushTimeout command failed (result: %s, "
                     "handle: %#.4x)",
                     bt_str(event.ToResult()),
                     handle);
            } else {
              bt_log(DEBUG,
                     "hci",
                     "automatic flush timeout updated (handle: %#.4x, timeout: "
                     "%lld ms)",
                     handle,
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         flush_timeout)
                         .count());
            }
            cb(event.ToResult());
          })
      .IgnoreError();
}

void LogicalLink::AttachInspect(inspect::Node& parent, std::string name) {
  if (!parent) {
    return;
  }

  auto node = parent.CreateChild(name);
  inspect_properties_.handle =
      node.CreateString(kInspectHandlePropertyName,
                        bt_lib_cpp_string::StringPrintf("%#.4x", handle_));
  inspect_properties_.link_type =
      node.CreateString(kInspectLinkTypePropertyName, LinkTypeToString(type_));
  inspect_properties_.channels_node =
      node.CreateChild(kInspectChannelsNodeName);
  flush_timeout_.AttachInspect(node, kInspectFlushTimeoutPropertyName);
  if (autosniff_.has_value()) {
    autosniff_->AttachInspect(node, kInspectAutosniffNodeName);
  }
  inspect_properties_.node = std::move(node);

  for (auto& [_, chan] : channels_) {
    chan->AttachInspect(inspect_properties_.channels_node,
                        inspect_properties_.channels_node.UniqueName(
                            kInspectChannelNodePrefix));
  }
}

void LogicalLink::HandleNextAclPriorityRequest() {
  if (pending_acl_requests_.empty() || closed_) {
    return;
  }

  auto& request = pending_acl_requests_.front();
  PW_CHECK(request.callback);

  // Prevent closed channels with queued requests from upgrading channel
  // priority. Allow closed channels to downgrade priority so that they can
  // clean up their priority on destruction.
  if (!request.channel.is_alive() && request.priority != AclPriority::kNormal) {
    request.callback(fit::failed());
    pending_acl_requests_.pop();
    HandleNextAclPriorityRequest();
    return;
  }

  // Skip sending command if desired priority is already set. Do this here
  // instead of Channel in case Channel queues up multiple requests.
  if (request.priority == acl_priority_) {
    request.callback(fit::ok());
    pending_acl_requests_.pop();
    HandleNextAclPriorityRequest();
    return;
  }

  // If priority is not kNormal, then a channel might be using a conflicting
  // priority, and the new priority should not be requested.
  if (acl_priority_ != AclPriority::kNormal) {
    for (auto& [chan_id, chan] : channels_) {
      if ((request.channel.is_alive() &&
           chan.get() == &request.channel.get()) ||
          chan->requested_acl_priority() == AclPriority::kNormal) {
        continue;
      }

      // If the request returns priority to normal but a different channel still
      // requires high priority, skip sending command and just report success.
      if (request.priority == AclPriority::kNormal) {
        request.callback(fit::ok());
        break;
      }

      // If the request tries to upgrade priority but it conflicts with another
      // channel's priority (e.g. sink vs. source), report an error.
      if (request.priority != chan->requested_acl_priority()) {
        request.callback(fit::failed());
        break;
      }
    }

    if (!request.callback) {
      pending_acl_requests_.pop();
      HandleNextAclPriorityRequest();
      return;
    }
  }

  auto cb_wrapper = [self = GetWeakPtr(),
                     cb = std::move(request.callback),
                     priority = request.priority](auto result) mutable {
    if (!self.is_alive()) {
      return;
    }
    if (result.is_ok()) {
      self->acl_priority_ = priority;
    }
    cb(result);
    self->pending_acl_requests_.pop();
    self->HandleNextAclPriorityRequest();
  };

  acl_data_channel_->RequestAclPriority(
      request.priority, handle_, std::move(cb_wrapper));
}

void LogicalLink::ServeConnectionParameterUpdateRequest() {
  PW_CHECK(signaling_channel_);
  PW_CHECK(type_ == bt::LinkType::kLE);

  LowEnergyCommandHandler cmd_handler(signaling_channel_.get());
  cmd_handler.ServeConnectionParameterUpdateRequest(
      fit::bind_member<&LogicalLink::OnRxConnectionParameterUpdateRequest>(
          this));
}

void LogicalLink::OnRxConnectionParameterUpdateRequest(
    uint16_t interval_min,
    uint16_t interval_max,
    uint16_t peripheral_latency,
    uint16_t timeout_multiplier,
    LowEnergyCommandHandler::ConnectionParameterUpdateResponder* responder) {
  // Only a LE peripheral can send this command. "If a Peripheral’s Host
  // receives an L2CAP_CONNECTION_PARAMETER_UPDATE_REQ packet it shall respond
  // with an L2CAP_COMMAND_REJECT_RSP packet with reason 0x0000 (Command not
  // understood)." (v5.0, Vol 3, Part A, Section 4.20)
  if (role_ == pw::bluetooth::emboss::ConnectionRole::PERIPHERAL) {
    bt_log(
        DEBUG, "l2cap", "rejecting conn. param. update request from central");
    responder->RejectNotUnderstood();
    return;
  }

  // Reject the connection parameters if they are outside the ranges allowed by
  // the HCI specification (see HCI_LE_Connection_Update command v5.0, Vol 2,
  // Part E, Section 7.8.18).
  bool reject = false;

  hci_spec::LEPreferredConnectionParameters params(
      interval_min, interval_max, peripheral_latency, timeout_multiplier);

  if (params.min_interval() > params.max_interval()) {
    bt_log(DEBUG, "l2cap", "conn. min interval larger than max");
    reject = true;
  } else if (params.min_interval() < hci_spec::kLEConnectionIntervalMin) {
    bt_log(DEBUG,
           "l2cap",
           "conn. min interval outside allowed range: %#.4x",
           params.min_interval());
    reject = true;
  } else if (params.max_interval() > hci_spec::kLEConnectionIntervalMax) {
    bt_log(DEBUG,
           "l2cap",
           "conn. max interval outside allowed range: %#.4x",
           params.max_interval());
    reject = true;
  } else if (params.max_latency() > hci_spec::kLEConnectionLatencyMax) {
    bt_log(DEBUG,
           "l2cap",
           "conn. peripheral latency too large: %#.4x",
           params.max_latency());
    reject = true;
  } else if (params.supervision_timeout() <
                 hci_spec::kLEConnectionSupervisionTimeoutMin ||
             params.supervision_timeout() >
                 hci_spec::kLEConnectionSupervisionTimeoutMax) {
    bt_log(DEBUG,
           "l2cap",
           "conn supv. timeout outside allowed range: %#.4x",
           params.supervision_timeout());
    reject = true;
  }

  ConnectionParameterUpdateResult result =
      reject ? ConnectionParameterUpdateResult::kRejected
             : ConnectionParameterUpdateResult::kAccepted;
  responder->Send(result);

  if (!reject) {
    if (!connection_parameter_update_callback_) {
      bt_log(DEBUG,
             "l2cap",
             "no callback set for LE Connection Parameter Update Request");
      return;
    }

    connection_parameter_update_callback_(params);
  }
}

void LogicalLink::ServeFlowControlCreditInd() {
  PW_CHECK(signaling_channel_);
  PW_CHECK(type_ == bt::LinkType::kLE);

  LowEnergyCommandHandler cmd_handler(signaling_channel_.get());
  cmd_handler.ServeFlowControlCreditInd(
      fit::bind_member<&LogicalLink::OnRxFlowControlCreditInd>(this));
}

void LogicalLink::OnRxFlowControlCreditInd(ChannelId remote_cid,
                                           uint16_t credits) {
  ChannelImpl* channel = nullptr;
  for (auto& [_, chan] : channels_) {
    if (chan->remote_id() == remote_cid) {
      channel = chan.get();
    }
  }
  if (!channel) {
    bt_log(
        WARN, "l2cap", "ignoring flow control credit ind for unknown channel");
    return;
  }
  channel->AddCredits(credits);
}

bool LogicalLink::AutosniffEnabled() const { return autosniff_.has_value(); }

bool LogicalLink::AutosniffIsSuppressed() const {
  return !AutosniffEnabled() || autosniff_->IsSuppressed();
}

std::unique_ptr<AutosniffSuppressInterest> LogicalLink::SuppressAutosniff(
    const char* reason) {
  if (!autosniff_.has_value()) {
    return nullptr;
  }
  return autosniff_->Suppress(reason);
}

pw::bluetooth::emboss::AclConnectionMode LogicalLink::AutosniffMode() const {
  if (autosniff_.has_value()) {
    return autosniff_->CurrentMode();
  } else {
    return pw::bluetooth::emboss::AclConnectionMode::ACTIVE;
  }
}

}  // namespace bt::l2cap::internal
