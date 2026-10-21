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

#include <cstddef>
#include <cstdint>

#include "pw_allocator/testing.h"
#include "pw_containers/deque.h"
#include "pw_containers/internal/container_tests.h"

namespace {

// The common tests are declared separate to keep the binary size manageable.
template <size_t kCapacity, typename SizeType>
class DynamicCommonTest : public ::pw::containers::test::CommonTestFixture<
                              DynamicCommonTest<kCapacity, SizeType>> {
 public:
  // "Container" declares an empty container usable in the test.
  template <typename T>
  class Container {
   public:
    Container(DynamicCommonTest& fixture)
        : container_(
              pw::FixedDeque<T, pw::containers::kExternalStorage, SizeType>::
                  Allocate(fixture.allocator_, kCapacity)) {}

    auto& get() { return container_; }
    const auto& get() const { return container_; }

   private:
    pw::FixedDeque<T, pw::containers::kExternalStorage, SizeType> container_;
  };

 private:
  ::pw::allocator::test::AllocatorForTest<1024> allocator_;
};

using DynamicDequeCommonTest9 = DynamicCommonTest<9, uint8_t>;
using DynamicDequeCommonTest16 = DynamicCommonTest<16, uint16_t>;

PW_CONTAINERS_COMMON_DEQUE_TESTS(DynamicDequeCommonTest9);
PW_CONTAINERS_COMMON_DEQUE_TESTS(DynamicDequeCommonTest16);

}  // namespace
