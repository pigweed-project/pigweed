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

#include "pw_allocator/origin_allocator.h"

#include "pw_allocator/testing.h"
#include "pw_tokenizer/tokenize.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fixtures.

using ::pw::allocator::Layout;
using ::pw::allocator::OriginAllocator;
using ::pw::allocator::SourceLocation;
using ::pw::tokenizer::Token;

class OriginAllocatorTest : public ::testing::Test {
 protected:
  pw::allocator::test::AllocatorForTest<1024> allocator_;
};

// Unit tests.

TEST_F(OriginAllocatorTest, Allocate) {
  OriginAllocator alloc(allocator_);
  Layout layout = Layout::Of<uint32_t>();
  uint32_t line = __LINE__ + 1;
  void* ptr = alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.Allocate(layout));
  ASSERT_NE(ptr, nullptr);

  SourceLocation loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Deallocate(ptr);

  line = __LINE__ + 1;
  ptr = PW_ALLOCATOR_SET_ORIGIN(alloc, Allocate(layout));
  ASSERT_NE(ptr, nullptr);

  loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Deallocate(ptr);
}

TEST_F(OriginAllocatorTest, Deallocate) {
  OriginAllocator alloc(allocator_);
  Layout layout = Layout::Of<uint32_t>();
  void* ptr = alloc.Allocate(layout);
  ASSERT_NE(ptr, nullptr);
  alloc.Deallocate(ptr);
}

TEST_F(OriginAllocatorTest, Resize) {
  OriginAllocator alloc(allocator_);
  Layout layout = Layout::Of<uint32_t>();
  uint32_t line = __LINE__ + 1;
  void* ptr = alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.Allocate(layout));

  ASSERT_NE(ptr, nullptr);

  size_t new_size = sizeof(uint32_t) * 2;
  EXPECT_TRUE(alloc.Resize(ptr, new_size));

  SourceLocation loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Deallocate(ptr);
}

TEST_F(OriginAllocatorTest, Reallocate) {
  OriginAllocator alloc(allocator_);
  Layout layout = Layout::Of<uint32_t>();
  void* ptr = alloc.Allocate(layout);
  ASSERT_NE(ptr, nullptr);

  Layout new_layout = Layout::Of<uint64_t>();
  uint32_t line = __LINE__ + 2;
  void* new_ptr =
      alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.Reallocate(ptr, new_layout));
  ASSERT_NE(new_ptr, nullptr);

  SourceLocation loc = alloc.GetOrigin(new_ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);

  line = __LINE__ + 1;
  ptr = PW_ALLOCATOR_SET_ORIGIN(alloc, Reallocate(new_ptr, layout));
  ASSERT_NE(ptr, nullptr);

  loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Deallocate(ptr);
}

TEST_F(OriginAllocatorTest, New) {
  OriginAllocator alloc(allocator_);

  // 1. Non-array object: New<T>(args...)
  uint32_t line = __LINE__ + 2;
  uint32_t* ptr =
      alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.New<uint32_t>(1u));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(*ptr, 1u);

  SourceLocation loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Delete(ptr);

  line = __LINE__ + 1;
  ptr = PW_ALLOCATOR_SET_ORIGIN(alloc, New<uint32_t>(2u));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(*ptr, 2u);

  loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Delete(ptr);

  // 2. Bounded array: New<T[N]>()
  line = __LINE__ + 1;
  ptr = alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.New<uint32_t[3]>());
  ASSERT_NE(ptr, nullptr);

  loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Delete<uint32_t[3]>(ptr);

  line = __LINE__ + 1;
  ptr = PW_ALLOCATOR_SET_ORIGIN(alloc, New<uint32_t[3]>());
  ASSERT_NE(ptr, nullptr);

  loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Delete<uint32_t[3]>(ptr);

  // 3. Unbounded array: New<T[]>(count)
  line = __LINE__ + 1;
  ptr = alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.New<uint32_t[]>(3));
  ASSERT_NE(ptr, nullptr);

  loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Delete<uint32_t[]>(ptr, 3);

  line = __LINE__ + 1;
  ptr = PW_ALLOCATOR_SET_ORIGIN(alloc, New<uint32_t[]>(3));
  ASSERT_NE(ptr, nullptr);

  loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Delete<uint32_t[]>(ptr, 3);

  // 4. Unbounded array with alignment: New<T[]>(count, alignment)
  line = __LINE__ + 1;
  ptr = alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(),
                        alloc.New<uint32_t[]>(3, alignof(uint64_t)));
  ASSERT_NE(ptr, nullptr);

  loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Delete<uint32_t[]>(ptr, 3);

  line = __LINE__ + 1;
  ptr = PW_ALLOCATOR_SET_ORIGIN(alloc, New<uint32_t[]>(3, alignof(uint64_t)));
  ASSERT_NE(ptr, nullptr);

  loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Delete<uint32_t[]>(ptr, 3);
}

TEST_F(OriginAllocatorTest, MakeUnique) {
  OriginAllocator alloc(allocator_);
  uint32_t line = __LINE__ + 2;
  pw::UniquePtr<uint32_t> ptr =
      alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.MakeUnique<uint32_t>(1u));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(*ptr, 1u);

  SourceLocation loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);

  line = __LINE__ + 1;
  ptr = PW_ALLOCATOR_SET_ORIGIN(alloc, MakeUnique<uint32_t>(2u));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(*ptr, 2u);

  loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
}

#if PW_ALLOCATOR_HAS_ATOMICS
TEST_F(OriginAllocatorTest, MakeShared) {
  OriginAllocator alloc(allocator_);
  uint32_t line = __LINE__ + 2;
  pw::SharedPtr<uint32_t> ptr =
      alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), alloc.MakeShared<uint32_t>(1u));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(*ptr, 1u);

  SourceLocation loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);

  line = __LINE__ + 1;
  ptr = PW_ALLOCATOR_SET_ORIGIN(alloc, MakeShared<uint32_t>(2u));
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(*ptr, 2u);

  loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
}
#endif  // PW_ALLOCATOR_HAS_ATOMICS

TEST_F(OriginAllocatorTest, SetOrigin) {
  OriginAllocator alloc(allocator_);
  Layout layout = Layout::Of<uint32_t>();
  void* ptr = alloc.Allocate(layout);
  ASSERT_NE(ptr, nullptr);

  uint32_t line = __LINE__ + 1;
  void* returned_ptr = alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), ptr);
  EXPECT_EQ(returned_ptr, ptr);

  SourceLocation loc = alloc.GetOrigin(ptr);
  EXPECT_EQ(loc.file, PW_TOKENIZE_STRING_EXPR(__FILE__));
  EXPECT_EQ(loc.line, line);
  alloc.Deallocate(ptr);
}

TEST_F(OriginAllocatorTest, SetOriginNullptr) {
  OriginAllocator alloc(allocator_);
  EXPECT_EQ(alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), static_cast<void*>(nullptr)),
            nullptr);
  EXPECT_EQ(alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), pw::UniquePtr<uint32_t>()),
            nullptr);
#if PW_ALLOCATOR_HAS_ATOMICS
  EXPECT_EQ(alloc.SetOrigin(PW_ALLOCATOR_ORIGIN(), pw::SharedPtr<uint32_t>()),
            nullptr);
#endif  // PW_ALLOCATOR_HAS_ATOMICS
}

}  // namespace
