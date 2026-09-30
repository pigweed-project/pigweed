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
#include <cstring>
#include <optional>

#include "pw_allocator/testing.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_unit_test/framework.h"

namespace {

using ::pw::allocator::FramingAllocator;
using ::pw::allocator::Layout;
using ::pw::allocator::internal::FrameError;

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

// An "error handler" that records the first error encountered, allowing
// failures to be detected and inspected without crashing.
struct FrameErrorRecorder {
  static inline std::optional<FrameError> error = std::nullopt;
  static inline const void* ptr1 = nullptr;
  static inline size_t val1 = 0;
  static inline const void* ptr2 = nullptr;
  static inline size_t val2 = 0;

  static void Reset() {
    error.reset();
    ptr1 = nullptr;
    val1 = 0;
    ptr2 = nullptr;
    val2 = 0;
  }

  static void HandleError(FrameError err,
                          const void* p1 = nullptr,
                          size_t v1 = 0,
                          const void* p2 = nullptr,
                          size_t v2 = 0) {
    if (!error.has_value()) {
      error = err;
      ptr1 = p1;
      val1 = v1;
      ptr2 = p2;
      val2 = v2;
    }
  }
};

template <typename Prefix, typename Suffix>
class TestFramingAllocator
    : public FramingAllocator<Prefix, Suffix, FrameErrorRecorder> {
 public:
  using Base = FramingAllocator<Prefix, Suffix, FrameErrorRecorder>;

  TestFramingAllocator(pw::Allocator& allocator) : Base(allocator) {}

  using Base::GetData;
  using Base::GetFrame;
  using Base::GetPrefix;
  using Base::GetSuffix;

  using Base::GetAllocatedLayout;
  using Base::GetInfo;
  using Base::GetRequestedLayout;
  using Base::GetUsableLayout;
  using Base::Recognizes;
  using typename Base::InfoType;
};

class NoUsableLayoutAllocator : public pw::allocator::ForwardingAllocator {
 public:
  explicit NoUsableLayoutAllocator(pw::Allocator& allocator)
      : ForwardingAllocator(allocator) {}

 private:
  pw::Result<Layout> DoGetInfo(InfoType info_type,
                               const void* ptr) const override {
    if (info_type == InfoType::kUsableLayoutOf) {
      return pw::Status::Unimplemented();
    }
    return ForwardingAllocator::DoGetInfo(info_type, ptr);
  }
};

class FramingAllocatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    NonTrivial::count = 0;
    FrameErrorRecorder::Reset();
  }

  pw::allocator::test::AllocatorForTest<2048> allocator_;
  NoUsableLayoutAllocator no_usable_layout_{allocator_};
};

