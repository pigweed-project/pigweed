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
#include <pw_async/dispatcher.h>

#include <unordered_map>

#include "pw_bluetooth_sapphire/internal/host/common/smart_task.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/bredr_command_handler.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/dynamic_channel_registry.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/signaling_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"

namespace bt::l2cap::internal {

// Implements factories for BR/EDR dynamic channels and dispatches incoming
// signaling channel requests to the corresponding channels by local ID.
//
// Must be run only on the L2CAP thread.
class BrEdrDynamicChannelRegistry final : public DynamicChannelRegistry {
 public:
  BrEdrDynamicChannelRegistry(SignalingChannelInterface* sig,
                              DynamicChannelCallback close_cb,
                              ServiceRequestCallback service_request_cb,
                              bool random_channel_ids,
                              pw::async::Dispatcher& dispatcher);
  ~BrEdrDynamicChannelRegistry() override = default;

  std::optional<ExtendedFeatures> extended_features() {
    return extended_features_;
  }

 private:
  // DynamicChannelRegistry override
  DynamicChannelPtr MakeOutbound(Psm psm,
                                 ChannelId local_cid,
                                 ChannelParameters params) override;
  DynamicChannelPtr MakeInbound(Psm psm,
                                ChannelId local_cid,
                                ChannelId remote_cid,
                                ChannelParameters params) override;

  // Signaling channel request handlers
  void OnRxConnReq(Psm psm,
                   ChannelId remote_cid,
                   BrEdrCommandHandler::ConnectionResponder* responder);
  void OnRxConfigReq(ChannelId local_cid,
                     uint16_t flags,
                     ChannelConfiguration config,
                     BrEdrCommandHandler::ConfigurationResponder* responder);
  void OnRxDisconReq(ChannelId local_cid,
                     ChannelId remote_cid,
                     BrEdrCommandHandler::DisconnectionResponder* responder);
  void OnRxInfoReq(InformationType type,
                   BrEdrCommandHandler::InformationResponder* responder);

  // Signaling channel response handlers
  void OnRxExtendedFeaturesInfoRsp(
      const BrEdrCommandHandler::InformationResponse& rsp);

  // Send extended features information request.
  // TODO(fxbug.dev/42174624): Send fixed channels information request.
  void SendInformationRequests();

  // If an extended features information response has been received, returns the
  // value of the ERTM bit in the peer's feature mask.
  std::optional<bool> PeerSupportsERTM() const;

  using State = uint8_t;
  enum StateBit : State {
    // Extended Features Information Request (transmitted from local to remote)
    kExtendedFeaturesSent = (1 << 0),

    // Extended Features Information Response (transmitted from remote to local)
    kExtendedFeaturesReceived = (1 << 1),
  };

  // Bit field assembled using the bit masks above.
  State state_;

  SignalingChannelInterface* const sig_;

  std::optional<ExtendedFeatures> extended_features_;

  pw::async::Dispatcher& dispatcher_;
};

class BrEdrDynamicChannel;
using BrEdrDynamicChannelPtr = std::unique_ptr<BrEdrDynamicChannel>;

// Creates, configures, and tears down dynamic channels using the BR/EDR
// signaling channel. The lifetime of this object matches that of the channel
// itself: created in order to start an outbound channel or in response to an
// inbound channel request, then destroyed immediately after the channel is
// closed. This is intended to be created and owned by
// BrEdrDynamicChannelRegistry.
//
// This implements the state machine described by v5.0 Vol 3 Part A Sec 6. The
// state of OPEN ("user data transfer state") matches the implementation of the
// |DynamicChannel::IsOpen()|.
//
// Channel Configuration design:
// Implementation-defined behavior:
// * Inbound and outbound configuration requests/responses are exchanged
// simultaneously
// * If the desired channel mode is ERTM, the configuration request is not sent
// until the extended
//   features mask is received from the peer.
//   * If the peer doesn't support ERTM, the local device will negotiate Basic
//   Mode instead of ERTM
// * after both configuration requests have been accepted, if they are
// inconsistent, the channel
//   is disconnected (this can happen if the peer doesn't follow the spec)
// Configuration Request Handling:
// * when the peer requests an MTU below the minumum, send an Unacceptable
// Parameters response
//     suggesting the minimum MTU.
// * allow peer to send a maximum of 2 configuration requests with undesired
// channel modes
//   before disconnecting
// * reject all channel modes other than Basic Mode and ERTM
// Negative Configuration Response Handling:
// A maximum of 2 negotiation attempts will be made before disconnecting,
// according to the following rules:
// * if the response does not contain the Retransmission & Flow Control option,
// disconnect
// * when the response specifies a different channel mode than the peer sent in
// a configuration
//   request, disconnect
// * when the response rejected Basic Mode, disconnect
// * otherwise, send a second configuration request with Basic Mode
//
// Must be run only on the L2CAP thread.
class BrEdrDynamicChannel final : public DynamicChannel {
 public:
  using ResponseHandlerAction = SignalingChannel::ResponseHandlerAction;

