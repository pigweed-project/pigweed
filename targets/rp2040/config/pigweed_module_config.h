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

// WARNING: If you update this file, update
// //targets/rp2040:pigweed_module_config as well!!!
// LINT.IfChange
// Enable thread joining for tests.
#define PW_THREAD_FREERTOS_CONFIG_JOINING_ENABLED 1

// When using assert_basic, don't fflush and instead just exit.
#define PW_ASSERT_BASIC_ACTION PW_ASSERT_BASIC_ACTION_EXIT

// TestThreadContextNative in pw_thread_freertos uses a 32Kb stack, increasing
// the size of test fixtures such as some in pw_bluetooth_proxy which use it by
// that amount.
#define PW_UNIT_TEST_CONFIG_MEMORY_POOL_SIZE 49152
// LINT.ThenChange(//targets/rp2040/BUILD.bazel)
