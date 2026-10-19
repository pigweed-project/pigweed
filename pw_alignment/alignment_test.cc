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

#include "pw_alignment/alignment.h"

#include <cstdint>

#include "pw_unit_test/framework.h"

namespace pw {
namespace {

struct ThreeBytes {
  uint8_t a;
  uint8_t b;
  uint8_t c;

  bool operator==(const ThreeBytes& other) const {
    return a == other.a && b == other.b && c == other.c;
  }
};

TEST(Alignment, BitCeil) {
  EXPECT_EQ(bit_ceil(0u), 1u);
  EXPECT_EQ(bit_ceil(1u), 1u);
  EXPECT_EQ(bit_ceil(2u), 2u);
  EXPECT_EQ(bit_ceil(3u), 4u);
  EXPECT_EQ(bit_ceil(4u), 4u);
  EXPECT_EQ(bit_ceil(5u), 8u);
}

TEST(Alignment, NaturallyAligned) {
  static_assert(sizeof(ThreeBytes) == 3);
  static_assert(alignof(ThreeBytes) == 1);

  static_assert(alignof(NaturallyAligned<ThreeBytes>) == 4);
  static_assert(sizeof(NaturallyAligned<ThreeBytes>) == 4);

  NaturallyAligned<ThreeBytes> obj;
  obj.a = 1;
  obj.b = 2;
  obj.c = 3;

  EXPECT_EQ(obj.a, 1);
  EXPECT_EQ(obj.b, 2);
  EXPECT_EQ(obj.c, 3);
}

TEST(Alignment, AlignedAtomic) {
  AlignedAtomic<ThreeBytes> atomic_obj;
  NaturallyAligned<ThreeBytes> val;
  val.a = 10;
  val.b = 20;
  val.c = 30;
  atomic_obj.store(val);

  NaturallyAligned<ThreeBytes> loaded = atomic_obj.load();
  EXPECT_EQ(loaded.a, 10);
  EXPECT_EQ(loaded.b, 20);
  EXPECT_EQ(loaded.c, 30);
}

}  // namespace
}  // namespace pw
