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

// DOCSTAG: [pw_buf-examples-unique_ptr]
#include "pw_buf/buf.h"

namespace examples {

pw::Buf CreateBufFromUniquePtr(pw::Allocator& allocator) {
  pw::UniquePtr<std::byte[]> unique_data =
      allocator.MakeUnique<std::byte[]>(100);
  if (unique_data == nullptr) {
    return pw::Buf();
  }
  // Construct a Buf by moving the UniquePtr into it.
  // The Buf now owns the allocation.
  return pw::Buf(std::move(unique_data));
}

}  // namespace examples
// DOCSTAG: [pw_buf-examples-unique_ptr]

namespace {

TEST(ExampleTests, CreateBufFromUniquePtr) {
  pw::allocator::test::AllocatorForTest<256> test_allocator;
  pw::Buf buf = examples::CreateBufFromUniquePtr(test_allocator);
  EXPECT_EQ(buf.size(), 100u);
}

}  // namespace
