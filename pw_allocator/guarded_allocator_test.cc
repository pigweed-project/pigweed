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

#include "pw_allocator/guarded_allocator.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>

#include "pw_allocator/first_fit.h"
#include "pw_allocator/sync_allocator_testing.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/mutex.h"
#include "pw_unit_test/framework.h"

// TODO: https://pwbug.dev/365161669 - Express joinability as a build-system
// constraint.
#if PW_THREAD_JOINING_ENABLED

namespace {

// Test fixtures.

static constexpr size_t kCapacity = 8192;

using ::pw::allocator::GuardedAllocator;
using ::pw::allocator::Layout;
using ::pw::allocator::internal::GuardError;
using ::pw::allocator::test::Background;
using ::pw::allocator::test::BackgroundThreadCore;
using ::pw::allocator::test::SyncAllocatorTest;
using BlockType = ::pw::allocator::FirstFitBlock<uintptr_t>;
using BlockAllocator = ::pw::allocator::FirstFitAllocator<BlockType>;

enum class Mode {
  kValidateOne,
  kValidateAll,
};

// An "error handler" that records the first error encountered, allowing
// failures to be detected and inspected without crashing.
struct GuardErrorRecorder {
  static inline std::optional<GuardError> error = std::nullopt;

  static void Reset() { error.reset(); }

  static void HandleError(GuardError err,
                          const void* = nullptr,
                          size_t = 0,
                          const void* = nullptr,
                          size_t = 0) {
    if (!error.has_value()) {
      error = err;
    }
  }
};

/// Thread body that validates a guarded allocator's blocks in the background.
template <typename GuardedAllocatorType>
class GuardedAllocatorTestThreadCore : public BackgroundThreadCore {
 public:
  using BlockType = typename GuardedAllocatorType::BlockType;

  GuardedAllocatorTestThreadCore(GuardedAllocatorType& allocator)
      : allocator_(allocator) {}

  void SetMode(Mode mode) {
    std::lock_guard lock(mutex_);
    mode_ = mode;
  }

  BlockType* GetInvalid() const PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard lock(mutex_);
    return invalid_;
  }

  /// Clobbers the byte at the given location and saves the original byte.
  ///
  /// This is done in a thread-safe manner to allow the allocator to detect the
  /// corruption without TSAN flagging it first.
  void Corrupt(uint8_t* ptr, size_t pattern = 0xFF) PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard lock(mutex_);
    original_ = *ptr;
    corrupted_ = ptr;
    *corrupted_ ^= static_cast<uint8_t>(pattern & 0xFF);
  }

  /// Restores a corrupted byte to its original value.
  void Restore() PW_LOCKS_EXCLUDED(mutex_) {
    std::lock_guard lock(mutex_);
    *corrupted_ = original_;
    invalid_ = nullptr;
  }

  bool RunOnce() PW_LOCKS_EXCLUDED(mutex_) override {
    std::lock_guard lock(mutex_);
    switch (mode_) {
      case Mode::kValidateOne:
        invalid_ = allocator_.ValidateOne();
        break;
      case Mode::kValidateAll:
        invalid_ = allocator_.ValidateAll();
        break;
    }
    return invalid_ == nullptr;
  }

 private:
  mutable ::pw::sync::Mutex mutex_;
  Mode mode_ = Mode::kValidateOne;
  BlockType* invalid_ PW_GUARDED_BY(mutex_) = nullptr;
  uint8_t* corrupted_ PW_GUARDED_BY(mutex_) = nullptr;
  uint8_t original_ PW_GUARDED_BY(mutex_) = 0;
  GuardedAllocatorType& allocator_;
};

/// Test fixture responsible for managing a guarded allocator and a
/// background thread that accesses it concurrently with unit tests.
///
/// @tparam LockType  Synchronization type used by the allocator.
template <typename LockType>
class GuardedAllocatorTestBase : public SyncAllocatorTest {
 protected:
  using GuardedAllocatorType =
      GuardedAllocator<BlockAllocator, LockType, GuardErrorRecorder>;
  using BlockType = BlockAllocator::BlockType;

  // This necessarily violates the encapsulation of GuardedAllocator in order
  // to precisely simulate overflows of a single byte. This should match
  // `sizeof(suffix_offset)` and `sizeof(Suffix)` for the `FramingAllocator`,
  // which for `GuardedAllocator` are both `size_t`.
  static constexpr size_t kGuardValueSize = sizeof(size_t);

