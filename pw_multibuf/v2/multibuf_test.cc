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

#include "pw_multibuf/multibuf.h"

#include <array>

#include "pw_allocator/chunk_pool.h"
#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_bytes/array.h"
#include "pw_bytes/span.h"
#include "pw_compilation_testing/negative_compilation.h"
#include "pw_result/result.h"
#include "pw_status/try.h"
#include "pw_unit_test/framework.h"

namespace {

// Test fixtures. //////////////////////////////////////////////////////////////

using ::pw::allocator::test::AllocatorForTest;
using ::pw::multibuf::v2::ConstMultiBuf;
using ::pw::multibuf::v2::FlatConstMultiBuf;
using ::pw::multibuf::v2::FlatMultiBuf;
using ::pw::multibuf::v2::MultiBuf;
using ::pw::multibuf::v2::TrackedConstMultiBuf;
using ::pw::multibuf::v2::TrackedFlatConstMultiBuf;
using ::pw::multibuf::v2::TrackedFlatMultiBuf;
using ::pw::multibuf::v2::TrackedMultiBuf;
using Event = ::pw::multibuf::v2::Observer::Event;

constexpr size_t kN = 32;

/// Test fixture that includes helper methods to set up structures used to test
/// MultiBufs
class MultiBufTest : public ::testing::Test {
 protected:
  MultiBufTest() {
    Fill(unowned_chunk_);
    owned_chunk_ = allocator_.MakeUnique<std::byte[]>(kN);
    PW_CHECK_NOTNULL(owned_chunk_.get());
    owned_bytes_ = pw::ByteSpan(owned_chunk_.get(), owned_chunk_.size());
    Fill(owned_bytes_);
  }

  /// Helper to fill a span with an ascending sequence of values.
  uint8_t Fill(pw::ByteSpan bytes, uint8_t value = 0) {
    auto counter = [&value]() { return static_cast<std::byte>(value++); };
    std::generate(bytes.begin(), bytes.end(), counter);
    return value;
  }

  /// Helper to make a MultiBuf with non-contiguous buffers.
  void MakeNonContiguous(ConstMultiBuf& out, size_t n, uint8_t value) {
    auto bytes1 = allocator_.MakeUnique<std::byte[]>(n / 2);
    auto bytes2 = allocator_.MakeUnique<std::byte[]>(n / 2);
    auto bytes3 = allocator_.MakeUnique<std::byte[]>(n / 2);
    PW_CHECK_NOTNULL(bytes1.get());
    PW_CHECK_NOTNULL(bytes2.get());
    PW_CHECK_NOTNULL(bytes3.get());
    PW_CHECK_PTR_NE(bytes1.get() + bytes1.size(), bytes3.get());
    std::memset(bytes1.get(), value, bytes1.size());
    std::memset(bytes3.get(), value, bytes3.size());
    out.PushBack(std::move(bytes1));
    out.PushBack(std::move(bytes3));
  }

  /// Helper method to instantiate a layered MultiBuf that resembles the entries
  /// used by `multibuf_impl::IteratorTest`.
  ///
  /// The created sequence represents 4 chunks with three layers, i.e.
  ///
  /// layer 3: <[0x3]={4, 8}>  [0x7]={0, 0}  <[0xB]={8, 8}  [0xF]={0,16}>
  /// layer 2: <[0x2]={2,12}> <[0x6]={0, 8}> <[0xA]={4,12}  [0xE]={0,16}>
  /// layer 1: <[0x1]={0,16}> <[0x5]={0,16}> <[0x9]={0,16}><[0xD]={0,16}>
  /// layer 0:  [0x0].data     [0x4].data     [0x8].data    [0xC].data
  ///
  /// where "<...>" represents a fragment. The bytes in "layer 0" are indexed
  /// from 0 to 63.
  void AddLayers(ConstMultiBuf& mb) {
    MultiBuf::Instance fragment(allocator_);
    auto chunk = allocator_.MakeUnique<std::byte[]>(16);
    uint8_t i = Fill(pw::ByteSpan(chunk.get(), chunk.size()));
    fragment->PushBack(std::move(chunk));
    PW_CHECK(fragment->AddLayer(2, 12));
    PW_CHECK(fragment->AddLayer(2, 8));
    mb.PushBack(std::move(*fragment));

    fragment = MultiBuf::Instance(allocator_);
    chunk = allocator_.MakeUnique<std::byte[]>(16);
    i = Fill(pw::ByteSpan(chunk.get(), chunk.size()), i);
    fragment->PushBack(std::move(chunk));
    PW_CHECK(fragment->AddLayer(0, 8));
    PW_CHECK(fragment->AddLayer(0, 0));
    mb.PushBack(std::move(*fragment));

    fragment = MultiBuf::Instance(allocator_);
    chunk = allocator_.MakeUnique<std::byte[]>(16);
    i = Fill(pw::ByteSpan(chunk.get(), chunk.size()), i);
    fragment->PushBack(std::move(chunk));
    chunk = allocator_.MakeUnique<std::byte[]>(16);
    Fill(pw::ByteSpan(chunk.get(), chunk.size()), i);
    fragment->PushBack(std::move(chunk));
    PW_CHECK(fragment->AddLayer(4));
    PW_CHECK(fragment->AddLayer(4));
    mb.PushBack(std::move(*fragment));
  }

  std::array<std::byte, kN / 2> unowned_chunk_;

  AllocatorForTest<1024> allocator_;
  pw::UniquePtr<std::byte[]> owned_chunk_;

  pw::ByteSpan owned_bytes_;
};

// A test fixture that receives events when a MultiBuf changes.
struct TestObserver : public pw::multibuf::v2::Observer {
  std::optional<Event> event;
  size_t value = 0;

 private:
  void DoNotify(Event event_, size_t value_) override {
    event = event_;
    value = value_;
  }
};

// Unit tests. /////////////////////////////////////////////////////////////////

static_assert(sizeof(pw::multibuf::v2::internal::Entry) == sizeof(std::byte*));

TEST_F(MultiBufTest, CheckProperties) {
  ConstMultiBuf::Instance cmbi(allocator_);
  ConstMultiBuf& cmb = cmbi;
  EXPECT_TRUE(cmb.is_const());
  EXPECT_TRUE(cmb.is_layerable());
  EXPECT_FALSE(cmb.is_observable());

  FlatConstMultiBuf::Instance fcmbi(allocator_);
  FlatConstMultiBuf& fcmb = fcmbi;
  EXPECT_TRUE(fcmb.is_const());
  EXPECT_FALSE(fcmb.is_layerable());
  EXPECT_FALSE(fcmb.is_observable());

  FlatMultiBuf::Instance fmbi(allocator_);
  FlatMultiBuf& fmb = fmbi;
  EXPECT_FALSE(fmb.is_const());
  EXPECT_FALSE(fmb.is_layerable());
  EXPECT_FALSE(fmb.is_observable());

  MultiBuf::Instance mbi(allocator_);
  MultiBuf& mb = mbi;
  EXPECT_FALSE(mb.is_const());
  EXPECT_TRUE(mb.is_layerable());
  EXPECT_FALSE(mb.is_observable());

  TrackedConstMultiBuf::Instance tcmbi(allocator_);
  TrackedConstMultiBuf& tcmb = tcmbi;
  EXPECT_TRUE(tcmb.is_const());
  EXPECT_TRUE(tcmb.is_layerable());
  EXPECT_TRUE(tcmb.is_observable());

  TrackedFlatConstMultiBuf::Instance tfcmbi(allocator_);
  TrackedFlatConstMultiBuf& tfcmb = tfcmbi;
  EXPECT_TRUE(tfcmb.is_const());
  EXPECT_FALSE(tfcmb.is_layerable());
  EXPECT_TRUE(tfcmb.is_observable());

  TrackedFlatMultiBuf::Instance tfmbi(allocator_);
  TrackedFlatMultiBuf& tfmb = tfmbi;
  EXPECT_FALSE(tfmb.is_const());
  EXPECT_FALSE(tfmb.is_layerable());
  EXPECT_TRUE(tfmb.is_observable());

  TrackedMultiBuf::Instance tmbi(allocator_);
  TrackedMultiBuf& tmb = tmbi;
  EXPECT_FALSE(tmb.is_const());
  EXPECT_TRUE(tmb.is_layerable());
  EXPECT_TRUE(tmb.is_observable());
}

TEST_F(MultiBufTest, CheckAllowedConversions) {
  ConstMultiBuf::Instance cmbi(allocator_);
  std::ignore = cmbi->as<ConstMultiBuf>();
  std::ignore = cmbi->as<FlatConstMultiBuf>();

  FlatConstMultiBuf::Instance fcmbi(allocator_);
  std::ignore = fcmbi->as<FlatConstMultiBuf>();

  FlatMultiBuf::Instance fmbi(allocator_);
  std::ignore = fmbi->as<FlatConstMultiBuf>();
  std::ignore = fmbi->as<FlatMultiBuf>();

  MultiBuf::Instance mbi(allocator_);
  std::ignore = mbi->as<ConstMultiBuf>();
  std::ignore = mbi->as<FlatConstMultiBuf>();
  std::ignore = mbi->as<FlatMultiBuf>();
  std::ignore = mbi->as<MultiBuf>();

  TrackedConstMultiBuf::Instance tcmbi(allocator_);
  std::ignore = tcmbi->as<ConstMultiBuf>();
  std::ignore = tcmbi->as<FlatConstMultiBuf>();
  std::ignore = tcmbi->as<TrackedConstMultiBuf>();
  std::ignore = tcmbi->as<TrackedFlatConstMultiBuf>();

  TrackedFlatConstMultiBuf::Instance tfcmbi(allocator_);
  std::ignore = tfcmbi->as<FlatConstMultiBuf>();
  std::ignore = tfcmbi->as<TrackedFlatConstMultiBuf>();

  TrackedFlatMultiBuf::Instance tfmbi(allocator_);
  std::ignore = tfmbi->as<FlatConstMultiBuf>();
  std::ignore = tfmbi->as<FlatMultiBuf>();
  std::ignore = tfmbi->as<TrackedFlatConstMultiBuf>();
  std::ignore = tfmbi->as<TrackedFlatMultiBuf>();

  TrackedMultiBuf::Instance tmbi(allocator_);
  std::ignore = tmbi->as<ConstMultiBuf>();
  std::ignore = tmbi->as<FlatConstMultiBuf>();
  std::ignore = tmbi->as<FlatMultiBuf>();
  std::ignore = tmbi->as<MultiBuf>();
  std::ignore = tmbi->as<TrackedConstMultiBuf>();
  std::ignore = tmbi->as<TrackedFlatConstMultiBuf>();
  std::ignore = tmbi->as<TrackedFlatMultiBuf>();
  std::ignore = tmbi->as<TrackedMultiBuf>();
}

#if PW_NC_TEST(CannotConvertConstMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}
#elif PW_NC_TEST(CannotConvertConstMultiBufToFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<FlatMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertConstMultiBufToMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}
#elif PW_NC_TEST(CannotConvertConstMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertConstMultiBufToTrackedFlatConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertConstMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertConstMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}
#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<ConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<FlatMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToTrackedFlatConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatConstMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(FlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}
#elif PW_NC_TEST(CannotConvertFlatMultiBufToConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<ConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatMultiBufToMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatMultiBufToTrackedFlatConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertFlatMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(MultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}
#elif PW_NC_TEST(CannotConvertMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(MultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertMultiBufToTrackedFlatConstMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(MultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(MultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] void ShouldAssert(MultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedConstMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(TrackedConstMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}
#elif PW_NC_TEST(CannotConvertTrackedConstMultiBufToFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedConstMultiBuf& mb) {
  std::ignore = mb.as<FlatMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedConstMultiBufToMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedConstMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedConstMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedConstMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<ConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<FlatMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToTrackedFlatMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedFlatMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatConstMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] void ShouldAssert(TrackedFlatConstMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(TrackedFlatMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatMultiBufToConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatMultiBuf& mb) {
  std::ignore = mb.as<ConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatMultiBufToMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatMultiBuf& mb) {
  std::ignore = mb.as<MultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatMultiBufToTrackedConstMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedConstMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedFlatMultiBufToTrackedMultiBuf)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] void ShouldAssert(TrackedFlatMultiBuf& mb) {
  std::ignore = mb.as<TrackedMultiBuf>();
}
#elif PW_NC_TEST(CannotConvertTrackedMultiBufToNonMultiBuf)
PW_NC_EXPECT("Only conversion to other MultiBuf types are supported.");
[[maybe_unused]] void ShouldAssert(TrackedMultiBuf& mb) {
  std::ignore = mb.as<pw::ByteSpan>();
}
#elif PW_NC_TEST(CannotConvertConstMultiBufToMultiBufInstance)
PW_NC_EXPECT("Read-only data cannot be converted to mutable data.");
[[maybe_unused]] MultiBuf::Instance ShouldAssert(ConstMultiBuf&& mb) {
  return MultiBuf::Instance(std::move(mb));
}
#elif PW_NC_TEST(CannotConvertMultiBufToFlatMultiBufInstance)
PW_NC_EXPECT("Flat MultiBufs do not have layer-related methods.");
[[maybe_unused]] MultiBuf::Instance ShouldAssert(FlatMultiBuf&& mb) {
  return MultiBuf::Instance(std::move(mb));
}
#elif PW_NC_TEST(CannotConvertTrackedMultiBufToMultiBufInstance)
PW_NC_EXPECT("Untracked MultiBufs do not have observer-related methods.");
[[maybe_unused]] TrackedMultiBuf::Instance ShouldAssert(MultiBuf&& mb) {
  return TrackedMultiBuf::Instance(std::move(mb));
}

