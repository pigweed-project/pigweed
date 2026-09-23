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

#include "pw_sync/shared_mutex.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <numeric>

#include "pw_sync/scoped_locker.h"
#include "pw_thread/sleep.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_thread/yield.h"
#include "pw_unit_test/framework.h"

using pw::sync::SharedMutex;
using SharedLockable = pw::sync::SharedLockable<SharedMutex>;
using ExclusiveLockable = pw::sync::ExclusiveLockable<SharedMutex>;

namespace {

std::array<pw::thread::test::TestThreadContext, 2> reader_thread_contexts;
std::array<pw::thread::test::TestThreadContext, 1> writer_thread_contexts;

// SharedMutex Tests ==========================================================

// lock() and unlock() should succeed without changing reader count
TEST(SharedMutexTest, LockUnlock) {
  SharedMutex lock;
  EXPECT_EQ(lock.reader_count(), 0U);
  lock.lock();
  EXPECT_EQ(lock.reader_count(), 0U);
  lock.unlock();
  EXPECT_EQ(lock.reader_count(), 0U);
}

// lock_exclusive() and unlock_exclusive() should succeed without changing
// reader count
TEST(SharedMutexTest, WriteLockUnlock) {
  SharedMutex lock;
  EXPECT_EQ(lock.reader_count(), 0U);
  lock.lock_exclusive();
  EXPECT_EQ(lock.reader_count(), 0U);
  lock.unlock_exclusive();
  EXPECT_EQ(lock.reader_count(), 0U);
}

// lock_shared() and unlock_shared() should succeed and increment reader count
TEST(SharedMutexTest, ReadLockUnlock) {
  SharedMutex lock;
  EXPECT_EQ(lock.reader_count(), 0U);
  lock.lock_shared();
  EXPECT_EQ(lock.reader_count(), 1U);
  lock.unlock_shared();
  EXPECT_EQ(lock.reader_count(), 0U);
}

#if PW_SYNC_SHARED_MUTEX_ENABLE_METRICS
// max_concurrent_readers() should track peak concurrent readers and not
// decrease
TEST(SharedMutexTest, MaxConcurrentReaders) {
  SharedMutex lock;
  EXPECT_EQ(lock.max_concurrent_readers(), 0U);
  EXPECT_EQ(lock.max_concurrent_readers_metric().value(), 0U);
  lock.lock_shared();
  EXPECT_EQ(lock.max_concurrent_readers(), 1U);
  EXPECT_EQ(lock.max_concurrent_readers_metric().value(), 1U);
  {
    SharedLockable shared_lockable(&lock);
    shared_lockable.lock();
    EXPECT_EQ(lock.max_concurrent_readers(), 2U);
    EXPECT_EQ(lock.max_concurrent_readers_metric().value(), 2U);
    shared_lockable.unlock();
  }
  EXPECT_EQ(lock.max_concurrent_readers(), 2U);
  EXPECT_EQ(lock.reader_count(), 1U);
  lock.unlock_shared();
  EXPECT_EQ(lock.max_concurrent_readers(), 2U);
  EXPECT_EQ(lock.reader_count(), 0U);
}
#endif  // PW_SYNC_SHARED_MUTEX_ENABLE_METRICS

// try_lock() should succeed if not already locked
TEST(SharedMutexTest, TryLockUnlock) {
  SharedMutex lock;
  auto is_locked = lock.try_lock();
  EXPECT_TRUE(is_locked);
  // Make Thread Safety Analysis happy
  if (is_locked) {
    lock.unlock();
  }
}

// try_lock() should fail if already locked
TEST(SharedMutexTest, LockTryLockUnlock) {
  SharedMutex lock;
  lock.lock();
  EXPECT_FALSE(lock.try_lock());
  lock.unlock();
}

// try_lock() should fail if already read locked
TEST(SharedMutexTest, ReadLockTryLockUnlock) {
  SharedMutex lock;
  lock.lock_shared();
  EXPECT_FALSE(lock.try_lock());
  lock.unlock_shared();
}

// try_lock_shared() should succeed if not already locked
TEST(SharedMutexTest, TryReadLockUnlock) {
  SharedMutex lock;
  auto is_locked = lock.try_lock_shared();
  EXPECT_TRUE(is_locked);
  // Make Thread Safety Analysis happy
  if (is_locked) {
    lock.unlock_shared();
  }
}

// try_lock_shared() should fail if already locked
TEST(SharedMutexTest, LockTryReadLockUnlock) {
  SharedMutex lock;
  lock.lock();
  EXPECT_FALSE(lock.try_lock_shared());
  lock.unlock();
}

// try_lock_shared() should succeed if already read locked
// Note: We have to do some gymnastics with threads to keep the Thread Safety
// Analysis happy -- otherwise, it complains about double locking.
TEST(SharedMutexTest, ReadLockTryReadLockUnlock) {
  SharedMutex lock;

  auto readlock_func = [&lock]() {
    // Try to lock the reader again, should successfully share the read lock
    auto is_locked = lock.try_lock_shared();
    EXPECT_TRUE(is_locked);
    // Make Thread Safety Analysis happy
    if (is_locked) {
      lock.unlock_shared();
    }
  };

  // Lock the first reader before starting the thread
  lock.lock_shared();

  pw::Thread thread(reader_thread_contexts[0].options(), readlock_func);

  // Wait for second reader to be locked
  thread.join();

  // Unlock the first reader
  lock.unlock_shared();
}

// SharedLockable Tests =======================================================

// lock() should succeed and increment the reader count
TEST(SharedLockableTest, LockUnlock) {
  SharedMutex lock;
  SharedLockable shared_lockable(&lock);
  EXPECT_EQ(lock.reader_count(), 0U);
  shared_lockable.lock();
  EXPECT_EQ(lock.reader_count(), 1U);
  shared_lockable.unlock();
  EXPECT_EQ(lock.reader_count(), 0U);
}

// lock()'ing multiple shared lockables should succeed and increment the reader
// count each time
TEST(SharedLockableTest, MultipleLockUnlock) {
  SharedMutex lock;
  SharedLockable shared_lockable1(&lock);
  SharedLockable shared_lockable2(&lock);
  EXPECT_EQ(lock.reader_count(), 0U);
  shared_lockable1.lock();
  EXPECT_EQ(lock.reader_count(), 1U);
  shared_lockable2.lock();
  EXPECT_EQ(lock.reader_count(), 2U);
  shared_lockable2.unlock();
  EXPECT_EQ(lock.reader_count(), 1U);
  shared_lockable1.unlock();
  EXPECT_EQ(lock.reader_count(), 0U);
}

// lock()'ing multiple shared lockables and unlocking them out of order should
// succeed and increment/decrement the reader count each time
TEST(SharedLockableTest, MultipleLockUnlockOutOfOrder) {
  SharedMutex lock;
  SharedLockable shared_lockable1(&lock);
  SharedLockable shared_lockable2(&lock);
  SharedLockable shared_lockable3(&lock);
  EXPECT_EQ(lock.reader_count(), 0U);
  shared_lockable1.lock();
  EXPECT_EQ(lock.reader_count(), 1U);
  shared_lockable2.lock();
  EXPECT_EQ(lock.reader_count(), 2U);
  shared_lockable1.unlock();
  EXPECT_EQ(lock.reader_count(), 1U);
  shared_lockable3.lock();
  EXPECT_EQ(lock.reader_count(), 2U);
  shared_lockable2.unlock();
  EXPECT_EQ(lock.reader_count(), 1U);
  shared_lockable3.unlock();
  EXPECT_EQ(lock.reader_count(), 0U);
}

TEST(SharedLockableTest, ScopedLocking) {
  SharedMutex lock;
  SharedLockable shared_lockable(&lock);

  EXPECT_EQ(lock.reader_count(), 0U);
  {
    pw::ScopedLocker l(shared_lockable);
    EXPECT_EQ(lock.reader_count(), 1U);
  }
  EXPECT_EQ(lock.reader_count(), 0U);
}

// A single SharedLockable instance should be usable across multiple threads
// concurrently
TEST(SharedLockableTest, SingleSharedLockableMultipleThreads) {
  SharedMutex lock;
  SharedLockable shared_lockable(&lock);

  auto read_func = [&shared_lockable]() {
    shared_lockable.lock();
    shared_lockable.unlock();
  };

  pw::Thread thread1(reader_thread_contexts[0].options(), read_func);
  pw::Thread thread2(reader_thread_contexts[1].options(), read_func);

  thread1.join();
  thread2.join();
  EXPECT_EQ(lock.reader_count(), 0U);
}

// ExclusiveLockable Tests ====================================================

// lock() should succeed and not change the reader count
TEST(ExclusiveLockableTest, LockUnlock) {
  SharedMutex lock;
  ExclusiveLockable exclusive_lockable(&lock);
  EXPECT_EQ(lock.reader_count(), 0U);
  exclusive_lockable.lock();
  EXPECT_EQ(lock.reader_count(), 0U);
  exclusive_lockable.unlock();
  EXPECT_EQ(lock.reader_count(), 0U);
}

TEST(ExclusiveLockableTest, ScopedLocking) {
  SharedMutex lock;
  ExclusiveLockable exclusive_lockable(&lock);

  EXPECT_EQ(lock.reader_count(), 0U);
  {
    pw::ScopedLocker l{exclusive_lockable};
    EXPECT_EQ(lock.reader_count(), 0U);
    auto is_locked = lock.try_lock();
    EXPECT_FALSE(is_locked);
    if (is_locked) {
      lock.unlock();
    }
    auto is_read_locked = lock.try_lock_shared();
    EXPECT_FALSE(is_read_locked);
    if (is_read_locked) {
      lock.unlock_shared();
    }
  }
  EXPECT_EQ(lock.reader_count(), 0U);
  auto is_locked = lock.try_lock();
  EXPECT_TRUE(is_locked);
  if (is_locked) {
    lock.unlock();
  }
}

// SharedMutex Functional Tests ===============================================

// Multiple concurrent reader locks can be taken
TEST(SharedMutexFunctionalTest, ConcurrentReaders) {
  struct Context {
    SharedMutex lock;
    std::atomic<bool> finished = false;
  } cx;
  Context* pcx = &cx;

  auto reader_func = [pcx]() {
    SharedLockable shared_lockable(&pcx->lock);
    while (!pcx->finished.load()) {
      pw::ScopedLocker l(shared_lockable);
      pw::this_thread::yield();
    }
  };

  EXPECT_EQ(cx.lock.reader_count(), 0U);
  pw::Thread thread1(reader_thread_contexts[0].options(), reader_func);
  pw::Thread thread2(reader_thread_contexts[1].options(), reader_func);

  pw::this_thread::sleep_for(
      pw::chrono::SystemClock::for_at_least(std::chrono::milliseconds(100)));

  cx.finished = true;
  thread1.join();
  thread2.join();
  EXPECT_EQ(cx.lock.reader_count(), 0U);
}

// Multiple readers and a writer should be able to lock and unlock
// without issues.
TEST(SharedMutexFunctionalTest, ConcurrentReadersAndWriter) {
  struct Context {
    SharedMutex lock;
    std::atomic<bool> finished = false;
  } cx;
  Context* pcx = &cx;

  auto reader_func = [pcx]() {
    SharedLockable shared_lockable(&pcx->lock);

    while (!pcx->finished.load()) {
      pw::ScopedLocker l(shared_lockable);
      pw::this_thread::yield();
    }
  };

  auto writer_func = [pcx]() {
    ExclusiveLockable exclusive_lockable(&pcx->lock);

    while (!pcx->finished.load()) {
      pw::ScopedLocker l(exclusive_lockable);
      pw::this_thread::yield();
    }
  };

  EXPECT_EQ(cx.lock.reader_count(), 0U);
  pw::Thread reader_thread1(reader_thread_contexts[0].options(), reader_func);
  pw::Thread reader_thread2(reader_thread_contexts[1].options(), reader_func);
  pw::Thread writer_thread(writer_thread_contexts[0].options(), writer_func);

  pw::this_thread::sleep_for(
      pw::chrono::SystemClock::for_at_least(std::chrono::milliseconds(100)));

  cx.finished = true;
  reader_thread1.join();
  reader_thread2.join();
  writer_thread.join();
  EXPECT_EQ(cx.lock.reader_count(), 0U);
}

}  // namespace
