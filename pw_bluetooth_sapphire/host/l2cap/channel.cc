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

#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"

#include <cpp-string/string_printf.h>
#include <pw_assert/check.h>

#include <memory>
#include <utility>

#include "lib/fit/result.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/common/weak_self.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/basic_mode_rx_engine.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/basic_mode_tx_engine.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/credit_based_flow_control_rx_engine.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/credit_based_flow_control_tx_engine.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/enhanced_retransmission_mode_engines.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/logical_link.h"
#include "pw_bluetooth_sapphire/internal/host/transport/link_type.h"

namespace bt::l2cap {

namespace android_hci = bt::hci_spec::vendor::android;
using pw::bluetooth::AclPriority;

Channel::Channel(ChannelId id,
                 ChannelId remote_id,
                 bt::LinkType link_type,
                 hci_spec::ConnectionHandle link_handle,
                 ChannelInfo info,
                 uint16_t max_tx_queued)
    : WeakSelf(this),
      id_(id),
      remote_id_(remote_id),
      link_type_(link_type),
      link_handle_(link_handle),
      info_(info),
      max_tx_queued_(max_tx_queued),
      requested_acl_priority_(AclPriority::kNormal) {
  PW_DCHECK(id_);
  PW_DCHECK(link_type_ == bt::LinkType::kLE ||
            link_type_ == bt::LinkType::kACL);
}

namespace internal {

namespace {

constexpr const char* kInspectPsmPropertyName = "psm";
constexpr const char* kInspectLocalIdPropertyName = "local_id";
constexpr const char* kInspectRemoteIdPropertyName = "remote_id";
constexpr const char* kInspectDroppedPacketsPropertyName = "dropped_packets";

}  // namespace

std::unique_ptr<ChannelImpl> ChannelImpl::CreateFixedChannel(
    pw::async::Dispatcher& dispatcher,
    ChannelId id,
    internal::LogicalLinkWeakPtr link,
    hci::CommandChannel::WeakPtr cmd_channel,
    uint16_t max_acl_payload_size,
    A2dpOffloadManager& a2dp_offload_manager,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
    uint16_t max_tx_queued) {
  // A fixed channel's endpoints have the same local and remote identifiers.
  // Setting the ChannelInfo MTU to kMaxMTU effectively cancels any L2CAP-level
  // MTU enforcement for services which operate over fixed channels. Such
  // services often define minimum MTU values in their specification, so they
  // are required to respect these MTUs internally by:
  //   1.) never sending packets larger than their spec-defined MTU.
  //   2.) handling inbound PDUs which are larger than their spec-defined MTU
  //   appropriately.
  return std::unique_ptr<ChannelImpl>(
      new ChannelImpl(dispatcher,
                      id,
                      id,
                      link,
                      ChannelInfo::MakeBasicMode(kMaxMTU, kMaxMTU),
                      std::move(cmd_channel),
                      max_acl_payload_size,
                      a2dp_offload_manager,
                      wake_lease_provider,
                      max_tx_queued));
}

std::unique_ptr<ChannelImpl> ChannelImpl::CreateDynamicChannel(
    pw::async::Dispatcher& dispatcher,
    ChannelId id,
    ChannelId peer_id,
    internal::LogicalLinkWeakPtr link,
    ChannelInfo info,
    hci::CommandChannel::WeakPtr cmd_channel,
    uint16_t max_acl_payload_size,
    A2dpOffloadManager& a2dp_offload_manager,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
    uint16_t max_tx_queued) {
  return std::unique_ptr<ChannelImpl>(new ChannelImpl(dispatcher,
                                                      id,
                                                      peer_id,
                                                      link,
                                                      info,
                                                      std::move(cmd_channel),
                                                      max_acl_payload_size,
                                                      a2dp_offload_manager,
                                                      wake_lease_provider,
                                                      max_tx_queued));
}

ChannelImpl::ChannelImpl(
    pw::async::Dispatcher& dispatcher,
    ChannelId id,
    ChannelId remote_id,
    internal::LogicalLinkWeakPtr link,
    ChannelInfo info,
    hci::CommandChannel::WeakPtr cmd_channel,
    uint16_t max_acl_payload_size,
    A2dpOffloadManager& a2dp_offload_manager,
    pw::bluetooth_sapphire::LeaseProvider& wake_lease_provider,
    uint16_t max_tx_queued)
    : Channel(id, remote_id, link->type(), link->handle(), info, max_tx_queued),
      pw_dispatcher_(dispatcher),
      active_(false),
      link_(link),
      cmd_channel_(std::move(cmd_channel)),
      fragmenter_(link->handle(), max_acl_payload_size),
      wake_lease_provider_(wake_lease_provider),
      a2dp_offload_manager_(a2dp_offload_manager),
      weak_self_(this) {
  PW_CHECK(link_.is_alive());
  PW_CHECK(
      info_.mode == RetransmissionAndFlowControlMode::kBasic ||
          info_.mode ==
              RetransmissionAndFlowControlMode::kEnhancedRetransmission ||
          info.mode == CreditBasedFlowControlMode::kLeCreditBasedFlowControl,
      "Channel constructed with unsupported mode: %s\n",
      AnyChannelModeToString(info_.mode).c_str());

  auto connection_failure_cb = [link] {
    if (link.is_alive()) {
      // |link| is expected to ignore this call if it has been closed.
      link->SignalError();
    }
  };

  if (info_.mode == RetransmissionAndFlowControlMode::kBasic) {
    rx_engine_ = std::make_unique<BasicModeRxEngine>();
    tx_engine_ =
        std::make_unique<BasicModeTxEngine>(id, max_tx_sdu_size(), *this);
  } else if (std::holds_alternative<CreditBasedFlowControlMode>(info_.mode)) {
    PW_CHECK(info_.remote_initial_credits.has_value());

    auto return_credits_cb = [this](uint16_t credits) {
      if (link_.is_alive()) {
        // |link| is expected to ignore this call if it has been closed.
        link_->SignalCreditsAvailable(id_, credits);
      }
    };

    auto mode = std::get<CreditBasedFlowControlMode>(info_.mode);
    rx_engine_ = std::make_unique<CreditBasedFlowControlRxEngine>(
        std::move(connection_failure_cb), std::move(return_credits_cb));
    tx_engine_ = std::make_unique<CreditBasedFlowControlTxEngine>(
        id,
        max_tx_sdu_size(),
        *this,
        mode,
        info_.max_tx_pdu_payload_size,
        *info_.remote_initial_credits);
  } else {
    std::tie(rx_engine_, tx_engine_) =
        MakeLinkedEnhancedRetransmissionModeEngines(
            id,
            max_tx_sdu_size(),
            info_.max_transmissions,
            info_.n_frames_in_tx_window,
            *this,
            std::move(connection_failure_cb),
            pw_dispatcher_);
  }
}

ChannelImpl::~ChannelImpl() {
  size_t removed_count = this->pending_tx_sdus_.size();
  if (removed_count > 0) {
    bt_log(TRACE,
           "hci",
           "packets dropped (count: %zu) due to channel destruction (link: "
           "%#.4x, id: %#.4x)",
           removed_count,
           link_handle(),
           id());
  }
}

void ChannelImpl::AddCredits(uint16_t credits) {
  if (tx_engine_) {
    tx_engine_->AddCredits(credits);
  }
}

const sm::SecurityProperties ChannelImpl::security() {
  if (link_.is_alive()) {
    return link_->security();
  }
  return sm::SecurityProperties();
}

bool ChannelImpl::Activate(RxCallback rx_callback,
                           ClosedCallback closed_callback) {
  PW_CHECK(rx_callback);
  PW_CHECK(closed_callback);

  // Activating on a closed link has no effect. We also clear this on
  // deactivation to prevent a channel from being activated more than once.
  if (!link_.is_alive())
    return false;

  PW_CHECK(!active_);
  active_ = true;
  rx_cb_ = std::move(rx_callback);
  closed_cb_ = std::move(closed_callback);

  // Route the buffered packets.
  if (!pending_rx_sdus_.empty()) {
    TRACE_DURATION("bluetooth", "ChannelImpl::Activate pending drain");
    // Channel may be destroyed in rx_cb_, so we need to check self after
    // calling rx_cb_.
    auto self = GetWeakPtr();
    std::queue<ByteBufferPtr, std::list<ByteBufferPtr>> pending;
    std::swap(pending_rx_sdus_, pending);
    while (self.is_alive() && !pending.empty()) {
      TRACE_FLOW_END(
          "bluetooth", "ChannelImpl::HandleRxPdu queued", pending.size());
      rx_cb_(std::move(pending.front()));
      pending.pop();
    }
    if (self.is_alive() && AreQueuesEmpty()) {
      wake_lease_.reset();
    }
  }

  return true;
}

void ChannelImpl::Deactivate() {
  bt_log(TRACE,
         "l2cap",
         "deactivating channel (link: %#.4x, id: %#.4x)",
         link_handle(),
         id());

  // De-activating on a closed link has no effect.
  if (!link_.is_alive() || !active_) {
    link_ = internal::LogicalLinkWeakPtr();
    return;
  }

  auto link = link_;

  CleanUp();

  // |link| is expected to ignore this call if it has been closed.
  link->RemoveChannel(this, /*removed_cb=*/[] {});
}

void ChannelImpl::SignalLinkError() {
  // Cannot signal an error on a closed or deactivated link.
  if (!link_.is_alive() || !active_)
    return;

  // |link_| is expected to ignore this call if it has been closed.
  link_->SignalError();
}

bool ChannelImpl::Send(ByteBufferPtr sdu) {
  PW_DCHECK(sdu);

  TRACE_DURATION(
      "bluetooth", "l2cap:send", "handle", link_->handle(), "id", id());

  if (!link_.is_alive()) {
    bt_log(ERROR, "l2cap", "cannot send SDU on a closed link");
    return false;
  }

  if (!active_) {
    bt_log(INFO, "l2cap", "not sending SDU on an inactive channel");
    return false;
  }

  // Cap the Tx SDU queue to |max_tx_queued|.
  if (pending_tx_sdus_.size() > max_tx_queued()) {
    return false;
  }

  if (!wake_lease_) {
    TryAcquireWakeLease();
  }

  pending_tx_sdus_.push(std::move(sdu));
  tx_engine_->NotifySduQueued();
  return true;
}

std::unique_ptr<hci::ACLDataPacket> ChannelImpl::GetNextOutboundPacket() {
  // Channel's next packet is a starting fragment
  if (!HasFragments() && HasPDUs()) {
    // B-frames for Basic Mode contain only an "Information payload" (v5.0 Vol
    // 3, Part A, Sec 3.1)
    FrameCheckSequenceOption fcs_option =
        info().mode == RetransmissionAndFlowControlMode::kEnhancedRetransmission
            ? FrameCheckSequenceOption::kIncludeFcs
            : FrameCheckSequenceOption::kNoFcs;
    // Get new PDU and release fragments
    auto pdu =
        fragmenter_.BuildFrame(remote_id(),
                               *pending_tx_pdus_.front(),
                               fcs_option,
                               /*flushable=*/info().flush_timeout.has_value());
    pending_tx_fragments_ = pdu.ReleaseFragments();
    pending_tx_pdus_.pop();
  }

  // Send next packet if it exists
  std::unique_ptr<hci::ACLDataPacket> fragment = nullptr;
  if (HasFragments()) {
    fragment = std::move(pending_tx_fragments_.front());
    pending_tx_fragments_.pop_front();
  }

  if (AreQueuesEmpty()) {
    wake_lease_.reset();
  }

  return fragment;
}

void ChannelImpl::UpgradeSecurity(sm::SecurityLevel level,
                                  sm::ResultFunction<> callback) {
  PW_CHECK(callback);

  if (!link_.is_alive() || !active_) {
    bt_log(DEBUG, "l2cap", "Ignoring security request on inactive channel");
    return;
  }

  link_->UpgradeSecurity(level, std::move(callback));
}

void ChannelImpl::RequestAclPriority(
    AclPriority priority,
    fit::callback<void(fit::result<fit::failed>)> callback) {
  if (!link_.is_alive() || !active_) {
    bt_log(DEBUG, "l2cap", "Ignoring ACL priority request on inactive channel");
    callback(fit::failed());
    return;
  }

  // Callback is only called after checking that the weak pointer passed is
  // alive, so using this in lambda is safe.
  link_->RequestAclPriority(
      GetWeakPtr(),
      priority,
      [self = GetWeakPtr(), this, priority, cb = std::move(callback)](
          auto result) mutable {
        if (self.is_alive() && result.is_ok()) {
          requested_acl_priority_ = priority;
        }
        cb(result);
      });
}

void ChannelImpl::SetBrEdrAutomaticFlushTimeout(
    pw::chrono::SystemClock::duration flush_timeout,
    hci::ResultCallback<> callback) {
  PW_CHECK(link_type_ == bt::LinkType::kACL);

  // Channel may be inactive if this method is called before activation.
  if (!link_.is_alive()) {
    bt_log(DEBUG, "l2cap", "Ignoring %s on closed channel", __FUNCTION__);
    callback(ToResult(pw::bluetooth::emboss::StatusCode::COMMAND_DISALLOWED));
    return;
  }

  auto cb_wrapper = [self = GetWeakPtr(),
                     this,
                     cb = std::move(callback),
                     flush_timeout](auto result) mutable {
    if (!self.is_alive()) {
      cb(ToResult(pw::bluetooth::emboss::StatusCode::UNSPECIFIED_ERROR));
      return;
    }

    if (result.is_ok()) {
      info_.flush_timeout = flush_timeout;
    }

    cb(result);
  };

  link_->SetBrEdrAutomaticFlushTimeout(flush_timeout, std::move(cb_wrapper));
}

void ChannelImpl::AttachInspect(inspect::Node& parent, std::string name) {
  inspect_.node = parent.CreateChild(name);
  if (info_.psm) {
    inspect_.psm = inspect_.node.CreateString(kInspectPsmPropertyName,
                                              PsmToString(info_.psm.value()));
  }
  inspect_.local_id = inspect_.node.CreateString(
      kInspectLocalIdPropertyName,
      bt_lib_cpp_string::StringPrintf("%#.4x", id()));
  inspect_.remote_id = inspect_.node.CreateString(
      kInspectRemoteIdPropertyName,
      bt_lib_cpp_string::StringPrintf("%#.4x", remote_id()));

  inspect_.dropped_packets = inspect_.node.CreateUint(
      kInspectDroppedPacketsPropertyName, dropped_packets_);
}

void ChannelImpl::StartA2dpOffload(
    const A2dpOffloadManager::Configuration& config,
    hci::ResultCallback<> callback) {
  a2dp_offload_manager_.StartA2dpOffload(config,
                                         id(),
                                         remote_id(),
                                         link_handle(),
                                         max_tx_sdu_size(),
                                         std::move(callback));
}

void ChannelImpl::StopA2dpOffload(hci::ResultCallback<> callback) {
  a2dp_offload_manager_.RequestStopA2dpOffload(
      id(), link_handle(), std::move(callback));
}

void ChannelImpl::OnClosed() {
  bt_log(TRACE,
         "l2cap",
         "channel closed (link: %#.4x, id: %#.4x)",
         link_handle(),
         id());

  if (!link_.is_alive() || !active_) {
    link_ = internal::LogicalLinkWeakPtr();
    return;
  }

  PW_CHECK(closed_cb_);
  auto closed_cb = std::move(closed_cb_);

  CleanUp();

  closed_cb();
}

void ChannelImpl::HandleRxPdu(PDU&& pdu) {
  TRACE_DURATION("bluetooth",
                 "ChannelImpl::HandleRxPdu",
                 "handle",
                 link_->handle(),
                 "channel_id",
                 id_);

  // link_ may be nullptr if a pdu is received after the channel has been
  // deactivated but before LogicalLink::RemoveChannel has been dispatched
  if (!link_.is_alive()) {
    bt_log(TRACE, "l2cap", "ignoring pdu on deactivated channel");
    return;
  }

  PW_CHECK(rx_engine_);

  ByteBufferPtr sdu = rx_engine_->ProcessPdu(std::move(pdu));
  if (!sdu) {
    // The PDU may have been invalid, out-of-sequence, part of a segmented
    // SDU, or a signaling PDU.
    // * If invalid, we drop the PDU (per Core Spec Ver 5, Vol 3, Part A,
    //   Secs. 3.3.6 and/or 3.3.7).
    // * If out-of-sequence or part of a segmented SDU, we expect that some
    //   later call to ProcessPdu() will return us an SDU containing this
    //   PDU's data.
    // * If a signaling PDU, packet queue state may have changed and we might be
    //   able to release the wake lease.
    if (AreQueuesEmpty()) {
      wake_lease_.reset();
    }
    return;
  }

  // Buffer the packets if the channel hasn't been activated.
  if (!active_) {
    if (pending_rx_sdus_.size() >= kMaxPendingRxSdus) {
      dropped_packets_ += 1;
      if (dropped_packets_ % 100 == 1) {
        bt_log(DEBUG,
               "l2cap",
               "pending rx queue full (%zu) on channel %#.4x (handle: %#.4x), "
               "dropping SDU",
               pending_rx_sdus_.size(),
               id(),
               link_.is_alive() ? link_->handle() : 0);
      }
      inspect_.dropped_packets.Set(dropped_packets_);
      return;
    }
    pending_rx_sdus_.emplace(std::move(sdu));
    // Tracing: we assume pending_rx_sdus_ is only filled once and use the
    // length of queue for trace ids.
    TRACE_FLOW_BEGIN("bluetooth",
                     "ChannelImpl::HandleRxPdu queued",
                     pending_rx_sdus_.size());

    // While waiting for a channel to be activated, acquire a wake lease so the
    // system does not suspend while higher layers process channel activation
    // and drain the queued SDUs.
    //
    // We restrict this to dynamic channels (id() >= kFirstDynamicChannelId)
    // because fixed channels (such as SMP) are pre-allocated upon logical link
    // establishment and might never be activated (e.g. during legacy pairing
    // fallback). If we acquired a wake lease for unactivated fixed channels
    // when packets arrive, pending_rx_sdus_ would never be drained, pinning
    // the wake lease indefinitely.
    if (!wake_lease_ && id() >= kFirstDynamicChannelId) {
      TryAcquireWakeLease();
    }

    return;
  }

  // The PDU may have been the last segment of an SDU, in which case an RxEngine
  // queue may have been emptied.
  if (AreQueuesEmpty()) {
    wake_lease_.reset();
  }

  PW_CHECK(rx_cb_);
  {
    TRACE_DURATION("bluetooth", "ChannelImpl::HandleRxPdu callback");
    rx_cb_(std::move(sdu));
  }
}

void ChannelImpl::CleanUp() {
  RequestAclPriority(AclPriority::kNormal, [](auto result) {
    if (result.is_error()) {
      bt_log(WARN, "l2cap", "Resetting ACL priority on channel closed failed");
    }
  });

  a2dp_offload_manager_.RequestStopA2dpOffload(
      id(), link_handle(), [](auto result) {
        if (result.is_error()) {
          bt_log(WARN,
                 "l2cap",
                 "Stopping A2DP offloading on channel closed failed: %s",
                 bt_str(result));
        }
      });

  active_ = false;
  link_ = internal::LogicalLinkWeakPtr();
  rx_cb_ = nullptr;
  closed_cb_ = nullptr;
  rx_engine_ = nullptr;
  tx_engine_ = nullptr;
  wake_lease_.reset();
}

void ChannelImpl::SendFrame(ByteBufferPtr pdu) {
  if (!link_.is_alive() || !active_) {
    bt_log(DEBUG,
           "l2cap",
           "dropping ACL packet for inactive connection (handle: %#.4x)",
           link_->handle());
    return;
  }

  pending_tx_pdus_.emplace(std::move(pdu));

  // Ensure that |pending_tx_pdus_| does not exceed its maximum queue size
  if (pending_tx_pdus_.size() > max_tx_queued()) {
    dropped_packets_ += 1;
    if (dropped_packets_ % 100 == 1) {
      bt_log(DEBUG,
             "l2cap",
             "Queued packets (%zu) exceeds maximum (%u). "
             "Dropping oldest ACL packet (handle: %#.4x)",
             pending_tx_pdus_.size(),
             max_tx_queued(),
             link_->handle());
    }
    inspect_.dropped_packets.Set(dropped_packets_);

    pending_tx_pdus_.pop();  // Remove the oldest (aka first) element
  }

  // Notify LogicalLink that a packet is available. This is only necessary for
  // the first packet of an empty queue (flow control will poll this connection
  // otherwise).
  if (pending_tx_pdus_.size() == 1u) {
    link_->OnOutboundPacketAvailable();
  }

  if (!wake_lease_ && !AreQueuesEmpty()) {
    TryAcquireWakeLease();
  }
}

std::optional<ByteBufferPtr> ChannelImpl::GetNextQueuedSdu() {
  if (pending_tx_sdus_.empty())
    return std::nullopt;
  ByteBufferPtr next_sdu = std::move(pending_tx_sdus_.front());
  pending_tx_sdus_.pop();
  return next_sdu;
}

bool ChannelImpl::AreQueuesEmpty() {
  return pending_rx_sdus_.empty() && pending_tx_sdus_.empty() &&
         pending_tx_pdus_.empty() && pending_tx_fragments_.empty() &&
         (!tx_engine_ || tx_engine_->IsQueueEmpty()) &&
         (!rx_engine_ || rx_engine_->IsQueueEmpty());
}

void ChannelImpl::TryAcquireWakeLease() {
  pw::Result<pw::bluetooth_sapphire::Lease> lease =
      PW_SAPPHIRE_ACQUIRE_LEASE(wake_lease_provider_, "l2cap::ChannelImpl");
  if (lease.ok()) {
    wake_lease_ = std::move(lease.value());
  }
}

}  // namespace internal
}  // namespace bt::l2cap