TEST_F(FramingAllocatorTest, AllocateDeallocateTrivialTrivial) {
  TestFramingAllocator<Trivial, Trivial> allocator(allocator_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  // Check if pointer is aligned.
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % alignof(size_t), 0u);

  Trivial* prefix = allocator.GetPrefix(ptr);
  Trivial* suffix = allocator.GetSuffix(ptr);

  prefix->value = 0x12345678;
  suffix->value = 0x87654321;

  EXPECT_EQ(allocator.GetPrefix(ptr)->value, 0x12345678u);
  EXPECT_EQ(allocator.GetSuffix(ptr)->value, 0x87654321u);

  // Confirm GetData works.
  EXPECT_EQ(allocator.GetData(prefix), ptr);

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, AllocateDeallocateTrivialAlignedTrivial) {
  TestFramingAllocator<Trivial, AlignedTrivial> allocator(allocator_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  Trivial* prefix = allocator.GetPrefix(ptr);
  AlignedTrivial* suffix = allocator.GetSuffix(ptr);

  EXPECT_EQ(reinterpret_cast<uintptr_t>(prefix) % alignof(Trivial), 0u);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(suffix) % alignof(AlignedTrivial), 0u);

  prefix->value = 0xAAAAAAAA;
  suffix->value = 0xBBBBBBBB;

  EXPECT_EQ(allocator.GetPrefix(ptr)->value, 0xAAAAAAAAu);
  EXPECT_EQ(allocator.GetSuffix(ptr)->value, 0xBBBBBBBBu);

  // Confirm GetData works.
  EXPECT_EQ(allocator.GetData(prefix), ptr);

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, ResizeTrivialTrivial) {
  TestFramingAllocator<Trivial, Trivial> allocator(allocator_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  allocator.GetPrefix(ptr)->value = 0x11111111;
  allocator.GetSuffix(ptr)->value = 0x22222222;

  EXPECT_TRUE(allocator.Resize(ptr, sizeof(uint32_t) * 4));
  EXPECT_EQ(allocator.GetPrefix(ptr)->value, 0x11111111u);
  EXPECT_EQ(allocator.GetSuffix(ptr)->value, 0x22222222u);

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, ReallocateTrivialTrivial) {
  TestFramingAllocator<Trivial, Trivial> allocator(allocator_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);
  allocator.GetPrefix(ptr)->value = 0x33333333;
  allocator.GetSuffix(ptr)->value = 0x44444444;

  void* new_ptr = allocator.Reallocate(ptr, Layout::Of<uint64_t>());
  ASSERT_NE(new_ptr, nullptr);
  EXPECT_EQ(allocator.GetPrefix(new_ptr)->value, 0x33333333u);
  EXPECT_EQ(allocator.GetSuffix(new_ptr)->value, 0x44444444u);

  allocator.Deallocate(new_ptr);
}

TEST_F(FramingAllocatorTest, NewDelete) {
  TestFramingAllocator<Trivial, Trivial> allocator(allocator_);
  EXPECT_EQ(NonTrivial::count, 0);

  NonTrivial* ptr = allocator.New<NonTrivial>(0x123u);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(NonTrivial::count, 1);
  EXPECT_EQ(ptr->value, 0x123u);

  allocator.Delete(ptr);
  EXPECT_EQ(NonTrivial::count, 0);
}

TEST_F(FramingAllocatorTest, MakeUnique) {
  TestFramingAllocator<Trivial, Trivial> allocator(allocator_);
  EXPECT_EQ(NonTrivial::count, 0);
  {
    auto unique_ptr = allocator.MakeUnique<NonTrivial>(0x456u);
    ASSERT_NE(unique_ptr.get(), nullptr);
    EXPECT_EQ(NonTrivial::count, 1);
    EXPECT_EQ(unique_ptr->value, 0x456u);
  }
  EXPECT_EQ(NonTrivial::count, 0);
}

TEST_F(FramingAllocatorTest, MakeShared) {
#if PW_ALLOCATOR_HAS_ATOMICS
  TestFramingAllocator<Trivial, Trivial> allocator(allocator_);
  EXPECT_EQ(NonTrivial::count, 0);
  {
    auto shared_ptr = allocator.MakeShared<NonTrivial>(0x789u);
    ASSERT_NE(shared_ptr.get(), nullptr);
    EXPECT_EQ(NonTrivial::count, 1);
    EXPECT_EQ(shared_ptr->value, 0x789u);
  }
  EXPECT_EQ(NonTrivial::count, 0);
#endif
}

TEST_F(FramingAllocatorTest, DerivedTypesAsFrame) {
  TestFramingAllocator<Derived, Base> allocator(allocator_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  Derived* prefix = allocator.GetPrefix(ptr);
  Base* suffix = allocator.GetSuffix(ptr);

  prefix->x = 1;
  prefix->y = 2;
  suffix->x = 3;

  EXPECT_EQ(allocator.GetPrefix(ptr)->x, 1u);
  EXPECT_EQ(allocator.GetPrefix(ptr)->y, 2u);
  EXPECT_EQ(allocator.GetSuffix(ptr)->x, 3u);

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, PrefixOnly) {
  TestFramingAllocator<Trivial, void> allocator(allocator_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  Trivial* prefix = allocator.GetPrefix(ptr);
  prefix->value = 0xDEADBEEF;
  EXPECT_EQ(allocator.GetPrefix(ptr)->value, 0xDEADBEEFu);

  // Confirm GetData works.
  EXPECT_EQ(allocator.GetData(prefix), ptr);

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, PrefixOnlyWithoutUsableLayoutAllocateDeallocate) {
  TestFramingAllocator<Trivial, void> allocator(no_usable_layout_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(allocator.GetUsableLayout(ptr).status(),
            pw::Status::Unimplemented());

  Trivial* prefix = allocator.GetPrefix(ptr);
  ASSERT_NE(prefix, nullptr);
  prefix->value = 0xDEADBEEF;
  EXPECT_EQ(allocator.GetPrefix(ptr)->value, 0xDEADBEEFu);
  EXPECT_EQ(allocator.GetFrame(ptr), prefix);
  EXPECT_EQ(allocator.GetData(prefix), ptr);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());

  allocator.Deallocate(ptr);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());
}

TEST_F(FramingAllocatorTest, PrefixOnlyWithoutUsableLayoutResize) {
  TestFramingAllocator<Trivial, void> allocator(no_usable_layout_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);
  allocator.GetPrefix(ptr)->value = 0x11111111;

  EXPECT_TRUE(allocator.Resize(ptr, sizeof(uint32_t) * 4));
  EXPECT_EQ(allocator.GetPrefix(ptr)->value, 0x11111111u);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, PrefixOnlyWithoutUsableLayoutReallocate) {
  TestFramingAllocator<Trivial, void> allocator(no_usable_layout_);
  void* ptr = allocator.Reallocate(nullptr, Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);
  allocator.GetPrefix(ptr)->value = 0x33333333;

  // In-place reallocation via Resize succeeds without needing GetUsableLayout.
  void* new_ptr = allocator.Reallocate(ptr, Layout::Of<uint32_t[2]>());
  ASSERT_NE(new_ptr, nullptr);
  EXPECT_EQ(new_ptr, ptr);
  EXPECT_EQ(allocator.GetPrefix(new_ptr)->value, 0x33333333u);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());

  // When in-place Resize fails (e.g. blocked by a subsequent allocation),
  // Reallocate cannot copy without GetUsableLayout and returns nullptr.
  void* blocker = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(blocker, nullptr);
  EXPECT_EQ(allocator.Reallocate(new_ptr, Layout::Of<uint32_t[8]>()), nullptr);
  EXPECT_EQ(allocator.GetPrefix(new_ptr)->value, 0x33333333u);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());

  allocator.Deallocate(blocker);
  allocator.Deallocate(new_ptr);
}

