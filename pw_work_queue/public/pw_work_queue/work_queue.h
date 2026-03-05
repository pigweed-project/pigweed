// Copyright 2021 The Pigweed Authors
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

#include <cstdint>
#include <mutex>

#include "pw_assert/assert.h"
#include "pw_containers/inline_queue.h"
#include "pw_function/function.h"
#include "pw_metric/metric.h"
#include "pw_status/status.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/thread_core.h"

/// Work queue library for threads and interrupts
namespace pw::work_queue {

/// @module{pw_work_queue}

/// Enables threads and interrupts to enqueue work as a
/// `pw::work_queue::WorkItem` for execution by the work queue.
///
/// **Queue sizing**: The number of outstanding work requests is limited
/// based on the internal queue size. The queue size is set through either
/// the size of the `queue_storage` buffer passed into the constructor or by
/// using the templated `pw::work_queue::WorkQueueWithBuffer` helper. When the
/// queue is full, the queue will not accept further work.
///
/// **Cooperative thread cancellation**: The class is a
/// `pw::thread::ThreadCore`, meaning it should be executed as a single thread.
/// To facilitate clean shutdown, it provides a `RequestStop()` method for
/// cooperative cancellation which should be invoked before joining the thread.
/// Once a stop has been requested the queue will no longer accept further work.
///
/// The entire API is thread-safe and interrupt-safe.
template <typename WorkItem>
class CustomWorkQueue : public thread::ThreadCore {
 public:
  /// @param[in] queue The work entries to enqueue.
  ///
  /// @param[in] fn The function to invoke on each enqueued WorkItem
  ///
  /// @note The `ThreadNotification` prevents this from being `constexpr`.
  CustomWorkQueue(InlineQueue<WorkItem>& queue,
                  pw::Function<void(WorkItem&)>&& fn)
      : stop_requested_(false), queue_(queue), fn_(std::move(fn)) {
    min_queue_remaining_.Set(static_cast<uint32_t>(queue.capacity()));
  }

  /// Enqueues a `work_item` for execution by the work queue thread.
  ///
  /// @param[in] work_item The entry to enqueue.
  ///
  /// @returns
  /// * @OK: Entry was enqueued for execution.
  /// * @FAILED_PRECONDITION: The work queue is shutting down. Entries are no
  ///   longer permitted.
  /// * @RESOURCE_EXHAUSTED: Internal work queue is full. Entry was not
  ///   enqueued.
  Status PushWork(WorkItem&& work_item) PW_LOCKS_EXCLUDED(lock_) {
    return InternalPushWork(std::move(work_item));
  }

  /// Queues work for execution. Crashes if the work cannot be queued due to a
  /// full queue or a stopped worker thread.
  ///
  /// This call is recommended where possible since it saves error handling code
  /// at the callsite; and in many practical cases, it is a bug if the work
  /// queue is full (and so a crash is useful to detect the problem).
  ///
  /// @param[in] work_item The entry to enqueue.
  ///
  /// @pre
  /// * The queue must not overflow, i.e. be full.
  /// * The queue must not have been requested to stop, i.e. it must
  ///   not be in the process of shutting down.
  void CheckPushWork(WorkItem&& work_item) PW_LOCKS_EXCLUDED(lock_) {
    PW_ASSERT_OK(InternalPushWork(std::move(work_item)),
                 "Failed to push work item into the work queue");
  }
  void CheckPushWork(WorkItem& work_item) PW_LOCKS_EXCLUDED(lock_) {
    PW_ASSERT_OK(InternalPushWork(std::move(work_item)),
                 "Failed to push work item into the work queue");
  }

  /// Locks the queue to prevent further work enqueing, finishes outstanding
  /// work, then shuts down the worker thread.
  ///
  /// The `WorkQueue` cannot be resumed after stopping because the `ThreadCore`
  /// thread returns and may be joined. The `WorkQueue` must be reconstructed
  /// for re-use after the thread has been joined.
  void RequestStop() PW_LOCKS_EXCLUDED(lock_) {
    {
      std::lock_guard lock(lock_);
      stop_requested_ = true;
    }  // Release lock before calling .release() on the semaphore.
    work_notification_.release();
  }

  /// Removes all pending work from the queue.
  ///
  /// This method does not stop the work queue thread. Any work item currently
  /// being processed by the work queue thread will continue to execute.
  void Clear() PW_LOCKS_EXCLUDED(lock_) {
    std::lock_guard lock(lock_);
    queue_.clear();
  }

