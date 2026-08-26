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

// DOCSTAG: [pw_buf-examples-const_buf_reference]
#include "pw_buf/buf.h"

namespace examples {

void ReadData(const pw::ConstBuf& const_buf) {
  if (!const_buf.empty()) {
    // Read operations only
    std::byte first = const_buf[0];
    (void)first;
  }
}

void PassBufAsConstBufReference(pw::Buf& buf) {
  // pw::Buf implicitly converts to pw::ConstBuf&, allowing read-only access.
  ReadData(buf);
}

}  // namespace examples
// DOCSTAG: [pw_buf-examples-const_buf_reference]

namespace {

TEST(ExampleTests, PassBufAsConstBufReference) {
  pw::allocator::test::AllocatorForTest<256> test_allocator;
  auto unique_data = test_allocator.MakeUnique<std::byte[]>(10);
  pw::Buf buf(std::move(unique_data));

  examples::PassBufAsConstBufReference(buf);
}

}  // namespace
