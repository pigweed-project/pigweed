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

#include "pw_bluetooth_sapphire/internal/host/l2cap/bredr_dynamic_channel.h"

#include <pw_assert/check.h>
#include <pw_preprocessor/compiler.h>

#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel_configuration.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"

namespace bt::l2cap::internal {
namespace {

// This value is arbitrary, but Core Spec v6.1 contains just 7 options so
// supporting 7 unknown options seems reasonable.
constexpr size_t kMaxUnknownOptionsPerResponse = 7;

ChannelConfiguration::RetransmissionAndFlowControlOption
WriteRfcOutboundTimeouts(
    ChannelConfiguration::RetransmissionAndFlowControlOption rfc_option) {
  rfc_option.set_rtx_timeout(kErtmReceiverReadyPollTimerMsecs);
  rfc_option.set_monitor_timeout(kErtmMonitorTimerMsecs);
  return rfc_option;
}

constexpr uint16_t kBrEdrDynamicChannelCount =
    kLastACLDynamicChannelId - kFirstDynamicChannelId + 1;

constexpr pw::chrono::SystemClock::duration
    kNoResourcesResponseTimeoutDuration = std::chrono::seconds(2);

const uint8_t kMaxNumBasicConfigRequests = 2;
}  // namespace

BrEdrDynamicChannelRegistry::BrEdrDynamicChannelRegistry(
    SignalingChannelInterface* sig,
    DynamicChannelCallback close_cb,
    ServiceRequestCallback service_request_cb,
    bool random_channel_ids,
    pw::async::Dispatcher& dispatcher)
    : DynamicChannelRegistry(kBrEdrDynamicChannelCount,
                             std::move(close_cb),
                             std::move(service_request_cb),
                             random_channel_ids),
      state_(0u),
      sig_(sig),
      dispatcher_(dispatcher) {
  PW_DCHECK(sig_);
  BrEdrCommandHandler cmd_handler(sig_);
  cmd_handler.ServeConnectionRequest(
      fit::bind_member<&BrEdrDynamicChannelRegistry::OnRxConnReq>(this));
  cmd_handler.ServeConfigurationRequest(
      fit::bind_member<&BrEdrDynamicChannelRegistry::OnRxConfigReq>(this));
  cmd_handler.ServeDisconnectionRequest(
      fit::bind_member<&BrEdrDynamicChannelRegistry::OnRxDisconReq>(this));
  cmd_handler.ServeInformationRequest(
      fit::bind_member<&BrEdrDynamicChannelRegistry::OnRxInfoReq>(this));
  SendInformationRequests();
}

std::unique_ptr<DynamicChannel> BrEdrDynamicChannelRegistry::MakeOutbound(
    Psm psm, ChannelId local_cid, ChannelParameters params) {
  return BrEdrDynamicChannel::MakeOutbound(
      this, sig_, psm, local_cid, params, PeerSupportsERTM(), dispatcher_);
}

std::unique_ptr<DynamicChannel> BrEdrDynamicChannelRegistry::MakeInbound(
    Psm psm,
    ChannelId local_cid,
    ChannelId remote_cid,
    ChannelParameters params) {
  return BrEdrDynamicChannel::MakeInbound(this,
                                          sig_,
                                          psm,
                                          local_cid,
                                          remote_cid,
                                          params,
                                          PeerSupportsERTM(),
                                          dispatcher_);
}

void BrEdrDynamicChannelRegistry::OnRxConnReq(
    Psm psm,
    ChannelId remote_cid,
    BrEdrCommandHandler::ConnectionResponder* responder) {
  bt_log(TRACE,
         "l2cap-bredr",
         "Got Connection Request for PSM %#.4x from channel %#.4x",
         psm,
         remote_cid);

  if (remote_cid == kInvalidChannelId) {
    bt_log(DEBUG,
           "l2cap-bredr",
           "Invalid source CID; rejecting connection for PSM %#.4x from "
           "channel %#.4x",
           psm,
           remote_cid);
    responder->Send(kInvalidChannelId,
                    ConnectionResult::kInvalidSourceCID,
                    ConnectionStatus::kNoInfoAvailable);
    return;
  }

  if (FindChannelByRemoteId(remote_cid) != nullptr) {
    bt_log(DEBUG,
           "l2cap-bredr",
           "Remote CID already in use; rejecting connection for PSM %#.4x from "
           "channel %#.4x",
           psm,
           remote_cid);
    responder->Send(kInvalidChannelId,
                    ConnectionResult::kSourceCIDAlreadyAllocated,
                    ConnectionStatus::kNoInfoAvailable);
    return;
  }

  ChannelId local_cid = FindAvailableChannelId();
  if (local_cid == kInvalidChannelId) {
    bt_log(DEBUG,
           "l2cap-bredr",
           "Out of IDs; rejecting connection for PSM %#.4x from channel %#.4x",
           psm,
           remote_cid);
    responder->Send(kInvalidChannelId,
                    ConnectionResult::kNoResources,
                    ConnectionStatus::kNoInfoAvailable);
    return;
  }

  auto dyn_chan = RequestService(psm, local_cid, remote_cid);
  if (!dyn_chan) {
    bt_log(DEBUG,
           "l2cap-bredr",
           "Rejecting connection for unsupported PSM %#.4x from channel %#.4x",
           psm,
           remote_cid);
    responder->Send(kInvalidChannelId,
                    ConnectionResult::kPsmNotSupported,
                    ConnectionStatus::kNoInfoAvailable);
    return;
  }

  static_cast<BrEdrDynamicChannel*>(dyn_chan)->CompleteInboundConnection(
      responder);
}

void BrEdrDynamicChannelRegistry::OnRxConfigReq(
    ChannelId local_cid,
    uint16_t flags,
    ChannelConfiguration config,
    BrEdrCommandHandler::ConfigurationResponder* responder) {
  auto channel =
      static_cast<BrEdrDynamicChannel*>(FindChannelByLocalId(local_cid));
  if (channel == nullptr) {
    bt_log(WARN,
           "l2cap-bredr",
           "ID %#.4x not found for Configuration Request",
           local_cid);
    responder->RejectInvalidChannelId();
    return;
  }

  channel->OnRxConfigReq(flags, std::move(config), responder);
}

void BrEdrDynamicChannelRegistry::OnRxDisconReq(
    ChannelId local_cid,
    ChannelId remote_cid,
    BrEdrCommandHandler::DisconnectionResponder* responder) {
  auto channel =
      static_cast<BrEdrDynamicChannel*>(FindChannelByLocalId(local_cid));
  if (channel == nullptr || channel->remote_cid() != remote_cid) {
    bt_log(WARN,
           "l2cap-bredr",
           "ID %#.4x not found for Disconnection Request (remote ID %#.4x)",
           local_cid,
           remote_cid);
    responder->RejectInvalidChannelId();
    return;
  }

  channel->OnRxDisconReq(responder);
}

void BrEdrDynamicChannelRegistry::OnRxInfoReq(
    InformationType type,
    BrEdrCommandHandler::InformationResponder* responder) {
  bt_log(TRACE,
         "l2cap-bredr",
         "Got Information Request for type %#.4hx",
         static_cast<unsigned short>(type));

  // TODO(fxbug.dev/42175069): The responses here will likely remain hardcoded
  // magics, but maybe they should live elsewhere.
  switch (type) {
    case InformationType::kConnectionlessMTU: {
      responder->SendNotSupported();
      break;
    }

    case InformationType::kExtendedFeaturesSupported: {
      const ExtendedFeatures extended_features =
          kExtendedFeaturesBitFixedChannels | kExtendedFeaturesBitFCSOption |
          kExtendedFeaturesBitEnhancedRetransmission;

      // Express support for the Fixed Channel Supported feature
      responder->SendExtendedFeaturesSupported(extended_features);
      break;
    }

    case InformationType::kFixedChannelsSupported: {
      // Express support for the ACL-U Signaling Channel (as required) and the
      // SM channel (used for CTKD).
      const FixedChannelsSupported channels_supported =
          kFixedChannelsSupportedBitSignaling | kFixedChannelsSupportedBitSM;
      responder->SendFixedChannelsSupported(channels_supported);
      break;
    }

    default:
      responder->RejectNotUnderstood();
      bt_log(DEBUG,
             "l2cap-bredr",
             "Rejecting Information Request type %#.4hx",
             static_cast<unsigned short>(type));
  }
}

void BrEdrDynamicChannelRegistry::OnRxExtendedFeaturesInfoRsp(
    const BrEdrCommandHandler::InformationResponse& rsp) {
  if (rsp.status() == BrEdrCommandHandler::Status::kReject) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Extended Features Information Request rejected, reason %#.4hx, "
           "disconnecting",
           static_cast<unsigned short>(rsp.reject_reason()));
    return;
  }

  if (rsp.result() != InformationResult::kSuccess) {
    bt_log(DEBUG,
           "l2cap-bredr",
           "Extended Features Information Response failure result (result: "
           "%#.4hx)",
           static_cast<uint16_t>(rsp.result()));
    // Treat failure result as if feature mask indicated no ERTM support so that
    // configuration can fall back to basic mode.
    ForEach([](DynamicChannel* chan) {
      std::ignore = static_cast<BrEdrDynamicChannel*>(chan)
                        ->SetEnhancedRetransmissionSupport(false);
    });
    return;
  }

  if (rsp.type() != InformationType::kExtendedFeaturesSupported) {
    bt_log(
        ERROR,
        "l2cap-bredr",
        "Incorrect extended features information response type (type: %#.4hx)",
        static_cast<unsigned short>(rsp.type()));
    return;
  }

  if ((state_ & kExtendedFeaturesReceived) ||
      !(state_ & kExtendedFeaturesSent)) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Unexpected extended features information response (state: %#x)",
           state_);
    return;
  }

  bt_log(
      TRACE,
      "l2cap-bredr",
      "Received Extended Features Information Response (feature mask: %#.4x)",
      rsp.extended_features());

  state_ |= kExtendedFeaturesReceived;

  extended_features_ = rsp.extended_features();

  // Notify all channels created before extended features received.
  bool ertm_support =
      *extended_features_ & kExtendedFeaturesBitEnhancedRetransmission;
  ForEach([ertm_support](DynamicChannel* chan) {
    std::ignore = static_cast<BrEdrDynamicChannel*>(chan)
                      ->SetEnhancedRetransmissionSupport(ertm_support);
  });
}

