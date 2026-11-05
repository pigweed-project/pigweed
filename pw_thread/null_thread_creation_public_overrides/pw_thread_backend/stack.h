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

#include <cstddef>

namespace pw::thread::backend {

inline constexpr size_t kDefaultStackSizeBytes = 0;

template <size_t kStackSizeBytes>
class Stack {
 public:
  template <bool kGenericThreadCreationIsSupported = false>
  constexpr Stack() {
    static_assert(
        kGenericThreadCreationIsSupported,
        "Generic thread creation is not yet supported for this pw_thread "
        "backend. Implement the generic thread creation backend to "
        "use pw::ThreadStack. See pigweed.dev/pw_thread.");
  }

  constexpr void* data() { return this; }
  constexpr size_t size() const { return 0; }
};

}  // namespace pw::thread::backend
