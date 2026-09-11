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

#include "pw_allocator/framing_allocator.h"

#include <cstddef>

#include "pw_allocator/testing.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::FramingAllocator;
using ::pw::allocator::Layout;

// Test types

struct Trivial {
  uint32_t value;
};

// This type is trivially copyable, but has a non-default alignment.
struct alignas(16) AlignedTrivial {
  uint32_t value;
};

struct Base {
  uint32_t x;
};

struct Derived : public Base {
  uint32_t y;
};

// Non-trivial type for the *allocated* data, not the prefix/suffix.
struct NonTrivial {
  static int count;
  uint32_t value;
  NonTrivial() : value(0) { count++; }
  NonTrivial(uint32_t v) : value(v) { count++; }
  ~NonTrivial() { count--; }
};
int NonTrivial::count = 0;

struct NotDefaultConstructible {
  NotDefaultConstructible() = delete;
};

struct NotTriviallyCopyable {
  NotTriviallyCopyable() = default;
  NotTriviallyCopyable(const NotTriviallyCopyable&) {}
};

const Layout kLayouts[] = {
    Layout(0, 1),
    Layout(1, 1),
    Layout(3, 2),
    Layout(64, 16),
    Layout(128, 64),
};

template <typename Prefix, typename Suffix>
class TestFramingAllocator : public FramingAllocator<Prefix, Suffix> {
 public:
  TestFramingAllocator(pw::Allocator& allocator)
      : FramingAllocator<Prefix, Suffix>(allocator) {}

  using FramingAllocator<Prefix, Suffix>::GetFrameLayout;
  using FramingAllocator<Prefix, Suffix>::IsValid;
  using FramingAllocator<Prefix, Suffix>::GetPrefix;
  using FramingAllocator<Prefix, Suffix>::GetData;
  using FramingAllocator<Prefix, Suffix>::GetSuffix;
};

class FramingAllocatorTest : public ::testing::Test {
 protected:
  void SetUp() override { NonTrivial::count = 0; }

  pw::allocator::test::AllocatorForTest<2048> allocator_;
};

TEST_F(FramingAllocatorTest, GetFrameLayoutVoidTrivial) {
  for (const auto& layout : kLayouts) {
    Layout frame_layout =
        TestFramingAllocator<void, Trivial>::GetFrameLayout(layout);

    size_t expected_size = 2 * sizeof(size_t);
    expected_size += layout.alignment() + layout.size();
    expected_size += alignof(Trivial) + sizeof(Trivial);

    EXPECT_EQ(frame_layout.size(), expected_size);
    EXPECT_EQ(frame_layout.alignment(), alignof(size_t));
  }
}

TEST_F(FramingAllocatorTest, GetFrameLayoutTrivialVoid) {
  for (const auto& layout : kLayouts) {
    Layout frame_layout =
        TestFramingAllocator<Trivial, void>::GetFrameLayout(layout);

    size_t expected_size = pw::AlignUp(sizeof(Trivial), alignof(size_t));
    expected_size += 2 * sizeof(size_t);
    expected_size += layout.alignment() + layout.size();

    EXPECT_EQ(frame_layout.size(), expected_size);
    EXPECT_EQ(frame_layout.alignment(), alignof(size_t));
  }
}

TEST_F(FramingAllocatorTest, GetFrameLayoutTrivialAlignedTrivial) {
  for (const auto& layout : kLayouts) {
    Layout frame_layout =
        TestFramingAllocator<Trivial, AlignedTrivial>::GetFrameLayout(layout);

    size_t expected_size = pw::AlignUp(sizeof(Trivial), alignof(size_t));
    expected_size += 3 * sizeof(size_t);
    expected_size += layout.alignment() + layout.size();
    expected_size += alignof(AlignedTrivial) + sizeof(AlignedTrivial);

    EXPECT_EQ(frame_layout.size(), expected_size);
    EXPECT_EQ(frame_layout.alignment(), alignof(size_t));
  }
}

