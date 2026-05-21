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

#include "pw_allocator/allocator.h"
#include "pw_async2/channel.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel_async.h"
#include "pw_multibuf/multibuf.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_thread/id.h"

namespace pw::bluetooth::proxy::internal {

/// Implementation detail class for GenericL2capChannel.
///
/// This type is used when `PW_BLUETOOTH_PROXY_ASYNC` is true; that is, when
/// the proxy is using an synchronous execution model with a dispatcher that
/// runs concurrent tasks sequentially.
class GenericL2capChannelImpl {
 public:
  using Request = L2capChannelImpl::Request;

  GenericL2capChannelImpl() = default;
  explicit GenericL2capChannelImpl(L2capChannel& channel);
  GenericL2capChannelImpl(GenericL2capChannelImpl&& other) {
    *this = std::move(other);
  }
  GenericL2capChannelImpl& operator=(GenericL2capChannelImpl&& other);
  ~GenericL2capChannelImpl();

  /// @copydoc GenericL2capChannel::Init
  Status Init();

  /// @copydoc GenericL2capChannel::Start
  void Start();

  /// @copydoc GenericL2capChannel::Write
  StatusWithMultiBuf Write(multibuf::MultiBuf&& payload);

  /// @copydoc GenericL2capChannel::IsWriteAvailable
  Status IsWriteAvailable();

  /// @copydoc GenericL2capChannel::SendAdditionalRxCredits
  Status SendAdditionalRxCredits(uint16_t additional_rx_credits);

  /// @copydoc GenericL2capChannel::GetState
  L2capChannel::State GetState() const;

  /// @copydoc GenericL2capChannel::Stop
  void Stop();

  /// @copydoc GenericL2capChannel::Close
  void Close();

  /// @copydoc GenericL2capChannel::InternalForTesting
  L2capChannel* InternalForTesting();

 private:
  GenericL2capChannelImpl(L2capChannel& channel,
                          L2capChannelManagerImpl& manager);

  /// Sends the request to the internal channel and notifies the dispatcher that
  /// the internal channel task has been awakened.
  Status Send(Request&& request) const;

  // Internal channel.
  //
  // If an API method is called while running on the dispatcher thread, it is
  // safe to access this directly.
  L2capChannel* channel_ = nullptr;

  /// @copydoc L2capChannelManagerImpl::dispatcher_
  async2::Dispatcher* dispatcher_ = nullptr;

  /// @copydoc L2capChannelManagerImpl::dispatcher_thread_id_
  thread::Id dispatcher_thread_id_;

  // Channel for sending requests.
  //
  // TODO(b/469150426): If `Sender<T>::BlockingSend` is made cost, the `mutable`
  // keyword can be dropped.
  async2::SpscChannelHandle<Request> request_handle_;
  mutable async2::Sender<Request> request_sender_;

  // Channel for sending payloads to write.
  async2::Sender<multibuf::MultiBuf> payload_sender_;
  std::optional<async2::SendReservation<multibuf::MultiBuf>> send_reservation_;
};

}  // namespace pw::bluetooth::proxy::internal

#endif  // PW_BLUETOOTH_PROXY_ASYNC
