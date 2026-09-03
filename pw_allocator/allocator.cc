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

#include "pw_allocator/allocator.h"

#include <algorithm>
#include <cstring>

#include "pw_bytes/alignment.h"

namespace pw {

using ::pw::allocator::Layout;

void* Allocator::DoReallocate(void* ptr, Layout new_layout) {
  DoBeforeReallocate(ptr, new_layout);

  // Can the reallocation be achieved by simply resizing?
  if (IsAlignedAs(ptr, new_layout.alignment()) &&
      Resize(ptr, new_layout.size())) {
    DoAfterReallocateCopy(ptr, new_layout, ptr);
    DoAfterReallocateDone(new_layout, ptr);
    return ptr;
  }

  // Allocate a new region of memory.
  Result<Layout> old_layout = GetUsableLayout(ptr);
  if (!old_layout.ok()) {
    DoAfterReallocateDone(new_layout, nullptr);
    return nullptr;
  }
  void* new_ptr = Allocate(new_layout);
  if (new_ptr != nullptr) {
    std::memcpy(new_ptr, ptr, std::min(new_layout.size(), old_layout->size()));
    DoAfterReallocateCopy(ptr, new_layout, new_ptr);
    Deallocate(ptr);
  }

  DoAfterReallocateDone(new_layout, new_ptr);
  return new_ptr;
}

}  // namespace pw