#endif  // PW_NC_TEST

TEST_F(MultiBufTest, DefaultConstructedIsEmpty) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = *mbi;
  EXPECT_TRUE(mb.empty());
  EXPECT_EQ(mb.size(), 0u);
}

TEST_F(MultiBufTest, InstancesAreMovable) {
  // The allocators must outlive their allocations.
  AllocatorForTest<128> allocator1;
  AllocatorForTest<128> allocator2;

  auto& metrics1 = allocator1.metrics();
  auto& metrics2 = allocator2.metrics();

  // Nothing is initially allocated.
  ConstMultiBuf::Instance mbi1(allocator1);
  ConstMultiBuf& mb1 = mbi1;
  EXPECT_EQ(metrics1.allocated_bytes.value(), 0u);

  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  ASSERT_TRUE(mb1.TryReserveForPushBack());
  mb1.PushBack(std::move(chunk));
  size_t allocated_bytes = metrics1.allocated_bytes.value();
  EXPECT_NE(allocated_bytes, 0u);

  // Moving clears the destination MultiBuf, and does not allocate any new
  // memory.
  ConstMultiBuf::Instance mbi2(allocator2);
  ConstMultiBuf& mb2 = mbi2;

  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  EXPECT_EQ(metrics2.allocated_bytes.value(), 0u);
  ASSERT_TRUE(mb2.TryReserveForPushBack());
  mb2.PushBack(std::move(chunk));
  EXPECT_NE(metrics2.allocated_bytes.value(), 0u);
  mbi2 = std::move(mbi1);
  EXPECT_EQ(metrics2.allocated_bytes.value(), 0u);
  EXPECT_EQ(metrics1.allocated_bytes.value(), allocated_bytes);

  // Allocator gets passed along with move and is used when freeing.
  {
    ConstMultiBuf::Instance mbi3(std::move(mbi2));
    EXPECT_EQ(metrics1.allocated_bytes.value(), allocated_bytes);
  }
  EXPECT_EQ(metrics1.allocated_bytes.value(), 0u);
}

void ConsumesConstMultiBuf(ConstMultiBuf&& cmb) {
  ConstMultiBuf::Instance cmbi(std::move(cmb));
  static_cast<void>(cmbi);
}

TEST_F(MultiBufTest, MultiBufInstanceRvalueConversion) {
  ConstMultiBuf::Instance cmbi(allocator_);
  ConsumesConstMultiBuf(std::move(cmbi));
}

TEST_F(MultiBufTest, MultiBufInstanceRvalueDereference) {
  ConstMultiBuf::Instance cmbi(allocator_);
  ConsumesConstMultiBuf(*std::move(cmbi));
}

TEST_F(MultiBufTest, MultiBufInstanceRvalueDereferenceAndConvert) {
  MultiBuf::Instance mbi(allocator_);
  ConsumesConstMultiBuf(*std::move(mbi));
}

TEST_F(MultiBufTest, MultiBufInstanceRvalueConvert) {
  MultiBuf::Instance mbi(allocator_);
  MultiBuf& mb = mbi;
  ConsumesConstMultiBuf(std::move(mb));
}

TEST_F(MultiBufTest, MultiBufInstanceRvalueAs) {
  MultiBuf::Instance mbi(allocator_);
  MultiBuf& mb = mbi;
  ConsumesConstMultiBuf(std::move(mb).as<ConstMultiBuf>());
}

TEST_F(MultiBufTest, MultiBufInstanceMoveAssignment) {
  MultiBuf::Instance mbi1(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi1->PushBack(std::move(chunk));
  EXPECT_EQ(mbi1->size(), kN);
  MultiBuf& mb1 = mbi1;

  MultiBuf::Instance mb2(allocator_);
  mb2 = std::move(mb1);
  EXPECT_EQ(mb2->size(), kN);
}

#if PW_NC_TEST(CannotCopyConstructMultiBuf)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBuf mb1(mb);
  EXPECT_TRUE(mb1.empty());
}
#elif PW_NC_TEST(CannotCopyAssignMultiBuf)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBuf mb1(std::move(mb));
  EXPECT_TRUE(mb1.empty());
}
#elif PW_NC_TEST(CannotMoveConstructMultiBuf)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBuf mb1 = mb;
  EXPECT_TRUE(mb1.empty());
}
#elif PW_NC_TEST(CannotMoveAssignMultiBuf)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBuf mb1 = std::move(mb);
  EXPECT_TRUE(mb1.empty());
}

#elif PW_NC_TEST(CannotConstructFromMultiBufInstance)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBuf::Instance&& mbi) {
  MultiBuf mb(mbi);
  EXPECT_TRUE(mb.empty());
}
#elif PW_NC_TEST(CannotAssignFromMultiBufInstance)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] void ShouldAssert(MultiBuf::Instance&& mbi) {
  MultiBuf mb = mbi;
  EXPECT_TRUE(mb.empty());
}
#elif PW_NC_TEST(CannotCopyConstructInstance)
PW_NC_EXPECT(
    "Instances can only be created from existing MultiBufs using "
    "move-construction or move-assignment.");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBuf::Instance tmp(mb);
  EXPECT_TRUE(tmp->empty());
}
#elif PW_NC_TEST(CannotCopyAssignInstance)
PW_NC_EXPECT(
    "Instances can only be created from existing MultiBufs using "
    "move-construction or move-assignment.");
[[maybe_unused]] void ShouldAssert(MultiBuf&& mb) {
  MultiBuf::Instance tmp = mb;
  EXPECT_TRUE(tmp->empty());
}
#endif  // PW_NC_TEST

TEST_F(MultiBufTest, SizeForEmptyMultiBuf) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = *mbi;
  EXPECT_EQ(mb.size(), 0u);
}

TEST_F(MultiBufTest, SizeForMultiBufWithOneChunk) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = *mbi;
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(chunk));
  EXPECT_EQ(mb.size(), kN);
}

TEST_F(MultiBufTest, SizeForMultiBufWithMultipleChunks) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = *mbi;
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(chunk));
  chunk = allocator_.MakeUnique<std::byte[]>(kN / 2);
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(chunk));
  EXPECT_EQ(mb.size(), kN + kN / 2);
}

TEST_F(MultiBufTest, IsDerefencableWithAt) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(unowned_chunk_);
  for (size_t i = 0; i < unowned_chunk_.size(); ++i) {
    EXPECT_EQ(mb.at(i), static_cast<std::byte>(i));
  }
}

#if PW_NC_TEST(MutableDereference)
PW_NC_EXPECT_CLANG(
    "cannot assign to return value because function 'at' returns a const "
    "value");
PW_NC_EXPECT_GCC("assignment of read-only location");
TEST_F(MultiBufTest, MutableDereference) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  mb.at(0) = std::byte(0);
}

#elif PW_NC_TEST(MutableAccess)
PW_NC_EXPECT_CLANG(
    "cannot assign to return value because function 'operator\[\]' returns a "
    "const value");
PW_NC_EXPECT_GCC("assignment of read-only location");
TEST_F(MultiBufTest, MutableAccess) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  mb[0] = std::byte(0);
}

#elif PW_NC_TEST(MutableIterators)
PW_NC_EXPECT_CLANG(
    "cannot assign to return value because function 'operator\*' returns a "
    "const value");
PW_NC_EXPECT_GCC("assignment of read-only location");
TEST_F(MultiBufTest, MutableIterators) {
  ConstMultiBuf::Instance mb(allocator_);
  *mb->begin() = std::byte(0);
}

#elif PW_NC_TEST(CannotReturnMultiBuf)
PW_NC_EXPECT(
    "Only copies and moves from `BasicMultiBuf<...>::Instance`"
    "to `BasicMultiBuf<...>&` or another "
    "`BasicMultiBuf<...>::Instance` are valid.");
[[maybe_unused]] MultiBuf ShouldAssert(MultiBuf&& mb) { return mb; }

#elif PW_NC_TEST(CannotModifyConstChunkContents)
PW_NC_EXPECT_CLANG(
    "cannot assign to return value because function "
    "'operator\[\]' returns a const value");
PW_NC_EXPECT_GCC("assignment of read-only location");
[[maybe_unused]] void ShouldNotCompile(const MultiBuf& mb) {
  auto chunks = mb.ConstChunks();
  (*chunks.begin())[0] = std::byte{1};
}

#elif PW_NC_TEST(CannotGetMutableChunkContents)
PW_NC_EXPECT_CLANG(
    "cannot assign to return value because function "
    "'operator\[\]' returns a const value");
PW_NC_EXPECT_GCC("assignment of read-only location");
[[maybe_unused]] void ShouldNotCompile(ConstMultiBuf& mb) {
  auto chunks = mb.Chunks();
  (*chunks.begin())[0] = std::byte{1};
}
#endif

TEST_F(MultiBufTest, IsDerefencableWithArrayOperator) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(unowned_chunk_);
  for (size_t i = 0; i < unowned_chunk_.size(); ++i) {
    EXPECT_EQ(mb[i], static_cast<std::byte>(i));
  }
}

TEST_F(MultiBufTest, IterateConstChunksOverEmpty) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  for ([[maybe_unused]] auto buffer : mb.ConstChunks()) {
    FAIL();
  }
}

TEST_F(MultiBufTest, IterateChunksOverEmpty) {
  MultiBuf::Instance mbi(allocator_);
  MultiBuf& mb = mbi;
  for ([[maybe_unused]] auto buffer : mb.Chunks()) {
    FAIL();
  }
}

TEST_F(MultiBufTest, IterateChunksOverEmptyChunks) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  for (size_t i = 0; i < 3; ++i) {
    pw::ByteSpan s;
    ASSERT_TRUE(mb.TryReserveForPushBack());
    mb.PushBack(s);
  }

  for ([[maybe_unused]] auto buffer : mb.Chunks()) {
    FAIL();
  }
}

TEST_F(MultiBufTest, IterateConstChunksOverOne) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(unowned_chunk_);

  for (auto buffer : mb.ConstChunks()) {
    EXPECT_EQ(buffer.data(), unowned_chunk_.data());
    EXPECT_EQ(buffer.size(), unowned_chunk_.size());
  }
}

TEST_F(MultiBufTest, IterateChunksOverOne) {
  MultiBuf::Instance mbi(allocator_);
  MultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(unowned_chunk_);

  for (auto buffer : mb.Chunks()) {
    EXPECT_EQ(buffer.data(), unowned_chunk_.data());
    EXPECT_EQ(buffer.size(), unowned_chunk_.size());
  }
}

TEST_F(MultiBufTest, IterateConstBytesOverEmpty) {
  ConstMultiBuf::Instance mbi(allocator_);

  const ConstMultiBuf& mb1 = mbi;
  EXPECT_EQ(mb1.begin(), mb1.end());

  ConstMultiBuf& mb2 = mbi;
  EXPECT_EQ(mb2.cbegin(), mb2.cend());
}

TEST_F(MultiBufTest, IterateConstBytesOverContiguous) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(unowned_chunk_);
  uint8_t value = 0;
  for (const std::byte& b : mb) {
    EXPECT_EQ(b, static_cast<std::byte>(value));
    ++value;
  }
  EXPECT_EQ(value, unowned_chunk_.size());
}

TEST_F(MultiBufTest, IterateBytesOverContiguous) {
  MultiBuf::Instance mbi(allocator_);
  MultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(unowned_chunk_);

  // Iterate and assign.
  uint8_t value = 0;
  for (std::byte& b : mb) {
    b = static_cast<std::byte>(value);
    value += 2;
  }
  EXPECT_EQ(value, unowned_chunk_.size() * 2);

  // Check the underlying bytes.
  value = 0;
  for (std::byte& b : unowned_chunk_) {
    EXPECT_EQ(b, static_cast<std::byte>(value));
    value += 2;
  }
  EXPECT_EQ(value, unowned_chunk_.size() * 2);
}

