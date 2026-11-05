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

#include "pw_allocator/deallocator.h"

#include <cstddef>
#include <optional>

namespace pw {

void Deallocator::Deallocate(void* ptr) {
  if (ptr != nullptr) {
    DoDeallocate(ptr);
  }
}

StatusWithSize Deallocator::GetCapacity() const {
  auto result = DoGetInfo(InfoType::kCapacity, nullptr);
  return StatusWithSize(result.status(), Layout::Unwrap(result).size());
}

size_t Deallocator::GetAllocated() const {
  if (auto allocated = DoGetAllocated();
      allocated != kGetAllocatedUnsupported) {
    return allocated;
  }
  if (auto allocated = DoGetAllocatedBytes(); allocated.has_value()) {
    return allocated.value();
  }
  return kGetAllocatedUnsupported;
}

std::optional<size_t> Deallocator::GetAllocatedBytes() const {
  if (auto allocated = DoGetAllocatedBytes(); allocated.has_value()) {
    return allocated;
  }
  if (auto allocated = DoGetAllocated();
      allocated != kGetAllocatedUnsupported) {
    return std::make_optional<size_t>(allocated);
  }
  return std::nullopt;
}

}  // namespace pw
