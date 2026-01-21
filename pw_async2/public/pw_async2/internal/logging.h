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
#pragma once

#include "pw_async2/internal/config.h"

// pw_log configuration for in pw_async2.
//
// Important:
//
// - This header may only be included in a .cc file.
// - This header must be included before any other includes.

#define PW_LOG_MODULE_NAME "PW_ASYNC2"
#define PW_LOG_LEVEL PW_ASYNC2_LOG_LEVEL