TEST_F(MultiBufTest, IterateConstBytesOverNonContiguous) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  MakeNonContiguous(mb, kN, 0xFF);
  for (const std::byte& b : mb) {
    EXPECT_EQ(b, static_cast<std::byte>(0xFF));
  }
}

TEST_F(MultiBufTest, IterateBytesOverNonContiguous) {
  MultiBuf::Instance mbi(allocator_);
  MultiBuf& mb = mbi;
  MakeNonContiguous(mb.as<ConstMultiBuf>(), kN, 0xFF);

  // Iterate and assign.
  uint8_t value = 0;
  for (std::byte& b : mb) {
    b = static_cast<std::byte>(value);
    value += 3;
  }
  EXPECT_EQ(value, kN * 3);

  // Check the underlying bytes.
  value = 0;
  for (auto chunk : mb.ConstChunks()) {
    for (const std::byte& b : chunk) {
      EXPECT_EQ(b, static_cast<std::byte>(value));
      value += 3;
    }
  }
  EXPECT_EQ(value, kN * 3);
}

TEST_F(MultiBufTest, TryReserveChunksWithNumChunksEqualToZero) {
  ConstMultiBuf::Instance mb(allocator_);
  EXPECT_TRUE(mb->TryReserveChunks(0));
}

TEST_F(MultiBufTest, TryReserveChunksWithNumChunksLessThanTheCurrentChunks) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  allocator_.Exhaust();
  EXPECT_TRUE(mb->TryReserveChunks(1));
}

TEST_F(MultiBufTest, TryReserveChunksWithNumChunksEqualToTheCurrentChunks) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  allocator_.Exhaust();
  EXPECT_TRUE(mb->TryReserveChunks(2));
}

TEST_F(MultiBufTest, TryReserveChunksWithNumChunksMoreThanTheCurrentChunks) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  EXPECT_TRUE(mb->TryReserveChunks(3));
}

TEST_F(MultiBufTest, TryReserveChunksWithNumChunksMoreThanCanBeSatisfied) {
  ConstMultiBuf::Instance mb(allocator_);
  allocator_.Exhaust();
  EXPECT_FALSE(mb->TryReserveChunks(1));
}

TEST_F(MultiBufTest, TryReserveForInsertOfMultiBufFailsDueToAllocationFailure) {
  ConstMultiBuf::Instance mb1(allocator_);
  ConstMultiBuf::Instance mb2(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb2->PushBack(std::move(chunk));
  allocator_.Exhaust();
  EXPECT_FALSE(mb1->TryReserveForInsert(mb1->begin(), *mb2));
}

TEST_F(MultiBufTest, TryReserveForInsertOfUniquePtrFailsDueToExcessiveSize) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  allocator_.Exhaust();
  EXPECT_FALSE(mb->TryReserveForInsert(mb->begin()));
}

TEST_F(MultiBufTest, InsertMultiBufIntoEmptyMultiBuf) {
  ConstMultiBuf::Instance mb1(allocator_);

  ConstMultiBuf::Instance mb2(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::byte* data = chunk.get();
  mb2->PushBack(std::move(chunk));
  auto iter = mb1->Insert(mb1->begin(), std::move(*mb2));

  EXPECT_EQ(&(*iter), data);
  EXPECT_EQ(iter, mb1->begin());
  EXPECT_EQ(mb1->size(), kN);
  EXPECT_TRUE(mb2->empty());
}

TEST_F(MultiBufTest, InsertMultiBufIntoNonEmptyMultiBufAtBoundary) {
  ConstMultiBuf::Instance mb1(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb1->PushBack(std::move(chunk));

  ConstMultiBuf::Instance mb2(allocator_);
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::byte* data = chunk.get();
  mb2->PushBack(std::move(chunk));
  auto iter = mb1->Insert(mb1->end(), std::move(*mb2));

  EXPECT_EQ(&(*iter), data);
  EXPECT_EQ(iter + kN, mb1->end());
  EXPECT_EQ(mb1->size(), 2 * kN);
  EXPECT_TRUE(mb2->empty());
}

TEST_F(MultiBufTest, InsertMultiBufIntoNonEmptyMultiBufMidChunk) {
  ConstMultiBuf::Instance mb1(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb1->PushBack(std::move(chunk));

  ConstMultiBuf::Instance mb2(allocator_);
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::byte* data = chunk.get();
  mb2->PushBack(std::move(chunk));
  auto iter = mb1->Insert(mb1->begin() + kN / 2, std::move(*mb2));

  EXPECT_EQ(&(*iter), data);
  EXPECT_EQ(iter, mb1->begin() + kN / 2);
  EXPECT_EQ(mb1->size(), 2 * kN);
  EXPECT_TRUE(mb2->empty());
}

TEST_F(MultiBufTest, InsertUnownedIntoEmptyMultiBuf) {
  ConstMultiBuf::Instance mb(allocator_);

  auto iter = mb->Insert(mb->begin(), unowned_chunk_);

  EXPECT_EQ(&(*iter), unowned_chunk_.data());
  EXPECT_EQ(iter, mb->begin());
  EXPECT_EQ(mb->size(), unowned_chunk_.size());
}

TEST_F(MultiBufTest, InsertUnownedIntoNonEmptyMultiBufAtBoundary) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));

  auto iter = mb->Insert(mb->end(), unowned_chunk_);

  EXPECT_EQ(&(*iter), unowned_chunk_.data());
  EXPECT_EQ(iter + unowned_chunk_.size(), mb->end());
  EXPECT_EQ(mb->size(), kN + unowned_chunk_.size());
}

TEST_F(MultiBufTest, InsertUnownedIntoNonEmptyMultiBufMidChunk) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));

  auto iter = mb->Insert(mb->begin() + kN / 2, unowned_chunk_);

  EXPECT_EQ(&(*iter), unowned_chunk_.data());
  EXPECT_EQ(iter, mb->begin() + kN / 2);
  EXPECT_EQ(mb->size(), kN + unowned_chunk_.size());
}

TEST_F(MultiBufTest, InsertUniquePtrIntoEmptyMultiBuf) {
  ConstMultiBuf::Instance mb(allocator_);

  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::byte* data = chunk.get();
  auto iter = mb->Insert(mb->begin(), std::move(chunk));

  EXPECT_EQ(&(*iter), data);
  EXPECT_EQ(iter, mb->begin());
  EXPECT_EQ(mb->size(), kN);
}

TEST_F(MultiBufTest, InsertUniquePtrIntoNonEmptyMultiBufAtBoundary) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));

  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::byte* data = chunk.get();
  auto iter = mb->Insert(mb->end(), std::move(chunk));

  EXPECT_EQ(&(*iter), data);
  EXPECT_EQ(iter + kN, mb->end());
  EXPECT_EQ(mb->size(), 2 * kN);
}

TEST_F(MultiBufTest, InsertUniquePtrIntoNonEmptyMultiBufMidChunk) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));

  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::byte* data = chunk.get();
  auto iter = mb->Insert(mb->begin() + kN / 2, std::move(chunk));

  EXPECT_EQ(&(*iter), data);
  EXPECT_EQ(iter, mb->begin() + kN / 2);
  EXPECT_EQ(mb->size(), 2 * kN);
}

TEST_F(MultiBufTest, InsertSharedPtrIntoEmptyMultiBuf) {
  ConstMultiBuf::Instance mb(allocator_);

  auto chunk = allocator_.MakeShared<std::byte[]>(kN);
  std::byte* data = chunk.get();
  auto iter = mb->Insert(mb->begin(), chunk);

  EXPECT_EQ(&(*iter), data);
  EXPECT_EQ(iter, mb->begin());
  EXPECT_EQ(mb->size(), kN);
}

TEST_F(MultiBufTest, InsertSharedPtrIntoNonEmptyMultiBufAtBoundary) {
  ConstMultiBuf::Instance mb(allocator_);

  auto chunk = allocator_.MakeShared<std::byte[]>(2 * kN);
  std::byte* data = chunk.get();
  Fill(pw::ByteSpan(data, chunk.size()));
  mb->PushBack(chunk, kN, kN);
  auto iter = mb->Insert(mb->end(), chunk, 0, kN);

  EXPECT_EQ(&(*iter), data);
  EXPECT_EQ(iter + kN, mb->end());
  EXPECT_EQ(mb->size(), 2 * kN);

  iter = mb->cbegin();
  for (size_t i = kN; i < 2 * kN; ++i) {
    ASSERT_NE(iter, mb->cend());
    EXPECT_EQ(data[i], std::byte(i));
  }
  for (size_t i = 0; i < kN; ++i) {
    ASSERT_NE(iter, mb->cend());
    EXPECT_EQ(data[i], std::byte(i));
  }
}

TEST_F(MultiBufTest, InsertSharedPtrIntoNonEmptyMultiBufMidChunk) {
  ConstMultiBuf::Instance mb(allocator_);

  auto chunk = allocator_.MakeShared<std::byte[]>(2 * kN);
  std::byte* data = chunk.get();
  Fill(pw::ByteSpan(data, chunk.size()));
  mb->PushBack(chunk, 0, kN);

  auto iter = mb->Insert(mb->begin() + kN / 2, chunk, kN, kN);
  EXPECT_EQ(&(*iter), data + kN);
  EXPECT_EQ(iter, mb->begin() + kN / 2);
  EXPECT_EQ(mb->size(), 2 * kN);

  iter = mb->cbegin();
  for (size_t i = 0; i < kN / 2; ++i) {
    ASSERT_NE(iter, mb->cend());
    EXPECT_EQ(data[i], std::byte(i));
  }
  for (size_t i = kN; i < kN * 2; ++i) {
    ASSERT_NE(iter, mb->cend());
    EXPECT_EQ(data[i], std::byte(i));
  }
  for (size_t i = kN / 2; i < kN; ++i) {
    ASSERT_NE(iter, mb->cend());
    EXPECT_EQ(data[i], std::byte(i));
  }
}

TEST_F(MultiBufTest,
       TryReserveForPushBackOfMultiBufFailsDueToAllocationFailure) {
  ConstMultiBuf::Instance mb1(allocator_);
  ConstMultiBuf::Instance mb2(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb2->PushBack(std::move(chunk));
  allocator_.Exhaust();
  EXPECT_FALSE(mb1->TryReserveForPushBack(*mb2));
}

TEST_F(MultiBufTest, TryReserveForPushBackOfUnownedFailsDueToExcessiveSize) {
  ConstMultiBuf::Instance mb(allocator_);
  allocator_.Exhaust();
  EXPECT_FALSE(mb->TryReserveForPushBack());
}

TEST_F(MultiBufTest, TryReserveForPushBackOfUniquePtrFailsDueToExcessiveSize) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  allocator_.Exhaust();
  EXPECT_FALSE(mb->TryReserveForPushBack());
}

TEST_F(MultiBufTest, TryReserveForPushBackFailsWhenMemoryExhausted) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;

  allocator_.Exhaust();
  EXPECT_FALSE(mb.TryReserveForPushBack());
}

TEST_F(MultiBufTest, PushBackSucceedsWithMultiBuf) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(owned_chunk_));

  ConstMultiBuf::Instance fragment(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN * 2);
  ASSERT_TRUE(fragment->TryReserveForPushBack());
  fragment->PushBack(std::move(chunk));

  ASSERT_TRUE(mb.TryReserveForPushBack(*fragment));
  mb.PushBack(std::move(*fragment));
  EXPECT_EQ(mb.size(), kN * 3);
  EXPECT_TRUE(fragment->empty());
}

TEST_F(MultiBufTest, PushBackSucceedsWithByteSpan) {
  {
    ConstMultiBuf::Instance mbi(allocator_);
    ConstMultiBuf& mb = mbi;
    ASSERT_TRUE(mb.TryReserveForPushBack());
    mb.PushBack(unowned_chunk_);
    EXPECT_FALSE(mb.empty());
    EXPECT_EQ(mb.size(), unowned_chunk_.size());
  }

  // Chunk still valid.
  EXPECT_EQ(unowned_chunk_[0], static_cast<std::byte>(0));
}

TEST_F(MultiBufTest, PushBackMultiBufIntoEmptyMultiBuf) {
  ConstMultiBuf::Instance mb1(allocator_);
  ConstMultiBuf::Instance mb2(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb2->PushBack(std::move(chunk));
  mb1->PushBack(std::move(*mb2));
  EXPECT_EQ(mb1->size(), kN);
  EXPECT_TRUE(mb2->empty());
}

TEST_F(MultiBufTest, PushBackMultiBufIntoNonEmptyMultiBuf) {
  ConstMultiBuf::Instance mb1(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb1->PushBack(std::move(chunk));
  ConstMultiBuf::Instance mb2(allocator_);
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb2->PushBack(std::move(chunk));
  mb1->PushBack(std::move(*mb2));
  EXPECT_EQ(mb1->size(), 2 * kN);
  EXPECT_TRUE(mb2->empty());
}

TEST_F(MultiBufTest, PushBackUnownedIntoEmptyMultiBuf) {
  ConstMultiBuf::Instance mb(allocator_);
  mb->PushBack(unowned_chunk_);
  EXPECT_EQ(mb->size(), unowned_chunk_.size());
}

TEST_F(MultiBufTest, PushBackUnownedIntoNonEmptyMultiBuf) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  mb->PushBack(unowned_chunk_);
  EXPECT_EQ(mb->size(), kN + unowned_chunk_.size());
}

TEST_F(MultiBufTest, PushBackUniquePtrIntoEmptyMultiBuf) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  EXPECT_EQ(mb->size(), kN);
}