TEST_F(FramingAllocatorTest, PrefixOnlyWithoutUsableLayoutNewDelete) {
  TestFramingAllocator<Trivial, void> allocator(no_usable_layout_);
  EXPECT_EQ(NonTrivial::count, 0);

  NonTrivial* ptr = allocator.New<NonTrivial>(0x123u);
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(NonTrivial::count, 1);
  EXPECT_EQ(ptr->value, 0x123u);
  allocator.Delete(ptr);
  EXPECT_EQ(NonTrivial::count, 0);

  NonTrivial* bounded = allocator.New<NonTrivial[3]>();
  ASSERT_NE(bounded, nullptr);
  EXPECT_EQ(NonTrivial::count, 3);
  allocator.Delete<NonTrivial[3]>(bounded);
  EXPECT_EQ(NonTrivial::count, 0);

  NonTrivial* unbounded = allocator.New<NonTrivial[]>(5);
  ASSERT_NE(unbounded, nullptr);
  EXPECT_EQ(NonTrivial::count, 5);
  allocator.Delete<NonTrivial[]>(unbounded, 5);
  EXPECT_EQ(NonTrivial::count, 0);

  NonTrivial* array = allocator.New<NonTrivial[4]>();
  ASSERT_NE(array, nullptr);
  EXPECT_EQ(NonTrivial::count, 4);
  allocator.DeleteArray(array, 4);
  EXPECT_EQ(NonTrivial::count, 0);
}

TEST_F(FramingAllocatorTest, PrefixOnlyWithoutUsableLayoutMakeUnique) {
  TestFramingAllocator<Trivial, void> allocator(no_usable_layout_);
  EXPECT_EQ(NonTrivial::count, 0);
  {
    auto unique_ptr = allocator.MakeUnique<NonTrivial>(0x456u);
    ASSERT_NE(unique_ptr.get(), nullptr);
    EXPECT_EQ(NonTrivial::count, 1);
    EXPECT_EQ(unique_ptr->value, 0x456u);
  }
  EXPECT_EQ(NonTrivial::count, 0);

  {
    auto unique_arr = allocator.MakeUnique<NonTrivial[]>(3);
    ASSERT_NE(unique_arr.get(), nullptr);
    EXPECT_EQ(NonTrivial::count, 3);
  }
  EXPECT_EQ(NonTrivial::count, 0);
}