void BrEdrDynamicChannelRegistry::SendInformationRequests() {
  if (state_ & kExtendedFeaturesSent) {
    bt_log(
        DEBUG, "l2cap-bredr", "Skipping sending info requests, already sent");
    return;
  }
  BrEdrCommandHandler cmd_handler(sig_);
  auto on_rx_info_rsp = [self = GetWeakPtr(), this](auto& rsp) {
    if (self.is_alive()) {
      OnRxExtendedFeaturesInfoRsp(rsp);
    }
  };
  if (!cmd_handler.SendInformationRequest(
          InformationType::kExtendedFeaturesSupported,
          std::move(on_rx_info_rsp))) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Failed to send Extended Features Information Request");
    return;
  }

  state_ |= kExtendedFeaturesSent;
}

std::optional<bool> BrEdrDynamicChannelRegistry::PeerSupportsERTM() const {
  if (!extended_features_) {
    return std::nullopt;
  }
  return *extended_features_ & kExtendedFeaturesBitEnhancedRetransmission;
}

BrEdrDynamicChannelPtr BrEdrDynamicChannel::MakeOutbound(
    DynamicChannelRegistry* registry,
    SignalingChannelInterface* signaling_channel,
    Psm psm,
    ChannelId local_cid,
    ChannelParameters params,
    std::optional<bool> peer_supports_ertm,
    pw::async::Dispatcher& dispatcher) {
  return std::unique_ptr<BrEdrDynamicChannel>(
      new BrEdrDynamicChannel(registry,
                              signaling_channel,
                              psm,
                              local_cid,
                              kInvalidChannelId,
                              params,
                              peer_supports_ertm,
                              dispatcher));
}

BrEdrDynamicChannelPtr BrEdrDynamicChannel::MakeInbound(
    DynamicChannelRegistry* registry,
    SignalingChannelInterface* signaling_channel,
    Psm psm,
    ChannelId local_cid,
    ChannelId remote_cid,
    ChannelParameters params,
    std::optional<bool> peer_supports_ertm,
    pw::async::Dispatcher& dispatcher) {
  auto channel = std::unique_ptr<BrEdrDynamicChannel>(
      new BrEdrDynamicChannel(registry,
                              signaling_channel,
                              psm,
                              local_cid,
                              remote_cid,
                              params,
                              peer_supports_ertm,
                              dispatcher));
  channel->state_ |= kConnRequested;
  return channel;
}

void BrEdrDynamicChannel::Open(fit::closure open_result_cb) {
  open_result_cb_ = std::move(open_result_cb);

  if (state_ & kConnRequested) {
    return;
  }

  auto on_conn_rsp = [self = weak_self_.GetWeakPtr()](
                         const BrEdrCommandHandler::ConnectionResponse& rsp) {
    if (self.is_alive()) {
      return self->OnRxConnRsp(rsp);
    }
    return BrEdrCommandHandler::ResponseHandlerAction::
        kCompleteOutboundTransaction;
  };

  auto on_conn_rsp_timeout = [this, self = weak_self_.GetWeakPtr()] {
    if (self.is_alive()) {
      bt_log(WARN,
             "l2cap-bredr",
             "Channel %#.4x: Timed out waiting for Connection Response",
             local_cid());
      PassOpenError();
    }
  };

  BrEdrCommandHandler cmd_handler(signaling_channel_,
                                  std::move(on_conn_rsp_timeout));
  if (!cmd_handler.SendConnectionRequest(
          psm(), local_cid(), std::move(on_conn_rsp))) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Channel %#.4x: Failed to send Connection Request",
           local_cid());
    PassOpenError();
    return;
  }

  bt_log(TRACE,
         "l2cap-bredr",
         "Channel %#.4x: Sent Connection Request",
         local_cid());

  state_ |= kConnRequested;
}