  static BrEdrDynamicChannelPtr MakeOutbound(
      DynamicChannelRegistry* registry,
      SignalingChannelInterface* signaling_channel,
      Psm psm,
      ChannelId local_cid,
      ChannelParameters params,
      std::optional<bool> peer_supports_ertm,
      pw::async::Dispatcher& dispatcher);

  static BrEdrDynamicChannelPtr MakeInbound(
      DynamicChannelRegistry* registry,
      SignalingChannelInterface* signaling_channel,
      Psm psm,
      ChannelId local_cid,
      ChannelId remote_cid,
      ChannelParameters params,
      std::optional<bool> peer_supports_ertm,
      pw::async::Dispatcher& dispatcher);

  // DynamicChannel overrides
  ~BrEdrDynamicChannel() override = default;

  void Open(fit::closure open_cb) override;

  // Mark this channel as closed and disconnected. Send a Disconnection Request
  // to the peer if possible (peer had sent an ID for its endpoint). |done_cb|
  // will be called when Disconnection Response is received or if channel is
  // already not connected.
  void Disconnect(DisconnectDoneCallback done_cb) override;

  bool IsConnected() const override;
  bool IsOpen() const override;

  // Must not be called until channel is open.
  ChannelInfo info() const override;

  // Inbound request handlers. Request must have a destination channel ID that
  // matches this instance's |local_cid|.
  void OnRxConfigReq(uint16_t flags,
                     ChannelConfiguration config,
                     BrEdrCommandHandler::ConfigurationResponder* responder);
  void OnRxDisconReq(BrEdrCommandHandler::DisconnectionResponder* responder);

  // Called when the peer indicates whether it supports Enhanced Retransmission
  // Mode. Kicks off the configuration process if the preferred channel mode is
  // ERTM.
  [[nodiscard]] bool SetEnhancedRetransmissionSupport(bool supported);

  // Reply with affirmative connection response and begin configuration.
  void CompleteInboundConnection(
      BrEdrCommandHandler::ConnectionResponder* responder);

  // Contains options configured by remote configuration requests (Core Spec
  // v5.1, Vol 3, Part A, Sections 5 and 7.1.1).
  const ChannelConfiguration& remote_config() const { return remote_config_; }

  // Contains options configured by local configuration requests (Core Spec
  // v5.1, Vol 3, Part A, Sections 5 and 7.1.2).
  const ChannelConfiguration& local_config() const { return local_config_; }

 private:
  // The channel configuration state is described in v5.0 Vol 3 Part A Sec 6 (in
  // particular in Fig. 6.2) as having numerous substates in order to capture
  // the different orders in which configuration packets may be transmitted. It
  // is implemented here as a bitfield where bits are set as each packet is
  // transmitted.
  //
  // The initial state for a channel prior to any signaling packets transmitted
  // is 0.
  using State = uint8_t;
  enum StateBit : State {
    // Connection Req (transmitted in either direction)
    kConnRequested = (1 << 0),

    // Connection Rsp (transmitted in opposite direction of Connection Req)
    kConnResponded = (1 << 1),

    // Configuration Req (transmitted from local to remote)
    kLocalConfigSent = (1 << 2),

    // Configuration Rsp (successful; transmitted from remote to local)
    kLocalConfigAccepted = (1 << 3),

    // Configuration Req (transmitted from remote to local)
    kRemoteConfigReceived = (1 << 4),

    // Configuration Rsp (successful; transmitted from local to remote)
    kRemoteConfigAccepted = (1 << 5),

    // Disconnection Req (transmitted in either direction)
    kDisconnected = (1 << 6),
  };

  // TODO(fxbug.dev/42182060): Add Extended Flow Specification steps (exchange &
  // controller configuration)

  BrEdrDynamicChannel(DynamicChannelRegistry* registry,
                      SignalingChannelInterface* signaling_channel,
                      Psm psm,
                      ChannelId local_cid,
                      ChannelId remote_cid,
                      ChannelParameters params,
                      std::optional<bool> peer_supports_ertm,
                      pw::async::Dispatcher& pw_dispatcher);

  // Deliver the result of channel connection and configuration to the |Open|
  // originator. Can be called multiple times but only the first invocation
  // passes the result.
  void PassOpenResult();

  // Error during channel connection or configuration (before it is open).
  // Deliver the error open result to the |Open| originator. Disconnect the
  // remote endpoint of the channel if possible. If the channel is already open,
  // use |Disconnect| instead.
  void PassOpenError();

  // Overwrites the local configuration's MTU and Retransmission & Flow Control
  // options using ChannelParameters provided by the local service and the
  // peer's support for Enhanced Retransmission Mode (set using
  // SetEnhancedRetransmissionSupport).
  void UpdateLocalConfigForErtm();

