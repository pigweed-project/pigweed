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

#include "pw_allocator/sync_allocator_testing.h"

#include <cstdint>

#include "pw_containers/vector.h"
#include "pw_status/status_with_size.h"
#include "pw_thread/yield.h"

// TODO: https://pwbug.dev/365161669 - Express joinability as a build-system
// constraint.
#if PW_THREAD_JOINING_ENABLED

namespace pw::allocator::test {
namespace {

using ::pw::allocator::Layout;

static constexpr size_t kNumAllocations = 128;
static constexpr size_t kSize = 64;
static constexpr size_t kAlignment = 4;

/// Tracks allocation and exercises their usable space.
struct Allocation {
  void* ptr;
  Layout layout;

  /// Fills a valid allocation with a pattern of data.
  void Paint() {
    if (ptr != nullptr) {
      auto* u8 = static_cast<uint8_t*>(ptr);
      for (size_t i = 0; i < layout.size(); ++i) {
        u8[i] = static_cast<uint8_t>(i & 0xFF);
      }
    }
  }

  /// Checks that a valid allocation has been properly `Paint`ed. Returns the
  /// first index where the expected pattern doesn't match, e.g. where memory
  /// has been corrupted, or `layout.size()` if the memory is all correct.
  size_t Inspect() const {
    if (ptr != nullptr) {
      auto* u8 = static_cast<uint8_t*>(ptr);
      for (size_t i = 0; i < layout.size(); ++i) {
        if (u8[i] != (i & 0xFF)) {
          return i;
        }
      }
    }
    return layout.size();
  }
};

}  // namespace

// BackgroundThreadCore methods.

BackgroundThreadCore::~BackgroundThreadCore() { Stop(); }

void BackgroundThreadCore::Stop() { semaphore_.release(); }

void BackgroundThreadCore::Await() {
  semaphore_.acquire();
  semaphore_.release();
}

void BackgroundThreadCore::Run() {
  while (!semaphore_.try_acquire() && RunOnce()) {
    pw::this_thread::yield();
  }
  semaphore_.release();
}

void BackgroundThreadCore::Reset() { std::ignore = semaphore_.try_acquire(); }

// Background methods.

Background::Background(BackgroundThreadCore& core) : core_(core) {
  thread_ = pw::Thread(context_.options(), core_);
}

Background::~Background() {
  core_.Stop();
  Await();
  core_.Reset();
}

void Background::Await() {
  core_.Await();
  if (thread_.joinable()) {
    thread_.join();
  }
}

// SyncAllocatorTest unit test methods.

void SyncAllocatorTest::TestGetCapacity(size_t capacity) {
  pw::Allocator& allocator = GetAllocator();
  Background background(GetCore());

  pw::StatusWithSize actual = allocator.GetCapacity();
  EXPECT_EQ(actual.status(), pw::OkStatus());
  EXPECT_EQ(actual.size(), capacity);
}

void SyncAllocatorTest::TestAllocate() {
  pw::Allocator& allocator = GetAllocator();
  Background background(GetCore());

  pw::Vector<Allocation, kNumAllocations> allocations;
  while (!allocations.full()) {
    Layout layout(kSize, kAlignment);
    void* ptr = allocator.Allocate(layout);
    if (ptr == nullptr) {
      break;
    }
    Allocation allocation{ptr, layout};
    allocation.Paint();
    allocations.push_back(allocation);
    pw::this_thread::yield();
  }

  for (const auto& allocation : allocations) {
    EXPECT_EQ(allocation.Inspect(), allocation.layout.size());
    allocator.Deallocate(allocation.ptr);
  }
}

void SyncAllocatorTest::TestResize() {
  pw::Allocator& allocator = GetAllocator();
  Background background(GetCore());

  pw::Vector<Allocation, kNumAllocations> allocations;
  while (!allocations.full()) {
    Layout layout(kSize, kAlignment);
    void* ptr = allocator.Allocate(layout);
    if (ptr == nullptr) {
      break;
    }
    Allocation allocation{ptr, layout};
    allocation.Paint();
    allocations.push_back(allocation);
    pw::this_thread::yield();
  }

  // First, resize them smaller.
  for (auto& allocation : allocations) {
    EXPECT_EQ(allocation.Inspect(), allocation.layout.size());
    size_t new_size = allocation.layout.size() / 2;
    if (!allocator.Resize(allocation.ptr, new_size)) {
      continue;
    }
    allocation.layout = Layout(new_size, allocation.layout.alignment());
    allocation.Paint();
    pw::this_thread::yield();
  }

  // Then, resize them back to their original size.
  for (auto& allocation : allocations) {
    EXPECT_EQ(allocation.Inspect(), allocation.layout.size());
    size_t old_size = allocation.layout.size() * 2;
    if (!allocator.Resize(allocation.ptr, old_size)) {
      continue;
    }
    allocation.layout = Layout(old_size, allocation.layout.alignment());
    allocation.Paint();
    pw::this_thread::yield();
  }

  for (const auto& allocation : allocations) {
    EXPECT_EQ(allocation.Inspect(), allocation.layout.size());
    allocator.Deallocate(allocation.ptr);
  }
}

void SyncAllocatorTest::TestReallocate() {
  pw::Allocator& allocator = GetAllocator();
  Background background(GetCore());

  pw::Vector<Allocation, kNumAllocations> allocations;
  while (!allocations.full()) {
    Layout layout(kSize, kAlignment);
    void* ptr = allocator.Allocate(layout);
    if (ptr == nullptr) {
      break;
    }
    Allocation allocation{ptr, layout};
    allocation.Paint();
    allocations.push_back(allocation);
    pw::this_thread::yield();
  }

  for (auto& allocation : allocations) {
    EXPECT_EQ(allocation.Inspect(), allocation.layout.size());
    Layout new_layout = allocation.layout.Extend(1);
    void* new_ptr = allocator.Reallocate(allocation.ptr, new_layout);
    if (new_ptr == nullptr) {
      continue;
    }
    allocation.ptr = new_ptr;
    allocation.layout = new_layout;
    allocation.Paint();
    pw::this_thread::yield();
  }

  for (const auto& allocation : allocations) {
    EXPECT_EQ(allocation.Inspect(), allocation.layout.size());
    allocator.Deallocate(allocation.ptr);
  }
}

}  // namespace pw::allocator::test

#endif  // PW_THREAD_JOINING_ENABLED
