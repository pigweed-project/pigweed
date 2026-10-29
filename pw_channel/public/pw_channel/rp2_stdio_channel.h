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

#include <cstdint>
#include <optional>

#include "pw_async2/poll.h"
#include "pw_channel/channel.h"
#include "pw_multibuf/allocator.h"
#include "pw_multibuf/multibuf.h"

namespace pw::channel {

/// @module{pw_channel}

/// @defgroup pw_channel_rp2_stdio RP2 stdio
/// @{

/// Initializes and returns a reference to a channel that speaks over rp2's
/// stdio.
///
/// The read and write allocators should be different to avoid deadlocks due to
/// the read or write side of the API attempting to acquire more memory while
/// the other holds it.
///
/// ***This must only be called at-most once.***
///
/// @deprecated pw_channel is deprecated; use pw::transport::Socket or
/// pw::transport::ReliableSocket.
[[deprecated(
    "pw_channel is deprecated; use pw::transport::Socket or "
    "pw::transport::ReliableSocket")]] ByteReaderWriter&
Rp2StdioChannelInit(pw::multibuf::MultiBufAllocator& read_allocator,
                    pw::multibuf::MultiBufAllocator& write_allocator);

/// @}
/// @endmodule

}  // namespace pw::channel