  GuardedAllocatorTestBase()
      : buffer_{}, allocator_(buffer_), guarded_(allocator_), core_(guarded_) {
    GuardErrorRecorder::Reset();
  }

  // Another encapsulation violation. This gets the block from the data pointer.
  const BlockType* GetBlock(const void* data) {
    auto addr = reinterpret_cast<uintptr_t>(data) - (sizeof(size_t) * 2);
    const auto* frame = reinterpret_cast<const void*>(addr);
    return BlockType::FromUsableSpace(frame);
  }

  void SetMode(Mode mode) { core_.SetMode(mode); }

  GuardedAllocatorType& GetAllocator() override { return guarded_; }

  BackgroundThreadCore& GetCore() override { return core_; }

  void CheckValid() { EXPECT_EQ(core_.GetInvalid(), nullptr); }

  // Unit tests

  void TestValidateAllAfterInit() {
    core_.SetMode(Mode::kValidateAll);
    core_.RunOnce();
    CheckValid();
  }

  void TestValidateAllAfterAllocation() {
    auto bytes = guarded_.template MakeUnique<uint8_t[]>(64);
    ASSERT_NE(bytes, nullptr);

    core_.SetMode(Mode::kValidateAll);
    core_.RunOnce();
    CheckValid();
  }

  void TestDetectHeapUnderrunOnDeallocate() {
    constexpr size_t kDataSize = 64;
    auto bytes = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    uint8_t* data = bytes.get();
    ASSERT_NE(data, nullptr);

    // Modify each bit of each byte of the suffix offset.
    for (size_t i = 0; i < kGuardValueSize; ++i) {
      for (size_t j = 0; j < 8; ++j) {
        GuardErrorRecorder::Reset();
        *(data - (i + 1)) ^= static_cast<uint8_t>(1 << j);
        guarded_.Deallocate(data);
        EXPECT_TRUE(GuardErrorRecorder::error.has_value());
        *(data - (i + 1)) ^= static_cast<uint8_t>(1 << j);
      }
    }

    GuardErrorRecorder::Reset();
    bytes.Reset();
    EXPECT_FALSE(GuardErrorRecorder::error.has_value());
  }

  void TestDetectHeapOverrunOnDeallocate() {
    constexpr size_t kDataSize = 64;
    auto bytes = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    uint8_t* data = bytes.get();
    ASSERT_NE(data, nullptr);

    // Modify each bit of each byte of the suffix.
    for (size_t i = 0; i < kGuardValueSize; ++i) {
      for (size_t j = 0; j < 8; ++j) {
        GuardErrorRecorder::Reset();
        *(data + kDataSize + i) ^= static_cast<uint8_t>(1 << j);
        guarded_.Deallocate(data);
        EXPECT_EQ(GuardErrorRecorder::error, GuardError(GuardError::kBadMagic));
        *(data + kDataSize + i) ^= static_cast<uint8_t>(1 << j);
      }
    }

    GuardErrorRecorder::Reset();
    bytes.Reset();
    EXPECT_FALSE(GuardErrorRecorder::error.has_value());
  }

  void TestDetectHeapUnderrunOnResize() {
    constexpr size_t kDataSize = 64;
    auto bytes = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    uint8_t* data = bytes.get();
    ASSERT_NE(data, nullptr);

    // Modify each bit of each byte of the suffix offset.
    for (size_t i = 0; i < kGuardValueSize; ++i) {
      for (size_t j = 0; j < 8; ++j) {
        GuardErrorRecorder::Reset();
        *(data - (i + 1)) ^= static_cast<uint8_t>(1 << j);
        EXPECT_FALSE(guarded_.Resize(data, kDataSize * 2));
        EXPECT_TRUE(GuardErrorRecorder::error.has_value());
        *(data - (i + 1)) ^= static_cast<uint8_t>(1 << j);
      }
    }

    GuardErrorRecorder::Reset();
    EXPECT_TRUE(guarded_.Resize(data, kDataSize * 2));
    EXPECT_FALSE(GuardErrorRecorder::error.has_value());
  }

  void TestDetectHeapOverrunOnResize() {
    constexpr size_t kDataSize = 64;
    auto bytes = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    uint8_t* data = bytes.get();
    ASSERT_NE(data, nullptr);

    // Modify each bit of each byte of the suffix.
    for (size_t i = 0; i < kGuardValueSize; ++i) {
      for (size_t j = 0; j < 8; ++j) {
        GuardErrorRecorder::Reset();
        *(data + kDataSize + i) ^= static_cast<uint8_t>(1 << j);
        EXPECT_FALSE(guarded_.Resize(data, kDataSize * 2));
        EXPECT_EQ(GuardErrorRecorder::error, GuardError(GuardError::kBadMagic));
        *(data + kDataSize + i) ^= static_cast<uint8_t>(1 << j);
      }
    }

    GuardErrorRecorder::Reset();
    EXPECT_TRUE(guarded_.Resize(data, kDataSize * 2));
    EXPECT_FALSE(GuardErrorRecorder::error.has_value());
  }

