// Copyright 2024 The Pigweed Authors
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

#include "pw_bluetooth/l2cap_frames.emb.h"
#include "pw_bluetooth_proxy/direction.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_bluetooth_proxy/internal/mutex.h"
#include "pw_bluetooth_proxy/l2cap_status_delegate.h"
#include "pw_containers/vector.h"
#include "pw_multibuf/multibuf.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"

namespace pw::bluetooth::proxy {

class L2capChannelManager;

// Interface for L2CAP signaling channels, which can be either ACL-U signaling
// channels or LE-U signaling channels.
//
// Write and Read payloads are L2CAP signal commands.
class L2capSignalingChannel final {
 public:
  explicit L2capSignalingChannel(L2capChannelManager& l2cap_channel_manager);

  L2capSignalingChannel(L2capSignalingChannel&&) = delete;
  L2capSignalingChannel& operator=(L2capSignalingChannel&& other) = delete;

  // Initializes the link and its underlying basic channel.
  Status Init(uint16_t connection_handle, AclTransportType transport);

  // Process the payload of a CFrame. Returns true if the CFrame was consumed by
  // the channel. Otherwise, returns false and the PDU containing this CFrame
  // will be forwarded on by the ProxyHost.
  bool OnCFramePayload(Direction direction,
                       pw::span<const uint8_t> cframe_payload);

  // Process an individual signaling command.
  //
  // Returns false if the command is not processed, either because it is not
  // directed to a channel managed by `L2capChannelManager` or because we do
  // not listen for that type of command.
  bool HandleL2capSignalingCommand(Direction direction,
                                   emboss::L2capSignalingCommandView cmd);

  // Handle L2CAP_CONNECTION_REQ.
  void HandleConnectionReq(Direction direction,
                           emboss::L2capConnectionReqView cmd);

  // Handle L2CAP_CONNECTION_RSP.
  void HandleConnectionRsp(Direction direction,
                           emboss::L2capConnectionRspView cmd);

  // Handle L2CAP_CONFIGURATION_REQ
  void HandleConfigurationReq(Direction direction,
                              emboss::L2capConfigureReqView cmd);

  void HandleConfigurationRsp(Direction direction,
                              emboss::L2capConfigureRspView cmd);

  // Handle L2CAP_DISCONNECTION_REQ.
  void HandleDisconnectionReq(Direction direction,
                              emboss::L2capDisconnectionReqView cmd);

  // Handle L2CAP_DISCONNECTION_RSP.
  void HandleDisconnectionRsp(Direction direction,
                              emboss::L2capDisconnectionRspView cmd);

  // Handle L2CAP_FLOW_CONTROL_CREDIT_IND.
  //
  // Returns false if the packet is invalid or not directed to a channel managed
  // by `L2capChannelManager`.
  bool HandleFlowControlCreditInd(emboss::L2capFlowControlCreditIndView cmd);

  // Send L2CAP_FLOW_CONTROL_CREDIT_IND to indicate local endpoint `cid` is
  // capable of receiving a number of additional K-frames (`credits`).
  //
  // @returns
  // * @OK: `L2CAP_FLOW_CONTROL_CREDIT_IND` was sent.
  // * @UNAVAILABLE: Send could not be queued due to lack of memory in the
  //   client-provided `multibuf_allocator` (transient error).
  // * @FAILED_PRECONDITION: Channel is not `State::kRunning`.
  Status SendFlowControlCreditInd(
      uint16_t cid,
      uint16_t credits,
      multibuf::MultiBufAllocator& multibuf_allocator);

  // Set the Identifier value to use for the next outbound signaling command.
  void SetNextIdentifier(uint8_t next_identifier) PW_LOCKS_EXCLUDED(mutex_);

  // Get the Identifier value that will be used for the next outbound signaling
  // command.
  uint8_t next_identifier() const PW_LOCKS_EXCLUDED(mutex_);

 private:
  struct PendingConnection {
    Direction direction;
    uint16_t source_cid;
    uint16_t psm;
  };

  struct PendingConfiguration {
    uint8_t identifier;
    L2capChannelConfigurationInfo info;
  };

  // Number of partially open l2cap connections the signaling channel can track.
  // These are kept open till the connection response comes through providing
  // the destination_cid to complete the connection info.
  static constexpr size_t kMaxPendingConnections = 10;

  // The maximum number of pending L2CAP configuration (inbound/outbound ).
  static constexpr size_t kMaxPendingConfigurations =
      2 * kMaxPendingConnections;

  // Process a C-frame payload.
  //
  // Returns false if the C-frame is to be forwarded on to the Bluetooth host,
  // either because the command is not directed towards a channel managed by
  // `L2capChannelManager` or because the C-frame is invalid and should be
  // handled by the Bluetooth host.
  bool HandlePayloadFromController(pw::span<uint8_t> payload);

  bool HandlePayloadFromHost(pw::span<uint8_t> payload);

  // Get the next Identifier value that should be written to a signaling
  // command and increment the Identifier.
  uint8_t GetNextIdentifierAndIncrement() PW_LOCKS_EXCLUDED(mutex_);

  L2capChannelManager& l2cap_channel_manager_;

  L2capChannel* channel_ = nullptr;

  // TODO(b/405190891): Properly clean-up pending_connections_ and
  // pending_configurations_
  Vector<PendingConnection, kMaxPendingConnections> pending_connections_
      PW_GUARDED_BY(mutex_){};

  Vector<PendingConfiguration, kMaxPendingConfigurations>
      pending_configurations_ PW_GUARDED_BY(mutex_){};

  mutable internal::Mutex mutex_;

  // Core Spec v6.0 Vol 3, Part A, Section 4: "The Identifier field is one octet
  // long and matches responses with requests. The requesting device sets this
  // field and the responding device uses the same value in its response. Within
  // each signaling channel a different Identifier shall be used for each
  // successive command. Following the original transmission of an Identifier in
  // a command, the Identifier may be recycled if all other Identifiers have
  // subsequently been used."
  // TODO: https://pwbug.dev/382553099 - Synchronize this value with AP host.
  uint8_t next_identifier_ PW_GUARDED_BY(mutex_) = 1;
};

}  // namespace pw::bluetooth::proxy
