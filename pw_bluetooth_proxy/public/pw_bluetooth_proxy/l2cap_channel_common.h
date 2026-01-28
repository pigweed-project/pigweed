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

#include <optional>

#include "pw_bluetooth_proxy/connection_handle.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_function/function.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy {

/// Events returned from all client-facing channel objects in their `event_fn`
/// callback.
// TODO: https://pwbug.dev/388082771 -  Rename to just ChannelEvent and move
// to channel_common.h
enum class L2capChannelEvent {
  /// The channel was closed by something other than `ProxyHost` or due to
  /// `ProxyHost` shutdown. The channel is now `State::kClosed` and should be
  /// cleaned up. See logs for details.
  kChannelClosedByOther,
  /// An invalid packet was received. The channel is now `State::kStopped` and
  /// should be closed. See error logs for details.
  kRxInvalid,
  /// During Rx, the channel ran out of memory. The channel is now
  /// `State::kStopped` and should be closed.
  kRxOutOfMemory,
  /// The channel has received a packet while in the `State::kStopped` state.
  /// The channel should have been closed.
  kRxWhileStopped,
  /// `ProxyHost` has been reset. As a result, the channel is now
  /// `State::kClosed`. (All channels are `State::kClosed` on a reset.)
  kReset,
  /// Write space is now available after a previous Write on this channel
  /// returned UNAVAILABLE.
  kWriteAvailable,
};

// TODO: https://pwbug.dev/400536541 - Pass connection handle and channel
// IDs to event_fn.
/// Event callback from channels.
using ChannelEventCallback = pw::InlineFunction<
    void(L2capChannelEvent event),
    // Set size to at least two words so we can accept lambdas
    // that have two pointers in their capture (e.g. callee and a
    // pointer argument). If platform has defined an even larger
    // PW_FUNCTION_INLINE_CALLABLE_SIZE use that.
    std::max(sizeof(void*) * 2, PW_FUNCTION_INLINE_CALLABLE_SIZE)>;

/// Result object with status, optional MultiBuf.
// `pw::Result` can't be used because it only has a value for `ok()` status.
// `std::expected` can't be used because it only has a value OR a status.
struct StatusWithMultiBuf {
  pw::Status status;
  std::optional<FlatConstMultiBufInstance> buf = std::nullopt;
};

/// Alias for a client provided callback function for that can receive data from
/// a channel and optionally own the handling of that data.
///
/// @param[in] payload  The payload being passed to the client.
///
///
/// @returns The client can choose to one of the following:
/// 1. Return std::nullopt to own the handling of the payload.
/// 2. Return the payload as-is, to be forwarded.
/// 3. Modify and return the payload to be forwarded.
using OptionalPayloadReceiveCallback =
    Function<std::optional<FlatConstMultiBufInstance>(FlatMultiBuf&& payload)>;
using OptionalBufferReceiveFunction =
    Function<std::optional<FlatConstMultiBufInstance>(
        FlatMultiBuf&& payload,
        ConnectionHandle connection_handle,
        uint16_t local_channel_id,
        uint16_t remote_channel_id)>;

/// Client provided callback for data received from a channel.
using MultiBufReceiveFunction = Function<void(FlatConstMultiBuf&& payload)>;

}  // namespace pw::bluetooth::proxy
