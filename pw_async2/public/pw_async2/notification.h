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

#include "pw_async2/value_future.h"

namespace pw::async2 {

/// @submodule{pw_async2,futures}

/// A notification that multiple futures can wait on.
class Notification {
 public:
  constexpr Notification() = default;

  Notification(Notification&&) = default;
  Notification& operator=(Notification&&) = default;
  Notification(const Notification&) = delete;
  Notification& operator=(const Notification&) = delete;

  /// Returns a future that will resolve when `Notify()` is called.
  ///
  /// Multiple futures can be obtained and will all resolve at once.
  VoidFuture Wait() { return provider_.Get(); }

  /// Wakes all waiting tasks.
  void Notify() { provider_.Resolve(); }

 private:
  BroadcastValueProvider<void> provider_;
};

}  // namespace pw::async2