TEST_F(FramingAllocatorTest, PrefixOnlyWithoutUsableLayoutMakeShared) {
#if PW_ALLOCATOR_HAS_ATOMICS
  TestFramingAllocator<Trivial, void> allocator(no_usable_layout_);
  EXPECT_EQ(NonTrivial::count, 0);
  {
    auto shared_ptr = allocator.MakeShared<NonTrivial>(0x789u);
    ASSERT_NE(shared_ptr.get(), nullptr);
    EXPECT_EQ(NonTrivial::count, 1);
    EXPECT_EQ(shared_ptr->value, 0x789u);
  }
  EXPECT_EQ(NonTrivial::count, 0);

  {
    auto shared_arr = allocator.MakeShared<NonTrivial[]>(3);
    ASSERT_NE(shared_arr.get(), nullptr);
    EXPECT_EQ(NonTrivial::count, 3);
  }
  EXPECT_EQ(NonTrivial::count, 0);
#endif
}

TEST_F(FramingAllocatorTest, PrefixOnlyWithoutUsableLayoutQueries) {
  TestFramingAllocator<Trivial, void> allocator(no_usable_layout_);
  EXPECT_EQ(allocator.GetAllocated(), 0u);

  auto capacity = allocator.GetCapacity();
  PW_TEST_ASSERT_OK(capacity.status());
  EXPECT_EQ(capacity.size(), allocator_.GetCapacity().size());

  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);
  EXPECT_GT(allocator.GetAllocated(), 0u);
  EXPECT_TRUE(allocator.MeasureFragmentation().has_value());
  EXPECT_TRUE(allocator.Recognizes(ptr));
  PW_TEST_EXPECT_OK(allocator.GetAllocatedLayout(ptr).status());
  EXPECT_EQ(allocator.GetRequestedLayout(ptr).status(),
            pw::Status::Unimplemented());
  EXPECT_EQ(allocator.GetUsableLayout(ptr).status(),
            pw::Status::Unimplemented());

  allocator.Deallocate(ptr);
  EXPECT_EQ(allocator.GetAllocated(), 0u);
}

TEST_F(FramingAllocatorTest, SuffixOnly) {
  TestFramingAllocator<void, Trivial> allocator(allocator_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  Trivial* suffix = allocator.GetSuffix(ptr);
  suffix->value = 0xFEEDFACE;
  EXPECT_EQ(allocator.GetSuffix(ptr)->value, 0xFEEDFACEu);

  // To check GetData without a prefix, find the block that holds the ptr.
  auto* data = reinterpret_cast<std::byte*>(ptr);
  for (auto* block : allocator_.blocks()) {
    std::byte* frame = block->UsableSpace();
    if (frame < data && data < frame + block->InnerSize()) {
      EXPECT_EQ(allocator.GetData(frame), data);
      break;
    }
  }

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, AllocatedPointerIsValid) {
  TestFramingAllocator<size_t, size_t> allocator(allocator_);
  auto bytes = allocator.MakeUnique<std::byte[]>(16);
  ASSERT_NE(bytes, nullptr);

  std::byte* data = bytes.get();
  void* frame = allocator.GetFrame(data);
  EXPECT_NE(frame, nullptr);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());

  EXPECT_EQ(allocator.GetPrefix(data), frame);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());

  EXPECT_EQ(allocator.GetData(frame), data);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());

  EXPECT_NE(allocator.GetSuffix(data), nullptr);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());
}

TEST_F(FramingAllocatorTest, MisalignedPointerIsInvalid) {
  TestFramingAllocator<size_t, size_t> allocator(allocator_);
  auto bytes = allocator.MakeUnique<std::byte[]>(16);
  ASSERT_NE(bytes, nullptr);

  std::byte* data = bytes.get();
  EXPECT_EQ(allocator.GetFrame(data + 1), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kDataNotAligned);
  EXPECT_EQ(FrameErrorRecorder::ptr1, data + 1);
  EXPECT_EQ(FrameErrorRecorder::val1, alignof(size_t));
  FrameErrorRecorder::Reset();

  auto frame = reinterpret_cast<uintptr_t>(allocator.GetFrame(data));
  auto* misaligned_frame = reinterpret_cast<void*>(frame + 1);
  EXPECT_EQ(allocator.GetData(misaligned_frame), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kUnrecognizedFrame);
  EXPECT_EQ(FrameErrorRecorder::ptr1, misaligned_frame);
}