TEST_F(FramingAllocatorTest, AllocateDeallocateTrivialTrivial) {
  TestFramingAllocator<Trivial, Trivial> framing_allocator(allocator_);
  void* ptr = framing_allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  // Check if pointer is aligned.
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % alignof(size_t), 0u);

  Trivial* prefix = framing_allocator.GetPrefix(ptr);
  Trivial* suffix = framing_allocator.GetSuffix(ptr);

  prefix->value = 0x12345678;
  suffix->value = 0x87654321;

  EXPECT_EQ(framing_allocator.GetPrefix(ptr)->value, 0x12345678u);
  EXPECT_EQ(framing_allocator.GetSuffix(ptr)->value, 0x87654321u);

  // Confirm GetData works.
  EXPECT_EQ(framing_allocator.GetData(prefix), ptr);

  framing_allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, AllocateDeallocateTrivialAlignedTrivial) {
  TestFramingAllocator<Trivial, AlignedTrivial> framing_allocator(allocator_);
  void* ptr = framing_allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  Trivial* prefix = framing_allocator.GetPrefix(ptr);
  AlignedTrivial* suffix = framing_allocator.GetSuffix(ptr);

  EXPECT_EQ(reinterpret_cast<uintptr_t>(prefix) % alignof(Trivial), 0u);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(suffix) % alignof(AlignedTrivial), 0u);

  prefix->value = 0xAAAAAAAA;
  suffix->value = 0xBBBBBBBB;

  EXPECT_EQ(framing_allocator.GetPrefix(ptr)->value, 0xAAAAAAAAu);
  EXPECT_EQ(framing_allocator.GetSuffix(ptr)->value, 0xBBBBBBBBu);

  // Confirm GetData works.
  EXPECT_EQ(framing_allocator.GetData(prefix), ptr);

  framing_allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, ResizeTrivialTrivial) {
  TestFramingAllocator<Trivial, Trivial> framing_allocator(allocator_);
  void* ptr = framing_allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  framing_allocator.GetPrefix(ptr)->value = 0x11111111;
  framing_allocator.GetSuffix(ptr)->value = 0x22222222;

  EXPECT_TRUE(framing_allocator.Resize(ptr, sizeof(uint32_t) * 2));
  EXPECT_EQ(framing_allocator.GetPrefix(ptr)->value, 0x11111111u);
  EXPECT_EQ(framing_allocator.GetSuffix(ptr)->value, 0x22222222u);

  framing_allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, ReallocateTrivialTrivial) {
  TestFramingAllocator<Trivial, Trivial> framing_allocator(allocator_);
  void* ptr = framing_allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);
  framing_allocator.GetPrefix(ptr)->value = 0x33333333;
  framing_allocator.GetSuffix(ptr)->value = 0x44444444;

  void* new_ptr = framing_allocator.Reallocate(ptr, Layout::Of<uint64_t>());
  ASSERT_NE(new_ptr, nullptr);
  EXPECT_EQ(framing_allocator.GetPrefix(new_ptr)->value, 0x33333333u);
  EXPECT_EQ(framing_allocator.GetSuffix(new_ptr)->value, 0x44444444u);

  framing_allocator.Deallocate(new_ptr);
}

TEST_F(FramingAllocatorTest, NewDelete) {
  TestFramingAllocator<Trivial, Trivial> framing_allocator(allocator_);
  EXPECT_EQ(NonTrivial::count, 0);

  NonTrivial* ptr = framing_allocator.New<NonTrivial>(0x123u);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(NonTrivial::count, 1);
  EXPECT_EQ(ptr->value, 0x123u);

  framing_allocator.Delete(ptr);
  EXPECT_EQ(NonTrivial::count, 0);
}

TEST_F(FramingAllocatorTest, MakeUnique) {
  TestFramingAllocator<Trivial, Trivial> framing_allocator(allocator_);
  EXPECT_EQ(NonTrivial::count, 0);
  {
    auto unique_ptr = framing_allocator.MakeUnique<NonTrivial>(0x456u);
    ASSERT_NE(unique_ptr.get(), nullptr);
    EXPECT_EQ(NonTrivial::count, 1);
    EXPECT_EQ(unique_ptr->value, 0x456u);
  }
  EXPECT_EQ(NonTrivial::count, 0);
}

