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
#include <pw_async/heap_dispatcher.h>

#include <memory>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fragmenter.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"

namespace bt::l2cap::testing {

// FakeChannel is a simple pass-through Channel implementation that is intended
// for L2CAP service level unit tests where data is transmitted over a L2CAP
// channel.
class FakeChannel : public Channel {
 public:
  FakeChannel(ChannelId id,
              ChannelId remote_id,
              hci_spec::ConnectionHandle handle,
              bt::LinkType link_type,
              ChannelInfo info = ChannelInfo::MakeBasicMode(kDefaultMTU,
                                                            kDefaultMTU),
              uint16_t max_tx_queued = 1);
  ~FakeChannel() override = default;

  // Routes the given data over to the rx handler as if it were received from
  // the controller.
  void Receive(const ByteBuffer& data);

  // Sets a delegate to notify when a frame was sent over the channel.
  // If a |dispatcher| is specified, |callback| will be invoked asynchronously.
  using SendCallback = fit::function<void(ByteBufferPtr)>;
  void SetSendCallback(SendCallback callback);
  void SetSendCallback(SendCallback callback,
                       pw::async::Dispatcher& dispatcher);

  // Sets a callback to emulate the result of "SignalLinkError()". In
  // production, this callback is invoked by the link.
  void SetLinkErrorCallback(LinkErrorCallback callback);

  // Sets a callback to emulate the result of "UpgradeSecurity()".
  void SetSecurityCallback(SecurityUpgradeCallback callback,
                           pw::async::Dispatcher& dispatcher);

  // Emulates channel closure.
  void Close();

  using WeakPtr = WeakSelf<FakeChannel>::WeakPtr;
  FakeChannel::WeakPtr AsWeakPtr() { return weak_fake_chan_.GetWeakPtr(); }

  // Activating always fails if true.
  void set_activate_fails(bool value) { activate_fails_ = value; }

  // True if SignalLinkError() has been called.
  bool link_error() const { return link_error_; }

  // True if Deactivate has yet not been called after Activate.
  bool activated() const { return static_cast<bool>(rx_cb_); }

  // Assigns a link security level.
  void set_security(const sm::SecurityProperties& sec_props) {
    security_ = sec_props;
  }

  // RequestAclPriority always fails if true.
  void set_acl_priority_fails(bool fail) { acl_priority_fails_ = fail; }

  void set_flush_timeout_succeeds(bool succeed) {
    flush_timeout_succeeds_ = succeed;
  }

  using FlushTimeoutCallback = fit::function<void(
      pw::chrono::SystemClock::duration, hci::ResultCallback<>)>;
  void set_flush_timeout_callback(FlushTimeoutCallback cb) {
    flush_timeout_cb_ = std::move(cb);
  }

  // StartA2dpOffload() and StopA2dpOffload() fail with given |error_code|.
  void set_a2dp_offload_fails(HostError error_code) {
    a2dp_offload_error_ = error_code;
  }

  A2dpOffloadStatus a2dp_offload_status() { return audio_offloading_status_; }

  // Channel overrides:
  const sm::SecurityProperties security() override { return security_; }
  bool Activate(RxCallback rx_callback,
                ClosedCallback closed_callback) override;
  void Deactivate() override;
  void SignalLinkError() override;
  bool Send(ByteBufferPtr sdu) override;
  void UpgradeSecurity(sm::SecurityLevel level,
                       sm::ResultFunction<> callback) override;
  void RequestAclPriority(
      pw::bluetooth::AclPriority priority,
      fit::callback<void(fit::result<fit::failed>)> cb) override;
  void SetBrEdrAutomaticFlushTimeout(
      pw::chrono::SystemClock::duration flush_timeout,
      hci::ResultCallback<> callback) override;
  void AttachInspect(inspect::Node&, std::string) override {}
  void StartA2dpOffload(const A2dpOffloadManager::Configuration& config,
                        hci::ResultCallback<> callback) override;
  void StopA2dpOffload(hci::ResultCallback<> callback) override;

 private:
  hci_spec::ConnectionHandle handle_;
  Fragmenter fragmenter_;

  sm::SecurityProperties security_;
  SecurityUpgradeCallback security_cb_;
  std::optional<pw::async::HeapDispatcher> security_dispatcher_;

  ClosedCallback closed_cb_;
  RxCallback rx_cb_;

  SendCallback send_cb_;
  std::optional<pw::async::HeapDispatcher> send_dispatcher_;

  LinkErrorCallback link_err_cb_;

  bool activate_fails_;
  bool link_error_;

  bool acl_priority_fails_;
  bool flush_timeout_succeeds_ = true;
  FlushTimeoutCallback flush_timeout_cb_;

  A2dpOffloadStatus audio_offloading_status_ = A2dpOffloadStatus::kStopped;

  std::optional<HostError> a2dp_offload_error_;

  // The pending SDUs on this channel. Received PDUs are buffered if |rx_cb_| is
  // currently not set.
  std::queue<ByteBufferPtr> pending_rx_sdus_;

  WeakSelf<FakeChannel> weak_fake_chan_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(FakeChannel);
};

}  // namespace bt::l2cap::testing
