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

#include "pw_async2/size_report/size_report.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>

#include "pw_allocator/allocator.h"
#include "pw_allocator/first_fit.h"
#include "pw_allocator/layout.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_preprocessor/compiler.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"

namespace pw::async2::size_report {
namespace {

sync::ThreadNotification notification;
sync::Mutex mutex;
sync::InterruptSpinLock isl;

[[maybe_unused]] int value = 0;

class Item : public IntrusiveForwardList<Item>::Item {
 public:
  constexpr Item(int v) : value(v) {}

  int value;
};

}  // namespace

PW_NO_INLINE std::optional<uint32_t> CheckMask(uint32_t mask) {
  if (mask == bloat::kDefaultMask) {
    return mask;
  }
  return std::nullopt;
}

int SetBaseline(uint32_t mask) {
  bloat::BloatThisBinary();

  Item one(1);
  Item two(2);
  Item three(2);
  IntrusiveForwardList<Item> items;
  items.push_front(two);
  items.push_front(one);
  items.pop_front();
  items.push_front(three);
  int item_count = static_cast<int>(std::distance(items.begin(), items.end()));

  {
    std::lock_guard lock(mutex);
    value |= mask;
  }

  if (mutex.try_lock()) {
    value &= ~mask;
    mutex.unlock();
  }

  {
    std::lock_guard lock(isl);
    value |= mask;
  }

  if (isl.try_lock()) {
    value &= ~mask;
    isl.unlock();
  }

  notification.acquire();
  value |= mask;
  notification.release();

  if (notification.try_acquire()) {
    notification.release();
  }

  std::optional<uint32_t> result = CheckMask(mask);
  if (result.has_value()) {
    return static_cast<int>(result.value());
  }

  void* ptr = GetAllocator().Allocate(allocator::Layout(32, 8));
  if (ptr != nullptr) {
    GetAllocator().Deallocate(ptr);
  }

  return item_count - 1;
}

Allocator& GetAllocator() {
  static std::array<std::byte, 1024> region;
  static allocator::FirstFitAllocator<> allocator(region);
  return allocator;
}

}  // namespace pw::async2::size_report