 private:
  void Run() override PW_LOCKS_EXCLUDED(lock_) {
    while (true) {
      work_notification_.acquire();

      // Drain the work queue.
      bool stop_requested;
      bool work_remaining;
      do {
        std::optional<WorkItem> possible_work_item;
        {
          std::lock_guard lock(lock_);
          if (!queue_.empty()) {
            possible_work_item.emplace(std::move(queue_.front()));
            queue_.pop();
          }
          work_remaining = !queue_.empty();
          stop_requested = stop_requested_;
        }
        if (!possible_work_item.has_value()) {
          continue;  // No work item to process.
        }
        WorkItem& work_item = possible_work_item.value();
        fn_(work_item);
      } while (work_remaining);

      // Queue was drained, return if we've been requested to stop.
      if (stop_requested) {
        return;
      }
    }
  }

  Status InternalPushWork(WorkItem&& work_item) PW_LOCKS_EXCLUDED(lock_) {
    {
      std::lock_guard lock(lock_);

      if (stop_requested_) {
        // Entries are not permitted to be enqueued once stop has been
        // requested.
        return Status::FailedPrecondition();
      }

      if (queue_.full()) {
        return Status::ResourceExhausted();
      }

      queue_.emplace(std::move(work_item));

      // Update the watermarks for the queue.
      const uint32_t queue_entries = queue_.size();
      if (queue_entries > max_queue_used_.value()) {
        max_queue_used_.Set(queue_entries);
      }
      const uint32_t queue_remaining = queue_.capacity() - queue_entries;
      if (queue_remaining < min_queue_remaining_.value()) {
        min_queue_remaining_.Set(queue_entries);
      }
    }  // Release lock before calling .release() on the semaphore.
    work_notification_.release();
    return OkStatus();
  }
  sync::InterruptSpinLock lock_;
  bool stop_requested_ PW_GUARDED_BY(lock_);
  InlineQueue<WorkItem>& queue_ PW_GUARDED_BY(lock_);
  sync::ThreadNotification work_notification_;
  pw::Function<void(WorkItem&)> fn_;

  // TODO(ewout): The group and/or its name token should be passed as a ctor
  // arg instead. Depending on the approach here the group should be exposed
  // While doing this evaluate whether perhaps we should instead construct
  // TypedMetric<uint32_t>s directly, avoiding the macro usage given the
  // min_queue_remaining_ initial value requires dependency injection.
  // And lastly when the restructure is finalized add unit tests to ensure these
  // metrics work as intended.
  PW_METRIC_GROUP(metrics_, "pw::work_queue::WorkQueue");
  PW_METRIC(metrics_, max_queue_used_, "max_queue_used", 0u);
  PW_METRIC(metrics_, min_queue_remaining_, "min_queue_remaining", 0u);
};

/// Creates a WorkQueue.
///
/// WorkQueue enqueues `pw_function::Closure` and the worker thread
/// invokes the Closure.
class WorkQueue : public CustomWorkQueue<Closure> {
 public:
  /// @param[in] queue The work entries to enqueue.
  WorkQueue(InlineQueue<Closure>& queue)
      : CustomWorkQueue(queue, [](Closure& fn) { fn(); }) {}
};

/// @}

namespace internal {

// Storage base class for the WorkQueueWithBuffer classes. The queue must be a
// base class instead of a member so the queue is initialized before it is
// passed to the CustomWorkQueue base.
template <typename WorkItem, size_t kWorkQueueEntries>
struct Storage {
  InlineQueue<WorkItem, kWorkQueueEntries> queue;
};

}  // namespace internal

/// @module{pw_work_queue}

/// Creates a WorkQueue and the backing queue.
///
/// @param kWorkQueueEntries The number of entries in the work queue
///
/// @param WorkItem The type that will enqueued.
template <size_t kWorkQueueEntries, typename WorkItem>
class CustomWorkQueueWithBuffer
    : private internal::Storage<WorkItem, kWorkQueueEntries>,
      public CustomWorkQueue<WorkItem> {
 public:
  /// @param[in] fn The function to invoke on each enqueued WorkItem
  constexpr CustomWorkQueueWithBuffer(pw::Function<void(WorkItem&)>&& fn)
      : CustomWorkQueue<WorkItem>(
            internal::Storage<WorkItem, kWorkQueueEntries>::queue,
            std::move(fn)) {}
};

/// Creates a WorkQueue and the backing queue.
///
/// WorkQueueWithBuffer enqueues `pw_function::Closure` and the worker thread
/// invokes the Closure.
///
/// @param kWorkQueueEntries The number of entries in the work queue (e.g. the
/// total number of work requests before the queue is full).
template <size_t kWorkQueueEntries>
class WorkQueueWithBuffer
    : private internal::Storage<Closure, kWorkQueueEntries>,
      public WorkQueue {
 public:
  constexpr WorkQueueWithBuffer()
      : WorkQueue(internal::Storage<Closure, kWorkQueueEntries>::queue) {}
};

}  // namespace pw::work_queue
