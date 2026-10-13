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

#include <memory>

#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"

namespace bt::l2cap {

struct ChannelParameters;

namespace internal {

class DynamicChannelRegistry;

// Lifetime management and abstract command interface for opening and closing a
// dynamic L2CAP channel. This is an internal object representing an entry in
// and owned by DynamicChannelRegistry and does not implement the outward-facing
// l2cap::Channel interface.
//
// Opening a channel may take multiple steps before data packets can flow. This
// interface uses the terms "connected" and "open" to distinguish between
// connection creation ("connected" but not ready to transfer data) and
// establishment ("connected and open" when configuration completes and can
// transfer data).
//
// For channels opened on ACL-U links, the lifetime is described by Core Spec
// v5.0 Vol 3, Part A, Section 6, "State Machine."
//
// For channels opened on LE-U links, the lifetime of Credit Based Flow Control
// connection-oriented channels is not explicitly described, but operation is
// outlined in Core Spec v5.0 Vol 3, Part A, Section 4.22 to 4.24 "Signaling
// Packet Formats" and Section 10 "Procedures for Credit Based Flow Control."
//
// A channel is considered "not connected and not open" after disconnection by
// either endpoint. There is no "closed but not yet disconnected" state.
//
// This only drives the command transactions over a link's signaling channel to
// manage a specific channel and is not used to send or receive data over that
// channel (it exists in parallel with an bt::l2cap::Channel for that
// purpose). It is intended only to be run from the L2CAP thread.
class DynamicChannel {
 public:
  // Public dtor for testing (tests can own channels through a base pointer
  // without a DynamicChannelRegistry).
  virtual ~DynamicChannel() = default;

  // For outbound channels: begin a connection then configure the channel upon
  // connection. For inbound channels: configure the channel (it is already
  // connected if it exists).
  //
  // |open_result_cb| will be invoked exactly once, when the channel is ready
  // for user data transfer or if an error occurred during connection or
  // configuration. The caller must check |IsOpen| on this channel. If it's
  // false, this channel must be destroyed and not reused. Otherwise, the
  // channel is considered open.
  virtual void Open(fit::closure open_result_cb) = 0;

  // If connected, close the channel. |disconnect_callback| will be called when
  // the peer confirms that the channel is disconnected, if the channel is
  // already not connected, or if the underlying Signaling Channel times out
  // waiting for the peer confirmation. The owner should then destroy this
  // object and not reuse it.
  using DisconnectDoneCallback = fit::callback<void()>;
  virtual void Disconnect(DisconnectDoneCallback done_cb) = 0;

  // If true, both local and remote endpoints are connected and this instance
  // shall have valid and unique identifiers.
  virtual bool IsConnected() const = 0;

  // If true, this channel has been connected, has not been disconnected, and
  // can transfer data.
  virtual bool IsOpen() const = 0;

  // Parameters to use for the outward-facing l2cap::Channel implementation.
  virtual ChannelInfo info() const = 0;

  // Service identifier provided by the endpoint requesting the channel.
  Psm psm() const { return psm_; }

  // Identifies the local device's endpoint of this channel. Will be unique on
  // this device as long as this channel remains open.
  ChannelId local_cid() const { return local_cid_; }

  // Identifies the endpoint of this channel on the peer device. Set upon
  // connection completion.
  ChannelId remote_cid() const { return remote_cid_; }

  // True if the channel was ever opened (that is, if |IsOpen| was ever true and
  // |Open| provided that result to its caller). Used by DynamicChannelRegistry
  // to track channel closure cleanup.
  bool opened() const { return opened_; }

 protected:
  // |registry| points to the registry that created and owns this channel. It
  // must be valid for the duration of this object.
  DynamicChannel(DynamicChannelRegistry* registry,
                 Psm psm,
                 ChannelId local_cid,
                 ChannelId remote_cid);

  // Signal the registry of a remote-requested closure.
  void OnDisconnected();

  // Checks remote_cid for validity and uniqueness.
  // Returns true on success, false on error.
  // bool SetRemoteChannelId(ChannelId remote_cid);
  [[nodiscard]] bool SetRemoteChannelId(ChannelId remote_cid);

  DynamicChannelRegistry* registry() { return registry_; }

 private:
  // Only DynamicChannelRegistry may mark a channel as opened, at the moment it
  // hands the open channel to its caller. This keeps |opened_| in sync with
  // whether the upper layer owns the channel, so that a later remote
  // disconnection is always reported via |close_cb_|.
  friend class DynamicChannelRegistry;
  void set_opened() { opened_ = true; }

  // Must be valid for the duration of this object.
  DynamicChannelRegistry* const registry_;

  const Psm psm_;
  const ChannelId local_cid_;
  ChannelId remote_cid_;
  bool opened_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(DynamicChannel);
};

using DynamicChannelPtr = std::unique_ptr<DynamicChannel>;

}  // namespace internal
}  // namespace bt::l2cap
