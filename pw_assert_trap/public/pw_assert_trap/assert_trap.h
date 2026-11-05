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

#include "pw_assert_trap/handler.h"

// TODO: https://pwbug.dev/353372406 - handle reentrant crashes
#define PW_ASSERT_HANDLE_FAILURE(expression)                        \
  pw_assert_trap_interrupt_lock();                                  \
  pw_assert_trap_HandleAssertFailure(__FILE__, __LINE__, __func__); \
  _PW_ASSERT_TRAP_EPILOG()
