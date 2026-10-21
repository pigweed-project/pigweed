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

#include "pw_containers/deque.h"
#include "pw_containers/internal/container_tests.h"

namespace {

// The common tests are declared separate to keep the binary size manageable.
template <size_t kCapacity, typename SizeType>
class StaticCommonTest : public ::pw::containers::test::CommonTestFixture<
                             StaticCommonTest<kCapacity, SizeType>> {
 public:
  // "Container" declares an empty container usable in the test.
  template <typename T>
  class Container {
   public:
    Container(StaticCommonTest&) {}

    pw::FixedDeque<T, kCapacity>& get() { return container_; }
    const pw::FixedDeque<T, kCapacity>& get() const { return container_; }

   private:
    pw::FixedDeque<T, kCapacity> container_;
  };
};

using StaticDequeCommonTest9 = StaticCommonTest<9, uint16_t>;
using StaticDequeCommonTest10 = StaticCommonTest<10, uint32_t>;

PW_CONTAINERS_COMMON_DEQUE_TESTS(StaticDequeCommonTest9);
PW_CONTAINERS_COMMON_DEQUE_TESTS(StaticDequeCommonTest10);

}  // namespace
