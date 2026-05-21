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

#include <cstddef>
#include <cstdint>

#include "pw_bluetooth_proxy/config.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_bluetooth_proxy/internal/mutex.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_multibuf/multibuf.h"
#include "pw_status/status.h"
#include "pw_sync/lock_annotations.h"

#if PW_BLUETOOTH_PROXY_ASYNC == 0
#include "pw_bluetooth_proxy/internal/generic_l2cap_channel_sync.h"
#else
#include "pw_bluetooth_proxy/internal/generic_l2cap_channel_async.h"
#endif  // PW_BLUETOOTH_PROXY_ASYNC

namespace pw::bluetooth::proxy {

class L2capChannelManager;

namespace internal {

/// Common base class for client-facing L2CAP channels.
///
/// L2CAP channels are implemented using two types: an "internal" type that is
/// and managed by the L2capChannelManager, and a client-facing channel that
/// provides the public channel interface.
class GenericL2capChannel {
 public:
  GenericL2capChannel(const GenericL2capChannel& other) = delete;
  GenericL2capChannel& operator=(const GenericL2capChannel& other) = delete;

  GenericL2capChannel(GenericL2capChannel&& other) = default;
  GenericL2capChannel& operator=(GenericL2capChannel&& other) = default;

  virtual ~GenericL2capChannel() = default;

  /// Completes initialization of the connection to the internal channel.
  Status Init() { return impl_.Init(); }

  /// Starts the L2CAP channel running.
  void Start() { impl_.Start(); }

  /// Send a payload to the remote peer.
  ///
  /// @param[in] payload The client payload to be sent. Payload will be
  /// destroyed once its data has been used.
  ///
  /// @returns A `StatusWithMultiBuf` with one of the statuses below. If status
  /// is not @OK then payload is also returned in `StatusWithMultiBuf`.
  /// * @OK: Packet was successfully queued for send.
  /// * @UNAVAILABLE: Channel could not acquire the resources to queue
  ///   the send at this time (transient error). If an `event_fn` has been
  ///   provided it will be called with `L2capChannelEvent::kWriteAvailable`
  ///   when there is queue space available again.
  /// * @INVALID_ARGUMENT: Payload is too large.
  /// * @FAILED_PRECONDITION: Channel is not `State::kRunning`.
  /// * @UNIMPLEMENTED: Channel does not support `Write(MultiBuf)`.
  StatusWithMultiBuf Write(multibuf::MultiBuf&& payload);

  /// Determine if channel is ready to accept one or more Write payloads.
  ///
  /// @returns
  /// * @OK: Channel is ready to accept one or more `Write` payloads.
  /// * @UNAVAILABLE: Channel does not yet have the resources to queue a Write
  ///   at this time (transient error). If an `event_fn` has been provided it
  ///   will be called with `L2capChannelEvent::kWriteAvailable` when there is
  ///   queue space available again.
  /// * @FAILED_PRECONDITION: Channel is not `State::kRunning`.
  Status IsWriteAvailable() { return impl_.IsWriteAvailable(); }

  static constexpr size_t QueueCapacity() {
    return L2capChannel::QueueCapacity();
  }

  uint16_t local_cid() const { return local_cid_; }

  uint16_t remote_cid() const { return remote_cid_; }

  uint16_t connection_handle() const { return connection_handle_; }

  AclTransportType transport() const { return transport_; }

  // Stop the channel.
  //
  // This typically is only used in internal tests.
  void Stop() { return impl_.Stop(); }

  // Close the channel.
  //
  // This typically is only used in internal tests.
  //
  // Unlike `L2capChannel::Close`, this will send an event to the client while
  // holding the lock to ensure the channel isn't concurrently removed.
  void Close() { return impl_.Close(); }

  /// Returns the internal channel. Unsafe; only meant for tests. DO NOT USE.
  L2capChannel* InternalForTesting() const {
    return impl_.InternalForTesting();
  }

 protected:
  explicit GenericL2capChannel(L2capChannel& channel);

  /// Send an L2CAP_FLOW_CONTROL_CREDIT_IND signaling packet to dispense the
  /// remote peer additional L2CAP connection-oriented channel credits for this
  /// channel.
  ///
  /// @param[in] additional_rx_credits Number of credits to dispense.
  ///
  /// @returns
  /// * @OK: The packet was sent.
  /// * @UNAVAILABLE: Send could not be queued due to lack of memory in the
  ///   client-provided rx_multibuf_allocator (transient error).
  /// * @FAILED_PRECONDITION: Channel is not `State::kRunning`.
  Status SendAdditionalRxCredits(uint16_t additional_rx_credits) {
    return impl_.SendAdditionalRxCredits(additional_rx_credits);
  }

 private:
  friend class GenericL2capChannelImpl;
  friend class L2capChannelImpl;
  friend class pw::bluetooth::proxy::L2capChannel;
  friend class pw::bluetooth::proxy::L2capChannelManager;

  /// Check if the passed Write parameter is acceptable.
  virtual Status DoCheckWriteParameter(const multibuf::MultiBuf& payload) = 0;

  // ACL connection handle.
  uint16_t connection_handle_ = 0;

  AclTransportType transport_ = AclTransportType::kBrEdr;

  // L2CAP channel ID of local endpoint.
  uint16_t local_cid_ = 0;

  // L2CAP channel ID of remote endpoint.
  uint16_t remote_cid_ = 0;

  // Implementation-specific details that may vary between sync and async modes.
  mutable GenericL2capChannelImpl impl_;
};

}  // namespace internal
}  // namespace pw::bluetooth::proxy