void BrEdrDynamicChannel::Disconnect(DisconnectDoneCallback done_cb) {
  PW_CHECK(done_cb);
  if (!IsConnected()) {
    done_cb();
    return;
  }

  state_ |= kDisconnected;

  // Don't send disconnect request if the peer never responded (also can't,
  // because we don't have their end's ID).
  if (remote_cid() == kInvalidChannelId) {
    done_cb();
    return;
  }

  auto on_discon_rsp =
      [local_cid = local_cid(),
       remote_cid = remote_cid(),
       self = weak_self_.GetWeakPtr(),
       done_cb_shared = done_cb.share()](
          const BrEdrCommandHandler::DisconnectionResponse& rsp) mutable {
        if (rsp.local_cid() != local_cid || rsp.remote_cid() != remote_cid) {
          bt_log(WARN,
                 "l2cap-bredr",
                 "Channel %#.4x: Got Disconnection Response with ID %#.4x/"
                 "remote ID %#.4x on channel with remote ID %#.4x",
                 local_cid,
                 rsp.local_cid(),
                 rsp.remote_cid(),
                 remote_cid);
        } else {
          bt_log(TRACE,
                 "l2cap-bredr",
                 "Channel %#.4x: Got Disconnection Response",
                 local_cid);
        }

        if (self.is_alive()) {
          done_cb_shared();
        }
      };

  auto on_discon_rsp_timeout = [local_cid = local_cid(),
                                self = weak_self_.GetWeakPtr(),
                                done_cb_shared = done_cb.share()]() mutable {
    bt_log(WARN,
           "l2cap-bredr",
           "Channel %#.4x: Timed out waiting for Disconnection Response; "
           "completing disconnection",
           local_cid);
    if (self.is_alive()) {
      done_cb_shared();
    }
  };

  BrEdrCommandHandler cmd_handler(signaling_channel_,
                                  std::move(on_discon_rsp_timeout));
  if (!cmd_handler.SendDisconnectionRequest(
          remote_cid(), local_cid(), std::move(on_discon_rsp))) {
    bt_log(WARN,
           "l2cap-bredr",
           "Channel %#.4x: Failed to send Disconnection Request",
           local_cid());
    done_cb();
    return;
  }

  bt_log(TRACE,
         "l2cap-bredr",
         "Channel %#.4x: Sent Disconnection Request",
         local_cid());
}

bool BrEdrDynamicChannel::IsConnected() const {
  // Remote-initiated channels have remote_cid_ already set.
  return (state_ & kConnRequested) && (state_ & kConnResponded) &&
         (remote_cid() != kInvalidChannelId) && !(state_ & kDisconnected);
}

bool BrEdrDynamicChannel::IsOpen() const {
  return IsConnected() && BothConfigsAccepted() &&
         AcceptedChannelModesAreConsistent();
}

ChannelInfo BrEdrDynamicChannel::info() const {
  PW_CHECK(local_config().retransmission_flow_control_option().has_value());
  PW_CHECK(local_config().mtu_option().has_value());

  const auto max_rx_sdu_size = local_config().mtu_option()->mtu();
  const auto peer_mtu = remote_config().mtu_option()->mtu();
  const auto flush_timeout = parameters_.flush_timeout;
  if (local_config().retransmission_flow_control_option()->mode() ==
      RetransmissionAndFlowControlMode::kBasic) {
    const auto max_tx_sdu_size = peer_mtu;
    return ChannelInfo::MakeBasicMode(
        max_rx_sdu_size, max_tx_sdu_size, psm(), flush_timeout);
  }
  const auto n_frames_in_tx_window =
      remote_config().retransmission_flow_control_option()->tx_window_size();
  const auto max_transmissions =
      remote_config().retransmission_flow_control_option()->max_transmit();
  const auto max_tx_pdu_payload_size =
      remote_config().retransmission_flow_control_option()->mps();
  const auto max_tx_sdu_size =
      std::min({peer_mtu, max_tx_pdu_payload_size, kMaxOutboundPduPayloadSize});
  if (max_tx_pdu_payload_size < peer_mtu ||
      kMaxOutboundPduPayloadSize < peer_mtu) {
    bt_log(DEBUG,
           "l2cap-bredr",
           "Channel %#.4x: reporting max TX SDU size of %hu to service to "
           "avoid segmenting outbound SDUs, which would otherwise be %hu "
           "according to MTU",
           local_cid(),
           max_tx_sdu_size,
           peer_mtu);
  }
  auto info =
      ChannelInfo::MakeEnhancedRetransmissionMode(max_rx_sdu_size,
                                                  max_tx_sdu_size,
                                                  n_frames_in_tx_window,
                                                  max_transmissions,
                                                  max_tx_pdu_payload_size,
                                                  psm(),
                                                  flush_timeout);
  return info;
}

