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

#include "pw_memory/no_destructor.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_sync/interrupt_spin_lock.h"

namespace pw::async2::internal {

// A lock guarding `Task` queue and `Waker` lists for all `Dispatcher`s. This is
// an internal implementation detail. Do not use it directly.
//
// This is an `InterruptSpinLock` in order to allow posting work from ISR
// contexts.
//
// This lock is global rather than per-dispatcher in order to allow `Task` and
// `Waker` to take out the lock without dereferencing their `Dispatcher*`
// fields, which are themselves guarded by the lock in order to allow the
// `Dispatcher` to `Deregister` itself upon destruction.
inline pw::sync::InterruptSpinLock& lock() {
  PW_CONSTINIT static NoDestructor<pw::sync::InterruptSpinLock> lock;
  return *lock;
}

}  // namespace pw::async2::internal
