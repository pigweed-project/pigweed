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

#include "pw_bluetooth_proxy/config.h"

#if PW_BLUETOOTH_PROXY_ASYNC != 0

#include <cstdint>
#include <optional>

#include "pw_async2/channel.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_bluetooth_proxy/internal/mutex.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_function/function.h"
#include "pw_status/status.h"
#include "pw_sync/no_lock.h"

namespace pw::bluetooth::proxy {

class L2capChannel;

namespace internal {

/// Implementation detail class for L2capChannel.
///
/// This type is used when `PW_BLUETOOTH_PROXY_ASYNC` is true; that is, when
/// the proxy is using an synchronous execution model with a dispatcher that
/// runs concurrent tasks sequentially.
class L2capChannelImpl {
 public:
  struct Request {
    enum class Type : uint16_t {
      kSendAdditionalRxCredits,
      kNotifyOnDequeue,
      kStop,
      kClose,
    } type = Type::kClose;
    uint16_t additional_rx_credits = 0;
  };

  // TODO: https://pwbug.dev/349700888 - Make capacity configurable.
  static constexpr size_t kQueueCapacity = 5;

  ~L2capChannelImpl();

  /// @copydoc L2capChannel::DequeuePacket
  async2::Poll<std::optional<H4PacketWithH4>> DequeuePacket(
      async2::Context& context);

  /// @copydoc L2capChannel::PayloadQueueEmpty
  bool PayloadQueueEmpty() const;

  /// @copydoc L2capChannel::SendEvent
  void SendEvent(L2capChannelEvent event);

 private:
  friend class pw::bluetooth::proxy::L2capChannel;
  friend class GenericL2capChannelImpl;

  explicit L2capChannelImpl(L2capChannel& channel)
      : channel_(channel), task_(*this) {}

  Allocator& allocator();

  L2capChannel& channel() { return channel_; }

  /// Completes initialization of the channel details.
  /// @returns
  /// * @OK: Channel is initialized.
  /// * @RESOURCE_EXHAUSTED: Failed to allocate storage for the payload channel.
  Status Init();

  /// Provides a connection to send requests from a client channel.
  ///
  /// @returns
  /// * @OK: Channel is connected.
  /// * @RESOURCE_EXHAUSTED: Failed to allocate storage for the request channel.
  Status Connect(async2::SpscChannelHandle<Request>& request_handle,
                 async2::Sender<Request>& request_sender);

  /// Provides a connection to send payloads from a client channel.
  void Connect(async2::Sender<FlatConstMultiBufInstance>& payload_sender);

  /// Trivial implementation since channels are not borrowed in async mode.
  void BlockWhileBorrowed(std::unique_lock<internal::Mutex>&) {}

  /// Disconnects the client channel, if connected.
  void Close();

  /// @copydoc L2capChannel::Write
  StatusWithMultiBuf Write(FlatConstMultiBuf&& payload);

  /// Request a "write available" event be sent when space become available in
  /// the payload queue.
  void NotifyOnDequeue();

  /// @copydoc L2capChannel::ReportNewTxPacketsOrCredits
  void ReportNewTxPacketsOrCredits();

  /// @copydoc L2capChannel::ClearQueue
  void ClearQueue();

  /// @copydoc L2capChannel::IsStale
  bool IsStale() const;

  /// Notifies the this object that the client channel has gone out of scope.
  void OnClientDisconnect();

  // Fake mutex for lock annotations.
  mutable sync::NoLock mutex_;

  L2capChannel& channel_;

  /// Channel used to notify the channel manager's drain task when additional
  /// send credits are available.
  async2::Sender<bool> credit_sender_;

  /// Request handler for L2CAP channel requests.
  class Task final : public async2::Task {
   public:
    explicit Task(L2capChannelImpl& impl)
        : async2::Task(PW_ASYNC_TASK_NAME("L2capChannel")), impl_(impl) {}

    ~Task() override { Deregister(); }

    void set_receiver(async2::Receiver<Request>&& receiver) {
      receiver_ = std::move(receiver);
    }

   private:
    async2::Poll<> DoPend(async2::Context& context) override;

    L2capChannelImpl& impl_;
    async2::Receiver<Request> receiver_;
    async2::ReceiveFuture<Request> future_;
  } task_;

  /// Handle to the async2 channel used to connect receive basic requests from
  /// the client. This is set when the L2CAP channels is acquired by the client
  /// so channels that are not acquired, e.g. signaling channels, will have
  /// std::nullopt. An internal channel is considered "stale" if this handle has
  /// a value, but is not open.
  std::optional<async2::SpscChannelHandle<Request>> request_handle_;

  /// Channel for reading payloads from the client.
  async2::MpscChannelHandle<FlatConstMultiBufInstance> payload_handle_;
  async2::Sender<FlatConstMultiBufInstance> payload_sender_;
  async2::Receiver<FlatConstMultiBufInstance> payload_receiver_;
  async2::ReceiveFuture<FlatConstMultiBufInstance> payload_future_;

  /// An L2CAP SDU that is being segmented into one or more L2CAP PDUs.
  std::optional<FlatConstMultiBufInstance> payload_;

  /// Wakes the drain tasks when new tx packets or credits are reported.
  async2::Waker waker_;

  /// True if the last queue attempt didn't have space. Will be cleared on
  /// successful dequeue.
  bool notify_on_dequeue_ = false;
};

}  // namespace internal
}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_ASYNC