TEST_F(FramingAllocatorTest, MakeShared) {
#if PW_ALLOCATOR_HAS_ATOMICS
  TestFramingAllocator<Trivial, Trivial> framing_allocator(allocator_);
  EXPECT_EQ(NonTrivial::count, 0);
  {
    auto shared_ptr = framing_allocator.MakeShared<NonTrivial>(0x789u);
    ASSERT_NE(shared_ptr.get(), nullptr);
    EXPECT_EQ(NonTrivial::count, 1);
    EXPECT_EQ(shared_ptr->value, 0x789u);
  }
  EXPECT_EQ(NonTrivial::count, 0);
#endif
}

TEST_F(FramingAllocatorTest, DerivedTypesAsFrame) {
  TestFramingAllocator<Derived, Base> framing_allocator(allocator_);
  void* ptr = framing_allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  Derived* prefix = framing_allocator.GetPrefix(ptr);
  Base* suffix = framing_allocator.GetSuffix(ptr);

  prefix->x = 1;
  prefix->y = 2;
  suffix->x = 3;

  EXPECT_EQ(framing_allocator.GetPrefix(ptr)->x, 1u);
  EXPECT_EQ(framing_allocator.GetPrefix(ptr)->y, 2u);
  EXPECT_EQ(framing_allocator.GetSuffix(ptr)->x, 3u);

  framing_allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, PrefixOnly) {
  TestFramingAllocator<Trivial, void> framing_allocator(allocator_);
  void* ptr = framing_allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  Trivial* prefix = framing_allocator.GetPrefix(ptr);
  prefix->value = 0xDEADBEEF;
  EXPECT_EQ(framing_allocator.GetPrefix(ptr)->value, 0xDEADBEEFu);

  // Confirm GetData works.
  EXPECT_EQ(framing_allocator.GetData(prefix), ptr);

  framing_allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, SuffixOnly) {
  TestFramingAllocator<void, Trivial> framing_allocator(allocator_);
  void* ptr = framing_allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  Trivial* suffix = framing_allocator.GetSuffix(ptr);
  suffix->value = 0xFEEDFACE;
  EXPECT_EQ(framing_allocator.GetSuffix(ptr)->value, 0xFEEDFACEu);

  // To check GetData without a prefix, find the block that holds the ptr.
  auto* data = reinterpret_cast<std::byte*>(ptr);
  for (auto* block : allocator_.blocks()) {
    std::byte* frame = block->UsableSpace();
    if (frame < data && data < frame + block->InnerSize()) {
      EXPECT_EQ(framing_allocator.GetData(frame), data);
      break;
    }
  }

  framing_allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, AllocatedPointerIsValid) {
  TestFramingAllocator<size_t, size_t> framing_allocator(allocator_);
  auto bytes = framing_allocator.MakeUnique<std::byte[]>(16);
  ASSERT_NE(bytes, nullptr);
  EXPECT_TRUE(framing_allocator.IsValid(bytes.get()));
}

TEST_F(FramingAllocatorTest, MisalignedPointerIsInvalid) {
  TestFramingAllocator<size_t, size_t> framing_allocator(allocator_);
  auto bytes = framing_allocator.MakeUnique<std::byte[]>(16);
  ASSERT_NE(bytes, nullptr);
  EXPECT_FALSE(framing_allocator.IsValid(bytes.get() + 1));
}

TEST_F(FramingAllocatorTest, OutOfRangePointerIsInvalid) {
  TestFramingAllocator<size_t, size_t> framing_allocator(allocator_);

  pw::ByteSpan buffer = allocator_.buffer();
  EXPECT_FALSE(framing_allocator.IsValid(buffer.data() - 1));
  EXPECT_FALSE(framing_allocator.IsValid(buffer.data() + buffer.size()));

  EXPECT_FALSE(framing_allocator.IsValid(nullptr));
  EXPECT_FALSE(framing_allocator.IsValid(
      reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max())));
}

