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

#include "pw_enum/traits.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "enum_example/basic_enum.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_unit_test/framework.h"

namespace {

// DOCSTAG: [pw_enum-examples-traits]
// A fixed-size array indexed by any contiguous PW_ENUM enum. Aliased
// enumerators share an entry, and enums that do not start at 0 are offset by
// kMin.
template <typename Enum, typename T>
class EnumArray {
 public:
  using Traits = pw::EnumTraits<Enum>;
  static_assert(Traits::kIsContiguous, "EnumArray requires a contiguous enum");

  constexpr T& operator[](Enum value) { return values_[Index(value)]; }
  constexpr const T& operator[](Enum value) const {
    return values_[Index(value)];
  }

  constexpr size_t size() const { return Traits::kDistinctValueCount; }

 private:
  static constexpr size_t Index(Enum value) {
    return static_cast<size_t>(
        static_cast<typename Traits::underlying_type>(value) -
        static_cast<typename Traits::underlying_type>(Traits::kMin));
  }

  std::array<T, Traits::kDistinctValueCount> values_{};
};
// DOCSTAG: [pw_enum-examples-traits]

// DOCSTAG: [pw_enum-examples-traits-validate]
using my::nested::pkg::MyEnum;

static_assert(pw::has_enum_traits_v<MyEnum>);

// The enum type is deduced when validating an enumerator.
static_assert(pw::IsValidEnum(MyEnum::kAlpha));

// Name the enum explicitly to validate an integer, such as a value received
// over the wire. Validating before the cast keeps out-of-range values from
// becoming enumerators that no switch case handles.
pw::Result<MyEnum> DecodeMyEnum(uint32_t wire_value) {
  if (!pw::IsValidEnum<MyEnum>(wire_value)) {
    return pw::Status::DataLoss();
  }
  return static_cast<MyEnum>(wire_value);
}
// DOCSTAG: [pw_enum-examples-traits-validate]

TEST(EnumTraitsExample, EnumArrayCountsDistinctValues) {
  EnumArray<MyEnum, size_t> counts;
  EXPECT_EQ(counts.size(), 2u);

  counts[MyEnum::kAlpha] += 1;
  counts[MyEnum::kBeta] += 1;
  counts[MyEnum::kAliasedBeta] += 1;

  EXPECT_EQ(counts[MyEnum::kAlpha], 1u);
  EXPECT_EQ(counts[MyEnum::kBeta], 2u);

  size_t total = 0;
  for (MyEnum value : pw::EnumTraits<MyEnum>::kValues) {
    total += counts[value];
  }
  EXPECT_EQ(total, 3u);
}

TEST(EnumTraitsExample, DecodeValidValues) {
  EXPECT_EQ(DecodeMyEnum(0).value(), MyEnum::kAlpha);
  EXPECT_EQ(DecodeMyEnum(1).value(), MyEnum::kBeta);

  // kAliasedBeta shares a value with kBeta, so it decodes to the same value.
  EXPECT_EQ(DecodeMyEnum(1).value(), MyEnum::kAliasedBeta);
}

TEST(EnumTraitsExample, RejectsOutOfRangeValues) {
  EXPECT_EQ(DecodeMyEnum(2).status(), pw::Status::DataLoss());
  EXPECT_EQ(DecodeMyEnum(1000).status(), pw::Status::DataLoss());
}

}  // namespace
