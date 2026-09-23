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

#include <limits>

#include "pw_sync/scoped_locker.h"
#include "pw_trace/trace.h"

namespace pw::sync {

SharedMutex::~SharedMutex() {
  PW_CHECK(reader_count() == 0, "One or more readers still hold the lock.");
}

void SharedMutex::lock_exclusive() {
  exclusive_lock_.lock();
  if (reader_count_.load(std::memory_order_acquire) > 0) {
    writer_waiting_.store(true, std::memory_order_seq_cst);
    while (reader_count_.load(std::memory_order_seq_cst) > 0) {
      writer_notification_.acquire();
    }
    writer_waiting_.store(false, std::memory_order_relaxed);
  }
}

[[nodiscard]] bool SharedMutex::try_lock_exclusive() {
  if (exclusive_lock_.try_lock()) {
    if (reader_count_.load(std::memory_order_acquire) == 0) {
      // No need to acquire the writer_notification_ since we aren't waiting on
      // any readers
      return true;
    }
    // At least one reader holds the lock, so we failed to acquire exclusive
    // access without waiting
    exclusive_lock_.unlock();
  }

  return false;
}

void SharedMutex::unlock_exclusive() { exclusive_lock_.unlock(); }

void SharedMutex::lock_shared() {
  exclusive_lock_.lock();
  auto concurrent_readers =
      reader_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  exclusive_lock_.unlock();
#if PW_SYNC_SHARED_MUTEX_ENABLE_METRICS
  if (concurrent_readers > max_concurrent_readers_.value()) {
    max_concurrent_readers_.Set(static_cast<uint32_t>(concurrent_readers));
  }
#endif  // PW_SYNC_SHARED_MUTEX_ENABLE_METRICS
  PW_TRACE_INSTANT_DATA("concurrent_readers",
                        "@pw_arg_counter",
                        &concurrent_readers,
                        sizeof(concurrent_readers));
}

[[nodiscard]] bool SharedMutex::try_lock_shared() {
  if (exclusive_lock_.try_lock()) {
    auto concurrent_readers =
        reader_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    exclusive_lock_.unlock();
#if PW_SYNC_SHARED_MUTEX_ENABLE_METRICS
    if (concurrent_readers > max_concurrent_readers_.value()) {
      max_concurrent_readers_.Set(static_cast<uint32_t>(concurrent_readers));
    }
#endif  // PW_SYNC_SHARED_MUTEX_ENABLE_METRICS
    PW_TRACE_INSTANT_DATA("concurrent_readers",
                          "@pw_arg_counter",
                          &concurrent_readers,
                          sizeof(concurrent_readers));
    return true;
  }

  return false;
}

void SharedMutex::unlock_shared() {
  auto remaining_readers = --reader_count_;
  PW_CHECK_INT_NE(remaining_readers,
                  std::numeric_limits<size_t>::max(),
                  "unlock_shared() called without a matching lock_shared()");

  PW_TRACE_INSTANT_DATA("concurrent_readers",
                        "@pw_arg_counter",
                        &remaining_readers,
                        sizeof(remaining_readers));
  if (remaining_readers == 0 &&
      writer_waiting_.load(std::memory_order_seq_cst)) {
    writer_notification_.release();
  }
}

}  // namespace pw::sync