TEST_F(MultiBufTest, PushBackUniquePtrIntoNonEmptyMultiBuf) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk1 = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk1));
  auto chunk2 = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk2));
  EXPECT_EQ(mb->size(), 2 * kN);
}

TEST_F(MultiBufTest, PushBackSucceedsWithUniquePtr) {
  auto& metrics = allocator_.metrics();
  {
    ConstMultiBuf::Instance mbi(allocator_);
    ConstMultiBuf& mb = mbi;
    ASSERT_TRUE(mb.TryReserveForPushBack());
    mb.PushBack(std::move(owned_chunk_));
    EXPECT_FALSE(mb.empty());
    EXPECT_EQ(mb.size(), kN);
    EXPECT_NE(metrics.allocated_bytes.value(), 0u);
  }

  // Chunk and deque automatically freed.
  EXPECT_EQ(metrics.allocated_bytes.value(), 0u);
}
TEST_F(MultiBufTest, PushBackSharedPtrIntoEmptyMultiBuf) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeShared<std::byte[]>(kN);
  mb->PushBack(chunk);
  EXPECT_EQ(mb->size(), kN);
}

TEST_F(MultiBufTest, PushBackSharedPtrIntoNonEmptyMultiBuf) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk1 = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk1));
  auto chunk2 = allocator_.MakeShared<std::byte[]>(kN);
  mb->PushBack(chunk2);
  EXPECT_EQ(mb->size(), 2 * kN);
}

TEST_F(MultiBufTest, IsRemovableReturnsFalseWhenOutOfRange) {
  ConstMultiBuf::Instance mb(allocator_);
  mb->PushBack(unowned_chunk_);
  EXPECT_FALSE(mb->IsRemovable(mb->begin() + 1, unowned_chunk_.size()));
}

TEST_F(MultiBufTest, RemoveFailsWhenUnableToAllocateForSplit) {
  ConstMultiBuf::Instance mbi(allocator_);
  mbi->PushBack(unowned_chunk_);
  allocator_.Exhaust();
  auto result = mbi->Remove(mbi->begin() + 1, unowned_chunk_.size() - 2);
  EXPECT_EQ(result.status(), pw::Status::ResourceExhausted());
  EXPECT_EQ(mbi->size(), unowned_chunk_.size());
}

TEST_F(MultiBufTest, RemoveOnlyUnownedChunk) {
  ConstMultiBuf::Instance mb(allocator_);
  mb->PushBack(unowned_chunk_);

  ASSERT_TRUE(mb->IsRemovable(mb->begin(), unowned_chunk_.size()));
  auto result = mb->Remove(mb->begin(), unowned_chunk_.size());
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_TRUE(mb->empty());
  EXPECT_EQ(result.value()->size(), unowned_chunk_.size());
}

TEST_F(MultiBufTest, RemoveChunkPrefix) {
  ConstMultiBuf::Instance mb(allocator_);
  mb->PushBack(unowned_chunk_);

  ASSERT_TRUE(mb->IsRemovable(mb->begin(), unowned_chunk_.size()));
  auto result = mb->Remove(mb->begin(), unowned_chunk_.size() / 2);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb->size(), unowned_chunk_.size() / 2);
  EXPECT_EQ(result.value()->size(), unowned_chunk_.size() / 2);
}

TEST_F(MultiBufTest, RemoveCompleteUnownedChunkFromMultiBufWithOtherChunks) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  mb->PushBack(unowned_chunk_);

  ASSERT_TRUE(mb->IsRemovable(mb->begin() + kN, unowned_chunk_.size()));
  auto result = mb->Remove(mb->begin() + kN, unowned_chunk_.size());
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb->size(), kN);
  EXPECT_EQ(result.value()->size(), unowned_chunk_.size());
}

TEST_F(MultiBufTest, RemovePartialUnownedChunkFromMultiBufWithOtherChunks) {
  ConstMultiBuf::Instance mb(allocator_);
  auto owned = allocator_.MakeUnique<std::byte[]>(kN / 2);
  mb->PushBack(std::move(owned));
  std::array<std::byte, kN * 2> unowned;
  mb->PushBack(unowned);

  ASSERT_TRUE(mb->IsRemovable(mb->begin() + kN, kN / 2));
  auto result = mb->Remove(mb->begin() + kN, kN / 2);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb->size(), kN * 2);
  EXPECT_EQ(result.value()->size(), kN / 2);
}

TEST_F(MultiBufTest, RemoveOnlyOwnedChunk) {
  ConstMultiBuf::Instance mbi1(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi1->PushBack(std::move(chunk));
  EXPECT_FALSE(mbi1->empty());
  EXPECT_TRUE(mbi1->IsReleasable(mbi1->begin()));
  EXPECT_EQ(mbi1->size(), kN);

  ASSERT_TRUE(mbi1->IsRemovable(mbi1->begin(), kN));
  auto result = mbi1->Remove(mbi1->begin(), kN);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_TRUE(mbi1->empty());
  EXPECT_EQ(mbi1->size(), 0u);

  ConstMultiBuf::Instance mbi2(std::move(*result));
  EXPECT_FALSE(mbi2->empty());
  EXPECT_TRUE(mbi2->IsReleasable(mbi2->begin()));
  EXPECT_EQ(mbi2->size(), kN);
}

TEST_F(MultiBufTest, RemoveCompleteOwnedChunkFromMultiBufWithOtherChunks) {
  ConstMultiBuf::Instance mbi1(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi1->PushBack(std::move(chunk));
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi1->PushBack(std::move(chunk));

  ASSERT_TRUE(mbi1->IsRemovable(mbi1->begin() + kN, kN));
  auto result = mbi1->Remove(mbi1->begin() + kN, kN);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_FALSE(mbi1->empty());
  EXPECT_TRUE(mbi1->IsReleasable(mbi1->begin()));
  EXPECT_EQ(mbi1->size(), kN);

  ConstMultiBuf::Instance mbi2(std::move(*result));
  EXPECT_FALSE(mbi2->empty());
  EXPECT_TRUE(mbi2->IsReleasable(mbi2->begin()));
  EXPECT_EQ(mbi2->size(), kN);
}

TEST_F(MultiBufTest, RemovePartialOwnedChunkFromMultiBufWithOtherChunks) {
  ConstMultiBuf::Instance mbi1(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi1->PushBack(std::move(chunk));
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi1->PushBack(std::move(chunk));
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi1->PushBack(std::move(chunk));

  ASSERT_TRUE(mbi1->IsRemovable(mbi1->begin() + kN, kN));
  auto result = mbi1->Remove(mbi1->begin() + kN / 2, kN * 2);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_FALSE(mbi1->empty());
  EXPECT_EQ(mbi1->size(), kN);

  // Owned chunks that are split should be converted to shared.
  EXPECT_FALSE(mbi1->IsReleasable(mbi1->begin()));
  EXPECT_FALSE(mbi1->IsReleasable(mbi1->begin() + kN / 2));

  // Owned chunks that are moved without being split should remained owned.
  ConstMultiBuf::Instance mbi2(std::move(*result));
  EXPECT_EQ(mbi2->size(), kN * 2);
  EXPECT_FALSE(mbi2->IsReleasable(mbi2->begin()));
  EXPECT_TRUE(mbi2->IsReleasable(mbi2->begin() + kN / 2));
  EXPECT_FALSE(mbi2->IsReleasable(mbi2->begin() + 3 * kN / 2));
}

TEST_F(MultiBufTest, RemoveOnlySharedChunk) {
  ConstMultiBuf::Instance mbi1(allocator_);
  auto chunk = allocator_.MakeShared<std::byte[]>(kN);
  mbi1->PushBack(chunk);
  EXPECT_FALSE(mbi1->empty());
  EXPECT_TRUE(mbi1->IsShareable(mbi1->begin()));
  EXPECT_EQ(mbi1->size(), kN);

  ASSERT_TRUE(mbi1->IsRemovable(mbi1->begin(), kN));
  auto result = mbi1->Remove(mbi1->begin(), kN);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_TRUE(mbi1->empty());
  EXPECT_EQ(mbi1->size(), 0u);

  ConstMultiBuf::Instance mbi2(std::move(*result));
  EXPECT_FALSE(mbi2->empty());
  EXPECT_TRUE(mbi2->IsShareable(mbi2->begin()));
  EXPECT_EQ(mbi2->size(), kN);
}

TEST_F(MultiBufTest, RemoveCompleteSharedChunkFromMultiBufWithOtherChunks) {
  ConstMultiBuf::Instance mbi1(allocator_);
  auto owned = allocator_.MakeUnique<std::byte[]>(kN);
  mbi1->PushBack(std::move(owned));
  auto shared = allocator_.MakeShared<std::byte[]>(kN);
  mbi1->PushBack(std::move(shared));
  EXPECT_TRUE(mbi1->IsShareable(mbi1->begin() + kN));

  ASSERT_TRUE(mbi1->IsRemovable(mbi1->begin() + kN, kN));
  auto result = mbi1->Remove(mbi1->begin() + kN, kN);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_FALSE(mbi1->empty());
  EXPECT_EQ(mbi1->size(), kN);

  ConstMultiBuf::Instance mbi2(std::move(*result));
  EXPECT_FALSE(mbi2->empty());
  EXPECT_TRUE(mbi2->IsShareable(mbi2->begin()));
  EXPECT_EQ(mbi2->size(), kN);
}

TEST_F(MultiBufTest, RemovePartialSharedChunkFromMultiBufWithOtherChunks) {
  ConstMultiBuf::Instance mbi1(allocator_);
  auto owned = allocator_.MakeUnique<std::byte[]>(kN / 2);
  mbi1->PushBack(std::move(owned));
  auto shared = allocator_.MakeShared<std::byte[]>(kN * 2);
  mbi1->PushBack(shared);
  EXPECT_TRUE(mbi1->IsShareable(mbi1->begin() + kN / 2));

  ASSERT_TRUE(mbi1->IsRemovable(mbi1->begin() + kN, kN / 2));
  auto result = mbi1->Remove(mbi1->begin() + kN, kN / 2);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_TRUE(mbi1->IsShareable(mbi1->begin() + kN / 2));
  EXPECT_EQ(mbi1->size(), kN * 2);

  ConstMultiBuf::Instance mbi2(std::move(*result));
  EXPECT_FALSE(mbi2->empty());
  EXPECT_TRUE(mbi2->IsShareable(mbi2->begin()));
  EXPECT_EQ(mbi2->size(), kN / 2);
}

TEST_F(MultiBufTest, RemoveMultipleChunksFromMultiBufWithMixedOwnership) {
  ConstMultiBuf::Instance mbi1(allocator_);

  // [0.0 * kN, 0.5 * kN)
  auto owned = allocator_.MakeUnique<std::byte[]>(kN / 2);
  mbi1->PushBack(std::move(owned));

  // [0.5 * kN, 1.5 * kN)
  std::array<std::byte, kN> unowned;
  mbi1->PushBack(unowned);

  // [1.5 * kN, 3.5 * kN)
  owned = allocator_.MakeUnique<std::byte[]>(kN * 2);
  mbi1->PushBack(std::move(owned));

  // [3.5 * kN, 5.0 * kN)
  auto shared = allocator_.MakeShared<std::byte[]>(3 * kN / 2);
  mbi1->PushBack(shared);

  // [5.0 * kN, 6.0 * kN)
  owned = allocator_.MakeUnique<std::byte[]>(kN);
  mbi1->PushBack(std::move(owned));

  EXPECT_EQ(mbi1->size(), kN * 6);
  EXPECT_TRUE(mbi1->IsShareable(mbi1->begin() + 7 * kN / 2));

  ASSERT_TRUE(mbi1->IsRemovable(mbi1->begin() + kN, kN * 3));
  auto result = mbi1->Remove(mbi1->begin() + kN, kN * 3);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_TRUE(mbi1->IsShareable(mbi1->begin() + kN));
  EXPECT_EQ(mbi1->size(), kN * 3);

  ConstMultiBuf::Instance mbi2(std::move(*result));
  EXPECT_FALSE(mbi2->empty());
  EXPECT_TRUE(mbi2->IsShareable(mbi2->begin() + 5 * kN / 2));
  EXPECT_EQ(mbi2->size(), kN * 3);
}

TEST_F(MultiBufTest, PopFrontFragmentFailsOnAllocationFailure) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;

  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(chunk));

  chunk = allocator_.MakeUnique<std::byte[]>(kN * 2);
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(chunk));

  allocator_.Exhaust();
  pw::Result<ConstMultiBuf::Instance> result = mb.PopFrontFragment();
  EXPECT_EQ(result.status(), pw::Status::ResourceExhausted());
  EXPECT_EQ(mb.size(), kN * 3);
}

