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

#include "pw_allocator/size_report/size_report.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>

#include "pw_allocator/allocator.h"
#include "pw_allocator/block_allocator.h"
#include "pw_allocator/layout.h"
#include "pw_allocator/unique_ptr.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_bytes/alignment.h"
#include "pw_bytes/span.h"

namespace pw::allocator::size_report {

ByteSpan GetBuffer() {
  static std::array<std::byte, 0x400> buffer;
  return buffer;
}

int SetBaseline(uint32_t mask) {
  bloat::BloatThisBinary();

  ByteSpan bytes = GetAlignedSubspan(GetBuffer(), 32);
  PW_BLOAT_COND(!bytes.empty(), mask);

  Layout layout(64, 1);
  PW_BLOAT_COND(layout.size() < bytes.size(), mask);

  return mask == bloat::kDefaultMask ? 0 : 1;
}

int MeasureAllocator(pw::Allocator& allocator, uint32_t mask) {
  if (SetBaseline(mask) != 0) {
    return 1;
  }

  // Measure `Allocate`.
  void* ptr = allocator.Allocate(Layout::Of<Foo>());
  if (ptr == nullptr) {
    return 1;
  }

  // Measure `Resize`.
  std::ignore = allocator.Resize(ptr, sizeof(Bar));

  // Measure `Reallocate`.
  ptr = allocator.Reallocate(ptr, Layout::Of<Baz>());
  if (ptr == nullptr) {
    return 1;
  }

  // Measure `Deallocate`.
  allocator.Deallocate(ptr);

  // Measure `New`.
  Foo* foo = allocator.template New<Foo>();
  if (foo == nullptr) {
    return 1;
  }

  // Measure `Delete`.
  allocator.Delete(foo);

  // Measure `MakeUnique`.
  UniquePtr<Foo> unique_foo = allocator.template MakeUnique<Foo>();
  unique_foo.Reset();

  return 0;
}

int MeasureBlockAllocator(BlockAllocator<BlockType>& allocator, uint32_t mask) {
  if (int rc = MeasureBlock<BlockType>(mask); rc != 0) {
    return rc;
  }
  return MeasureAllocator(allocator, mask);
}

}  // namespace pw::allocator::size_report
