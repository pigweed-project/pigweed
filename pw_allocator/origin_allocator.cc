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

#include "pw_allocator/origin_allocator.h"

#include "pw_assert/check.h"

namespace pw::allocator {

SourceLocation OriginAllocator::GetOrigin(const void* ptr) const {
  PW_CHECK(Recognizes(ptr));
  return *(GetPrefix(ptr));
}

void OriginAllocator::DoSetOrigin(SourceLocation origin, void* ptr) {
  if (ptr != nullptr) {
    PW_CHECK(Recognizes(ptr));
    *(GetPrefix(ptr)) = origin;
  }
}

}  // namespace pw::allocator