TEST_F(MultiBufTest, PopFrontFragmentSucceedsWhenNotEmpty) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;

  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(chunk));

  chunk = allocator_.MakeUnique<std::byte[]>(kN * 2);
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(chunk));

  pw::Result<ConstMultiBuf::Instance> result = mb.PopFrontFragment();
  ASSERT_EQ(result.status(), pw::OkStatus());
  ConstMultiBuf::Instance fragment = std::move(*result);
  EXPECT_EQ(fragment->size(), kN);
  EXPECT_EQ(mb.size(), kN * 2);
}

TEST_F(MultiBufTest, DiscardFailsOnAllocationFailure) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(2 * kN);
  mbi->PushBack(std::move(chunk));

  allocator_.Exhaust();
  auto result = mbi->Discard(mbi->begin() + kN / 2, kN);
  EXPECT_EQ(result.status(), pw::Status::ResourceExhausted());
  EXPECT_EQ(mbi->size(), 2 * kN);
}

TEST_F(MultiBufTest, DiscardOnlyUnownedChunk) {
  ConstMultiBuf::Instance mb(allocator_);
  mb->PushBack(unowned_chunk_);
  auto result = mb->Discard(mb->begin(), unowned_chunk_.size());
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_TRUE(mb->empty());
}

TEST_F(MultiBufTest, DiscardCompleteUnownedChunkFromMultiBufWithOtherChunks) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  mb->PushBack(unowned_chunk_);
  auto result = mb->Discard(mb->begin() + kN, unowned_chunk_.size());
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb->size(), kN);
}

TEST_F(MultiBufTest, DiscardPartialUnownedChunkFromMultiBufWithOtherChunks) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  mb->PushBack(unowned_chunk_);
  auto result = mb->Discard(mb->begin() + kN, unowned_chunk_.size() / 2);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb->size(), kN + unowned_chunk_.size() / 2);
}

TEST_F(MultiBufTest, DiscardOnlyOwnedChunk) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  auto result = mb->Discard(mb->begin(), kN);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_TRUE(mb->empty());
}

TEST_F(MultiBufTest, DiscardCompleteOwnedChunkFromMultiBufWithOtherChunks) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk1 = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk1));
  auto chunk2 = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk2));
  auto result = mb->Discard(mb->begin() + kN, kN);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb->size(), kN);
}

TEST_F(MultiBufTest, DiscardPartialOwnedChunkFromMultiBufWithOtherChunks) {
  ConstMultiBuf::Instance mb(allocator_);
  // Each step modifies the contents as listed, in units of kN.
  // Step 1: [0, 1]
  auto chunk1 = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk1));

  // Step 2: [0, 1)[1, 5)
  auto chunk2 = allocator_.MakeUnique<std::byte[]>(4 * kN);
  pw::ByteSpan bytes2(chunk2.get(), chunk2.size());
  mb->PushBack(std::move(chunk2));

  // Step 3: [0, 1)[1, 5)[5, 6)
  auto chunk3 = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk3));

  // Step 4: [0, 1)[1, 2)[2.5, 5)[5, 6)
  // 2 portions of chunk2 remain, so no deallocations should occur.
  allocator_.ResetParameters();
  auto result = mb->Discard(mb->begin() + 2 * kN, kN / 2);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb->size(), 11 * kN / 2);
  EXPECT_EQ(allocator_.deallocate_ptr(), nullptr);
  EXPECT_EQ(allocator_.deallocate_size(), 0u);

  // Step 5: [0, 1)[1, 2)[2.5, 3.5)[4, 5)[5, 6)
  // 3 portion of chunk2 remains, so no deallocations should occur.
  result = mb->Discard(mb->begin() + 3 * kN, kN / 2);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb->size(), 5 * kN);
  EXPECT_EQ(allocator_.deallocate_ptr(), nullptr);
  EXPECT_EQ(allocator_.deallocate_size(), 0u);

  // Step 6: [0, 1)[1, 2)[2.5, 3.5)[5, 6)
  // 2 portions of chunk2 remain, so no deallocations should occur.
  result = mb->Discard(mb->begin() + 3 * kN, kN);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb->size(), 4 * kN);
  EXPECT_EQ(allocator_.deallocate_ptr(), nullptr);
  EXPECT_EQ(allocator_.deallocate_size(), 0u);

  // Step 7: [0, 1)[2.5, 3.5)[5, 6)
  // 1 portion of chunk2 remains, so no deallocations should occur.
  result = mb->Discard(mb->begin() + 2 * kN, kN);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb->size(), 3 * kN);
  EXPECT_EQ(allocator_.deallocate_ptr(), nullptr);
  EXPECT_EQ(allocator_.deallocate_size(), 0u);

  // Step 8: [0, 1)[5, 6)
  // No portions of chunk2 remain, so deallocations should occur.
  result = mb->Discard(mb->begin() + kN, kN);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb->size(), 2 * kN);
  EXPECT_NE(allocator_.deallocate_ptr(), nullptr);
  EXPECT_EQ(allocator_.deallocate_size(),
            sizeof(pw::allocator::internal::ControlBlock));
}

TEST_F(MultiBufTest, DiscardContiguousChunks) {
  ConstMultiBuf::Instance mbi(allocator_);
  std::array<std::byte, 2 * kN> unowned;
  pw::ConstByteSpan first(unowned.data(), kN);
  pw::ConstByteSpan second(unowned.data() + kN, kN);
  mbi->PushBack(first);
  mbi->PushBack(second);

  // This test breaks the abstraction a bit, and exists only to tickle the edge
  // case where a chunk iterator coaleces multiple chunks into a single span.
  auto result = mbi->Discard(mbi->begin(), 3 * kN / 2);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mbi->size(), kN / 2);
}

TEST_F(MultiBufTest, IsReleasableReturnsFalseWhenNotOwned) {
  ConstMultiBuf::Instance mb(allocator_);
  mb->PushBack(unowned_chunk_);
  EXPECT_FALSE(mb->IsReleasable(mb->begin()));
}

TEST_F(MultiBufTest, ReleaseSucceedsWhenNotEmptyAndOwned) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(owned_chunk_));

  auto chunk = allocator_.MakeUnique<std::byte[]>(kN * 2);
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(chunk));

  pw::UniquePtr<const std::byte[]> released = mb.Release(mb.begin());
  EXPECT_EQ(released.get(), owned_bytes_.data());
  EXPECT_EQ(released.size(), owned_bytes_.size());
  EXPECT_EQ(mb.size(), kN * 2);
}

TEST_F(MultiBufTest, ReleaseSucceedsWithoutMatchingChunkBoundary) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi->PushBack(std::move(chunk));
  auto released = mbi->Release(mbi->begin() + 1);
  EXPECT_EQ(released.size(), kN);
  EXPECT_TRUE(mbi->empty());
}

TEST_F(MultiBufTest, IsShareableReturnsFalseWhenNotShared) {
  ConstMultiBuf::Instance mbi(allocator_);
  mbi->PushBack(std::move(owned_chunk_));
  EXPECT_FALSE(mbi->IsShareable(mbi->begin()));
}

TEST_F(MultiBufTest, ShareSucceedsWhenNotEmptyAndShared) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto shared1 = allocator_.MakeShared<std::byte[]>(kN * 2);
  mbi->PushBack(shared1);

  auto owned = allocator_.MakeUnique<std::byte[]>(kN);
  mbi->PushBack(std::move(owned));

  pw::SharedPtr<const std::byte[]> shared2 = mbi->Share(mbi->begin());
  EXPECT_EQ(shared1.get(), shared2.get());
  EXPECT_EQ(shared1.size(), shared2.size());
  EXPECT_EQ(mbi->size(), 3 * kN);
}

TEST_F(MultiBufTest, ShareSucceedsWithoutMatchingChunkBoundary) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto shared1 = allocator_.MakeShared<std::byte[]>(kN);
  mbi->PushBack(shared1);
  auto shared2 = mbi->Share(mbi->begin() + 1);
  EXPECT_EQ(shared2.size(), kN);
  EXPECT_EQ(mbi->size(), kN);
}

TEST_F(MultiBufTest, CopyToWithContiguousChunks) {
  ConstMultiBuf::Instance mbi(allocator_);
  std::array<std::byte, kN> unowned;
  std::memset(unowned.data(), 0xAA, unowned.size());
  pw::ConstByteSpan first(unowned.data(), unowned.size() / 2);
  pw::ConstByteSpan second(unowned.data() + unowned.size() / 2,
                           unowned.size() / 2);
  mbi->PushBack(first);
  mbi->PushBack(second);

  std::array<std::byte, kN> out;
  pw::ByteSpan bytes(out);
  for (size_t offset = 0; offset < kN; ++offset) {
    // Reset the destination.
    std::memset(bytes.data(), 0xBB, bytes.size());

    // Perform the copy.
    pw::ByteSpan dst = bytes.subspan(offset);
    EXPECT_EQ(mbi->CopyTo(dst, offset), dst.size());

    // Check the destination.
    for (size_t i = 0; i < offset; ++i) {
      EXPECT_EQ(bytes[i], static_cast<std::byte>(0xBB));
    }
    for (size_t i = offset; i < kN; ++i) {
      EXPECT_EQ(bytes[i], static_cast<std::byte>(0xAA));
    }
  }
}

TEST_F(MultiBufTest, CopyToWithNonContiguousChunks) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  MakeNonContiguous(mb, kN, 0xAA);

  std::array<std::byte, kN> out;
  pw::ByteSpan bytes(out);
  for (size_t offset = 0; offset < kN; ++offset) {
    // Reset the destination.
    std::memset(bytes.data(), 0xBB, bytes.size());

    // Perform the copy.
    pw::ByteSpan dst = bytes.subspan(offset);
    EXPECT_EQ(mb.CopyTo(dst, offset), dst.size());

    // Check the destination.
    for (size_t i = 0; i < offset; ++i) {
      EXPECT_EQ(bytes[i], static_cast<std::byte>(0xBB));
    }
    for (size_t i = offset; i < kN; ++i) {
      EXPECT_EQ(bytes[i], static_cast<std::byte>(0xAA));
    }
  }
}

TEST_F(MultiBufTest, CopyToWithMultipleChunks) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::memset(chunk.get(), 0xAA, chunk.size());
  mbi->PushBack(std::move(chunk));

  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::memset(chunk.get(), 0xBB, chunk.size());
  mbi->PushBack(std::move(chunk));

  // Check that CopyTo exits at the expected spot.
  std::array<std::byte, kN> out;
  pw::ByteSpan bytes(out);
  EXPECT_EQ(mbi->CopyTo(bytes, 0), kN);
  EXPECT_EQ(out[kN - 1], static_cast<std::byte>(0xAA));
}

TEST_F(MultiBufTest, CopyFromWithContiguousChunks) {
  MultiBuf::Instance mbi(allocator_);
  MultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(unowned_chunk_);

  std::array<std::byte, kN / 2> in;
  pw::ByteSpan bytes(in);
  ASSERT_EQ(bytes.size(), unowned_chunk_.size());
  std::memset(bytes.data(), 0xAA, bytes.size());
  for (size_t offset = 0; offset < unowned_chunk_.size(); ++offset) {
    // Reset the destination.
    std::memset(unowned_chunk_.data(), 0xBB, unowned_chunk_.size());

    // Perform the copy.
    pw::ByteSpan src = bytes.subspan(offset);
    EXPECT_EQ(mb.CopyFrom(src, offset), src.size());

    // Check the destination.
    for (size_t i = 0; i < offset; ++i) {
      EXPECT_EQ(unowned_chunk_[i], static_cast<std::byte>(0xBB));
    }
    for (size_t i = offset; i < unowned_chunk_.size(); ++i) {
      EXPECT_EQ(unowned_chunk_[i], static_cast<std::byte>(0xAA));
    }
  }
}

TEST_F(MultiBufTest, CopyFromWithNonContiguousChunks) {
  MultiBuf::Instance mbi(allocator_);
  MultiBuf& mb = mbi;
  MakeNonContiguous(mb.as<ConstMultiBuf>(), kN, 0xAA);

  std::array<std::byte, kN> in;
  pw::ByteSpan bytes(in);
  std::memset(bytes.data(), 0xBB, bytes.size());
  for (size_t offset = 0; offset < kN; ++offset) {
    // Reset the destination.
    for (auto chunk : mb.Chunks()) {
      std::memset(chunk.data(), 0xAA, chunk.size());
    }

    // Perform the copy.
    pw::ByteSpan src = bytes.subspan(offset);
    EXPECT_EQ(mb.CopyFrom(src, offset), src.size());

    // Check the destination.
    for (size_t i = 0; i < offset; ++i) {
      EXPECT_EQ(mb[i], static_cast<std::byte>(0xAA));
    }
    for (size_t i = offset; i < kN; ++i) {
      EXPECT_EQ(mb[i], static_cast<std::byte>(0xBB));
    }
  }
}