void BrEdrDynamicChannel::OnRxConfigReq(
    uint16_t flags,
    ChannelConfiguration config,
    BrEdrCommandHandler::ConfigurationResponder* responder) {
  bool continuation = flags & kConfigurationContinuation;
  bt_log(TRACE,
         "l2cap-bredr",
         "Channel %#.4x: Got Configuration Request (C: %d, options: %s)",
         local_cid(),
         continuation,
         bt_str(config));

  if (!IsConnected()) {
    bt_log(WARN,
           "l2cap-bredr",
           "Channel %#.4x: Unexpected Configuration Request, state %x",
           local_cid(),
           state_);
    return;
  }

  // Reject configuration requests after channel is open because we don't
  // support re-negotiation.
  if (IsOpen()) {
    bt_log(WARN,
           "l2cap-bredr",
           "Channel %#.4x: Rejecting Configuration Request after channel open",
           local_cid());
    responder->Send(remote_cid(),
                    /*flags=*/0x0000,
                    ConfigurationResult::kRejected,
                    ChannelConfiguration::ConfigurationOptions());
    return;
  }

  // Reject request if it contains unknown options.
  // See Core Spec v5.1, Volume 3, Section 4.5: Configuration Options
  if (!config.unknown_options().empty()) {
    // This configuration transaction was a failure, so clear the accumulator.
    remote_config_accum_.reset();

    std::string unknown_string;
    ChannelConfiguration::ConfigurationOptions unknown_options;
    // Return just the first kMaxUnknownOptionsPerResponse unknown options.
    for (size_t i = 0; i < config.unknown_options().size() &&
                       i < kMaxUnknownOptionsPerResponse;
         i++) {
      const ChannelConfiguration::UnknownOption& option =
          config.unknown_options()[i];
      unknown_string += std::string(" ") + option.ToString();
      unknown_options.push_back(
          std::make_unique<ChannelConfiguration::UnknownOption>(option));
    }

    bt_log(DEBUG,
           "l2cap-bredr",
           "Channel %#.4x: config request contained unknown options (options: "
           "%s)\n",
           local_cid(),
           unknown_string.c_str());

    uint16_t rsp_flags = continuation ? kConfigurationContinuation : 0;
    responder->Send(remote_cid(),
                    rsp_flags,
                    ConfigurationResult::kUnknownOptions,
                    std::move(unknown_options));
    return;
  }

  // Always add options to accumulator, even if C = 0, for later code
  // simplicity.
  if (remote_config_accum_.has_value()) {
    remote_config_accum_->Merge(std::move(config));
  } else {
    // TODO(fxbug.dev/42115983): if channel is being re-configured, merge with
    // existing configuration
    remote_config_accum_ = std::move(config);
  }

  if (continuation) {
    // keep responding with success until all options have been received (C flag
    // is 0)
    responder->Send(remote_cid(),
                    /*flags=*/kConfigurationContinuation,
                    ConfigurationResult::kSuccess,
                    ChannelConfiguration::ConfigurationOptions());
    bt_log(TRACE,
           "l2cap-bredr",
           "Channel %#.4x: Sent Configuration Response (C: 1)",
           local_cid());
    return;
  }

  auto req_config = std::exchange(remote_config_accum_, std::nullopt).value();

  const auto req_mode =
      req_config.retransmission_flow_control_option()
          ? req_config.retransmission_flow_control_option()->mode()
          : RetransmissionAndFlowControlMode::kBasic;

  // Record peer support for ERTM even if they haven't sent a Extended Features
  // Mask.
  if (req_mode == RetransmissionAndFlowControlMode::kEnhancedRetransmission) {
    // This can send a config request, which can fail and cause the channel to
    // be destroyed, in which case we should not proceed.
    if (!SetEnhancedRetransmissionSupport(true)) {
      return;
    }
  }

  // Set default config options if not already in request.
  if (!req_config.mtu_option()) {
    req_config.set_mtu_option(ChannelConfiguration::MtuOption(kDefaultMTU));
  }

  if (state_ & kRemoteConfigReceived) {
    // Disconnect if second configuration request does not contain desired mode.
    const auto local_mode =
        local_config_.retransmission_flow_control_option()->mode();
    if (req_mode != local_mode) {
      bt_log(WARN,
             "l2cap-bredr",
             "Channel %#.4x: second configuration request doesn't have desired "
             "mode, sending unacceptable parameters response and disconnecting "
             "(req mode: %#.2x, desired: %#.2x)",
             local_cid(),
             static_cast<uint8_t>(req_mode),
             static_cast<uint8_t>(local_mode));
      ChannelConfiguration::ConfigurationOptions options;
      options.push_back(
          std::make_unique<
              ChannelConfiguration::RetransmissionAndFlowControlOption>(
              *local_config().retransmission_flow_control_option()));
      responder->Send(remote_cid(),
                      0x0000,
                      ConfigurationResult::kUnacceptableParameters,
                      std::move(options));
      PassOpenError();
      return;
    }

    bt_log(DEBUG,
           "l2cap-bredr",
           "Channel %#.4x: Reconfiguring, state %x",
           local_cid(),
           state_);
  }

  state_ |= kRemoteConfigReceived;

  auto unacceptable_config = CheckForUnacceptableConfigReqOptions(req_config);
  auto unacceptable_options = unacceptable_config.Options();
  if (!unacceptable_options.empty()) {
    responder->Send(remote_cid(),
                    /*flags=*/0x0000,
                    ConfigurationResult::kUnacceptableParameters,
                    std::move(unacceptable_options));
    bt_log(TRACE,
           "l2cap-bredr",
           "Channel %#.4x: Sent unacceptable parameters configuration response "
           "(options: %s)",
           local_cid(),
           bt_str(unacceptable_config));
    return;
  }

  // TODO(fxbug.dev/42057179): Defer accepting config req using a Pending
  // response
  state_ |= kRemoteConfigAccepted;

  ChannelConfiguration response_config;

  // Successful response should include actual MTU local device will use. This
  // must be min(received MTU, local outgoing MTU capability). Currently, we
  // accept any MTU.
  // TODO(fxbug.dev/42117452): determine the upper bound of what we are actually
  // capable of sending
  uint16_t actual_mtu = req_config.mtu_option()->mtu();
  response_config.set_mtu_option(ChannelConfiguration::MtuOption(actual_mtu));
  req_config.set_mtu_option(response_config.mtu_option());

  if (req_mode == RetransmissionAndFlowControlMode::kEnhancedRetransmission) {
    auto outbound_rfc_option = WriteRfcOutboundTimeouts(
        req_config.retransmission_flow_control_option().value());
    response_config.set_retransmission_flow_control_option(
        std::move(outbound_rfc_option));
  } else {
    response_config.set_retransmission_flow_control_option(
        req_config.retransmission_flow_control_option());
  }

  responder->Send(remote_cid(),
                  /*flags=*/0x0000,
                  ConfigurationResult::kSuccess,
                  response_config.Options());

  bt_log(TRACE,
         "l2cap-bredr",
         "Channel %#.4x: Sent Configuration Response (options: %s)",
         local_cid(),
         bt_str(response_config));

  // Save accepted options.
  remote_config_.Merge(std::move(req_config));

  if (!remote_config_.retransmission_flow_control_option()) {
    remote_config_.set_retransmission_flow_control_option(
        ChannelConfiguration::RetransmissionAndFlowControlOption::
            MakeBasicMode());
  }

  if (BothConfigsAccepted() && !AcceptedChannelModesAreConsistent()) {
    bt_log(WARN,
           "l2cap-bredr",
           "Channel %#.4x: inconsistent channel mode negotiation (local mode: "
           "%#.2x, remote "
           "mode: %#.2x); falling back to Basic Mode",
           local_cid(),
           static_cast<uint8_t>(
               local_config().retransmission_flow_control_option()->mode()),
           static_cast<uint8_t>(
               remote_config().retransmission_flow_control_option()->mode()));

    // The most applicable guidance for the case where the peer send conflicting
    // modes is in Core Spec v5.0 Vol 3, Part A, Sec 5.4: "If the mode proposed
    // by the remote device has a higher precedence (according to the state 1
    // precedence) then the algorithm will operate such that creation of a
    // channel using the remote device's mode has higher priority than
    // disconnecting the channel."
    //
    // Note also that, "In state 1, Basic L2CAP mode has the highest precedence
    // and shall take precedence over Enhanced Retransmission mode..."
    //
    // So, if we are to continue the connection, it makes the most sense to use
    // Basic Mode.
    local_config_.set_retransmission_flow_control_option(
        ChannelConfiguration::RetransmissionAndFlowControlOption::
            MakeBasicMode());
    remote_config_.set_retransmission_flow_control_option(
        ChannelConfiguration::RetransmissionAndFlowControlOption::
            MakeBasicMode());
    PassOpenResult();
    return;
  }

  if (IsOpen()) {
    set_opened();
    PassOpenResult();
  }
}