TEST_F(FramingAllocatorTest, OutOfRangePointerIsInvalid) {
  TestFramingAllocator<size_t, size_t> allocator(allocator_);

  pw::ByteSpan buffer = allocator_.buffer();
  void* non_const_null = nullptr;
  auto* maxptr = reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max());

  EXPECT_EQ(allocator.GetFrame(buffer.data() - 1), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kDataNotAligned);
  EXPECT_EQ(FrameErrorRecorder::ptr1, buffer.data() - 1);
  FrameErrorRecorder::Reset();

  EXPECT_EQ(allocator.GetFrame(buffer.data() + buffer.size()), nullptr);
  EXPECT_TRUE(FrameErrorRecorder::error.has_value());
  FrameErrorRecorder::Reset();

  EXPECT_EQ(allocator.GetFrame(non_const_null), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kDataTooSmall);
  EXPECT_EQ(FrameErrorRecorder::ptr1, nullptr);
  FrameErrorRecorder::Reset();

  EXPECT_EQ(allocator.GetFrame(maxptr), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kDataNotAligned);
  EXPECT_EQ(FrameErrorRecorder::ptr1, maxptr);
  FrameErrorRecorder::Reset();

  EXPECT_EQ(allocator.GetData(buffer.data() - 1), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kUnrecognizedFrame);
  EXPECT_EQ(FrameErrorRecorder::ptr1, buffer.data() - 1);
  FrameErrorRecorder::Reset();

  EXPECT_EQ(allocator.GetData(buffer.data() + buffer.size()), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kUnrecognizedFrame);
  EXPECT_EQ(FrameErrorRecorder::ptr1, buffer.data() + buffer.size());
  FrameErrorRecorder::Reset();

  EXPECT_EQ(allocator.GetData(non_const_null), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kUnrecognizedFrame);
  EXPECT_EQ(FrameErrorRecorder::ptr1, nullptr);
  FrameErrorRecorder::Reset();

  EXPECT_EQ(allocator.GetData(maxptr), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kUnrecognizedFrame);
  EXPECT_EQ(FrameErrorRecorder::ptr1, maxptr);
}

TEST_F(FramingAllocatorTest, UnrecognizedPointerIsInvalid) {
  pw::allocator::test::AllocatorForTest<256> test_allocator1;
  pw::allocator::test::AllocatorForTest<256> test_allocator2;

  TestFramingAllocator<void, Trivial> allocator1(test_allocator1);
  TestFramingAllocator<void, Trivial> allocator2(test_allocator2);

  auto bytes = allocator1.MakeUnique<std::byte[]>(16);
  ASSERT_NE(bytes, nullptr);

  void* data = bytes.get();
  void* frame = allocator1.GetFrame(data);

  EXPECT_EQ(allocator1.GetFrame(data), frame);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());

  EXPECT_EQ(allocator2.GetFrame(data), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kUnrecognizedFrame);
  EXPECT_EQ(FrameErrorRecorder::ptr1, frame);
  FrameErrorRecorder::Reset();

  EXPECT_EQ(allocator1.GetData(frame), data);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());

  EXPECT_EQ(allocator2.GetData(frame), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kUnrecognizedFrame);
  EXPECT_EQ(FrameErrorRecorder::ptr1, frame);
}

TEST_F(FramingAllocatorTest, CorruptedFrameIsInvalid) {
  TestFramingAllocator<size_t, size_t> allocator(allocator_);
  auto bytes = allocator.MakeUnique<uint8_t[]>(16);
  ASSERT_NE(bytes, nullptr);

  uint8_t* data = bytes.get();
  void* frame = allocator.GetFrame(data);
  size_t* suffix = allocator.GetSuffix(data);

  for (size_t i = 1; i <= sizeof(size_t) * 2; ++i) {
    FrameErrorRecorder::Reset();
    EXPECT_EQ(allocator.GetPrefix(data), frame);
    EXPECT_EQ(allocator.GetData(frame), data);
    EXPECT_EQ(allocator.GetSuffix(data), suffix);
    EXPECT_FALSE(FrameErrorRecorder::error.has_value());

    *(data - i) ^= 0xFF;
    if (i <= sizeof(size_t)) {
      EXPECT_EQ(allocator.GetSuffix(data), nullptr);
      EXPECT_EQ(FrameErrorRecorder::error, FrameError::kDataTooSmallForSuffix);
      FrameErrorRecorder::Reset();
    } else {
      EXPECT_EQ(allocator.GetPrefix(data), nullptr);
      EXPECT_TRUE(FrameErrorRecorder::error.has_value());
      FrameErrorRecorder::Reset();

      EXPECT_EQ(allocator.GetData(frame), nullptr);
      EXPECT_EQ(FrameErrorRecorder::error, FrameError::kFrameOffsetTooSmall);
      FrameErrorRecorder::Reset();
    }

    *(data - i) ^= 0xFF;
    EXPECT_EQ(allocator.GetPrefix(data), frame);
    EXPECT_EQ(allocator.GetData(frame), data);
    EXPECT_EQ(allocator.GetSuffix(data), suffix);
    EXPECT_FALSE(FrameErrorRecorder::error.has_value());
  }
}