TEST_F(MultiBufTest, CopyFromWithMultipleChunks) {
  MultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::memset(chunk.get(), 0xAA, chunk.size());
  mbi->PushBack(std::move(chunk));

  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::memset(chunk.get(), 0xBB, chunk.size());
  mbi->PushBack(std::move(chunk));

  // Check that CopyTo exits at the expected spot.
  std::array<std::byte, kN> in;
  std::memset(in.data(), 0xCC, in.size());
  pw::ConstByteSpan bytes(in);
  EXPECT_EQ(mbi->CopyFrom(bytes, 0), kN);
  EXPECT_EQ((*mbi)[kN - 1], static_cast<std::byte>(0xCC));
}

TEST_F(MultiBufTest, GetContiguousDoesNotCopy) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(unowned_chunk_);
  std::memset(unowned_chunk_.data(), 0xAA, unowned_chunk_.size());

  std::array<std::byte, kN / 2> tmp;
  ASSERT_EQ(tmp.size(), unowned_chunk_.size());
  std::memset(tmp.data(), 0xBB, tmp.size());

  for (size_t offset = 0; offset < unowned_chunk_.size(); ++offset) {
    pw::ConstByteSpan bytes = mb.Get(tmp, offset);
    EXPECT_NE(bytes.data(), tmp.data());
    EXPECT_EQ(offset + bytes.size(), unowned_chunk_.size());

    // Returned span has correct data.
    for (size_t i = 0; i < bytes.size(); ++i) {
      EXPECT_EQ(bytes[i], static_cast<std::byte>(0xAA));
    }

    // Provided span is untouched.
    for (size_t i = 0; i < tmp.size(); ++i) {
      EXPECT_EQ(tmp[i], static_cast<std::byte>(0xBB));
    }
  }
}

TEST_F(MultiBufTest, GetNonContiguousCopies) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  MakeNonContiguous(mb, kN, 0xAA);

  std::array<std::byte, kN> tmp;
  std::memset(tmp.data(), 0xBB, tmp.size());

  for (size_t offset = 0; offset < unowned_chunk_.size(); ++offset) {
    pw::ConstByteSpan bytes = mb.Get(tmp, offset);
    EXPECT_EQ(bytes.data(), tmp.data());
    EXPECT_EQ(offset + bytes.size(), kN);

    // Returned span has correct data.
    for (size_t i = 0; i < bytes.size(); ++i) {
      EXPECT_EQ(bytes[i], static_cast<std::byte>(0xAA));
    }
  }
}

TEST_F(MultiBufTest, GetMoreThanAvailableTruncates) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(unowned_chunk_);

  std::array<std::byte, kN> tmp;
  std::memset(tmp.data(), 0xBB, tmp.size());
  EXPECT_LT(unowned_chunk_.size(), tmp.size());

  for (size_t offset = 0; offset < unowned_chunk_.size(); ++offset) {
    pw::ConstByteSpan bytes = mb.Get(tmp, offset);
    EXPECT_EQ(offset + bytes.size(), unowned_chunk_.size());
  }
}

TEST_F(MultiBufTest, GetPastTheEndReturnsEmpty) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(unowned_chunk_);
  std::array<std::byte, kN> tmp;
  pw::ConstByteSpan bytes = mb.Get(tmp, unowned_chunk_.size());
  EXPECT_EQ(bytes.data(), nullptr);
  EXPECT_EQ(bytes.size(), 0u);
}

TEST_F(MultiBufTest, VisitContiguousDoesNotCopy) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(unowned_chunk_);
  std::memset(unowned_chunk_.data(), 0x01, unowned_chunk_.size());

  std::array<std::byte, kN / 2> tmp;
  ASSERT_EQ(tmp.size(), unowned_chunk_.size());
  std::memset(tmp.data(), 0x02, tmp.size());

  for (size_t offset = 0; offset < unowned_chunk_.size(); ++offset) {
    size_t total = 0;
    mb.Visit(
        [&total](pw::ConstByteSpan bytes) {
          for (const std::byte& b : bytes) {
            total += static_cast<size_t>(b);
          }
        },
        tmp,
        offset);
    EXPECT_EQ(total, unowned_chunk_.size() - offset);

    // Provided span is untouched.
    for (size_t i = 0; i < tmp.size(); ++i) {
      EXPECT_EQ(tmp[i], static_cast<std::byte>(0x02));
    }
  }
}

TEST_F(MultiBufTest, VisitNonContiguousCopies) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  MakeNonContiguous(mb, kN, 0x01);

  std::array<std::byte, kN> tmp;
  std::memset(tmp.data(), 0x02, tmp.size());

  for (size_t offset = 0; offset < kN; ++offset) {
    size_t total = 0;
    mb.Visit(
        [&total](pw::ConstByteSpan bytes) {
          for (const std::byte& b : bytes) {
            total += static_cast<size_t>(b);
          }
        },
        tmp,
        offset);
    EXPECT_EQ(total, kN - offset);

    // Provided span is modified.
    bool modified = false;
    for (size_t i = 0; i < tmp.size(); ++i) {
      modified |= tmp[i] != static_cast<std::byte>(0x02);
    }
    EXPECT_TRUE(modified);
  }
}

TEST_F(MultiBufTest, ClearFreesChunks) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(owned_chunk_));
  size_t allocated = allocator_.GetAllocated();

  mb.Clear();
  EXPECT_LT(allocator_.GetAllocated(), allocated);
}

TEST_F(MultiBufTest, ShrinkToFitFreesUnusedCapacity) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;

  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(owned_chunk_));
  size_t allocated = allocator_.GetAllocated();

  ASSERT_TRUE(mb.TryReserveForPushBack());
  EXPECT_GT(allocator_.GetAllocated(), allocated);

  // Don't actually push chunks. Shrink to fit should free the unused slots.
  mb.ShrinkToFit();
  EXPECT_EQ(allocator_.GetAllocated(), allocated);
}

TEST_F(MultiBufTest, ClearAndShrinkToFitFreesAllMemory) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(owned_chunk_));

  mb.Clear();
  mb.ShrinkToFit();
  EXPECT_EQ(allocator_.GetAllocated(), 0U);
}

TEST_F(MultiBufTest, IsReusableAfterClear) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(chunk));
  mb.Clear();

  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  ASSERT_TRUE(mb.TryReserveForPushBack());
  mb.PushBack(std::move(chunk));
}

#if PW_NC_TEST(CannotCallNumFragmentsWhenUnlayered)
PW_NC_EXPECT("`NumFragments` may only be called on layerable MultiBufs");
[[maybe_unused]] size_t ShouldAssert(const FlatMultiBuf& mb) {
  return mb.NumFragments();
}
#endif  // PW_NC_TEST

TEST_F(MultiBufTest, NumFragmentsIsZeroWhenEmpty) {
  ConstMultiBuf::Instance mbi(allocator_);
  EXPECT_EQ(mbi->NumFragments(), 0u);
}

TEST_F(MultiBufTest, NumFragmentsWithoutLayersMatchesChunks) {
  ConstMultiBuf::Instance mbi1(allocator_);
  ConstMultiBuf& mb1 = mbi1;

  auto chunk = allocator_.MakeUnique<std::byte[]>(kN * 2);
  mb1.PushBack(std::move(chunk));
  EXPECT_EQ(mb1.NumFragments(), 1u);

  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb1.PushBack(std::move(chunk));
  EXPECT_EQ(mb1.NumFragments(), 2u);

  chunk = allocator_.MakeUnique<std::byte[]>(kN / 2);
  mb1.PushBack(std::move(chunk));
  EXPECT_EQ(mb1.NumFragments(), 3u);

  auto result = mb1.PopFrontFragment();
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb1.NumFragments(), 2u);

  result = mb1.PopFrontFragment();
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb1.NumFragments(), 1u);

  result = mb1.PopFrontFragment();
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mb1.NumFragments(), 0u);
}

TEST_F(MultiBufTest, NumFragmentsWithLayersMatchesAddedFragments) {
  ConstMultiBuf::Instance mb(allocator_);
  AddLayers(*mb);
  EXPECT_EQ(mb->NumFragments(), 2u);
}

#if PW_NC_TEST(CannotCallNumLayersWhenUnlayered)
PW_NC_EXPECT("`NumLayers` may only be called on layerable MultiBufs");
[[maybe_unused]] size_t ShouldAssert(const FlatMultiBuf& mb) {
  return mb.NumLayers();
}
#endif  // PW_NC_TEST

TEST_F(MultiBufTest, NumLayersIsOneWhenEmpty) {
  ConstMultiBuf::Instance mbi(allocator_);
  ConstMultiBuf& mb = mbi;
  EXPECT_EQ(mb.NumLayers(), 1u);
}

TEST_F(MultiBufTest, NumLayersMatchesAddedLayers) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  EXPECT_EQ(mb->NumLayers(), 1u);
  EXPECT_TRUE(mb->AddLayer(0));
  EXPECT_EQ(mb->NumLayers(), 2u);
  EXPECT_TRUE(mb->AddLayer(0));
  EXPECT_EQ(mb->NumLayers(), 3u);
}

TEST_F(MultiBufTest, TryReserveLayersWithNumLayersEqualToZero) {
  ConstMultiBuf::Instance mb(allocator_);
  allocator_.ResetParameters();
  EXPECT_TRUE(mb->TryReserveLayers(0, 3));
  EXPECT_EQ(allocator_.allocate_size(), 0u);
}

TEST_F(MultiBufTest, TryReserveLayersWithNumChunksEqualToZero) {
  ConstMultiBuf::Instance mb(allocator_);
  allocator_.ResetParameters();
  EXPECT_TRUE(mb->TryReserveLayers(3, 0));
  EXPECT_EQ(allocator_.allocate_size(), 0u);
}

TEST_F(MultiBufTest, TryReserveLayersWhenAlreadyReserved) {
  ConstMultiBuf::Instance mb(allocator_);
  EXPECT_TRUE(mb->TryReserveLayers(3, 4));

  // No additional memory needed.
  allocator_.ResetParameters();
  EXPECT_TRUE(mb->TryReserveLayers(2, 3));
  EXPECT_EQ(allocator_.allocate_size(), 0u);
}

TEST_F(MultiBufTest, TryReserveLayers) {
  ConstMultiBuf::Instance mb(allocator_);
  allocator_.ResetParameters();
  EXPECT_TRUE(mb->TryReserveLayers(2, 3));
  EXPECT_NE(allocator_.allocate_size(), 0u);
}

TEST_F(MultiBufTest, TryReserveLayersWithAllocationFailure) {
  ConstMultiBuf::Instance mb(allocator_);
  allocator_.Exhaust();
  EXPECT_FALSE(mb->TryReserveLayers(1, 1));
}

TEST_F(MultiBufTest, IterateChunksOverLayers) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(mbi);
  size_t i = 0;
  size_t total = 0;
  for (auto chunk : mbi->Chunks()) {
    i++;
    total += chunk.size();
  }
  // See `AddLayers`. Span lengths should be [8, 8, 16].
  EXPECT_EQ(i, 3u);
  EXPECT_EQ(total, 32u);
}

TEST_F(MultiBufTest, IterateBytesOverLayers) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(mbi);
  // See `AddLayers`. Span lengths should be [8, 8, 16].
  EXPECT_EQ(mbi->end() - mbi->begin(), 32);
}

