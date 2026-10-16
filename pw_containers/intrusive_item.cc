// Copyright 2020 The Pigweed Authors
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

#include "pw_containers/internal/intrusive_item.h"

#include "pw_assert/check.h"

namespace pw::containers::internal {

void CrashIntrusiveContainerIsNotEmpty() {
  PW_CRASH("Intrusive container is not empty and cannot be destroyed");
}

void CrashIntrusiveItemIsContained() {
  PW_CRASH(
      "Item is in an intrusive container and cannot be added or destroyed");
}

}  // namespace pw::containers::internal
