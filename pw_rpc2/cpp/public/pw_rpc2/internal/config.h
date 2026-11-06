// Copyright 2026 The Pigweed Authors
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

// Configuration macros for the pw_rpc2 module.
#pragma once

#include "pw_log/levels.h"

/// @submodule{pw_rpc2,config}

/// The log level for `pw_rpc2`'s internal logs. Logs below this level are
/// compiled out.
///
/// Set it to `PW_LOG_LEVEL_DEBUG` to trace connections and calls while
/// debugging, or to `PW_LOG_LEVEL_WARN` to keep only problems.
#ifndef PW_RPC2_LOG_LEVEL
#define PW_RPC2_LOG_LEVEL PW_LOG_LEVEL_INFO
#endif  // PW_RPC2_LOG_LEVEL

/// @endsubmodule