  // Returns the maximum outbound SDU size based on service preference provided
  // in ChannelParameters, specification limits, and ERTM transmission
  // constraints.
  uint16_t CalculateLocalMtu() const;

  // Returns true if the local service prefers ERTM and the peer has indicated
  // support for ERTM.
  bool ShouldRequestEnhancedRetransmission() const;

  // Returns true if the preferred channel parameters require waiting for an
  // extended features information response and the response has not yet been
  // received. Must be false before sending local config.
  bool IsWaitingForPeerErtmSupport();

  // Begin the local channel Configuration Request flow if it has not yet
  // happened. The channel must not be waiting for the extended features info
  // response.
  // Returns true if the local config was successfully sent, false if there was
  // an error. On an error, the error will already have been reported and the
  // caller should return and not attempt to dereference `this`.
  [[nodiscard]] bool TrySendLocalConfig();

  // Send local configuration request, bypassing all checks.
  // Returns true if the local config was successfully sent, false if there was
  // an error. On an error, the error will already have been reported and the
  // caller should return and not attempt to dereference `this`.
  [[nodiscard]] bool SendLocalConfig();

  // Returns true if both the remote and local configs have been accepted.
  bool BothConfigsAccepted() const;

  // Returns true if negotiated channel modes are consistent. Must not be called
  // until after both configs have been accepted (|BothConfigsAccepted()| is
  // true).
  [[nodiscard]] bool AcceptedChannelModesAreConsistent() const;

  // Checks options in a configuration request for unacceptable MTU and
  // Retransmission and Flow Control options. Returns a configuration object
  // where for each unacceptable option, there is a corresponding option with a
  // value that would have been accepted if sent in the original request.
  [[nodiscard]] ChannelConfiguration CheckForUnacceptableConfigReqOptions(
      const ChannelConfiguration& config) const;

  // Checks parameters for Enhanced Retransmission Mode in the Retransmission &
  // Flow Control and if there are any unacceptable values, returns an option
  // containing values that would have been accepted. Otherwise, returns
  // std::nullopt.
  [[nodiscard]] std::optional<
      ChannelConfiguration::RetransmissionAndFlowControlOption>
  CheckForUnacceptableErtmOptions(const ChannelConfiguration& config) const;

  // Try to recover from a configuration response with the "Unacceptable
  // Parameters" result, signaling an error on failure.
  void TryRecoverFromUnacceptableParametersConfigRsp(
      const ChannelConfiguration& config);

  // Response handlers for outbound requests
  ResponseHandlerAction OnRxConnRsp(
      const BrEdrCommandHandler::ConnectionResponse& rsp);
  ResponseHandlerAction OnRxConfigRsp(
      const BrEdrCommandHandler::ConfigurationResponse& rsp);

  SignalingChannelInterface* const signaling_channel_;

  // Preferred parameters from the local service.
  const ChannelParameters parameters_;

  // Bit field assembled using the bit masks above. When zero, it represents a
  // closed (i.e. not yet open) channel.
  State state_;

  // This shall be reset to nullptr after invocation to enforce its single-use
  // semantics. See |DynamicChannel::Open| for details.
  fit::closure open_result_cb_;

  // Support for ERTM is indicated in the peer's extended features mask,
  // received in the extended features information response. Since the response
  // may not yet have been received when this channel is created, this value may
  // be assigned in either the constructor or in the
  // |SetEnhancedRetransmissionSupport| callback. This will be set to false if
  // the peer rejects ERTM during negotiation and set to true if the peer
  // requests ERTM before responding to the extended features information
  // request.
  std::optional<bool> peer_supports_ertm_;

  // Contains options configured by remote configuration requests (Core Spec
  // v5.1, Vol 3, Part A, Sections 5 and 7.1.1).
  ChannelConfiguration remote_config_;

  // Each peer configuration request may be split into multiple requests with
  // the continuation flag (C) set to 1. This variable accumulates the options
  // received in each request until C is 0. This is not simply done with
  // |remote_config_| because the previous value of |remote_config_| may be
  // accessed during this process, or the final request may be rejected. This
  // variable is nullopt when no sequence of continuation requests is being
  // processed.
  std::optional<ChannelConfiguration> remote_config_accum_;

  // Contains options configured by local configuration requests (Core Spec
  // v5.1, Vol 3, Part A, Sections 5 and 7.1.2).
  ChannelConfiguration local_config_;

  pw::async::Dispatcher& dispatcher_;

  // Timer set after receiving a kNoResources connection response.
  SmartTask conn_rsp_no_resources_timer_{dispatcher_};

  // Timer set during the configuration phase to prevent indefinite parking of
  // half-open channels.
  SmartTask config_timer_{dispatcher_};

  // Counter for number of basic mode configuration requests
  uint8_t num_basic_config_requests_ = 0;

  WeakSelf<BrEdrDynamicChannel> weak_self_;
};

}  // namespace bt::l2cap::internal
