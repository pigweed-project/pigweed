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

// This is a stress test for pw_async2 dispatchers and tasks. Do NOT use this as
// an example for how to use pw_async2! Instead, visit
// https://pigweed.dev/pw_async2/ for pw_async2 examples and a codelab.
//
// This test focuses on the core pw_async2 dispatcher implementation:
// dispatcher, tasks and wakers. It uses NotifiedDispatcher.
//
// The test consists of N threads and one driver thread. Each of the N threads
// runs a dispatcher. The driver thread generates events that:
//
// - wake a task on one of the dispatchers, which in turn wakes other tasks,
// - allocate and post a task to a dispatcher,
// - deregister and free a task, or
// - destroy and recreate a thread's dispatcher.

#define PW_LOG_MODULE_NAME "pw_async2 test"
#define PW_LOG_LEVEL PW_LOG_LEVEL_INFO

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <random>
#include <utility>

#include "pw_allocator/libc_allocator.h"
#include "pw_allocator/shared_ptr.h"
#include "pw_assert/check.h"
#include "pw_async2/notified_dispatcher.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_async2/waker.h"
#include "pw_containers/dynamic_queue.h"
#include "pw_containers/dynamic_vector.h"
#include "pw_log/log.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_thread/yield.h"
#include "pw_unit_test/framework.h"

namespace {

constexpr bool kEnableVerboseLogging = false;

constexpr size_t kNumThreads = 4;
constexpr size_t kIterations = 20'000;
constexpr int kRandomSeed = 42;

class WakerTask;
class TestState;

enum class Event {
  kWakeRandomTask,
  kPostNewTask,
  kDeregisterRandomTask,
  kDestroyRandomDispatcher,
  kStopTest,
};

constexpr const char* EventToString(Event event) {
  switch (event) {
    case Event::kWakeRandomTask:
      return "kWakeRandomTask";
    case Event::kPostNewTask:
      return "kPostNewTask";
    case Event::kDeregisterRandomTask:
      return "kDeregisterRandomTask";
    case Event::kDestroyRandomDispatcher:
      return "kDestroyRandomDispatcher";
    case Event::kStopTest:
      return "kStopTest";
  }
}

// A task that runs a `Dispatcher` on a dedicated thread and executes queued
// `Event`s.
class EventHandler : public pw::async2::Task {
 public:
  // Constructs an event handler with the given ID.
  EventHandler(TestState& test_state, size_t id)
      : pw::async2::Task(PW_ASYNC_TASK_NAME("EventHandler")),
        test_state_(test_state),
        events_(allocator_),
        id_(id) {}

  // Starts the thread that runs the dispatcher.
  void Start() {
    thread_ = pw::Thread(context_.options(), [this] { Run(); });
  }

  // Queues an event to be executed by this handler and wakes it.
  void HandleEvent(Event type);

  // Notifies the thread to stop and waits for it to finish.
  void Halt();

 private:
  void Run();

  // Pulls an event from this handler's event queue and executes it.
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override;

  TestState& test_state_;
  pw::thread::test::TestThreadContext context_;
  pw::Thread thread_;
  pw::sync::ThreadNotification notification_;
  std::optional<pw::async2::NotifiedDispatcher> dispatcher_;
  pw::allocator::LibCAllocator allocator_;
  pw::sync::Mutex mutex_;
  pw::DynamicQueue<Event> events_ PW_GUARDED_BY(mutex_);
  const size_t id_;
  pw::async2::Waker waker_;
  bool terminate_ = false;
};

// Manages the shared state of the dispatcher stress test.
class TestState {
 public:
  // Initializes the test state, including the event handlers.
  TestState();

  void StartTest() {
    for (EventHandler& handler : handlers_) {
      handler.Start();
    }
  }

  // Signals all threads to stop and waits for them to terminate.
  void HaltTest() PW_LOCKS_EXCLUDED(mutex_);

  // Queues an event for a specific handler.
  void HandleEvent(Event type, size_t handler_id) PW_LOCKS_EXCLUDED(mutex_);

  void RecordEventHandled() {
    events_handled_.fetch_add(1, std::memory_order_relaxed);
  }

  unsigned EventsHandled() const {
    return events_handled_.load(std::memory_order_relaxed);
  }

  // Cleans up memory for waker tasks that have completed.
  void GarbageCollectTasks() PW_LOCKS_EXCLUDED(mutex_);

  // Called from any thread to randomly wake a pending `WakerTask`.
  void WakeRandomTask() PW_LOCKS_EXCLUDED(mutex_);

  // Used by an EventHandler to register a newly allocated `WakerTask`.
  // The dispatcher remains valid for this call since the EventHandler controls
  // its lifetime.
  void RegisterWakerTask(const pw::SharedPtr<WakerTask>& task)
      PW_LOCKS_EXCLUDED(mutex_);