TEST_F(MultiBufTest, InsertAddsLayersAsNeeded) {
  ConstMultiBuf::Instance mbi1(allocator_);

  // Insert a MultiBuf of greater depth.
  ConstMultiBuf::Instance mbi2(allocator_);
  AddLayers(mbi2);
  EXPECT_EQ(mbi1->NumLayers(), 1u);
  EXPECT_EQ(mbi2->NumLayers(), 3u);
  mbi1->Insert(mbi1->end(), std::move(*mbi2));
  EXPECT_EQ(mbi1->NumLayers(), 3u);

  // Insert a (non-empty) MultiBuf of less depth.
  ConstMultiBuf::Instance mbi3(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi3->PushBack(std::move(chunk));
  EXPECT_EQ(mbi1->NumLayers(), 3u);
  EXPECT_EQ(mbi3->NumLayers(), 1u);
  mbi1->Insert(mbi1->end(), std::move(*mbi3));
  EXPECT_EQ(mbi1->NumLayers(), 3u);

  // Insert a chunk directly into a layered MultiBuf.
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi1->PushBack(std::move(chunk));
  EXPECT_EQ(mbi1->NumLayers(), 3u);
}

TEST_F(MultiBufTest, TryReserveForInsertAddsNoLayersOnAllocationFailure) {
  ConstMultiBuf::Instance mbi1(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi1->PushBack(std::move(chunk));

  ConstMultiBuf::Instance mbi2(allocator_);
  AddLayers(mbi2);

  // Add, exhaust, and pop to ensure we can add one but not all layers.
  EXPECT_TRUE(mbi1->AddLayer(0));
  allocator_.Exhaust();
  EXPECT_FALSE(mbi1->IsTopLayerSealed());
  mbi1->PopLayer();

  EXPECT_EQ(mbi1->NumLayers(), 1u);
  EXPECT_EQ(mbi2->NumLayers(), 3u);
  EXPECT_FALSE(mbi1->TryReserveForInsert(mbi1->end(), *mbi2));
  EXPECT_EQ(mbi1->NumLayers(), 1u);
  EXPECT_EQ(mbi2->NumLayers(), 3u);
}

TEST_F(MultiBufTest, RemoveFromLayeredIsRelativeToTopLayer) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeShared<std::byte[]>(5 * kN);
  std::byte* data = chunk.get();
  mbi->PushBack(chunk);
  EXPECT_EQ(mbi->size(), 5 * kN);

  EXPECT_TRUE(mbi->AddLayer(kN, 3 * kN));
  EXPECT_EQ(mbi->size(), 3 * kN);

  auto result = mbi->Remove(mbi->begin() + kN, kN);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(mbi->size(), 2 * kN);

  mbi->PopLayer();
  EXPECT_EQ(&(*(mbi->begin())), data);
  EXPECT_EQ(&(*(mbi->begin() + 2 * kN)), data + 3 * kN);
}

TEST_F(MultiBufTest, DiscardFromLayeredIsRelativeToTopLayer) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeShared<std::byte[]>(5 * kN);
  std::byte* data = chunk.get();
  mbi->PushBack(chunk);
  EXPECT_EQ(mbi->size(), 5 * kN);

  EXPECT_TRUE(mbi->AddLayer(kN, 3 * kN));
  EXPECT_EQ(mbi->size(), 3 * kN);

  auto result = mbi->Discard(mbi->begin() + kN, kN);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(*result, mbi->begin() + kN);
  EXPECT_EQ(mbi->size(), 2 * kN);

  mbi->PopLayer();
  EXPECT_EQ(&(*(mbi->begin())), data);
  EXPECT_EQ(&(*(mbi->begin() + 2 * kN)), data + 3 * kN);
}

TEST_F(MultiBufTest, ReleaseFromLayeredIsRelativeToTopLayer) {
  MultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(2 * kN);
  mbi->PushBack(std::move(chunk));
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::byte* data = chunk.get();
  mbi->PushBack(std::move(chunk));
  chunk = allocator_.MakeUnique<std::byte[]>(2 * kN);
  mbi->PushBack(std::move(chunk));
  EXPECT_EQ(mbi->size(), 5 * kN);

  EXPECT_TRUE(mbi->AddLayer(kN, 3 * kN));
  EXPECT_EQ(mbi->size(), 3 * kN);

  chunk = mbi->Release(mbi->begin() + kN);
  EXPECT_EQ(chunk.get(), data);
  EXPECT_EQ(mbi->size(), 2 * kN);
}

TEST_F(MultiBufTest, ShareFromLayeredIsRelativeToTopLayer) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(2 * kN);
  mbi->PushBack(std::move(chunk));
  auto shared1 = allocator_.MakeShared<std::byte[]>(kN);
  mbi->PushBack(shared1);
  chunk = allocator_.MakeUnique<std::byte[]>(2 * kN);
  mbi->PushBack(std::move(chunk));
  EXPECT_EQ(mbi->size(), 5 * kN);

  EXPECT_TRUE(mbi->AddLayer(kN, 3 * kN));
  EXPECT_EQ(mbi->size(), 3 * kN);

  auto shared2 = mbi->Share(mbi->begin() + kN);
  EXPECT_EQ(shared1.get(), shared2.get());
}

#if PW_NC_TEST(CannotCallAddLayerWhenUnlayered)
PW_NC_EXPECT("`AddLayer` may only be called on layerable MultiBufs");
[[maybe_unused]] bool ShouldAssert(const FlatMultiBuf& mb) {
  return mb.AddLayer(0);
}
#endif  // PW_NC_TEST

TEST_F(MultiBufTest, AddLayerSucceedsWhenEmpty) {
  ConstMultiBuf::Instance mb(allocator_);
  EXPECT_TRUE(mb->AddLayer(0));
  EXPECT_EQ(mb->NumLayers(), 2u);
}

TEST_F(MultiBufTest, AddLayerFailsUnableToGrowQueue) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN * 2);
  mbi->PushBack(std::move(chunk));
  allocator_.Exhaust();

  EXPECT_EQ(mbi->NumLayers(), 1u);
  EXPECT_FALSE(mbi->AddLayer(0, 0));
  EXPECT_EQ(mbi->NumLayers(), 1u);
}

TEST_F(MultiBufTest, AddLayerSucceedsWithZeroOffset) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi->PushBack(std::move(chunk));

  EXPECT_TRUE(mbi->AddLayer(0));
  EXPECT_EQ(mbi->size(), kN);
  EXPECT_EQ(mbi->NumLayers(), 2u);

  EXPECT_TRUE(mbi->AddLayer(0));
  EXPECT_EQ(mbi->size(), kN);
  EXPECT_EQ(mbi->NumLayers(), 3u);

  EXPECT_TRUE(mbi->AddLayer(0));
  EXPECT_EQ(mbi->size(), kN);
  EXPECT_EQ(mbi->NumLayers(), 4u);
}

TEST_F(MultiBufTest, AddLayerSucceedsWithNonzeroOffset) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi->PushBack(std::move(chunk));

  EXPECT_TRUE(mbi->AddLayer(2));
  EXPECT_EQ(mbi->size(), kN - 2);
  EXPECT_EQ(mbi->NumLayers(), 2u);

  EXPECT_TRUE(mbi->AddLayer(4));
  EXPECT_EQ(mbi->size(), kN - 6);
  EXPECT_EQ(mbi->NumLayers(), 3u);

  EXPECT_TRUE(mbi->AddLayer(8));
  EXPECT_EQ(mbi->size(), kN - 14);
  EXPECT_EQ(mbi->NumLayers(), 4u);
}

TEST_F(MultiBufTest, AddLayerSucceedsWithZeroLength) {
  ConstMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  EXPECT_TRUE(mb->AddLayer(0, 0));
}

TEST_F(MultiBufTest, AddLayerSucceedsWithNonzeroLength) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi->PushBack(std::move(chunk));

  EXPECT_TRUE(mbi->AddLayer(0, kN - 3));
  EXPECT_EQ(mbi->size(), kN - 3);
  EXPECT_EQ(mbi->NumLayers(), 2u);

  EXPECT_TRUE(mbi->AddLayer(0, kN - 7));
  EXPECT_EQ(mbi->size(), kN - 7);
  EXPECT_EQ(mbi->NumLayers(), 3u);

  EXPECT_TRUE(mbi->AddLayer(0, kN - 11));
  EXPECT_EQ(mbi->size(), kN - 11);
  EXPECT_EQ(mbi->NumLayers(), 4u);
}

TEST_F(MultiBufTest, AddLayerCreatesNewFragment) {
  ConstMultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi->PushBack(std::move(chunk));
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi->PushBack(std::move(chunk));
  chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mbi->PushBack(std::move(chunk));

  EXPECT_EQ(mbi->NumFragments(), 3u);
  EXPECT_TRUE(mbi->AddLayer(0));
  EXPECT_EQ(mbi->NumFragments(), 1u);
  mbi->PopLayer();
  EXPECT_EQ(mbi->NumFragments(), 3u);
}

TEST_F(MultiBufTest, PopFrontFragmentWithMultipleLayers) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(*mbi);
  EXPECT_EQ(mbi->NumFragments(), 2u);

  // See `AddLayers`. Fragment lengths should be [8, 24].
  auto result = mbi->PopFrontFragment();
  EXPECT_EQ(mbi->NumFragments(), 1u);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(result.value()->size(), 8u);
  EXPECT_EQ(mbi->size(), 24u);

  result = mbi->PopFrontFragment();
  EXPECT_EQ(mbi->NumFragments(), 0u);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(result.value()->size(), 24u);
  EXPECT_TRUE(mbi->empty());
}

TEST_F(MultiBufTest, PopFrontFragmentSkipsZeroLengthChunks) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(*mbi);

  // Adding an extra layer makes the zero-length chunk fall within a fragment.
  EXPECT_TRUE(mbi->AddLayer(0));
  auto result = mbi->PopFrontFragment();
  EXPECT_EQ(mbi->NumFragments(), 0u);
  ASSERT_EQ(result.status(), pw::OkStatus());
  EXPECT_EQ(result.value()->size(), 32u);
  EXPECT_TRUE(mbi->empty());
}

#if PW_NC_TEST(CannotCallSealTopLayerWhenUnlayered)
PW_NC_EXPECT("`SealTopLayer` may only be called on layerable MultiBufs");
[[maybe_unused]] void ShouldAssert(const FlatMultiBuf& mb) {
  mb.SealTopLayer();
}

#elif PW_NC_TEST(CannotCallUnsealTopLayerWhenUnlayered)
PW_NC_EXPECT("`UnsealTopLayer` may only be called on layerable MultiBufs");
[[maybe_unused]] void ShouldAssert(const FlatMultiBuf& mb) {
  mb.UnsealTopLayer();
}

#elif PW_NC_TEST(CannotCallTruncateTopLayerWhenUnlayered)
PW_NC_EXPECT("`TruncateTopLayer` may only be called on layerable MultiBufs");
[[maybe_unused]] void ShouldAssert(const FlatMultiBuf& mb) {
  mb.TruncateTopLayer(6);
}
#endif  // PW_NC_TEST

TEST_F(MultiBufTest, TruncateTopLayerSucceedsWithZeroLength) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(*mbi);
  EXPECT_EQ(mbi->size(), 32u);
  mbi->TruncateTopLayer(0);
  EXPECT_EQ(mbi->size(), 0u);
}

TEST_F(MultiBufTest, TruncateTopLayerSucceedsWithNonzeroLength) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(*mbi);
  EXPECT_EQ(mbi->size(), 32u);
  mbi->TruncateTopLayer(6);
  constexpr auto expected =  // Keeps existing top layer start offset 4.
      pw::bytes::Array<0x04, 0x05, 0x06, 0x07, 0x08, 0x09>();
  EXPECT_TRUE(
      std::equal(mbi->begin(), mbi->end(), expected.begin(), expected.end()));
}

TEST_F(MultiBufTest, TruncateTopLayerSucceedsWithCurrentMultiBufLength) {
  ConstMultiBuf::Instance mbi1(allocator_);
  ConstMultiBuf::Instance mbi2(allocator_);
  AddLayers(*mbi1);
  AddLayers(*mbi2);
  mbi1->TruncateTopLayer(mbi1->size());
  EXPECT_TRUE(
      std::equal(mbi1->begin(), mbi1->end(), mbi2->begin(), mbi2->end()));
}

TEST_F(MultiBufTest, PopLayerSucceedsWithLayers) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(*mbi);

  // See `AddLayers`.
  EXPECT_EQ(mbi->NumFragments(), 2u);
  EXPECT_EQ(mbi->NumLayers(), 3u);
  EXPECT_EQ(mbi->size(), 32u);

  mbi->PopLayer();
  EXPECT_EQ(mbi->NumFragments(), 3u);
  EXPECT_EQ(mbi->NumLayers(), 2u);
  EXPECT_EQ(mbi->size(), 48u);

  mbi->PopLayer();
  EXPECT_EQ(mbi->NumFragments(), 4u);
  EXPECT_EQ(mbi->NumLayers(), 1u);
  EXPECT_EQ(mbi->size(), 64u);
}

#if PW_NC_TEST(CannotCallSetTopLayerWhenUnlayered)
PW_NC_EXPECT(
    "`SetTopLayer` may only be called on mutable, layerable MultiBufs");
[[maybe_unused]] void ShouldAssert(FlatMultiBuf& mb) {
  mb.SetTopLayer(pw::ConstByteSpan());
}
#endif  // PW_NC_TEST

