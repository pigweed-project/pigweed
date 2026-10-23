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

#include <array>
#include <cstdint>

#include "examples/named_u32.h"
#include "pw_allocator/allocator.h"
#include "pw_allocator/layout.h"
#include "pw_allocator/synchronized_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_thread/options.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_thread/thread_core.h"
#include "pw_unit_test/framework.h"

// TODO: https://pwbug.dev/365161669 - Express joinability as a build-system
// constraint.
#if PW_THREAD_JOINING_ENABLED

namespace examples {

/// Threaded task that performs several allocations.
class MyTask final {
 public:
  using Layout = pw::allocator::Layout;

  MyTask(pw::Allocator& allocator, const pw::thread::Options& options)
      : allocator_(allocator) {
    thread_ = pw::Thread(options, [this] { Run(); });
  }

  void join() { thread_.join(); }

 private:
  void Run() {
    std::array<NamedU32*, 10> values = {};
    uint32_t counter = 0;
    for (auto& value : values) {
      void* ptr = allocator_.Allocate(Layout::Of<NamedU32>());
      PW_CHECK_NOTNULL(ptr);
      value = new (ptr) NamedU32("test", ++counter);
    }
    counter = 0;
    for (auto& value : values) {
      PW_CHECK_INT_EQ(value->value(), ++counter);
      allocator_.Deallocate(value);
    }
  }

  pw::Allocator& allocator_;
  pw::Thread thread_;
};

// DOCSTAG: [pw_allocator-examples-spin_lock]
void RunTasks(pw::Allocator& allocator, const pw::thread::Options& options) {
  pw::allocator::SynchronizedAllocator<pw::sync::InterruptSpinLock> synced(
      allocator);
  MyTask task1(synced, options);
  MyTask task2(synced, options);
  task1.join();
  task2.join();
}
// DOCSTAG: [pw_allocator-examples-spin_lock]

}  // namespace examples

namespace {

using AllocatorForTest = ::pw::allocator::test::AllocatorForTest<2048>;

TEST(SpinLockExample, RunTasks) {
  // TODO: b/328831791 - This example did a nice job of uncovering some
  // pathological fragmentation by `Block::AllocFirst`. Reduce the size of this
  // allocator once that is addressed.
  AllocatorForTest allocator;
  pw::thread::test::TestThreadContext context;
  examples::RunTasks(allocator, context.options());
}

}  // namespace

#endif  // PW_THREAD_JOINING_ENABLED
