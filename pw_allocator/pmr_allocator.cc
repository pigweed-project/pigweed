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

#include "pw_allocator/pmr_allocator.h"

#include <cstddef>
#include <memory_resource>

#include "pw_allocator/hardening.h"
#include "pw_allocator/layout.h"
#include "pw_assert/check.h"

namespace pw::allocator::internal {

void* MemoryResource::do_allocate(size_t bytes, size_t alignment) {
  void* ptr = nullptr;
  if (bytes != 0) {
    ptr = allocator_->Allocate(Layout(bytes, alignment));
  }

  // The standard library expects the memory resource to throw an
  // exception if storage of the requested size and alignment cannot be
  // obtained. As a result, the uses-allocator types are not required to check
  // for allocation failure. In lieu of using exceptions, this type asserts that
  // an allocation must succeed.
  if constexpr (Hardening::kIncludesBasicChecks) {
    PW_CHECK_NOTNULL(
        ptr, "failed to allocate %zu bytes for PMR container", bytes);
  }
  return ptr;
}

void MemoryResource::do_deallocate(void* p, size_t, size_t) {
  allocator_->Deallocate(p);
}

bool MemoryResource::do_is_equal(
    const pw::pmr::memory_resource& other) const noexcept {
  if (this == &other) {
    return true;
  }
  // If `other` is not the same object as this one, it is only equal if
  // the other object is a `MemoryResource` with the same allocator. That checks
  // requires runtime type identification. Without RTTI, two `MemoryResource`s
  // with the same allocator will be treated as unequal, and moving objects
  // between them may lead to an extra allocation, copy, and deallocation.
#if defined(__cpp_rtti) && __cpp_rtti
  if (typeid(*this) == typeid(other)) {
    return allocator_ == static_cast<const MemoryResource&>(other).allocator_;
  }
#endif  // defined(__cpp_rtti) && __cpp_rtti
  return false;
}

}  // namespace pw::allocator::internal
