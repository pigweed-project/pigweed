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

#include "pw_async2/time_provider.h"
#include "pw_bluetooth_proxy_backend/clock.h"

namespace pw::bluetooth::proxy {

using Clock = backend::Clock;

namespace internal {

/// Returns a `TimeProvider` for the `Clock` type used by the bluetooth proxy.
inline async2::TimeProvider<Clock>& GetDefaultTimeProvider() {
  return backend::GetClockTimeProvider();
}

}  // namespace internal
}  // namespace pw::bluetooth::proxy