  void TestDetectHeapUnderrunOnReallocate() {
    constexpr size_t kDataSize = 64;
    constexpr Layout kNewLayout(kDataSize * 2, 1);
    auto bytes = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    uint8_t* data = bytes.get();
    ASSERT_NE(data, nullptr);

    // Modify each bit of each byte of the suffix offset.
    for (size_t i = 0; i < kGuardValueSize; ++i) {
      for (size_t j = 0; j < 8; ++j) {
        GuardErrorRecorder::Reset();
        *(data - (i + 1)) ^= static_cast<uint8_t>(1 << j);
        EXPECT_EQ(guarded_.Reallocate(data, kNewLayout), nullptr);
        EXPECT_TRUE(GuardErrorRecorder::error.has_value());
        *(data - (i + 1)) ^= static_cast<uint8_t>(1 << j);
      }
    }

    GuardErrorRecorder::Reset();
    void* new_data = guarded_.Reallocate(bytes.Release(), kNewLayout);
    EXPECT_NE(new_data, nullptr);
    EXPECT_FALSE(GuardErrorRecorder::error.has_value());
    guarded_.Deallocate(new_data);
  }

  void TestDetectHeapOverrunOnReallocate() {
    constexpr size_t kDataSize = 64;
    constexpr Layout kNewLayout(kDataSize * 2, 1);
    auto bytes = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    uint8_t* data = bytes.get();
    ASSERT_NE(data, nullptr);

    // Modify each bit of each byte of the suffix.
    for (size_t i = 0; i < kGuardValueSize; ++i) {
      for (size_t j = 0; j < 8; ++j) {
        GuardErrorRecorder::Reset();
        *(data + kDataSize + i) ^= static_cast<uint8_t>(1 << j);
        EXPECT_EQ(guarded_.Reallocate(data, kNewLayout), nullptr);
        EXPECT_EQ(GuardErrorRecorder::error, GuardError(GuardError::kBadMagic));
        *(data + kDataSize + i) ^= static_cast<uint8_t>(1 << j);
      }
    }

    GuardErrorRecorder::Reset();
    void* new_data = guarded_.Reallocate(bytes.Release(), kNewLayout);
    EXPECT_NE(new_data, nullptr);
    EXPECT_FALSE(GuardErrorRecorder::error.has_value());
    guarded_.Deallocate(new_data);
  }

  void TestDetectHeapUnderrun() {
    constexpr size_t kDataSize = 64;
    auto bytes1 = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    auto bytes2 = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    auto bytes3 = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    uint8_t* data = bytes2.get();
    ASSERT_NE(data, nullptr);

    // Modify each bit of each byte of the suffix offset.
    const BlockType* block = GetBlock(data);
    for (size_t i = 0; i < kGuardValueSize; ++i) {
      for (size_t j = 0; j < 8; ++j) {
        Background background(core_);

        // Modify a bit of the suffix offset.
        core_.Corrupt(data - (i + 1), 1 << j);

        core_.Await();
        EXPECT_EQ(core_.GetInvalid(), block);
        core_.Restore();
      }
    }
  }

  void TestDetectHeapOverrun() {
    constexpr size_t kDataSize = 64;
    auto bytes1 = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    auto bytes2 = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    auto bytes3 = guarded_.template MakeUnique<uint8_t[]>(kDataSize);
    uint8_t* data = bytes2.get();
    ASSERT_NE(data, nullptr);

    // Modify each bit of each byte of the suffix.
    const BlockType* block = GetBlock(data);
    for (size_t i = 0; i < kGuardValueSize; ++i) {
      for (size_t j = 0; j < 8; ++j) {
        Background background(core_);

        core_.Corrupt(data + kDataSize + i, 1 << j);

        core_.Await();
        EXPECT_EQ(core_.GetInvalid(), block);
        core_.Restore();
      }
    }
  }

 private:
  alignas(BlockType::kAlignment) std::array<std::byte, kCapacity> buffer_;
  BlockAllocator allocator_;
  GuardedAllocatorType guarded_;
  GuardedAllocatorTestThreadCore<GuardedAllocatorType> core_;
};

