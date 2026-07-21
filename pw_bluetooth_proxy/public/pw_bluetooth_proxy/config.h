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

/// @module{pw_bluetooth_proxy}

/// User-provided header to optionally override options in this file.
#ifdef PW_BLUETOOTH_PROXY_CONFIG_HEADER
#include PW_BLUETOOTH_PROXY_CONFIG_HEADER
#endif  // PW_BLUETOOTH_PROXY_CONFIG_HEADER

#ifndef PW_BLUETOOTH_PROXY_ASYNC
/// Indicates whether the proxy should operate asynchronously or not.
///
/// When set, public API methods will be able to use `async2::Channel` to
/// communicate with tasks running on the dispatcher thread.
#define PW_BLUETOOTH_PROXY_ASYNC 0
#endif  // PW_BLUETOOTH_PROXY_ASYNC

#ifndef PW_BLUETOOTH_PROXY_MULTIBUF_ALLOCATOR_SIZE
/// The size of the internal multibuf allocator used for ChannelProxy & RFCOMM
/// channels. This is temporary until all code is migrated to MultiBuf v2, at
/// which point the client provided pw::Allocator will be used instead.
#define PW_BLUETOOTH_PROXY_MULTIBUF_ALLOCATOR_SIZE 1024
#endif  // PW_BLUETOOTH_PROXY_MULTIBUF_ALLOCATOR_SIZE

#ifndef PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
/// Whether ProxyHost emits incremental state update callbacks on dynamic credit
/// mutations. Disabling reduces power consumption for high-throughput streams.
#define PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES 1
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES

/// @endmodule