TEST_F(FramingAllocatorTest, GetCapacity) {
  TestFramingAllocator<Trivial, Trivial> allocator(allocator_);
  auto capacity = allocator.GetCapacity();
  PW_TEST_ASSERT_OK(capacity.status());
  EXPECT_EQ(capacity.size(), allocator_.GetCapacity().size());

  using InfoType = TestFramingAllocator<Trivial, Trivial>::InfoType;
  auto info_capacity = allocator.GetInfo(InfoType::kCapacity, nullptr);
  PW_TEST_ASSERT_OK(info_capacity);
  EXPECT_EQ(info_capacity->size(), capacity.size());
}

TEST_F(FramingAllocatorTest, GetRequestedLayoutUnimplemented) {
  TestFramingAllocator<Trivial, Trivial> allocator(allocator_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  using InfoType = TestFramingAllocator<Trivial, Trivial>::InfoType;
  EXPECT_TRUE(allocator.GetRequestedLayout(ptr).status().IsUnimplemented());
  EXPECT_TRUE(allocator.GetInfo(InfoType::kRequestedLayoutOf, ptr)
                  .status()
                  .IsUnimplemented());

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, GetInfoUnrecognizedOrNullPointerReturnsNotFound) {
  pw::allocator::test::AllocatorForTest<256> other_allocator;
  TestFramingAllocator<Trivial, Trivial> allocator(allocator_);

  void* other_ptr = other_allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(other_ptr, nullptr);

  using InfoType = TestFramingAllocator<Trivial, Trivial>::InfoType;

  // Null pointer queries should safely return NotFound, not crash.
  EXPECT_TRUE(allocator.GetUsableLayout(nullptr).status().IsNotFound());
  EXPECT_TRUE(allocator.GetAllocatedLayout(nullptr).status().IsNotFound());
  EXPECT_TRUE(
      allocator.GetInfo(InfoType::kRecognizes, nullptr).status().IsNotFound());

  // Foreign pointer queries should safely return NotFound, not crash.
  EXPECT_TRUE(allocator.GetUsableLayout(other_ptr).status().IsNotFound());
  EXPECT_TRUE(allocator.GetAllocatedLayout(other_ptr).status().IsNotFound());
  EXPECT_TRUE(allocator.GetInfo(InfoType::kRecognizes, other_ptr)
                  .status()
                  .IsNotFound());

  // Unaligned pointer queries should safely return NotFound, not crash.
  auto* unaligned =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(other_ptr) | 1);
  EXPECT_TRUE(allocator.GetUsableLayout(unaligned).status().IsNotFound());

  other_allocator.Deallocate(other_ptr);
}