  // Randomly selects a registered `WakerTask` and deregisters it.
  void DeregisterRandomWakerTask(pw::async2::Task& skip_this_task)
      PW_LOCKS_EXCLUDED(mutex_);

 private:
  std::array<EventHandler, kNumThreads> handlers_;

  pw::allocator::LibCAllocator allocator_;
  pw::sync::Mutex mutex_;
  std::mt19937 rng_ PW_GUARDED_BY(mutex_);
  pw::DynamicVector<pw::SharedPtr<WakerTask>, size_t> all_waker_tasks_
      PW_GUARDED_BY(mutex_);
  std::atomic<unsigned> events_handled_ = 0;
};

// A simple task that can be awoken, which in turn wakes other random tasks.
class WakerTask : public pw::async2::Task {
 public:
  explicit WakerTask(TestState& test_state)
      : pw::async2::Task(PW_ASYNC_TASK_NAME("WakerTask")),
        test_state_(test_state) {}

  // Advances the task, randomly waking another task and yielding.
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
    test_state_.WakeRandomTask();
    PW_ASYNC_STORE_WAKER(cx, waker_, "WakerTask");
    return pw::async2::Pending();
  }

  // Awakes this task so it will be polled again.
  void Wake() { waker_.Wake(); }

 private:
  TestState& test_state_;
  pw::async2::Waker waker_;
};

void EventHandler::HandleEvent(Event type) {
  {
    std::lock_guard lock(mutex_);
    PW_CHECK(events_.try_push(type));
  }
  waker_.Wake();
}

void EventHandler::Halt() {
  notification_.release();
  thread_.join();
}

void EventHandler::Run() {
  while (true) {
    // Run() and DoPend() run on the same thread, so no locking is needed to
    // set dispatcher_, and dispatcher_ will always be set when DoPend() is
    // running.
    dispatcher_.emplace(notification_);
    dispatcher_->Post(*this);

    // Destroy the dispatcher when this task returns Ready().
    while (true) {
      dispatcher_->RunUntilStalled();
      if (terminate_) {
        PW_LOG_DEBUG("Destroying dispatcher %zu", id_);
        dispatcher_.reset();
        return;
      }
      if (!IsRegistered()) {
        // EventHandler returned Ready(), so destroy the dispatcher
        PW_LOG_DEBUG("Destroying and reinstantiating dispatcher %zu", id_);
        dispatcher_.emplace(notification_);
        dispatcher_->Post(*this);
        test_state_.GarbageCollectTasks();
        continue;
      }
      // Block until the dispatcher is woken to avoid busy waiting
      notification_.acquire();
    }
  }
}

pw::async2::Poll<> EventHandler::DoPend(pw::async2::Context& cx) {
  Event event;
  // Pop the next event from the queue.
  {
    std::lock_guard lock(mutex_);
    if (events_.empty()) {
      PW_ASYNC_STORE_WAKER(cx, waker_, "EventHandler");
      return pw::async2::Pending();
    };
    event = events_.front();
    events_.pop();
  }
  PW_LOG_DEBUG("Handler %zu/%zu executing event %s",
               id_ + 1,
               kNumThreads,
               EventToString(event));

  test_state_.RecordEventHandled();

  // Handle the event.
  switch (event) {
    case Event::kPostNewTask:
      PW_DCHECK(dispatcher_);
      test_state_.RegisterWakerTask(
          dispatcher_->Post<WakerTask>(allocator_, test_state_));
      break;
    case Event::kWakeRandomTask:
      test_state_.WakeRandomTask();
      break;
    case Event::kDeregisterRandomTask:
      test_state_.DeregisterRandomWakerTask(*this);
      break;
    case Event::kDestroyRandomDispatcher:
      // The dispatcher is destroyed when the EventHandler is deregistered.
      return pw::async2::Ready();
    case Event::kStopTest:
      terminate_ = true;
      return pw::async2::Ready();
  }

  // Allow other tasks to run
  cx.ReEnqueue();
  return pw::async2::Pending();
}

// Initialize array of handlers with IDs 0-kHandlers-1 using
// std::index_sequence.
template <size_t... kIndices>
std::array<EventHandler, sizeof...(kIndices)> InitializeHandlers(
    TestState& test_state, std::index_sequence<kIndices...>) {
  return {EventHandler(test_state, kIndices)...};
}

template <size_t kHandlers = kNumThreads>
std::array<EventHandler, kNumThreads> InitializeHandlers(
    TestState& test_state) {
  return InitializeHandlers(test_state, std::make_index_sequence<kHandlers>{});
}

TestState::TestState()
    : handlers_{InitializeHandlers(*this)},
      rng_(kRandomSeed),
      all_waker_tasks_(allocator_) {}