void BrEdrDynamicChannel::OnRxDisconReq(
    BrEdrCommandHandler::DisconnectionResponder* responder) {
  bt_log(TRACE,
         "l2cap-bredr",
         "Channel %#.4x: Got Disconnection Request",
         local_cid());

  // Unconnected channels only exist if they are waiting for a Connection
  // Response from the peer or are disconnected yet undestroyed for some reason.
  // Getting a Disconnection Request implies some error condition or misbehavior
  // but the reaction should still be to terminate this channel.
  if (!IsConnected()) {
    bt_log(WARN,
           "l2cap-bredr",
           "Channel %#.4x: Unexpected Disconnection Request",
           local_cid());
  }

  state_ |= kDisconnected;
  responder->Send();
  if (opened()) {
    OnDisconnected();
  } else {
    PassOpenError();
  }
}

void BrEdrDynamicChannel::CompleteInboundConnection(
    BrEdrCommandHandler::ConnectionResponder* responder) {
  bt_log(DEBUG,
         "l2cap-bredr",
         "Channel %#.4x: connected for PSM %#.4x from remote channel %#.4x",
         local_cid(),
         psm(),
         remote_cid());

  responder->Send(local_cid(),
                  ConnectionResult::kSuccess,
                  ConnectionStatus::kNoInfoAvailable);
  bt_log(TRACE,
         "l2cap-bredr",
         "Channel %#.4x: Sent Connection Response",
         local_cid());
  state_ |= kConnResponded;

  UpdateLocalConfigForErtm();
  if (!IsWaitingForPeerErtmSupport()) {
    std::ignore = TrySendLocalConfig();
  }
}

BrEdrDynamicChannel::BrEdrDynamicChannel(
    DynamicChannelRegistry* registry,
    SignalingChannelInterface* signaling_channel,
    Psm psm,
    ChannelId local_cid,
    ChannelId remote_cid,
    ChannelParameters params,
    std::optional<bool> peer_supports_ertm,
    pw::async::Dispatcher& dispatcher)
    : DynamicChannel(registry, psm, local_cid, remote_cid),
      signaling_channel_(signaling_channel),
      parameters_(params),
      state_(0u),
      peer_supports_ertm_(peer_supports_ertm),
      dispatcher_(dispatcher),
      weak_self_(this) {
  PW_DCHECK(signaling_channel_);
  PW_DCHECK(local_cid != kInvalidChannelId);

  UpdateLocalConfigForErtm();
}

void BrEdrDynamicChannel::PassOpenResult() {
  if (open_result_cb_) {
    // Guard against use-after-free if this object's owner destroys it while
    // running |open_result_cb_|.
    auto cb = std::move(open_result_cb_);
    cb();
  }
}

// This only checks that the channel had failed to open before passing to the
// client. The channel may still be connected, in case it's useful to perform
// channel configuration at this point.
void BrEdrDynamicChannel::PassOpenError() {
  PW_CHECK(!IsOpen());
  PassOpenResult();
}

void BrEdrDynamicChannel::UpdateLocalConfigForErtm() {
  local_config_.set_mtu_option(
      ChannelConfiguration::MtuOption(CalculateLocalMtu()));

  if (ShouldRequestEnhancedRetransmission()) {
    // Core Spec v5.0 Vol 3, Part A, Sec 8.6.2.1 "When configuring a channel
    // over an ACL-U logical link the values sent in a Configuration Request
    // packet for Retransmission timeout and Monitor timeout shall be 0."
    auto option = ChannelConfiguration::RetransmissionAndFlowControlOption::
        MakeEnhancedRetransmissionMode(
            /*tx_window_size=*/kErtmMaxUnackedInboundFrames,
            /*max_transmit=*/kErtmMaxInboundRetransmissions,
            /*rtx_timeout=*/0,
            /*monitor_timeout=*/0,
            /*mps=*/kMaxInboundPduPayloadSize);
    local_config_.set_retransmission_flow_control_option(option);
  } else {
    local_config_.set_retransmission_flow_control_option(
        ChannelConfiguration::RetransmissionAndFlowControlOption::
            MakeBasicMode());
  }
}

uint16_t BrEdrDynamicChannel::CalculateLocalMtu() const {
  const bool request_ertm = ShouldRequestEnhancedRetransmission();
  const auto kDefaultPreferredMtu =
      request_ertm ? kMaxInboundPduPayloadSize : kMaxMTU;
  uint16_t mtu = parameters_.max_rx_sdu_size.value_or(kDefaultPreferredMtu);
  if (mtu < kMinACLMTU) {
    bt_log(WARN,
           "l2cap-bredr",
           "Channel %#.4x: preferred MTU channel parameter below minimum "
           "allowed, using minimum "
           "instead (mtu param: %#x, min mtu: %#x)",
           local_cid(),
           mtu,
           kMinACLMTU);
    mtu = kMinACLMTU;
  }
  if (request_ertm && mtu > kMaxInboundPduPayloadSize) {
    bt_log(DEBUG,
           "l2cap-bredr",
           "Channel %#.4x: preferred MTU channel parameter above MPS; using "
           "MPS instead to avoid "
           "segmentation (mtu param: %#x, max pdu: %#x)",
           local_cid(),
           mtu,
           kMaxInboundPduPayloadSize);
    mtu = kMaxInboundPduPayloadSize;
  }
  return mtu;
}

bool BrEdrDynamicChannel::ShouldRequestEnhancedRetransmission() const {
  return parameters_.mode &&
         *parameters_.mode ==
             RetransmissionAndFlowControlMode::kEnhancedRetransmission &&
         peer_supports_ertm_.value_or(false);
}

bool BrEdrDynamicChannel::IsWaitingForPeerErtmSupport() {
  const auto local_mode =
      parameters_.mode.value_or(RetransmissionAndFlowControlMode::kBasic);
  return !peer_supports_ertm_.has_value() &&
         (local_mode != RetransmissionAndFlowControlMode::kBasic);
}

bool BrEdrDynamicChannel::TrySendLocalConfig() {
  if (state_ & kLocalConfigSent) {
    return true;
  }

  PW_CHECK(!IsWaitingForPeerErtmSupport());

  return SendLocalConfig();
}