TEST_F(FramingAllocatorTest, GetUsableLayoutPrefixOnly) {
  TestFramingAllocator<Trivial, void> allocator(allocator_);
  constexpr Layout layout = Layout::Of<uint32_t>();
  void* ptr = allocator.Allocate(layout);
  ASSERT_NE(ptr, nullptr);

  auto usable_layout = allocator.GetUsableLayout(ptr);
  PW_TEST_ASSERT_OK(usable_layout);
  EXPECT_GE(usable_layout->size(), layout.size());

  void* frame = allocator.GetFrame(ptr);
  auto frame_layout =
      TestFramingAllocator<Trivial, void>::GetUsableLayout(allocator_, frame);
  PW_TEST_ASSERT_OK(frame_layout);

  auto frame_offset =
      reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(frame);
  EXPECT_EQ(usable_layout->size(), frame_layout->size() - frame_offset);

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, GetUsableLayoutWithSuffixBoundsUsableSpace) {
  TestFramingAllocator<Trivial, Trivial> allocator(allocator_);
  constexpr Layout layout = Layout::Of<uint32_t>();
  void* ptr = allocator.Allocate(layout);
  ASSERT_NE(ptr, nullptr);

  Trivial* suffix = allocator.GetSuffix(ptr);
  suffix->value = 0xCAFEBABE;

  auto usable_layout = allocator.GetUsableLayout(ptr);
  PW_TEST_ASSERT_OK(usable_layout);

  // The usable layout must be bounded by the suffix offset so writing to the
  // usable space does not overwrite the suffix.
  auto suffix_offset =
      reinterpret_cast<uintptr_t>(suffix) - reinterpret_cast<uintptr_t>(ptr);
  EXPECT_EQ(usable_layout->size(), suffix_offset);

  // Writing to the entire usable region should leave the suffix intact.
  std::memset(ptr, 0xAA, usable_layout->size());
  EXPECT_EQ(allocator.GetSuffix(ptr)->value, 0xCAFEBABEu);

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, GetAllocatedLayoutPreservesFrameOverhead) {
  TestFramingAllocator<Trivial, Trivial> allocator(allocator_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  auto allocated_layout = allocator.GetAllocatedLayout(ptr);
  PW_TEST_ASSERT_OK(allocated_layout);

  void* frame = allocator.GetFrame(ptr);
  auto underlying_allocated =
      TestFramingAllocator<Trivial, Trivial>::GetAllocatedLayout(allocator_,
                                                                 frame);
  PW_TEST_ASSERT_OK(underlying_allocated);

  // Allocated layout should return the underlying block's allocated layout
  // without subtracting the frame prefix or suffix overhead.
  EXPECT_EQ(allocated_layout->size(), underlying_allocated->size());

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, GetUsableLayoutAlignmentMatchesPointer) {
  TestFramingAllocator<Trivial, AlignedTrivial> allocator(allocator_);
  void* ptr = allocator.Allocate(Layout::Of<uint32_t>());
  ASSERT_NE(ptr, nullptr);

  auto usable_layout = allocator.GetUsableLayout(ptr);
  PW_TEST_ASSERT_OK(usable_layout);

  // The returned alignment must divide the pointer's actual address.
  EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % usable_layout->alignment(), 0u);

  allocator.Deallocate(ptr);
}

TEST_F(FramingAllocatorTest, ValidateFrameDetectsSuffixPastFrameBounds) {
  TestFramingAllocator<size_t, size_t> allocator(allocator_);
  auto bytes = allocator.MakeUnique<uint8_t[]>(16);
  ASSERT_NE(bytes.get(), nullptr);

  uint8_t* data = bytes.get();
  void* frame = allocator.GetFrame(data);
  size_t* suffix = allocator.GetSuffix(data);
  ASSERT_NE(suffix, nullptr);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());

  // Suffix offset is stored immediately before data.
  auto* suffix_offset_ptr = reinterpret_cast<size_t*>(data) - 1;
  size_t original_offset = *suffix_offset_ptr;

  auto frame_usable =
      TestFramingAllocator<size_t, size_t>::GetUsableLayout(allocator_, frame);
  PW_TEST_ASSERT_OK(frame_usable);

  size_t data_offset =
      reinterpret_cast<uintptr_t>(data) - reinterpret_cast<uintptr_t>(frame);
  size_t data_size = frame_usable->size() - data_offset;

  // Set suffix_offset such that:
  // suffix_offset <= frame_usable->size() - sizeof(size_t)
  // (which would pass the old buggy check)
  // BUT:
  // suffix_offset > frame_usable->size() - data_offset - sizeof(size_t)
  // (which should fail because suffix extends past the frame).
  size_t invalid_offset = frame_usable->size() - sizeof(size_t);
  ASSERT_GT(invalid_offset, data_size - sizeof(size_t));

  *suffix_offset_ptr = invalid_offset;
  EXPECT_EQ(allocator.GetSuffix(data), nullptr);
  EXPECT_EQ(FrameErrorRecorder::error, FrameError::kDataTooSmallForSuffix);
  EXPECT_EQ(FrameErrorRecorder::ptr1, data);
  EXPECT_EQ(FrameErrorRecorder::val1, data_size);
  EXPECT_EQ(FrameErrorRecorder::ptr2, nullptr);
  EXPECT_EQ(FrameErrorRecorder::val2, invalid_offset);
  FrameErrorRecorder::Reset();

  // Restore
  *suffix_offset_ptr = original_offset;
  EXPECT_EQ(allocator.GetSuffix(data), suffix);
  EXPECT_FALSE(FrameErrorRecorder::error.has_value());
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
#endif

}  // namespace
