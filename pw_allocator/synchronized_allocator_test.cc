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

#include "pw_allocator/synchronized_allocator.h"

#include <cstddef>
#include <cstdint>
#include <limits>

#include "pw_allocator/sync_allocator_testing.h"
#include "pw_allocator/test_harness.h"
#include "pw_allocator/testing.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/mutex.h"
#include "pw_unit_test/framework.h"

// TODO: https://pwbug.dev/365161669 - Express joinability as a build-system
// constraint.
#if PW_THREAD_JOINING_ENABLED

namespace {

// Test fixtures.

static constexpr size_t kCapacity = 8192;
static constexpr size_t kMaxSize = 512;
static constexpr size_t kBackgroundRequests = 8;

using ::pw::allocator::SynchronizedAllocator;
using ::pw::allocator::test::BackgroundThreadCore;
using ::pw::allocator::test::SyncAllocatorTest;
using ::pw::allocator::test::TestHarness;
using AllocatorForTest = ::pw::allocator::test::AllocatorForTest<kCapacity>;

/// Thread body that uses a test harness to perform random sequences of
/// allocations on a synchronous allocator.
class SynchronizedAllocatorTestThreadCore : public BackgroundThreadCore {
 public:
  SynchronizedAllocatorTestThreadCore(pw::Allocator& allocator,
                                      uint64_t seed,
                                      size_t num_iterations)
      : num_iterations_(num_iterations) {
    test_harness_.set_allocator(&allocator);
    test_harness_.set_prng_seed(seed);
  }

 private:
  bool RunOnce() override {
    if (iteration_ >= num_iterations_) {
      iteration_ = 0;
      return false;
    }
    test_harness_.GenerateRequests(kMaxSize, kBackgroundRequests);
    iteration_++;
    return true;
  }

  TestHarness test_harness_;
  size_t iteration_ = 0;
  size_t num_iterations_;
};

/// Test fixture responsible for managing a synchronized allocator and a
/// background thread that accesses it concurrently with unit tests.
///
/// @tparam LockType  Synchronization type used by the allocator.
template <typename LockType>
class SynchronizedAllocatorTestBase : public SyncAllocatorTest {
 protected:
  SynchronizedAllocatorTestBase()
      : synchronized_(allocator_),
        core_(synchronized_, 1, std::numeric_limits<size_t>::max()) {}

  SynchronizedAllocator<LockType>& GetAllocator() override {
    return synchronized_;
  }

  BackgroundThreadCore& GetCore() override { return core_; }

 private:
  AllocatorForTest allocator_;
  SynchronizedAllocator<LockType> synchronized_;
  SynchronizedAllocatorTestThreadCore core_;
};

using SynchronizedAllocatorInterruptSpinLockTest =
    SynchronizedAllocatorTestBase<::pw::sync::InterruptSpinLock>;
using SynchronizedAllocatorMutexTest =
    SynchronizedAllocatorTestBase<::pw::sync::Mutex>;

// Unit tests.

TEST_F(SynchronizedAllocatorInterruptSpinLockTest, GetCapacity) {
  TestGetCapacity(kCapacity);
}

TEST_F(SynchronizedAllocatorMutexTest, GetCapacity) {
  TestGetCapacity(kCapacity);
}

TEST_F(SynchronizedAllocatorInterruptSpinLockTest,
       AllocateDeallocateInterrupt) {
  TestAllocate();
}

TEST_F(SynchronizedAllocatorMutexTest, AllocateDeallocate) { TestAllocate(); }

TEST_F(SynchronizedAllocatorInterruptSpinLockTest, ResizeInterrupt) {
  TestResize();
}

TEST_F(SynchronizedAllocatorMutexTest, Resize) { TestResize(); }

TEST_F(SynchronizedAllocatorInterruptSpinLockTest, ReallocateInterrupt) {
  TestReallocate();
}

TEST_F(SynchronizedAllocatorMutexTest, Reallocate) { TestReallocate(); }

template <typename LockType>
void TestGenerateRequests() {
  constexpr size_t kNumIterations = 1000;
  AllocatorForTest allocator;
  SynchronizedAllocator<LockType> synchronized(allocator);
  SynchronizedAllocatorTestThreadCore core1(synchronized, 1, kNumIterations);
  SynchronizedAllocatorTestThreadCore core2(synchronized, 2, kNumIterations);
  ::pw::allocator::test::Background background1(core1);
  ::pw::allocator::test::Background background2(core2);
  background1.Await();
  background2.Await();
}

TEST(SynchronizedAllocatorTest, GenerateRequestsInterruptSpinLock) {
  TestGenerateRequests<pw::sync::InterruptSpinLock>();
}

TEST(SynchronizedAllocatorTest, GenerateRequestsMutex) {
  TestGenerateRequests<pw::sync::Mutex>();
}

}  // namespace

#endif  // PW_THREAD_JOINING_ENABLED
