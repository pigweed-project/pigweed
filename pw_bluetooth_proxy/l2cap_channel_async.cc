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

#include "pw_bluetooth_proxy/config.h"

#if PW_BLUETOOTH_PROXY_ASYNC != 0

#include "pw_assert/check.h"
#include "pw_async2/channel.h"
#include "pw_async2/dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/try.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_async.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_manager.h"
#include "pw_log/log.h"

namespace pw::bluetooth::proxy::internal {

L2capChannelImpl::~L2capChannelImpl() {
  if (request_handle_.has_value()) {
    request_handle_->Close();
  }
}

Allocator& L2capChannelImpl::allocator() {
  return channel_.l2cap_channel_manager_.impl().allocator();
}

Status L2capChannelImpl::Init() {
  // One payload is held in `payload_`, the rest in the payload channel.
  auto result = async2::CreateMpscChannel<FlatConstMultiBufInstance>(
      allocator(), kQueueCapacity - 1);
  if (!result.has_value()) {
    return Status::ResourceExhausted();
  }
  std::tie(payload_handle_, payload_receiver_) = std::move(*result);
  payload_sender_ = payload_handle_.CreateSender();

  L2capChannelManagerImpl& manager = channel_.l2cap_channel_manager_.impl();
  credit_sender_ = manager.CreateCreditSender();
  return OkStatus();
}

Status L2capChannelImpl::Connect(
    async2::SpscChannelHandle<Request>& request_handle,
    async2::Sender<Request>& request_sender) {
  auto req_result = async2::CreateSpscChannel<Request>(allocator(), 1);
  if (!req_result.has_value()) {
    return Status::ResourceExhausted();
  }

  async2::Receiver<Request> req_receiver;
  std::tie(request_handle, request_sender, req_receiver) =
      std::move(*req_result);
  request_handle_ = request_handle;
  task_.set_receiver(std::move(req_receiver));

  L2capChannelManagerImpl& manager = channel_.l2cap_channel_manager_.impl();
  manager.dispatcher().Post(task_);
  return OkStatus();
}

void L2capChannelImpl::Connect(
    async2::Sender<FlatConstMultiBufInstance>& payload_sender) {
  payload_sender = payload_handle_.CreateSender();
}

void L2capChannelImpl::Close() {
  if (request_handle_.has_value() && request_handle_->is_open()) {
    request_handle_->Close();
  }
  // Always set to default to mark channel as stale, even if it wasn't acquired.
  request_handle_ = async2::SpscChannelHandle<Request>();
}

StatusWithMultiBuf L2capChannelImpl::Write(FlatConstMultiBuf&& payload) {
  if (channel_.state() != L2capChannel::State::kRunning) {
    PW_LOG_WARN(
        "btproxy: L2capChannel::Write called when not running. "
        "local_cid: %#x, remote_cid: %#x, state: %u",
        channel_.local_cid(),
        channel_.remote_cid(),
        cpp23::to_underlying(channel_.state()));
    return {Status::FailedPrecondition(), std::move(payload)};
  }
  auto send_reservation = payload_sender_.TryReserveSend();
  if (!send_reservation.ok()) {
    return {send_reservation.status(), std::move(payload)};
  }
  send_reservation->Commit(std::move(payload));
  return {OkStatus(), std::nullopt};
}

void L2capChannelImpl::NotifyOnDequeue() { notify_on_dequeue_ = true; }

async2::Poll<std::optional<H4PacketWithH4>> L2capChannelImpl::DequeuePacket(
    async2::Context& context) {
  std::optional<H4PacketWithH4> packet;

  // First, try to receive a payload if we don't already have one.
  if (!payload_.has_value()) {
    if (!payload_future_.is_pendable()) {
      payload_future_ = payload_receiver_.Receive();
    }
    PW_TRY_READY_ASSIGN(payload_, payload_future_.Pend(context));

    // Either the handle was closed, or the future returned std::nullopt. Either
    // way the channel is closed.
    if (!payload_.has_value()) {
      return async2::Ready(std::move(packet));
    }

    // If a previous client call to `Write` or `IsWriteAvailable` returned
    // UNAVAILABLE, notify them that more space is now available.
    if (notify_on_dequeue_) {
      notify_on_dequeue_ = false;
      SendEvent(L2capChannelEvent::kWriteAvailable);
    }
  }

  // Create a packet from the payload.
  bool keep_payload = false;
  {
    // Fake lock to satisfy annotations.
    std::lock_guard lock(channel_.impl_.mutex_);
    packet = channel_.GenerateNextTxPacket(*payload_, keep_payload);
  }

  // If no additional packets can be created from the payload, remove it.
  if (!keep_payload) {
    payload_.reset();
  }

  if (!packet.has_value()) {
    PW_ASYNC_STORE_WAKER(
        context, waker_, "Waiting for new tx packets or credits");
    return async2::Pending();
  }

  return async2::Ready(std::move(packet));
}

void L2capChannelImpl::ReportNewTxPacketsOrCredits() {
  waker_.Wake();
  channel_.l2cap_channel_manager_.ReportNewTxPacketsOrCredits();
}

void L2capChannelImpl::ClearQueue() {
  payload_handle_.Close();
  payload_future_ = async2::ReceiveFuture<FlatConstMultiBufInstance>();
  payload_.reset();
}

bool L2capChannelImpl::IsStale() const {
  return request_handle_.has_value() && !request_handle_->is_open();
}

async2::Poll<> L2capChannelImpl::Task::DoPend(async2::Context& context) {
  while (true) {
    if (!future_.is_pendable()) {
      future_ = receiver_.Receive();
    }
    std::optional<Request> request;
    PW_TRY_READY_ASSIGN(request, future_.Pend(context));
    future_ = async2::ReceiveFuture<Request>();
    if (!request.has_value()) {
      return async2::Ready();
    }
    switch (request->type) {
      case Request::Type::kSendAdditionalRxCredits: {
        std::ignore = impl_.channel().SendAdditionalRxCredits(
            request->additional_rx_credits);
        break;
      }
      case Request::Type::kNotifyOnDequeue: {
        impl_.NotifyOnDequeue();
        break;
      }
      case Request::Type::kStop: {
        impl_.channel().Stop();
        break;
      }
      case Request::Type::kClose: {
        impl_.channel().Close();
        break;
      }
    }
  }
}

void L2capChannelImpl::SendEvent(L2capChannelEvent event) {
  if (!request_handle_.has_value() || !request_handle_->is_open()) {
    return;
  }

  // We don't log kWriteAvailable since they happen often. Optimally we would
  // just debug log them also, but one of our downstreams logs all levels.
  if (event != L2capChannelEvent::kWriteAvailable) {
    PW_LOG_INFO(
        "btproxy: L2capChannel::SendEvent -  connection: %#x, "
        "local_cid: %#x, event: %u,",
        channel_.connection_handle(),
        channel_.local_cid(),
        cpp23::to_underlying(event));
  }

  if (channel_.event_fn_) {
    channel_.event_fn_(event);
  }
}

}  // namespace pw::bluetooth::proxy::internal

#endif  // PW_BLUETOOTH_PROXY_ASYNC
