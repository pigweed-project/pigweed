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

#include "pw_rpc2/size_report/size_report.h"

#include <array>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <optional>

#include "pw_allocator/first_fit.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_async2/basic_dispatcher.h"
#include "pw_bloat/bloat_this_binary.h"
#include "pw_containers/intrusive_forward_list.h"
#include "pw_preprocessor/compiler.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"

namespace pw::rpc2::size_report {
namespace {

sync::ThreadNotification notification;
sync::Mutex mutex;
sync::InterruptSpinLock isl;

[[maybe_unused]] int value = 0;

class Item : public IntrusiveForwardList<Item>::Item {
 public:
  constexpr explicit Item(int v) : value_(v) {}
  int value_;
};

}  // namespace

MockConnection::~MockConnection() = default;
MockListener::~MockListener() = default;

PW_NO_INLINE std::optional<uint32_t> CheckMask(uint32_t mask) {
  if (mask == bloat::kDefaultMask) {
    return mask;
  }
  return std::nullopt;
}

Allocator& GetAllocator() {
  static std::array<std::byte, 2048> region;
  static allocator::FirstFitAllocator<> allocator(region);
  return allocator;
}

uint32_t SetBaseline(uint32_t mask) {
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

  void* ptr = GetAllocator().Allocate(pw::allocator::Layout(32, 8));
  if (ptr != nullptr) {
    GetAllocator().Deallocate(ptr);
  }

  auto shared = GetAllocator().MakeShared<int>(42);
  if (shared != nullptr) {
    value |= mask;
  }

  async2::BasicDispatcher dispatcher;
  MockListener listener;
  auto accept_fut = listener.Accept();
  (void)accept_fut;
  MockConnection conn(GetAllocator());
  transport::ReliableSocket sock = MockListener::WrapSocket(conn);
  (void)sock;
  if (dispatcher.RunUntilStalled()) {
    value |= mask;
  }

  std::optional<uint32_t> result = CheckMask(mask);
  if (result.has_value()) {
    return result.value();
  }

  return static_cast<uint32_t>(item_count - 1);
}

}  // namespace pw::rpc2::size_report
