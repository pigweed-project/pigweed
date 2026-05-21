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

#if PW_BLUETOOTH_PROXY_ASYNC == 0

#include <cstdint>

#include "pw_allocator/allocator.h"
#include "pw_bluetooth_proxy/config.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_multibuf/multibuf.h"
#include "pw_status/status.h"
#include "pw_sync/mutex.h"

namespace pw::bluetooth::proxy::internal {

class GenericL2capChannel;

/// Implementation detail class for GenericL2capChannel.
///
/// This type is used when `PW_BLUETOOTH_PROXY_ASYNC` is false; that is, when
/// the proxy is using a purely synchronous execution model using one or more
/// threads and using mutexes to protect shared state from concurrent
/// modification.
class GenericL2capChannelImpl {
 public:
  constexpr GenericL2capChannelImpl() = default;
  explicit GenericL2capChannelImpl(L2capChannel& channel);
  GenericL2capChannelImpl(GenericL2capChannelImpl&& other) {
    *this = std::move(other);
  }
  GenericL2capChannelImpl& operator=(GenericL2capChannelImpl&& other);
  ~GenericL2capChannelImpl();

  /// @copydoc GenericL2capChannel::Init
  Status Init() PW_LOCKS_EXCLUDED(L2capChannelImpl::mutex());

  /// @copydoc GenericL2capChannel::Start
  void Start() PW_LOCKS_EXCLUDED(L2capChannelImpl::mutex());

  /// @copydoc GenericL2capChannel::Write
  StatusWithMultiBuf Write(multibuf::MultiBuf&& payload)
      PW_LOCKS_EXCLUDED(L2capChannelImpl::mutex());

  /// @copydoc GenericL2capChannel::IsWriteAvailable
  Status IsWriteAvailable() PW_LOCKS_EXCLUDED(L2capChannelImpl::mutex());

  /// @copydoc GenericL2capChannel::SendAdditionalRxCredits
  Status SendAdditionalRxCredits(uint16_t additional_rx_credits)
      PW_LOCKS_EXCLUDED(L2capChannelImpl::mutex());

  /// @copydoc GenericL2capChannel::GetState
  L2capChannel::State GetState() const
      PW_LOCKS_EXCLUDED(L2capChannelImpl::mutex());

  /// @copydoc GenericL2capChannel::Stop
  void Stop() PW_LOCKS_EXCLUDED(L2capChannelImpl::mutex());

  /// @copydoc GenericL2capChannel::Close
  void Close() PW_LOCKS_EXCLUDED(L2capChannelImpl::mutex());

  /// @copydoc GenericL2capChannel::InternalForTesting
  L2capChannel* InternalForTesting() const
      PW_LOCKS_EXCLUDED(L2capChannelImpl::mutex());

 private:
  friend class L2capChannelImpl;

  /// Returns a borrowed L2CAP channel, which can be dereferenced to get the
  /// L2CAP channel. The channel is guaranteed not to be destroyed as long as
  /// the `BorrowedL2capChannel` is in scope.
  ///
  /// @param[in] expected The expected state of the internal channel.
  ///
  /// @returns
  /// * @OK: Returns the channel.
  /// * @FAILED_PRECONDITION: This object is not connected to an L2CAP channel,
  ///                         or the channel is not in the expected state.
  Result<BorrowedL2capChannel> BorrowL2capChannel(
      L2capChannel::State expected = L2capChannel::State::kRunning) const;

  L2capChannel* channel_ = nullptr;
};

}  // namespace pw::bluetooth::proxy::internal

#endif  // PW_BLUETOOTH_PROXY_ASYNC
