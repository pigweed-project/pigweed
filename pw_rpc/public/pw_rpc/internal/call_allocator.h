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

#include "pw_assert/assert.h"
#include "pw_rpc/internal/config.h"

#include PW_RPC_MAKE_UNIQUE_PTR_INCLUDE

#if PW_RPC_ALLOW_INVOCATIONS_ON_STACK
#define _PW_RPC_DECLARE_CALL(type, name, ...) type name(__VA_ARGS__)
#else
#define _PW_RPC_DECLARE_CALL(type, name, ...)                    \
  auto name##_ptr = PW_RPC_MAKE_UNIQUE_PTR(type, ##__VA_ARGS__); \
  PW_ASSERT(name##_ptr != nullptr);                              \
  type& name = *name##_ptr
#endif  // PW_RPC_ALLOW_INVOCATIONS_ON_STACK
