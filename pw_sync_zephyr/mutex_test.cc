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

#include "pw_sync/mutex.h"

#include <zephyr/kernel.h>

#include <mutex>

#include "pw_unit_test/framework.h"

namespace pw::sync {
namespace {

TEST(MutexZephyr, LockUnlock) {
  Mutex mutex;
  EXPECT_EQ(mutex.native_handle().lock_count, 0U);

  mutex.lock();
  EXPECT_EQ(mutex.native_handle().lock_count, 1U);

  mutex.unlock();
  EXPECT_EQ(mutex.native_handle().lock_count, 0U);
}

TEST(MutexZephyr, TryLockUnlock) {
  Mutex mutex;
  EXPECT_EQ(mutex.native_handle().lock_count, 0U);

  const bool locked = mutex.try_lock();
  EXPECT_TRUE(locked);
  EXPECT_EQ(mutex.native_handle().lock_count, 1U);

  mutex.unlock();
  EXPECT_EQ(mutex.native_handle().lock_count, 0U);
}

TEST(MutexZephyr, LockGuard) {
  Mutex mutex;
  EXPECT_EQ(mutex.native_handle().lock_count, 0U);

  {
    std::lock_guard lock(mutex);
    EXPECT_EQ(mutex.native_handle().lock_count, 1U);
  }

  EXPECT_EQ(mutex.native_handle().lock_count, 0U);
}

TEST(MutexZephyr, RepeatedLockUnlock) {
  Mutex mutex;
  for (int i = 0; i < 5; ++i) {
    mutex.lock();
    EXPECT_EQ(mutex.native_handle().lock_count, 1U);
    mutex.unlock();
    EXPECT_EQ(mutex.native_handle().lock_count, 0U);
  }
}

TEST(MutexZephyr, TryLockAfterUnlock) {
  Mutex mutex;
  mutex.lock();
  mutex.unlock();

  EXPECT_TRUE(mutex.try_lock());
  EXPECT_EQ(mutex.native_handle().lock_count, 1U);

  mutex.unlock();
  EXPECT_EQ(mutex.native_handle().lock_count, 0U);
}

TEST(MutexZephyr, NativeHandle) {
  Mutex mutex;
  struct k_mutex& handle = mutex.native_handle();
  EXPECT_EQ(handle.lock_count, 0U);

  mutex.lock();
  EXPECT_EQ(handle.lock_count, 1U);

  mutex.unlock();
  EXPECT_EQ(handle.lock_count, 0U);
}

Mutex static_mutex;

TEST(MutexZephyr, StaticMutex) {
  EXPECT_EQ(static_mutex.native_handle().lock_count, 0U);

  static_mutex.lock();
  EXPECT_EQ(static_mutex.native_handle().lock_count, 1U);

  static_mutex.unlock();
  EXPECT_EQ(static_mutex.native_handle().lock_count, 0U);
}

}  // namespace
}  // namespace pw::sync
