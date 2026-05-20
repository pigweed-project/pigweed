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
#pragma once

#include <cstddef>

#include "pw_allocator/allocator.h"
#include "pw_sync/binary_semaphore.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_thread/thread_core.h"
#include "pw_unit_test/framework.h"

// TODO: https://pwbug.dev/365161669 - Express joinability as a build-system
// constraint.
#if PW_THREAD_JOINING_ENABLED

namespace pw::allocator::test {

/// @submodule{pw_allocator,impl_test}

// Test fixtures.

/// Thread body that repeatedly performs some action during a test.
///
/// Unit tests for specific sync allocators should derive from this class and
/// provide the `RunOnce` method.
class BackgroundThreadCore : public ::pw::thread::ThreadCore {
 public:
  ~BackgroundThreadCore() override;

  /// Requests that the background thread stop.
  void Stop();

  /// Blocks until the background thread stops.
  void Await();

 private:
  void Run() override;

  // Performs the test action, and returns whether the thread should continue.
  virtual bool RunOnce() = 0;

  pw::sync::BinarySemaphore semaphore_;
};

/// Test fixture that manages a background allocation thread.
class Background {
 public:
  explicit Background(BackgroundThreadCore& core);
  ~Background();

  const BackgroundThreadCore& core() const { return core_; }

  /// Blocks until the background threads is finished.
  void Await();

 private:
  BackgroundThreadCore& core_;
  pw::thread::test::TestThreadContext context_;
  pw::Thread thread_;
};

/// Base class for unit tests of sync allocators.
///
/// Unit tests for specific sync allocators should derive from this class and
/// provide the `GetAllocator` and `GetCore` methods.
///
/// The provided tests manipulate dynamically allocated memory while a
/// background thread simultaneously exercises the allocator. Allocations,
/// queries and resizes may fail, but memory must not be corrupted and the test
/// must not deadlock.
class SyncAllocatorTest : public ::testing::Test {
 protected:
  /// Returns the allocator to be used in the unit tests.
  virtual pw::Allocator& GetAllocator() = 0;

  /// Returns the thread core to be used for the background thread.
  virtual BackgroundThreadCore& GetCore() = 0;

  void TestGetCapacity(size_t capacity);
  void TestAllocate();
  void TestResize();
  void TestReallocate();
};

/// @endsubmodule

}  // namespace pw::allocator::test

#endif  // PW_THREAD_JOINING_ENABLED
