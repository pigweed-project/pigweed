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

// DOCSTAG: [pw_allocator-examples-custom_allocator]
#include "examples/custom_allocator.h"

#include <cstdint>

#include "pw_allocator/capability.h"
#include "pw_log/log.h"
#include "pw_result/result.h"

namespace examples {

CustomAllocator::CustomAllocator(Allocator& allocator, size_t threshold)
    : Base(allocator), threshold_(threshold) {}

// Allocates, and reports if allocated memory exceeds its threshold.
void* CustomAllocator::DoAllocate(Layout layout) {
  void* ptr = Base::DoAllocate(layout);
  if (ptr == nullptr) {
    return nullptr;
  }
  size_t prev = used_;
  used_ = Base::DoGetAllocated();
  if (prev <= threshold_ && threshold_ < used_) {
    PW_LOG_INFO("more than %zu bytes allocated.", threshold_);
  }
  return ptr;
}

void CustomAllocator::DoDeallocate(void* ptr) {
  if (ptr == nullptr) {
    return;
  }
  Base::DoDeallocate(ptr);
  used_ = Base::DoGetAllocated();
}

}  // namespace examples