bool BrEdrDynamicChannel::SendLocalConfig() {
  auto on_config_rsp_timeout = [this, self = weak_self_.GetWeakPtr()] {
    if (self.is_alive()) {
      bt_log(WARN,
             "l2cap-bredr",
             "Channel %#.4x: Timed out waiting for Configuration Response",
             local_cid());
      PassOpenError();
    }
  };

  BrEdrCommandHandler cmd_handler(signaling_channel_,
                                  std::move(on_config_rsp_timeout));

  auto request_config = local_config_;

  // Don't send Retransmission & Flow Control option for basic mode
  if (request_config.retransmission_flow_control_option()->mode() ==
      RetransmissionAndFlowControlMode::kBasic) {
    request_config.set_retransmission_flow_control_option(std::nullopt);
  }

  if (!request_config.retransmission_flow_control_option()) {
    num_basic_config_requests_++;
  }

  if (!cmd_handler.SendConfigurationRequest(
          remote_cid(),
          0,
          request_config.Options(),
          [self = weak_self_.GetWeakPtr()](auto& rsp) {
            if (self.is_alive()) {
              return self->OnRxConfigRsp(rsp);
            }
            return ResponseHandlerAction::kCompleteOutboundTransaction;
          })) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Channel %#.4x: Failed to send Configuration Request",
           local_cid());
    PassOpenError();
    return false;
  }

  bt_log(TRACE,
         "l2cap-bredr",
         "Channel %#.4x: Sent Configuration Request (options: %s)",
         local_cid(),
         bt_str(request_config));

  state_ |= kLocalConfigSent;
  return true;
}

bool BrEdrDynamicChannel::BothConfigsAccepted() const {
  return (state_ & kLocalConfigAccepted) && (state_ & kRemoteConfigAccepted);
}

bool BrEdrDynamicChannel::AcceptedChannelModesAreConsistent() const {
  PW_CHECK(BothConfigsAccepted());
  auto local_mode = local_config_.retransmission_flow_control_option()->mode();
  auto remote_mode =
      remote_config_.retransmission_flow_control_option()->mode();
  return local_mode == remote_mode;
}

ChannelConfiguration BrEdrDynamicChannel::CheckForUnacceptableConfigReqOptions(
    const ChannelConfiguration& config) const {
  // TODO(fxbug.dev/42115983): reject reconfiguring MTU if mode is Enhanced
  // Retransmission or Streaming mode.
  ChannelConfiguration unacceptable;

  // Reject MTUs below minimum size
  if (config.mtu_option()->mtu() < kMinACLMTU) {
    bt_log(DEBUG,
           "l2cap",
           "Channel %#.4x: config request contains MTU below minimum (mtu: "
           "%hu, min: %hu)",
           local_cid(),
           config.mtu_option()->mtu(),
           kMinACLMTU);
    // Respond back with a proposed MTU value of the required minimum (Core Spec
    // v5.1, Vol 3, Part A, Section 5.1: "It is implementation specific whether
    // the local device continues the configuration process or disconnects the
    // channel.")
    unacceptable.set_mtu_option(ChannelConfiguration::MtuOption(kMinACLMTU));
  }

  const auto req_mode =
      config.retransmission_flow_control_option()
          ? config.retransmission_flow_control_option()->mode()
          : RetransmissionAndFlowControlMode::kBasic;
  const auto local_mode =
      local_config().retransmission_flow_control_option()->mode();

  PW_MODIFY_DIAGNOSTICS_PUSH();
  PW_MODIFY_DIAGNOSTIC(ignored, "-Wswitch-enum");
  switch (req_mode) {
    case RetransmissionAndFlowControlMode::kBasic:
      // Local device must accept, as basic mode has highest precedence.
      if (local_mode ==
          RetransmissionAndFlowControlMode::kEnhancedRetransmission) {
        bt_log(DEBUG,
               "l2cap-bredr",
               "Channel %#.4x: accepting peer basic mode configuration option "
               "when preferred mode "
               "was ERTM",
               local_cid());
      }
      break;
    case RetransmissionAndFlowControlMode::kEnhancedRetransmission:
      // Basic mode has highest precedence, so if local mode is basic, reject
      // ERTM and send local mode.
      if (local_mode == RetransmissionAndFlowControlMode::kBasic) {
        bt_log(DEBUG,
               "l2cap-bredr",
               "Channel %#.4x: rejecting peer ERTM mode configuration option "
               "because preferred "
               "mode is basic",
               local_cid());
        unacceptable.set_retransmission_flow_control_option(
            ChannelConfiguration::RetransmissionAndFlowControlOption::
                MakeBasicMode());
        break;
      }
      unacceptable.set_retransmission_flow_control_option(
          CheckForUnacceptableErtmOptions(config));
      break;
    default:
      bt_log(DEBUG,
             "l2cap-bredr",
             "Channel %#.4x: rejecting unsupported retransmission and flow "
             "control configuration "
             "option (mode: %#.2x)",
             local_cid(),
             static_cast<uint8_t>(req_mode));

      // All other modes are lower precedence than what local device supports,
      // so send local mode.
      if (local_mode ==
          RetransmissionAndFlowControlMode::kEnhancedRetransmission) {
        // Retransmission & Flow Control fields for ERTM are not negotiable, so
        // do not propose acceptable values per Core Spec v5.0, Vol 3, Part A,
        // Sec 7.1.4.
        unacceptable.set_retransmission_flow_control_option(
            ChannelConfiguration::RetransmissionAndFlowControlOption::
                MakeEnhancedRetransmissionMode(
                    /*tx_window_size=*/0,
                    /*max_transmit=*/0,
                    /*rtx_timeout=*/0,
                    /*monitor_timeout=*/0,
                    /*mps=*/0));
      } else {
        unacceptable.set_retransmission_flow_control_option(
            local_config().retransmission_flow_control_option());
      }
  }
  PW_MODIFY_DIAGNOSTICS_POP();

  return unacceptable;
}

std::optional<ChannelConfiguration::RetransmissionAndFlowControlOption>
BrEdrDynamicChannel::CheckForUnacceptableErtmOptions(
    const ChannelConfiguration& config) const {
  PW_CHECK(config.retransmission_flow_control_option()->mode() ==
           RetransmissionAndFlowControlMode::kEnhancedRetransmission);
  PW_CHECK(local_config().retransmission_flow_control_option()->mode() ==
           RetransmissionAndFlowControlMode::kEnhancedRetransmission);

  std::optional<ChannelConfiguration::RetransmissionAndFlowControlOption>
      unacceptable_rfc_option;
  const auto& peer_rfc_option =
      config.retransmission_flow_control_option().value();

  // TxWindow has a range of 1 to 63 (Core Spec v5.0, Vol 3, Part A, Sec 5.4).
  if (peer_rfc_option.tx_window_size() < kErtmMinUnackedInboundFrames) {
    bt_log(DEBUG,
           "l2cap-bredr",
           "Channel %#.4x: rejecting too-small ERTM TxWindow of %hhu",
           local_cid(),
           peer_rfc_option.tx_window_size());
    unacceptable_rfc_option = unacceptable_rfc_option.value_or(peer_rfc_option);
    unacceptable_rfc_option->set_tx_window_size(kErtmMinUnackedInboundFrames);
  } else if (peer_rfc_option.tx_window_size() > kErtmMaxUnackedInboundFrames) {
    bt_log(DEBUG,
           "l2cap-bredr",
           "Channel %#.4x: rejecting too-small ERTM TxWindow of %hhu",
           local_cid(),
           peer_rfc_option.tx_window_size());
    unacceptable_rfc_option = unacceptable_rfc_option.value_or(peer_rfc_option);
    unacceptable_rfc_option->set_tx_window_size(kErtmMaxUnackedInboundFrames);
  }

  // NOTE(fxbug.dev/42054330): MPS must be large enough to fit the largest SDU
  // in the minimum MTU case, because ERTM does not segment in the outbound
  // direction.
  if (peer_rfc_option.mps() < kMinACLMTU) {
    bt_log(DEBUG,
           "l2cap-bredr",
           "Channel %#.4x: rejecting too-small ERTM MPS of %hu",
           local_cid(),
           peer_rfc_option.mps());
    unacceptable_rfc_option = unacceptable_rfc_option.value_or(peer_rfc_option);
    unacceptable_rfc_option->set_mps(kMinACLMTU);
  }

  return unacceptable_rfc_option;
}

