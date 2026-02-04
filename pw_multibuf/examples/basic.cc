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

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_allocator/allocator.h"
#include "pw_allocator/testing.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/multibuf.h"

namespace pw::multibuf::examples {

TEST(BasicTest, DemonstrateMultiBuf) {
  // DOCSTAG: [pw_multibuf-examples-basic-allocator]
  allocator::test::AllocatorForTest<512> allocator;

  // DOCSTAG: [pw_multibuf-examples-basic]
  MultiBuf::Instance mbuf(allocator);
  // DOCSTAG: [pw_multibuf-examples-basic-allocator]

  // Add some memory regions.
  std::array<std::byte, 16> buffer;
  mbuf->PushBack(buffer);
  mbuf->Insert(mbuf->begin() + 8, allocator.MakeShared<std::byte[]>(16));
  mbuf->PushBack(allocator.MakeUnique<std::byte[]>(16));

  // Iterate and fill with data.
  std::fill(mbuf->begin(), mbuf->end(), std::byte(0xFF));

  // Access a discontiguous region.
  std::array<std::byte, 16> tmp;
  ConstByteSpan bytes = mbuf->Get(tmp, /*offset=*/16);
  for (const auto b : bytes) {
    EXPECT_EQ(static_cast<uint8_t>(b), 0xFF);
  }

  // Free owned memory.
  mbuf->Clear();
  // DOCSTAG: [pw_multibuf-examples-basic]
}

}  // namespace pw::multibuf::examples