void TestState::HaltTest() {
  for (size_t i = 0; i < kNumThreads; ++i) {
    HandleEvent(Event::kStopTest, i);
  }
  for (auto& handler : handlers_) {
    handler.Halt();
  }
}

void TestState::HandleEvent(Event type, size_t handler_id) {
  std::lock_guard lock(mutex_);
  handlers_[handler_id].HandleEvent(type);
}

void TestState::GarbageCollectTasks() {
  size_t i = 0;
  while (true) {
    pw::SharedPtr<WakerTask> task_to_delete;
    {
      std::lock_guard lock(mutex_);
      if (i >= all_waker_tasks_.size()) {
        break;
      }
      if (!all_waker_tasks_[i]->IsRegistered()) {
        task_to_delete = std::move(all_waker_tasks_[i]);
        std::swap(all_waker_tasks_[i], all_waker_tasks_.back());
        all_waker_tasks_.pop_back();
      } else {
        i++;
      }
    }
  }
}

void TestState::WakeRandomTask() {
  pw::SharedPtr<WakerTask> task;
  {  // Lock to get a task to wake.
    std::lock_guard lock(mutex_);
    if (all_waker_tasks_.empty()) {
      return;  // Ignore this event; there are no tasks to wake.
    }
    if (rng_() % 8 == 0) {
      return;  // Skip the wake
    }

    const size_t index = rng_() % all_waker_tasks_.size();
    task = all_waker_tasks_[index];
    if (kEnableVerboseLogging) {
      PW_LOG_DEBUG(
          "Waking task at index %zu/%zu", index + 1, all_waker_tasks_.size());
    }
  }
  task->Wake();
}

void TestState::RegisterWakerTask(const pw::SharedPtr<WakerTask>& task) {
  PW_DCHECK(task != nullptr);
  std::lock_guard lock(mutex_);
  all_waker_tasks_.push_back(task);
  PW_LOG_DEBUG("Posted task %zu", all_waker_tasks_.size());
}

void TestState::DeregisterRandomWakerTask(pw::async2::Task& skip_this_task) {
  pw::SharedPtr<WakerTask> task;
  {
    std::lock_guard lock(mutex_);
    // If this is the only task, return.
    if (all_waker_tasks_.size() <= 1u) {
      return;
    }
    size_t index = rng_() % all_waker_tasks_.size();
    if (all_waker_tasks_[index].get() == &skip_this_task) {
      // Don't deregister the calling task!
      index += 1;
      if (index >= all_waker_tasks_.size()) {
        index = 0;
      }
    }
    task = all_waker_tasks_[index];
    std::swap(all_waker_tasks_[index], all_waker_tasks_.back());
    all_waker_tasks_.pop_back();
    PW_LOG_DEBUG(
        "Deregistering task %zu/%zu", index + 1, all_waker_tasks_.size());
  }
  task->Deregister();
}

constexpr int kWakeProbability = 50;
constexpr int kPostProbability = 30;
constexpr int kDeregisterProbability = 15;
constexpr int kDestroyProbability = 5;

static_assert(kWakeProbability + kPostProbability + kDeregisterProbability +
                  kDestroyProbability ==
              100);

TEST(MultiThreadedTest, StressTest) {
  PW_LOG_INFO("Starting stress test with %zu threads for %zu iterations",
              kNumThreads,
              kIterations);
  std::mt19937 rng{kRandomSeed};

  TestState test_state;
  test_state.StartTest();

  for (size_t i = 0; i < kIterations; ++i) {
    const size_t handler_id = static_cast<size_t>(rng() % kNumThreads);
    const int rand_val = rng() % 100;

    Event type;
    if (rand_val < kWakeProbability) {
      type = Event::kWakeRandomTask;
    } else if (rand_val < kWakeProbability + kPostProbability) {
      type = Event::kPostNewTask;
    } else if (rand_val <
               kWakeProbability + kPostProbability + kDeregisterProbability) {
      type = Event::kDeregisterRandomTask;
    } else {
      type = Event::kDestroyRandomDispatcher;
    }
    if (kEnableVerboseLogging) {
      PW_LOG_DEBUG("[%zu/%zu] %s -> handler %zu/%zu",
                   i + 1,
                   kIterations,
                   EventToString(type),
                   handler_id + 1,
                   kNumThreads);
    }
    test_state.HandleEvent(type, handler_id);
    pw::this_thread::yield();
  }

  test_state.HaltTest();

  // Total number of events handled, plus a kStopTest event for each thread.
  EXPECT_EQ(test_state.EventsHandled(), kIterations + kNumThreads);
  PW_LOG_INFO("Finished stress test!");
}

}  // namespace
