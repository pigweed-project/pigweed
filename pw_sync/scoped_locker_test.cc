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

#include "pw_sync/scoped_locker.h"

#include <mutex>

#include "pw_sync/test/lock_testing.h"
#include "pw_unit_test/framework.h"

using pw::ScopedLocker;
using pw::sync::test::FakeBasicLockable;

namespace {

TEST(ScopedLockerTest, AcquireConstructionAndLockedDestruction) {
  FakeBasicLockable lock;
  ASSERT_FALSE(lock.locked());
  {
    ScopedLocker locker(lock);
    EXPECT_TRUE(lock.locked());
  }
  EXPECT_FALSE(lock.locked());
}

TEST(ScopedLockerTest, AdoptConstructionAndUnlockedDestruction) {
  FakeBasicLockable lock;
  ASSERT_FALSE(lock.locked());
  {
    lock.lock();
    ScopedLocker locker(lock, std::adopt_lock);
    EXPECT_TRUE(lock.locked());
  }
  EXPECT_FALSE(lock.locked());
}

TEST(ScopedLockerTest, AdoptConstructionWithTryLock) {
  pw::sync::test::FakeLockable lock;
  ASSERT_FALSE(lock.locked());
  if (lock.try_lock()) {
    ScopedLocker locker(lock, std::adopt_lock);
    EXPECT_TRUE(lock.locked());
  }
  EXPECT_FALSE(lock.locked());
}

TEST(ScopedLockerTest, DeferConstructionAndUnlockedDestruction) {
  FakeBasicLockable lock;
  ASSERT_FALSE(lock.locked());
  {
    ScopedLocker locker(lock, std::defer_lock);
    EXPECT_FALSE(lock.locked());
  }
  EXPECT_FALSE(lock.locked());
}

TEST(ScopedLockerTest, LockAndDestruction) {
  FakeBasicLockable lock;
  ASSERT_FALSE(lock.locked());
  {
    ScopedLocker locker(lock, std::defer_lock);
    EXPECT_FALSE(lock.locked());
    locker.lock();
    EXPECT_TRUE(lock.locked());
  }
  EXPECT_FALSE(lock.locked());
}

TEST(ScopedLockerTest, UnlockAndDestruction) {
  FakeBasicLockable lock;
  ASSERT_FALSE(lock.locked());
  {
    ScopedLocker locker(lock);
    EXPECT_TRUE(lock.locked());
    locker.unlock();
    EXPECT_FALSE(lock.locked());
  }
  EXPECT_FALSE(lock.locked());
}

// Class used to verify lock analysis annotations with ScopedLocker.
class AnnotatedGuardedData {
 public:
  void SetValueDirect(int value) PW_LOCKS_EXCLUDED(lock_) {
    ScopedLocker locker(lock_);
    value_ = value;
  }

  int GetValue() PW_LOCKS_EXCLUDED(lock_) {
    ScopedLocker locker(lock_);
    return value_;
  }

  void SetValueDeferredThenLocked(int value) PW_LOCKS_EXCLUDED(lock_) {
    ScopedLocker locker(lock_, std::defer_lock);
    locker.lock();
    value_ = value;
  }

  bool SetValueOpenCoded(int value, bool may_block) PW_LOCKS_EXCLUDED(lock_) {
    if (may_block) {
      lock_.lock();
    } else if (!lock_.try_lock()) {
      return false;
    }
    ScopedLocker locker(lock_, std::adopt_lock);
    value_ = value;
    return true;
  }

 private:
  pw::sync::test::FakeLockable lock_;
  pw::sync::test::FakeLockable other_lock_;
  int value_ PW_GUARDED_BY(lock_) = 0;
};

TEST(ScopedLockerTest, LockAnalysisAnnotations) {
  AnnotatedGuardedData data;
  data.SetValueDirect(42);
  EXPECT_EQ(data.GetValue(), 42);

  data.SetValueDeferredThenLocked(500);
  EXPECT_EQ(data.GetValue(), 500);

  EXPECT_TRUE(data.SetValueOpenCoded(600, /*may_block=*/true));
  EXPECT_EQ(data.GetValue(), 600);

  EXPECT_TRUE(data.SetValueOpenCoded(700, /*may_block=*/false));
  EXPECT_EQ(data.GetValue(), 700);
}

}  // namespace
