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

#include "pw_async2/value_future.h"

namespace codelab {

class ItemDropSensor {
 public:
  constexpr ItemDropSensor() = default;

  // Pends until the item drop sensor triggers.
  pw::async2::ValueFuture<void> Wait();

  // Records an item drop event. Typically called from the drop sensor ISR.
  void Drop();

 private:
  pw::async2::BroadcastValueProvider<void> provider_;
};

}  // namespace codelab
