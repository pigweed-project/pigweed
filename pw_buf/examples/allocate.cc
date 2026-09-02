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

#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

// DOCSTAG: [pw_buf-examples-allocate]
#include "pw_buf/buf.h"

namespace examples {

pw::Buf AllocateBuf(pw::Allocator& allocator) {
  // Allocate 100 bytes using the allocator.
  // This will PW_ASSERT if allocation fails.
  return pw::Buf::Allocate(allocator, 100);
}

pw::Buf TryAllocateBuf(pw::Allocator& allocator) {
  // Try to allocate 100 bytes. Returns a null Buf on failure.
  pw::Buf buf = pw::Buf::TryAllocate(allocator, 100);
  if (buf == nullptr) {
    // Handle allocation failure.
  }
  return buf;
}

}  // namespace examples
// DOCSTAG: [pw_buf-examples-allocate]

namespace {

TEST(ExampleTests, AllocateBuf) {
  pw::allocator::test::AllocatorForTest<256> allocator;

  // DOCSTAG: [pw_buf-examples-move]
  pw::Buf buf = examples::AllocateBuf(allocator);
  pw::ConstBuf const_buf = std::move(buf);
  // DOCSTAG: [pw_buf-examples-move]

  EXPECT_EQ(buf, nullptr);  // NOLINT(bugprone-use-after-move)
  EXPECT_NE(const_buf, nullptr);
  EXPECT_EQ(const_buf.size(), 100u);
}

TEST(ExampleTests, TryAllocateBuf) {
  pw::allocator::test::AllocatorForTest<256> test_allocator;
  pw::Buf buf = examples::TryAllocateBuf(test_allocator);
  EXPECT_EQ(buf.size(), 100u);
}

}  // namespace