void BrEdrDynamicChannel::TryRecoverFromUnacceptableParametersConfigRsp(
    const ChannelConfiguration& rsp_config) {
  // Check if channel mode was unacceptable.
  if (rsp_config.retransmission_flow_control_option()) {
    // Check if peer rejected basic mode. Do not disconnect, in case peer will
    // accept resending basic mode (as is the case with PTS test
    // L2CAP/COS/CFD/BV-02-C).
    if (local_config().retransmission_flow_control_option()->mode() ==
        RetransmissionAndFlowControlMode::kBasic) {
      bt_log(WARN,
             "l2cap-bredr",
             "Channel %#.4x: Peer rejected basic mode with unacceptable "
             "parameters result (rsp mode: %#.2x)",
             local_cid(),
             static_cast<uint8_t>(
                 rsp_config.retransmission_flow_control_option()->mode()));
    }

    // Core Spec v5.1, Vol 3, Part A, Sec 5.4:
    // If the mode in the remote device's negative Configuration Response does
    // not match the mode in the remote device's Configuration Request then the
    // local device shall disconnect the channel.
    if (state_ & kRemoteConfigAccepted) {
      const auto rsp_mode =
          rsp_config.retransmission_flow_control_option()->mode();
      const auto remote_mode =
          remote_config_.retransmission_flow_control_option()->mode();
      if (rsp_mode != remote_mode) {
        bt_log(ERROR,
               "l2cap-bredr",
               "Channel %#.4x: Unsuccessful config: mode in unacceptable "
               "parameters config "
               "response does not match mode in remote config request (rsp "
               "mode: %#.2x, req mode: "
               "%#.2x)",
               local_cid(),
               static_cast<uint8_t>(rsp_mode),
               static_cast<uint8_t>(remote_mode));
        PassOpenError();
        return;
      }
    }

    bt_log(TRACE,
           "l2cap-bredr",
           "Channel %#.4x: Attempting to recover from unacceptable parameters "
           "config response by falling back to basic mode and resending config "
           "request",
           local_cid());

    // Fall back to basic mode and try sending config again up to
    // kMaxNumBasicConfigRequests times
    peer_supports_ertm_ = false;
    if (num_basic_config_requests_ == kMaxNumBasicConfigRequests) {
      bt_log(WARN,
             "l2cap-bredr",
             "Channel %#.4x: Peer rejected config request. Channel's limit of "
             "%#.2x basic mode config request attempts has been met",
             local_cid(),
             kMaxNumBasicConfigRequests);
      PassOpenError();
      return;
    }
    UpdateLocalConfigForErtm();
    std::ignore = SendLocalConfig();
    return;
  }

  // Other unacceptable parameters cannot be recovered from.
  bt_log(WARN,
         "l2cap-bredr",
         "Channel %#.4x: Unsuccessful config: could not recover from "
         "unacceptable parameters config "
         "response",
         local_cid());
  PassOpenError();
}

BrEdrDynamicChannel::ResponseHandlerAction BrEdrDynamicChannel::OnRxConnRsp(
    const BrEdrCommandHandler::ConnectionResponse& rsp) {
  if (rsp.status() == BrEdrCommandHandler::Status::kReject) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Channel %#.4x: Connection Request rejected reason %#.4hx",
           local_cid(),
           static_cast<unsigned short>(rsp.reject_reason()));
    PassOpenError();
    return ResponseHandlerAction::kCompleteOutboundTransaction;
  }

  if (rsp.local_cid() != local_cid()) {
    bt_log(
        ERROR,
        "l2cap-bredr",
        "Channel %#.4x: Got Connection Response for another channel ID %#.4x",
        local_cid(),
        rsp.local_cid());
    PassOpenError();
    return ResponseHandlerAction::kCompleteOutboundTransaction;
  }

  if ((state_ & kConnResponded) || !(state_ & kConnRequested)) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Channel %#.4x: Unexpected Connection Response, state %#x",
           local_cid(),
           state_);
    PassOpenError();
    return ResponseHandlerAction::kCompleteOutboundTransaction;
  }

  if (conn_rsp_no_resources_timer_.is_pending() &&
      rsp.result() != ConnectionResult::kNoResources) {
    conn_rsp_no_resources_timer_.Cancel();
  }

  if (rsp.result() == ConnectionResult::kPending) {
    bt_log(TRACE,
           "l2cap-bredr",
           "Channel %#.4x: Remote is pending open, status %#.4hx",
           local_cid(),
           static_cast<unsigned short>(rsp.conn_status()));

    if (rsp.remote_cid() == kInvalidChannelId) {
      return ResponseHandlerAction::kExpectAdditionalResponse;
    }

    // If the remote provides a channel ID, then we store it. It can be used for
    // disconnection from this point forward.
    if (!SetRemoteChannelId(rsp.remote_cid())) {
      bt_log(ERROR,
             "l2cap-bredr",
             "Channel %#.4x: Remote channel ID %#.4x is not unique",
             local_cid(),
             rsp.remote_cid());
      PassOpenError();
      return ResponseHandlerAction::kCompleteOutboundTransaction;
    }

    bt_log(TRACE,
           "l2cap-bredr",
           "Channel %#.4x: Got remote channel ID %#.4x",
           local_cid(),
           remote_cid());
    return ResponseHandlerAction::kExpectAdditionalResponse;
  }

  if (rsp.result() != ConnectionResult::kSuccess) {
    // Workaround for Apple interop bug (e.g. AirPods 4 AVDTP, Apple RFCOMM):
    // when opening a channel, the device initially responds with "Refused - no
    // resources available", but subsequently sends "Pending" and then "Success"
    // responses. Wait for a short duration instead of failing the connection
    // immediately.
    // TODO: https://fxbug.dev/477697118 - Remove this once Apple fixes the
    // firmware bug.
    if (rsp.result() == ConnectionResult::kNoResources &&
        (psm() == kAVDTP || psm() == kRFCOMM)) {
      bt_log(WARN,
             "l2cap-bredr",
             "Channel %#.4x: Connection Response indicates 'no resources', "
             "but continuing to wait due to interop bug",
             local_cid());
      if (!conn_rsp_no_resources_timer_.is_pending()) {
        conn_rsp_no_resources_timer_.set_function([self =
                                                       weak_self_.GetWeakPtr()](
                                                      pw::async::
                                                          Context& /*ctx*/,
                                                      pw::Status status) {
          if (self.is_alive() && status.ok()) {
            bt_log(WARN,
                   "l2cap-bredr",
                   "Channel %#.4x: Timed out waiting for subsequent Connection "
                   "Responses after 'no resources' workaround",
                   self->local_cid());
            self->PassOpenError();
          }
        });
        conn_rsp_no_resources_timer_.PostAfter(
            kNoResourcesResponseTimeoutDuration);
      }
      return ResponseHandlerAction::kExpectAdditionalResponse;
    }

    bt_log(ERROR,
           "l2cap-bredr",
           "Channel %#.4x: Unsuccessful Connection Response result %#.4hx, "
           "status %#.4x",
           local_cid(),
           static_cast<unsigned short>(rsp.result()),
           static_cast<unsigned int>(rsp.status()));
    PassOpenError();
    return ResponseHandlerAction::kCompleteOutboundTransaction;
  }

  if (rsp.remote_cid() < kFirstDynamicChannelId) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Channel %#.4x: received Connection Response with invalid channel "
           "ID %#.4x, disconnecting",
           local_cid(),
           remote_cid());

    // This results in sending a Disconnection Request for non-zero remote IDs,
    // which is probably what we want because there's no other way to send back
    // a failure in this case.
    PassOpenError();
    return ResponseHandlerAction::kCompleteOutboundTransaction;
  }

  // TODO(xow): To be stricter, we can disconnect if the remote ID changes on us
  // during connection like this, but not sure if that would be beneficial.
  if (remote_cid() != kInvalidChannelId && remote_cid() != rsp.remote_cid()) {
    bt_log(WARN,
           "l2cap-bredr",
           "Channel %#.4x: using new remote ID %#.4x after previous Connection "
           "Response provided %#.4x",
           local_cid(),
           rsp.remote_cid(),
           remote_cid());
  }

  state_ |= kConnResponded;

  if (!SetRemoteChannelId(rsp.remote_cid())) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Channel %#.4x: Remote channel ID %#.4x is not unique",
           local_cid(),
           rsp.remote_cid());
    PassOpenError();
    return ResponseHandlerAction::kCompleteOutboundTransaction;
  }

  bt_log(TRACE,
         "l2cap-bredr",
         "Channel %#.4x: Got remote channel ID %#.4x",
         local_cid(),
         rsp.remote_cid());

  UpdateLocalConfigForErtm();
  if (!IsWaitingForPeerErtmSupport()) {
    std::ignore = TrySendLocalConfig();
  }
  return ResponseHandlerAction::kCompleteOutboundTransaction;
}

