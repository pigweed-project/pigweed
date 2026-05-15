// Copyright 2024 The Pigweed Authors
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

#include "examples/named_u32.h"
#include "pw_allocator/allocator.h"
#include "pw_allocator/testing.h"
#include "pw_unit_test/framework.h"

namespace examples {

// DOCSTAG: [pw_allocator-examples-basic-allocate]
using pw::allocator::Layout;

void* AllocateNamedU32(pw::Allocator& allocator) {
  return allocator.Allocate(Layout::Of<NamedU32>());
}
// DOCSTAG: [pw_allocator-examples-basic-allocate]

// DOCSTAG: [pw_allocator-examples-basic-deallocate]
void DeallocateNamedU32(pw::Allocator& allocator, void* ptr) {
  allocator.Deallocate(ptr);
}
// DOCSTAG: [pw_allocator-examples-basic-deallocate]

// DOCSTAG: [pw_allocator-examples-basic-new_delete]
NamedU32* NewNamedU32(pw::Allocator& allocator,
                      std::string_view name,
                      uint32_t value) {
  return allocator.New<NamedU32>(name, value);
}

void DeleteNamedU32(pw::Allocator& allocator, NamedU32* named_u32) {
  allocator.Delete<NamedU32>(named_u32);
}
// DOCSTAG: [pw_allocator-examples-basic-new_delete]

// DOCSTAG: [pw_allocator-examples-basic-make_unique]
pw::UniquePtr<NamedU32> MakeNamedU32(pw::Allocator& allocator,
                                     std::string_view name,
                                     uint32_t value) {
  return allocator.MakeUnique<NamedU32>(name, value);
}
// DOCSTAG: [pw_allocator-examples-basic-make_unique]

}  // namespace examples

namespace {

using AllocatorForTest = ::pw::allocator::test::AllocatorForTest<256>;

class BasicExampleTest : public ::testing::Test {
 protected:
  AllocatorForTest allocator_;
};

TEST_F(BasicExampleTest, AllocateNamedU32) {
  void* ptr = examples::AllocateNamedU32(allocator_);
  ASSERT_NE(ptr, nullptr);
  examples::DeallocateNamedU32(allocator_, ptr);
}

TEST_F(BasicExampleTest, NewNamedU32) {
  auto* named_u32 = examples::NewNamedU32(allocator_, "test1", 111u);
  ASSERT_NE(named_u32, nullptr);
  EXPECT_STREQ(named_u32->name().data(), "test1");
  EXPECT_EQ(named_u32->value(), 111U);
  examples::DeleteNamedU32(allocator_, named_u32);
}

TEST_F(BasicExampleTest, MakeNamedU32) {
  pw::UniquePtr<examples::NamedU32> named_u32 =
      examples::MakeNamedU32(allocator_, "test2", 222u);
  ASSERT_NE(named_u32, nullptr);
  EXPECT_STREQ(named_u32->name().data(), "test2");
  EXPECT_EQ(named_u32->value(), 222U);
}

}  // namespace
