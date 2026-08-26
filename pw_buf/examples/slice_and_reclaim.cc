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

// DOCSTAG: [pw_buf-examples-slice_and_reclaim]
#include "pw_buf/buf.h"

namespace examples {

pw::Buf SliceAndReclaim(pw::Buf&& buf) {
  // Trim 10 bytes from the front and 5 bytes from the back.
  // The resulting sliced buffer view has size = original_size - 15.
  pw::Buf sliced = pw::Slice(std::move(buf), 10, buf.size() - 15);

  // ... perform operations on the sliced buffer ...

  // Reclaim the 10 prefix bytes and 5 suffix bytes.
  return pw::Reclaim(std::move(sliced), 10, 5);
}

}  // namespace examples
// DOCSTAG: [pw_buf-examples-slice_and_reclaim]

namespace {

TEST(ExampleTests, SliceAndReclaim) {
  pw::allocator::test::AllocatorForTest<256> test_allocator;
  auto unique_data = test_allocator.MakeUnique<std::byte[]>(100);
  pw::Buf buf(std::move(unique_data));

  pw::Buf reclaimed = examples::SliceAndReclaim(std::move(buf));
  EXPECT_EQ(reclaimed.size(), 100u);
}

}  // namespace
