// Copyright 2023 The Pigweed Authors
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
#include "pw_assert/assert.h"

#define FATAL_ERROR_QUIT() \
  do {                     \
    PW_ASSERT(false);      \
  } while (0)

#define CHRE_HANDLE_FATAL_ERROR(fmt, ...) \
  do {                                    \
    LOGE(fmt, ##__VA_ARGS__);             \
    FATAL_ERROR_QUIT();                   \
    while (1) {                           \
    }                                     \
  } while (0)
