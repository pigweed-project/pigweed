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

#include <stdlib.h>

#include <cstdint>

namespace chre {

template <typename T>
inline T* memoryAlignedAlloc() {
  return static_cast<T*>(aligned_alloc(alignof(T), sizeof(T)));
}

template <typename T>
inline T* memoryAlignedAllocArray(size_t count) {
  if (count > SIZE_MAX / sizeof(T)) {
    return nullptr;
  }

  return static_cast<T*>(aligned_alloc(alignof(T), sizeof(T) * count));
}

}  // namespace chre
