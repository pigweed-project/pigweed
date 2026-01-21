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

// Configuration macros for the pw_async2 module.
#pragma once

/// @submodule{pw_async2,config}

/// Controls how the `wait_reason_string` argument to `PW_ASYNC_STORE_WAKER` and
/// `PW_ASYNC_CLONE_WAKER` is used.  If enabled, wait reasons are stored within
/// their wakers, allowing easier debugging of sleeping tasks.
///
/// Enabled by default.
///
/// Note: The module dependencies of `pw_async2` vary based on on the value of
/// `PW_ASYNC2_DEBUG_WAIT_REASON`.  When building pw_async2 with Bazel, you
/// should NOT set this module config value directly. Instead, tell the build
/// system which value you wish to select by adding one of the following
/// constraint_values to the target platform:
///
///   - `@pigweed//pw_async2:debug_wait_reason_disabled` (default)
///   - `@pigweed//pw_async2:debug_wait_reason_enabled`
#ifndef PW_ASYNC2_DEBUG_WAIT_REASON
#define PW_ASYNC2_DEBUG_WAIT_REASON 1
#endif  // PW_ASYNC2_DEBUG_WAIT_REASON

/// The log level to use for this module. Logs below this level are omitted.
#ifndef PW_ASYNC2_LOG_LEVEL
#define PW_ASYNC2_LOG_LEVEL PW_LOG_LEVEL_INFO
#endif  // PW_ASYNC2_LOG_LEVEL

/// @endsubmodule
