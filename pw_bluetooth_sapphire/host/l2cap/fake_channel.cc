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

#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/internal/host/common/host_error.h"
#include "pw_bluetooth_sapphire/internal/host/common/log.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"

namespace bt::l2cap::testing {

FakeChannel::FakeChannel(ChannelId id,
                         ChannelId remote_id,
                         hci_spec::ConnectionHandle handle,
                         bt::LinkType link_type,
                         ChannelInfo info,
                         uint16_t max_tx_queued)
    : Channel(id, remote_id, link_type, handle, info, max_tx_queued),
      handle_(handle),
      fragmenter_(handle),
      activate_fails_(false),
      link_error_(false),
      acl_priority_fails_(false),
      weak_fake_chan_(this) {}

void FakeChannel::Receive(const ByteBuffer& data) {
  auto pdu =
      fragmenter_.BuildFrame(id(), data, FrameCheckSequenceOption::kNoFcs);
  auto sdu = std::make_unique<DynamicByteBuffer>(pdu.length());
  pdu.Copy(sdu.get());
  if (rx_cb_) {
    rx_cb_(std::move(sdu));
  } else {
    pending_rx_sdus_.push(std::move(sdu));
  }
}

void FakeChannel::SetSendCallback(SendCallback callback) {
  send_cb_ = std::move(callback);
}

void FakeChannel::SetSendCallback(SendCallback callback,
                                  pw::async::Dispatcher& dispatcher) {
  SetSendCallback(std::move(callback));
  send_dispatcher_.emplace(dispatcher);
}

void FakeChannel::SetLinkErrorCallback(LinkErrorCallback callback) {
  link_err_cb_ = std::move(callback);
}

void FakeChannel::SetSecurityCallback(SecurityUpgradeCallback callback,
                                      pw::async::Dispatcher& dispatcher) {
  security_cb_ = std::move(callback);
  security_dispatcher_.emplace(dispatcher);
}

void FakeChannel::Close() {
  if (closed_cb_)
    closed_cb_();
}

bool FakeChannel::Activate(RxCallback rx_callback,
                           ClosedCallback closed_callback) {
  PW_DCHECK(rx_callback);
  PW_DCHECK(closed_callback);
  PW_DCHECK(!rx_cb_);
  PW_DCHECK(!closed_cb_);

  if (activate_fails_)
    return false;

  closed_cb_ = std::move(closed_callback);
  rx_cb_ = std::move(rx_callback);

  while (!pending_rx_sdus_.empty()) {
    rx_cb_(std::move(pending_rx_sdus_.front()));
    pending_rx_sdus_.pop();
  }

  return true;
}

void FakeChannel::Deactivate() {
  closed_cb_ = {};
  rx_cb_ = {};
}

void FakeChannel::SignalLinkError() {
  if (link_error_) {
    return;
  }
  link_error_ = true;

  if (link_err_cb_) {
    link_err_cb_();
  }
}

bool FakeChannel::Send(ByteBufferPtr sdu) {
  PW_DCHECK(sdu);

  if (!send_cb_) {
    bt_log(
        DEBUG,
        "l2cap",
        "Dropping SDU and returning error because no send callback configured");
    return false;
  }

  if (sdu->size() > max_tx_sdu_size()) {
    bt_log(ERROR,
           "l2cap",
           "Dropping oversized SDU (sdu->size()=%zu, max_tx_sdu_size()=%u)",
           sdu->size(),
           max_tx_sdu_size());
    return false;
  }

  if (send_dispatcher_) {
    (void)send_dispatcher_->Post(
        [cb = send_cb_.share(), sdu = std::move(sdu)](
            pw::async::Context /*ctx*/, pw::Status status) mutable {
          if (status.ok()) {
            cb(std::move(sdu));
          }
        });
  } else {
    send_cb_(std::move(sdu));
  }

  return true;
}

void FakeChannel::UpgradeSecurity(sm::SecurityLevel level,
                                  sm::ResultFunction<> callback) {
  PW_CHECK(security_dispatcher_);
  (void)security_dispatcher_->Post(
      [cb = std::move(callback),
       f = security_cb_.share(),
       handle = handle_,
       level](pw::async::Context /*ctx*/, pw::Status status) mutable {
        if (status.ok()) {
          f(handle, level, std::move(cb));
        }
      });
}

void FakeChannel::RequestAclPriority(
    pw::bluetooth::AclPriority priority,
    fit::callback<void(fit::result<fit::failed>)> cb) {
  if (acl_priority_fails_) {
    cb(fit::failed());
    return;
  }
  requested_acl_priority_ = priority;
  cb(fit::ok());
}

void FakeChannel::SetBrEdrAutomaticFlushTimeout(
    pw::chrono::SystemClock::duration flush_timeout,
    hci::ResultCallback<> callback) {
  if (flush_timeout_cb_) {
    flush_timeout_cb_(flush_timeout, std::move(callback));
    return;
  }
  if (!flush_timeout_succeeds_) {
    callback(ToResult(pw::bluetooth::emboss::StatusCode::UNSPECIFIED_ERROR));
    return;
  }
  info_.flush_timeout = flush_timeout;
  callback(fit::ok());
}

void FakeChannel::StartA2dpOffload(const A2dpOffloadManager::Configuration&,
                                   hci::ResultCallback<> callback) {
  if (a2dp_offload_error_.has_value()) {
    callback(ToResult(a2dp_offload_error_.value()));
    audio_offloading_status_ = A2dpOffloadStatus::kStopped;
    return;
  }
  audio_offloading_status_ = A2dpOffloadStatus::kStarted;
  callback(fit::ok());
}

void FakeChannel::StopA2dpOffload(hci::ResultCallback<> callback) {
  if (a2dp_offload_error_.has_value()) {
    callback(ToResult(a2dp_offload_error_.value()));
    return;
  }
  audio_offloading_status_ = A2dpOffloadStatus::kStopped;
  callback(fit::ok());
}

}  // namespace bt::l2cap::testing
