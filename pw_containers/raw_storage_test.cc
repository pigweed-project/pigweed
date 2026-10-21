// Copyright 2020 The Pigweed Authors
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

#include "pw_containers/internal/raw_storage.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "pw_containers/internal/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace pw::containers {
namespace {

using test::CopyOnly;
using test::Counter;

class Empty {
 public:
  constexpr Empty(size_t) {}
};

class ClearTracker {
 public:
  constexpr ClearTracker(size_t) {}
  void clear() {
    if (clear_called_) {
      *clear_called_ = true;
    }
  }

  void set_clear_called(bool* clear_called) { clear_called_ = clear_called; }

 private:
  bool* clear_called_ = nullptr;
};

TEST(RawStorage, Construct_ZeroSized) {
  internal::RawStorage<Empty, int, 0> array;
  EXPECT_EQ(array.max_size(), 0u);
}

TEST(RawStorage, Construct_NonZeroSized) {
  internal::RawStorage<Empty, int, 3> array;
  EXPECT_EQ(array.max_size(), 3u);
}

TEST(RawStorage, Construct_Constexpr) {
  constexpr internal::RawStorage<Empty, int, 2> kArray;
  EXPECT_EQ(kArray.max_size(), 2u);
}

TEST(RawStorage, Construct_CopyOnly) {
  internal::RawStorage<Empty, CopyOnly, 2> array;
  EXPECT_EQ(array.max_size(), 2u);
}

TEST(RawStorage, Construct_MoveOnly) {
  internal::RawStorage<Empty, test::TrivialMoveOnly, 2> array;
  EXPECT_EQ(array.max_size(), 2u);
}

TEST(RawStorage, DestructTrivial) {
  bool clear_called = false;
  {
    internal::RawStorage<ClearTracker, int, 2> array;
    array.set_clear_called(&clear_called);
  }
  EXPECT_FALSE(clear_called);
}

TEST(RawStorage, DestructNonTrivial) {
  Counter::Reset();
  bool clear_called = false;
  {
    internal::RawStorage<ClearTracker, Counter, 128> array;
    array.set_clear_called(&clear_called);
  }

  EXPECT_EQ(Counter::created, 0);
  EXPECT_EQ(Counter::destroyed, 0);
  EXPECT_TRUE(clear_called);
}

static_assert(sizeof(internal::RawStorage<Empty, uint8_t, 42>) ==
              42 * sizeof(uint8_t));
static_assert(sizeof(internal::RawStorage<Empty, uint16_t, 42>) ==
              42 * sizeof(uint16_t));
static_assert(sizeof(internal::RawStorage<Empty, uint32_t, 42>) ==
              42 * sizeof(uint32_t));

}  // namespace
}  // namespace pw::containers