TEST_F(MultiBufTest, SetTopLayerCopiesDataAndTruncates) {
  MultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::memset(chunk.get(), 0xAA, chunk.size());
  mbi->PushBack(std::move(chunk));
  ASSERT_TRUE(mbi->AddLayer(0));

  std::array<std::byte, kN / 2> new_data;
  std::memset(new_data.data(), 0xBB, new_data.size());
  mbi->SetTopLayer(new_data);

  EXPECT_EQ(mbi->size(), new_data.size());
  for (size_t i = 0; i < mbi->size(); ++i) {
    EXPECT_EQ((*mbi)[i], static_cast<std::byte>(0xBB));
  }

  mbi->PopLayer();
  EXPECT_EQ(mbi->size(), kN);
  for (size_t i = 0; i < new_data.size(); ++i) {
    EXPECT_EQ((*mbi)[i], static_cast<std::byte>(0xBB));
  }
  for (size_t i = new_data.size(); i < kN; ++i) {
    EXPECT_EQ((*mbi)[i], static_cast<std::byte>(0xAA));
  }
}

TEST_F(MultiBufTest, SetTopLayerWorksWithEmptySpan) {
  MultiBuf::Instance mbi(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  std::memset(chunk.get(), 0xAA, chunk.size());
  mbi->PushBack(std::move(chunk));
  ASSERT_TRUE(mbi->AddLayer(0));

  mbi->SetTopLayer(pw::ConstByteSpan());
  EXPECT_EQ(mbi->size(), 0u);
}

#if PW_NC_TEST(CannotCallPopLayerWhenUnlayered)
PW_NC_EXPECT("`PopLayer` may only be called on layerable MultiBufs");
void ShouldAssert(const FlatMultiBuf& mb) { mb.PopLayer(); }
#endif  // PW_NC_TEST

TEST_F(MultiBufTest, PopLayerFailsWhenSealed) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(*mbi);
  mbi->SealTopLayer();
  EXPECT_EQ(mbi->NumLayers(), 3u);
  EXPECT_TRUE(mbi->IsTopLayerSealed());
  EXPECT_DEATH_IF_SUPPORTED(mbi->PopLayer(), ".*");
}

TEST_F(MultiBufTest, PopLayerSucceedsAfterUnseal) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(*mbi);
  mbi->SealTopLayer();
  EXPECT_EQ(mbi->NumLayers(), 3u);
  EXPECT_TRUE(mbi->IsTopLayerSealed());
  EXPECT_DEATH_IF_SUPPORTED(mbi->PopLayer(), ".*");
  mbi->UnsealTopLayer();
  mbi->PopLayer();
  EXPECT_EQ(mbi->NumLayers(), 2u);
}

TEST_F(MultiBufTest, CopyToGetsDataFromTopLayerOnly) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(*mbi);

  // See `AddLayers`. Byte sequences should be [0x04..0x0C), [0x28..0x40)
  std::array<std::byte, kN> tmp;
  EXPECT_EQ(mbi->CopyTo(tmp, 1), kN - 1);
  uint8_t expected = 0x04 + 1;
  for (uint8_t i = 0; i < kN - 1; ++i) {
    if (expected == 0x0C) {
      expected = 0x28;
    }
    EXPECT_EQ(tmp[i], static_cast<std::byte>(expected));
    ++expected;
  }
}

TEST_F(MultiBufTest, CopyFromSetsDataInTopLayerOnly) {
  MultiBuf::Instance mbi(allocator_);
  AddLayers(*mbi);

  // See `AddLayers`. Byte sequences should be [0x04..0x0C), [0x28..0x40)
  std::array<std::byte, kN - 4> tmp;
  std::memset(tmp.data(), 0xAA, tmp.size());
  EXPECT_EQ(mbi->CopyFrom(tmp, 2), tmp.size());
  EXPECT_EQ(mbi->at(0), std::byte(0x04));
  EXPECT_EQ(mbi->at(1), std::byte(0x05));
  EXPECT_EQ(mbi->at(2), std::byte(0xAA));
  EXPECT_EQ(mbi->at(kN - 3), std::byte(0xAA));
  EXPECT_EQ(mbi->at(kN - 2), std::byte(0x3E));
  EXPECT_EQ(mbi->at(kN - 1), std::byte(0x3F));
}

TEST_F(MultiBufTest, GetReturnsDataFromTopLayerOnly) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(*mbi);

  // See `AddLayers`. Byte sequences should be [0x04..0x0C), [0x28..0x40)
  std::array<std::byte, kN> tmp;
  pw::ConstByteSpan bytes = mbi->Get(tmp, 3);
  EXPECT_EQ(bytes.size(), kN - 3);
  uint8_t expected = 0x04 + 3;
  for (uint8_t i = 0; i < bytes.size(); ++i) {
    if (expected == 0x0C) {
      expected = 0x28;
    }
    EXPECT_EQ(bytes[i], static_cast<std::byte>(expected));
    ++expected;
  }
}

TEST_F(MultiBufTest, GetDoesNotCopyIfTopLayerIsContiguous) {
  std::array<std::byte, 2 * kN> unowned;
  pw::ByteSpan bytes(unowned);

  ConstMultiBuf::Instance mbi1(allocator_);
  mbi1->PushBack(bytes.subspan(0, kN));
  EXPECT_TRUE(mbi1->AddLayer(0));

  ConstMultiBuf::Instance mbi2(allocator_);
  mbi2->PushBack(bytes.subspan(kN));
  EXPECT_TRUE(mbi2->AddLayer(0));

  mbi1->PushBack(std::move(*mbi2));

  std::array<std::byte, kN> tmp;
  pw::ConstByteSpan retrieved = mbi1->Get(tmp, kN / 2);
  EXPECT_NE(tmp.data(), retrieved.data());
}

TEST_F(MultiBufTest, GetCopiesIfTopLayerIsNonContiguous) {
  std::array<std::byte, 2 * kN> unowned;
  pw::ByteSpan bytes(unowned);

  ConstMultiBuf::Instance mbi1(allocator_);
  mbi1->PushBack(bytes.subspan(0, kN));
  EXPECT_TRUE(mbi1->AddLayer(0, kN - 1));

  ConstMultiBuf::Instance mbi2(allocator_);
  mbi2->PushBack(bytes.subspan(kN + 1));
  EXPECT_TRUE(mbi2->AddLayer(0));

  mbi1->PushBack(std::move(*mbi2));

  std::array<std::byte, kN - 2> tmp;
  pw::ConstByteSpan retrieved = mbi1->Get(tmp, kN / 2);
  EXPECT_EQ(tmp.data(), retrieved.data());
}

TEST_F(MultiBufTest, VisitActsOnDataFromTopLayerOnly) {
  ConstMultiBuf::Instance mbi(allocator_);
  AddLayers(*mbi);

  // See `AddLayers`. Byte sequences should be [0x04..0x0C), [0x28..0x40)
  std::array<std::byte, kN> tmp;
  EXPECT_TRUE(mbi->Visit(
      [](pw::ConstByteSpan bytes) {
        uint8_t expected = 0x04 + 5;
        for (uint8_t i = 0; i < bytes.size(); ++i) {
          if (expected == 0x0C) {
            expected = 0x28;
          }
          if (bytes[i] != static_cast<std::byte>(expected)) {
            return false;
          }
          ++expected;
        }
        return true;
      },
      tmp,
      5));
}

#if PW_NC_TEST(CannotSetObserverWhenUntracked)
PW_NC_EXPECT("`set_observer` may only be called on observable MultiBufs");
[[maybe_unused]] void ShouldAssert(ConstMultiBuf& mb, TestObserver& observer) {
  return mb.set_observer(&observer);
}
#endif  // PW_NC_TEST

TEST_F(MultiBufTest, InsertMultiBufNotifiesObserver) {
  TestObserver observer1, observer2;

  TrackedMultiBuf::Instance mb1(allocator_);
  mb1->set_observer(&observer1);

  TrackedMultiBuf::Instance mb2(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb2->PushBack(std::move(chunk));
  mb2->set_observer(&observer2);

  mb1->Insert(mb1->begin(), std::move(*mb2));
  ASSERT_TRUE(observer1.event.has_value());
  EXPECT_EQ(observer1.event.value(), Event::kBytesAdded);
  EXPECT_EQ(observer1.value, kN);

  ASSERT_TRUE(observer2.event.has_value());
  EXPECT_EQ(observer2.event.value(), Event::kBytesRemoved);
  EXPECT_EQ(observer2.value, kN);
}

TEST_F(MultiBufTest, InsertUnownedNotifiesObserver) {
  TestObserver observer;
  TrackedMultiBuf::Instance mb(allocator_);
  mb->set_observer(&observer);
  mb->Insert(mb->begin(), unowned_chunk_);
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kBytesAdded);
  EXPECT_EQ(observer.value, unowned_chunk_.size());
}

TEST_F(MultiBufTest, InsertUniquePtrNotifiesObserver) {
  TestObserver observer;
  TrackedMultiBuf::Instance mb(allocator_);
  mb->set_observer(&observer);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->Insert(mb->begin(), std::move(chunk));
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kBytesAdded);
  EXPECT_EQ(observer.value, kN);
}

TEST_F(MultiBufTest, PushBackMultiBufNotifiesObserver) {
  TestObserver observer;

  TrackedMultiBuf::Instance mbi1(allocator_);
  TrackedMultiBuf& mb1 = mbi1;
  mb1.set_observer(&observer);

  TrackedMultiBuf::Instance mbi2(allocator_);
  TrackedMultiBuf& mb2 = mbi2;
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN * 2);
  mb2.PushBack(std::move(chunk));

  EXPECT_FALSE(observer.event.has_value());
  mb1.PushBack(std::move(mb2));
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kBytesAdded);
  EXPECT_EQ(observer.value, kN * 2);
}

TEST_F(MultiBufTest, PushBackUnownedNotifiesObserver) {
  TestObserver observer;

  TrackedMultiBuf::Instance mbi(allocator_);
  TrackedMultiBuf& mb = mbi;
  mb.set_observer(&observer);

  EXPECT_FALSE(observer.event.has_value());
  mb.PushBack(unowned_chunk_);
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kBytesAdded);
  EXPECT_EQ(observer.value, unowned_chunk_.size());
}

TEST_F(MultiBufTest, PushBackUniquePtrNotifiesObserver) {
  TestObserver observer;
  TrackedMultiBuf::Instance mb(allocator_);
  mb->set_observer(&observer);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kBytesAdded);
  EXPECT_EQ(observer.value, kN);
}

TEST_F(MultiBufTest, RemoveNotifiesObserver) {
  TestObserver observer;

  TrackedMultiBuf::Instance mbi(allocator_);
  TrackedMultiBuf& mb = mbi;
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb.PushBack(std::move(chunk));

  mb.set_observer(&observer);
  EXPECT_FALSE(observer.event.has_value());
  auto result = mb.Remove(mb.begin(), kN);
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kBytesRemoved);
  EXPECT_EQ(observer.value, kN);
}

TEST_F(MultiBufTest, DiscardNotifiesObserver) {
  TestObserver observer;
  TrackedMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  mb->set_observer(&observer);
  std::ignore = mb->Discard(mb->begin(), kN);
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kBytesRemoved);
  EXPECT_EQ(observer.value, kN);
}

TEST_F(MultiBufTest, ReleaseNotifiesObserver) {
  TestObserver observer;
  TrackedMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  mb->set_observer(&observer);
  mb->Release(mb->begin());
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kBytesRemoved);
  EXPECT_EQ(observer.value, kN);
}

TEST_F(MultiBufTest, PopFrontFragmentNotifiesObserver) {
  TestObserver observer;
  TrackedMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  mb->set_observer(&observer);
  std::ignore = mb->PopFrontFragment();
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kBytesRemoved);
  EXPECT_EQ(observer.value, kN);
}

TEST_F(MultiBufTest, AddLayerNotifiesObserver) {
  TestObserver observer;
  TrackedMultiBuf::Instance mb(allocator_);
  AddLayers(*mb);
  mb->set_observer(&observer);
  EXPECT_TRUE(mb->AddLayer(0));
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kLayerAdded);
  EXPECT_EQ(observer.value, 2u);
}

TEST_F(MultiBufTest, PopLayerNotifiesObserver) {
  TestObserver observer;
  TrackedMultiBuf::Instance mb(allocator_);
  AddLayers(*mb);
  mb->set_observer(&observer);
  mb->PopLayer();
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kLayerRemoved);
  EXPECT_EQ(observer.value, 2u);
}

TEST_F(MultiBufTest, ClearNotifiesObserver) {
  TestObserver observer;
  TrackedMultiBuf::Instance mb(allocator_);
  auto chunk = allocator_.MakeUnique<std::byte[]>(kN);
  mb->PushBack(std::move(chunk));
  mb->set_observer(&observer);
  EXPECT_EQ(mb->observer(), &observer);

  mb->Clear();
  ASSERT_TRUE(observer.event.has_value());
  EXPECT_EQ(observer.event.value(), Event::kBytesRemoved);
  EXPECT_EQ(observer.value, kN);
  EXPECT_EQ(mb->observer(), nullptr);
}

}  // namespace