BrEdrDynamicChannel::ResponseHandlerAction BrEdrDynamicChannel::OnRxConfigRsp(
    const BrEdrCommandHandler::ConfigurationResponse& rsp) {
  if (rsp.status() == BrEdrCommandHandler::Status::kReject) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Channel %#.4x: Configuration Request rejected, reason %#.4hx, "
           "disconnecting",
           local_cid(),
           static_cast<unsigned short>(rsp.reject_reason()));

    // Configuration Request being rejected is fatal because the remote is not
    // trying to negotiate parameters (any more).
    PassOpenError();
    return ResponseHandlerAction::kCompleteOutboundTransaction;
  }

  // Pending responses may contain return values and adjustments to
  // non-negotiated values.
  if (rsp.result() == ConfigurationResult::kPending) {
    bt_log(TRACE,
           "l2cap-bredr",
           "Channel %#.4x: remote pending config (options: %s)",
           local_cid(),
           bt_str(rsp.config()));

    if (rsp.config().mtu_option()) {
      local_config_.set_mtu_option(rsp.config().mtu_option());
    }

    return ResponseHandlerAction::kExpectAdditionalResponse;
  }

  if (rsp.result() == ConfigurationResult::kUnacceptableParameters) {
    bt_log(DEBUG,
           "l2cap-bredr",
           "Channel %#.4x: Received unacceptable parameters config response "
           "(options: %s)",
           local_cid(),
           bt_str(rsp.config()));
    TryRecoverFromUnacceptableParametersConfigRsp(rsp.config());
    return ResponseHandlerAction::kCompleteOutboundTransaction;
  }

  if (rsp.result() != ConfigurationResult::kSuccess) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Channel %#.4x: unsuccessful config (result: %#.4hx, options: %s)",
           local_cid(),
           static_cast<unsigned short>(rsp.result()),
           bt_str(rsp.config()));
    PassOpenError();
    return ResponseHandlerAction::kCompleteOutboundTransaction;
  }

  if (rsp.local_cid() != local_cid()) {
    bt_log(ERROR,
           "l2cap-bredr",
           "Channel %#.4x: dropping Configuration Response for %#.4x",
           local_cid(),
           rsp.local_cid());
    PassOpenError();
    return ResponseHandlerAction::kCompleteOutboundTransaction;
  }

  state_ |= kLocalConfigAccepted;

  bt_log(TRACE,
         "l2cap-bredr",
         "Channel %#.4x: Got Configuration Response (options: %s)",
         local_cid(),
         bt_str(rsp.config()));

  if (rsp.config().mtu_option()) {
    local_config_.set_mtu_option(rsp.config().mtu_option());
  }

  if (BothConfigsAccepted() && !AcceptedChannelModesAreConsistent()) {
    bt_log(WARN,
           "l2cap-bredr",
           "Channel %#.4x: inconsistent channel mode negotiation (local mode: "
           "%#.2x, remote "
           "mode: %#.2x); falling back to Basic Mode",
           local_cid(),
           static_cast<uint8_t>(
               local_config().retransmission_flow_control_option()->mode()),
           static_cast<uint8_t>(
               remote_config().retransmission_flow_control_option()->mode()));

    // See spec justification in OnRxConfigReq.
    local_config_.set_retransmission_flow_control_option(
        ChannelConfiguration::RetransmissionAndFlowControlOption::
            MakeBasicMode());
    remote_config_.set_retransmission_flow_control_option(
        ChannelConfiguration::RetransmissionAndFlowControlOption::
            MakeBasicMode());
    PassOpenResult();
    return ResponseHandlerAction::kCompleteOutboundTransaction;
  }

  if (IsOpen()) {
    set_opened();
    PassOpenResult();
  }

  return ResponseHandlerAction::kCompleteOutboundTransaction;
}

bool BrEdrDynamicChannel::SetEnhancedRetransmissionSupport(bool supported) {
  peer_supports_ertm_ = supported;

  UpdateLocalConfigForErtm();

  // Don't send local config before connection response.
  if (state_ & kConnResponded) {
    return TrySendLocalConfig();
  }
  return true;
}

}  // namespace bt::l2cap::internal