TEST_F(FramingAllocatorTest, UnrecognizedPointerIsInvalid) {
  pw::allocator::test::AllocatorForTest<256> allocator1;
  pw::allocator::test::AllocatorForTest<256> allocator2;

  TestFramingAllocator<void, Trivial> framing_allocator1(allocator1);
  TestFramingAllocator<void, Trivial> framing_allocator2(allocator2);

  auto bytes = framing_allocator1.MakeUnique<std::byte[]>(16);
  ASSERT_NE(bytes, nullptr);

  EXPECT_TRUE(framing_allocator1.IsValid(bytes.get()));
  EXPECT_FALSE(framing_allocator2.IsValid(bytes.get()));
}

TEST_F(FramingAllocatorTest, CorruptedFrameIsInvalid) {
  TestFramingAllocator<size_t, size_t> framing_allocator(allocator_);
  auto bytes = framing_allocator.MakeUnique<std::byte[]>(16);
  ASSERT_NE(bytes, nullptr);

  auto* data = reinterpret_cast<uint8_t*>(bytes.get());
  for (size_t i = 1; i <= sizeof(size_t) * 2; ++i) {
    EXPECT_TRUE(framing_allocator.IsValid(data));
    *(data - i) ^= 0xFF;
    EXPECT_FALSE(framing_allocator.IsValid(data));
    *(data - i) ^= 0xFF;
    EXPECT_TRUE(framing_allocator.IsValid(data));
  }
}

#if PW_NC_TEST(PrefixAndSuffixCannotBothBeVoid)
PW_NC_EXPECT("prefix and suffix types cannot both be null");
void GetPrefixVoid(pw::Allocator& allocator) {
  TestFramingAllocator<void, void> framing(allocator);
}
#elif PW_NC_TEST(GetPrefixFailsWhenVoid)
PW_NC_EXPECT("prefix type is void");
void GetPrefixVoid(TestFramingAllocator<void, uint16_t>& allocator, void* ptr) {
  allocator.GetPrefix(ptr);
}
#elif PW_NC_TEST(GetSuffixFailsWhenVoid)
PW_NC_EXPECT("suffix type is void");
void GetSuffixVoid(TestFramingAllocator<uint16_t, void>& allocator, void* ptr) {
  allocator.GetSuffix(ptr);
}
#elif PW_NC_TEST(PrefixNotDefaultConstructible)
PW_NC_EXPECT("prefix type must be void or default constructible");
void PrefixNotDef(pw::Allocator& allocator) {
  FramingAllocator<NotDefaultConstructible, void> framing(allocator);
}
#elif PW_NC_TEST(SuffixNotDefaultConstructible)
PW_NC_EXPECT("suffix type must be void or default constructible");
void SuffixNotDef(pw::Allocator& allocator) {
  FramingAllocator<void, NotDefaultConstructible> framing(allocator);
}
#elif PW_NC_TEST(PrefixNotTriviallyCopyable)
PW_NC_EXPECT("prefix type must be void or trivially copyable");
void PrefixNotTrivial(pw::Allocator& allocator) {
  FramingAllocator<NotTriviallyCopyable, void> framing(allocator);
}
#elif PW_NC_TEST(SuffixNotTriviallyCopyable)
PW_NC_EXPECT("suffix type must be void or trivially copyable");
void SuffixNotTrivial(pw::Allocator& allocator) {
  FramingAllocator<void, NotTriviallyCopyable> framing(allocator);
}
#elif PW_NC_TEST(GetFrameLayoutFailsWhenPrefixAlignmentTooLarge)
PW_NC_EXPECT("alignof\(Prefix\) cannot exceed alignof\(size_t\)");
void GetFrameLayoutLargePrefixAlign() {
  TestFramingAllocator<AlignedTrivial, void>::GetFrameLayout(Layout(0, 1));
}
#endif

}  // namespace
