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

#include "pw_allocator/tagging_allocator.h"

#include "pw_allocator/testing.h"
#include "pw_tokenizer/tokenize.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fixtures.

using ::pw::allocator::Layout;
using ::pw::allocator::TaggingAllocator;
using ::pw::tokenizer::Token;

class TaggingAllocatorTest : public ::testing::Test {
 protected:
  pw::allocator::test::AllocatorForTest<1024> allocator_;
};

// Unit tests.

TEST_F(TaggingAllocatorTest, AllocateSetsDefaultToken) {
  Token default_token = 0x12345678;
  TaggingAllocator tagging(default_token, allocator_);
  void* ptr = tagging.Allocate(Layout::Of<int>());
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(tagging.GetToken(ptr), default_token);
}

TEST_F(TaggingAllocatorTest, SetAndGetToken) {
  Token default_token = 0x12345678;
  TaggingAllocator tagging(default_token, allocator_);
  void* ptr = tagging.Allocate(Layout::Of<int>());
  ASSERT_NE(ptr, nullptr);

  Token new_token = 0xabcdef01;
  tagging.SetToken(ptr, new_token);
  EXPECT_EQ(tagging.GetToken(ptr), new_token);
}

TEST_F(TaggingAllocatorTest, ReallocatePreservesToken) {
  allocator_.DisableResize();  // Force reallocation.
  Token default_token = 0x12345678;
  TaggingAllocator tagging(default_token, allocator_);

  size_t size = 16;
  void* ptr = tagging.Allocate(Layout(size, 1));
  ASSERT_NE(ptr, nullptr);

  Token custom_token = 0xabcdef01;
  tagging.SetToken(ptr, custom_token);

  // Reallocate ptr to a larger size.
  size_t new_size = size * 2;
  void* new_ptr = tagging.Reallocate(ptr, Layout(new_size, 1));
  ASSERT_NE(new_ptr, nullptr);
  EXPECT_NE(new_ptr, ptr);
  EXPECT_EQ(tagging.GetToken(new_ptr), custom_token);
}

}  // namespace