using GuardedAllocatorInterruptSpinLockTest =
    GuardedAllocatorTestBase<::pw::sync::InterruptSpinLock>;
using GuardedAllocatorMutexTest = GuardedAllocatorTestBase<::pw::sync::Mutex>;
using GuardedAllocatorNoLockTest = GuardedAllocatorTestBase<::pw::sync::NoLock>;

// Unit tests.

TEST_F(GuardedAllocatorInterruptSpinLockTest, GetCapacity) {
  TestGetCapacity(kCapacity);
  CheckValid();
}

TEST_F(GuardedAllocatorMutexTest, GetCapacity) {
  TestGetCapacity(kCapacity);
  CheckValid();
}

TEST_F(GuardedAllocatorInterruptSpinLockTest, AllocateDeallocate) {
  TestAllocate();
  CheckValid();
}

TEST_F(GuardedAllocatorMutexTest, AllocateDeallocate) {
  TestAllocate();
  CheckValid();
}

TEST_F(GuardedAllocatorNoLockTest, DetectHeapUnderrun_Deallocate) {
  TestDetectHeapUnderrunOnDeallocate();
}

TEST_F(GuardedAllocatorNoLockTest, DetectHeapOverrun_Deallocate) {
  TestDetectHeapOverrunOnDeallocate();
}

TEST_F(GuardedAllocatorInterruptSpinLockTest, Resize) {
  TestResize();
  CheckValid();
}

TEST_F(GuardedAllocatorMutexTest, Resize) {
  TestResize();
  CheckValid();
}

TEST_F(GuardedAllocatorNoLockTest, DetectHeapUnderrun_Resize) {
  TestDetectHeapUnderrunOnResize();
}

TEST_F(GuardedAllocatorNoLockTest, DetectHeapOverrun_Resize) {
  TestDetectHeapOverrunOnResize();
}

TEST_F(GuardedAllocatorInterruptSpinLockTest, Reallocate) {
  TestReallocate();
  CheckValid();
}

TEST_F(GuardedAllocatorMutexTest, Reallocate) {
  TestReallocate();
  CheckValid();
}

TEST_F(GuardedAllocatorNoLockTest, DetectHeapUnderrun_Reallocate) {
  TestDetectHeapUnderrunOnReallocate();
}

TEST_F(GuardedAllocatorNoLockTest, DetectHeapOverrun_Reallocate) {
  TestDetectHeapOverrunOnReallocate();
}

TEST_F(GuardedAllocatorNoLockTest, ValidateAllAfterInit) {
  TestValidateAllAfterInit();
}

TEST_F(GuardedAllocatorNoLockTest, ValidateAllAfterAllocation) {
  TestValidateAllAfterAllocation();
}

TEST_F(GuardedAllocatorInterruptSpinLockTest, DetectHeapUnderrun_ValidateOne) {
  SetMode(Mode::kValidateOne);
  TestDetectHeapUnderrun();
}

TEST_F(GuardedAllocatorMutexTest, DetectHeapUnderrun_ValidateOne) {
  SetMode(Mode::kValidateOne);
  TestDetectHeapUnderrun();
}

TEST_F(GuardedAllocatorInterruptSpinLockTest, DetectHeapUnderrun_ValidateAll) {
  SetMode(Mode::kValidateAll);
  TestDetectHeapUnderrun();
}

TEST_F(GuardedAllocatorMutexTest, DetectHeapUnderrun_ValidateAll) {
  SetMode(Mode::kValidateAll);
  TestDetectHeapUnderrun();
}

TEST_F(GuardedAllocatorInterruptSpinLockTest, DetectHeapOverrun_ValidateOne) {
  SetMode(Mode::kValidateOne);
  TestDetectHeapOverrun();
}

TEST_F(GuardedAllocatorMutexTest, DetectHeapOverrun_ValidateOne) {
  SetMode(Mode::kValidateOne);
  TestDetectHeapOverrun();
}

TEST_F(GuardedAllocatorInterruptSpinLockTest, DetectHeapOverrun_ValidateAll) {
  SetMode(Mode::kValidateAll);
  TestDetectHeapOverrun();
}

TEST_F(GuardedAllocatorMutexTest, DetectHeapOverrun_ValidateAll) {
  SetMode(Mode::kValidateAll);
  TestDetectHeapOverrun();
}

}  // namespace

#endif  // PW_THREAD_JOINING_ENABLED
