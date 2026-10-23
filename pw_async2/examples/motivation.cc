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

#include <chrono>
#include <mutex>
#include <tuple>
#include <utility>

#include "pw_assert/check.h"
#include "pw_async2/await.h"
#include "pw_async2/channel.h"
#include "pw_async2/coro.h"
#include "pw_async2/join.h"
#include "pw_async2/notification.h"
#include "pw_async2/poll.h"
#include "pw_async2/select.h"
#include "pw_async2/system_time_provider.h"
#include "pw_async2/task.h"
#include "pw_async2/time_provider.h"
#include "pw_async2/value_future.h"
#include "pw_chrono/system_clock.h"
#include "pw_function/function.h"
#include "pw_log/log.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"
#include "pw_thread/attrs.h"
#include "pw_thread/context.h"
#include "pw_thread/native_options.h"
#include "pw_thread/options.h"
#include "pw_thread/sleep.h"
#include "pw_thread/thread.h"
#include "pw_work_queue/work_queue.h"

// The following examples demonstrate a common embedded transaction in different
// async programming models, showcasing the complexities and pitfalls of each.
//
// The transaction is as follows:
//
// 1. Read data from two independent sensors, with timeouts.
// 2. Transmit the results in a single packet, with backpressure.
// 3. Repeat this at a regular interval.
// 4. At any point, the system may signal that it is entering a low-power mode.
//    When this happens, pending operations should be cancelled.
//

// ============================================================================
// Example 1: Callback-based async
// ============================================================================

// DOCSTAG: [pw_async2-examples-motivation-callbacks]
struct SensorData {
  int value;
};

class CallbackSensor {
 public:
  void ReadAsync(pw::Function<void(pw::Result<SensorData>)>&& callback);
};

class CallbackTransmitter {
 public:
  void SendAsync(const SensorData& data,
                 pw::Function<void(pw::Status)>&& callback);
};

class CallbackTimer {
 public:
  void Schedule(pw::chrono::SystemClock::duration delay,
                pw::Function<void()>&& callback);
  void Cancel();
};

class CallbackSensorProcessor {
 public:
  CallbackSensorProcessor(CallbackSensor& s1,
                          CallbackSensor& s2,
                          CallbackTransmitter& tx,
                          CallbackTimer& timer)
      : s1_(s1), s2_(s2), tx_(tx), timer_(timer) {}

  ~CallbackSensorProcessor() {
    // Since there is no way to cancel an ongoing sensor read call, if the
    // processor is destroyed while reads are pending, their callbacks can
    // corrupt unused memory. We detect this by crashing, requiring users to
    // remember to call Cancel() before destroying the processor.
    //
    // In practice, an object like this would be statically allocated for the
    // lifetime of the program.
    PW_CHECK(!cycle_active_);
    Cancel();
  }

  // Begins the sensor read cycle.
  void Start() { ScheduleNextCycle(); }

  // Stops the sensor read cycle.
  //
  // WARNING: This does not cancel pending reads. Users must ensure that the
  // processor is not destroyed until `cycle_active()` returns false.
  //
  // Returns true if there were no pending reads and the object is safe to
  // destroy, or false if reads are still pending.
  bool Cancel() {
    std::lock_guard guard(lock_);
    cancelled_ = true;
    timer_.Cancel();
    return !cycle_active_;
  }

  bool cycle_active() const {
    std::lock_guard guard(lock_);
    return cycle_active_;
  }

  // Called by the system to signal it is entering a low power mode.
  void OnSleep() {
    PW_LOG_INFO("Sensor processor entering sleep mode");
    Cancel();
  }

 private:
  enum class Sensor { kSensor1, kSensor2 };

  void ScheduleNextCycle() {
    bool proceed;
    {
      std::lock_guard guard(lock_);
      cycle_active_ = true;
      proceed = !cancelled_;
    }

    if (proceed) {
      timer_.Schedule(std::chrono::milliseconds(100), [this]() { RunCycle(); });
    }

    {
      std::lock_guard guard(lock_);
      cycle_active_ = false;
    }
  }

  void RunCycle() {
    {
      std::lock_guard guard(lock_);
      if (cancelled_ || cycle_active_) {
        return;
      }
      cycle_active_ = true;

      state_.results_received = 0;
      state_.timed_out = false;
    }

    timer_.Schedule(std::chrono::milliseconds(10), [this]() {
      bool failed_to_receive = false;
      {
        // There is a race condition where the timeout and sensor callbacks may
        // execute concurrently on separate threads or interrupt contexts, so
        // we have to lock and track which operation finished first so that the
        // later callback becomes a no-op.
        std::lock_guard guard(lock_);
        state_.timed_out = true;
        if (state_.results_received < 2) {
          cycle_active_ = false;
          failed_to_receive = true;
        }
      }
      if (failed_to_receive) {
        PW_LOG_WARN("Timed out reading sensors");
      }
    });

    // Depending on the behavior of the sensor driver, some additional
    // considerations have to be made with these read calls.
    //
    // - If the driver starts a new hardware read on each `ReadAsync` call,
    //   we get duplicate interrupts/completions if operations overlap (e.g.,
    //   following a timeout), which can corrupt state on later cycles.
    //
    // - If the driver ignores a new read while one is pending, it may return
    //   data sampled in the past, causing timing uncertainty.
    //
    // These can be addressed by capturing a monotonic "read ID" in each
    // callback, incremented on each cycle, and comparing the captured ID to
    // the processor's current ID on response. However, `pw::Function` only
    // has space to capture one pointer by default, so this would require
    // globally increasing the storage size of every function in the system.
    s1_.ReadAsync([this](pw::Result<SensorData> res) {
      HandleSensorResult(res, Sensor::kSensor1);
    });
    s2_.ReadAsync([this](pw::Result<SensorData> res) {
      HandleSensorResult(res, Sensor::kSensor2);
    });
  }

  void HandleSensorResult(pw::Result<SensorData> res, Sensor sensor) {
    bool should_proceed = false;
    {
      std::lock_guard guard(lock_);
      if (state_.timed_out) {
        // Stale callback: A late interrupt from a previous cycle may access
        // repurposed state.
        return;
      }
      if (sensor == Sensor::kSensor1) {
        state_.r1 = res;
      } else {
        state_.r2 = res;
      }
      state_.results_received++;
      if (state_.results_received == 2) {
        should_proceed = true;
      }
    }

    if (!res.ok()) {
      PW_LOG_WARN("Failed to read sensor %d",
                  sensor == Sensor::kSensor1 ? 1 : 2);
    }

    if (should_proceed) {
      ProceedToSend();
    }
  }

  void ProceedToSend() {
    // Lock inversion: We must not hold locks across external API calls
    // (e.g. Cancel or SendAsync) to prevent deadlocks.
    timer_.Cancel();

    bool success = false;
    SensorData combined;
    {
      std::lock_guard guard(lock_);
      if (state_.r1.ok() && state_.r2.ok()) {
        combined.value = state_.r1->value + state_.r2->value;
        success = true;
      }
    }

    if (!success) {
      ScheduleNextCycle();
      return;
    }

    tx_.SendAsync(combined, [this](pw::Status status) {
      ScheduleNextCycle();

      if (!status.ok()) {
        // Handle send errors...
      }
    });
  }

  CallbackSensor& s1_;
  CallbackSensor& s2_;
  CallbackTransmitter& tx_;
  CallbackTimer& timer_;

  struct State {
    int results_received = 0;
    pw::Result<SensorData> r1;
    pw::Result<SensorData> r2;
    bool timed_out = false;
  };
  State state_ PW_GUARDED_BY(lock_);
  bool cycle_active_ PW_GUARDED_BY(lock_) = false;
  bool cancelled_ PW_GUARDED_BY(lock_) = false;

  mutable pw::sync::InterruptSpinLock lock_;
};
// DOCSTAG: [pw_async2-examples-motivation-callbacks]

// ============================================================================
// Example 2: Work Queue based async
// ============================================================================

// DOCSTAG: [pw_async2-examples-motivation-work-queue]
class WorkQueueSensorProcessor;

class WorkQueueSensor {
 public:
  enum class Id { kSensor1, kSensor2 };

  WorkQueueSensor(Id id) : id_(id) {}
  void ReadAsync(pw::work_queue::WorkQueue& wq);

 private:
  Id id_;
};

class WorkQueueTransmitter {
 public:
  void SendAsync(pw::work_queue::WorkQueue& wq, const SensorData&);
};

class WorkQueueTimer {
 public:
  enum class Type { kCycle, kTimeout };
  void Schedule(pw::work_queue::WorkQueue& wq,
                pw::chrono::SystemClock::duration,
                Type type);
  void Cancel() {}
};

// The WorkQueueSensorProcessor demonstrates callbacks being run from a work
// queue. This improves on ad-hoc callbacks by serializing execution on a
// centralized thread, removing the need for complex locking around internal
// state.
class WorkQueueSensorProcessor {
 public:
  WorkQueueSensorProcessor() = default;

  void Init(WorkQueueSensor& s1,
            WorkQueueSensor& s2,
            WorkQueueTransmitter& tx,
            WorkQueueTimer& timer,
            pw::work_queue::WorkQueue& work_queue) {
    s1_ = &s1;
    s2_ = &s2;
    tx_ = &tx;
    timer_ = &timer;
    work_queue_ = &work_queue;
  }

  ~WorkQueueSensorProcessor() { Cancel(); }

  void Start();

  bool Cancel() {
    // External signals like cancellation still arrive on arbitrary threads, and
    // require explicit synchronization to coordinate with the work queue.
    std::lock_guard guard(lock_);
    cancelled_ = true;
    if (timer_ != nullptr) {
      timer_->Cancel();
    }
    return true;
  }

  void OnSleep() {
    PW_LOG_INFO("Sensor processor entering sleep mode");
    Cancel();
  }

  void RunCycle() {
    {
      std::lock_guard guard(lock_);
      if (cancelled_) {
        return;
      }
    }

    // `RunCycle` and all of the callbacks below are invoked from the same work
    // queue thread, so we can access `state_` without a lock.
    state_.results_received = 0;
    state_.timed_out = false;

    timer_->Schedule(*work_queue_,
                     std::chrono::milliseconds(10),
                     WorkQueueTimer::Type::kTimeout);

    s1_->ReadAsync(*work_queue_);
    s2_->ReadAsync(*work_queue_);
  }

  void HandleSensorResult(pw::Result<SensorData> res,
                          WorkQueueSensor::Id sensor) {
    if (state_.timed_out) {
      return;
    }

    if (sensor == WorkQueueSensor::Id::kSensor1) {
      state_.r1 = res;
    } else {
      state_.r2 = res;
    }

    state_.results_received++;
    if (state_.results_received == 2) {
      ProceedToSend();
    }
  }

  void HandleTimeout() {
    state_.timed_out = true;
    if (state_.results_received < 2) {
      PW_LOG_WARN("Timed out reading sensors");
      ScheduleNext();
    }
  }

  void ProceedToSend() {
    timer_->Cancel();

    if (!state_.r1.ok() || !state_.r2.ok()) {
      ScheduleNext();
      return;
    }

    SensorData combined;
    combined.value = state_.r1->value + state_.r2->value;

    tx_->SendAsync(*work_queue_, combined);
  }

  void HandleTransmitComplete(pw::Status status) {
    if (!status.ok()) {
      // Handle send errors...
    }
    ScheduleNext();
  }

  void ScheduleNext() {
    timer_->Schedule(*work_queue_,
                     std::chrono::milliseconds(100),
                     WorkQueueTimer::Type::kCycle);
  }

 private:
  WorkQueueSensor* s1_ = nullptr;
  WorkQueueSensor* s2_ = nullptr;
  WorkQueueTransmitter* tx_ = nullptr;
  WorkQueueTimer* timer_ = nullptr;
  pw::work_queue::WorkQueue* work_queue_ = nullptr;

  struct State {
    int results_received = 0;
    pw::Result<SensorData> r1;
    pw::Result<SensorData> r2;
    bool timed_out = false;
  };
  State state_;

  mutable pw::sync::Mutex lock_;
  bool cancelled_ PW_GUARDED_BY(lock_) = false;
};

namespace {

WorkQueueSensorProcessor work_queue_processor;

}  // namespace

void WorkQueueSensorProcessor::Start() {
  work_queue_->CheckPushWork([]() { work_queue_processor.RunCycle(); });
}

void WorkQueueSensor::ReadAsync(pw::work_queue::WorkQueue& wq) {
  wq.CheckPushWork([this]() {
    work_queue_processor.HandleSensorResult(SensorData{42}, id_);
  });
}

void WorkQueueTransmitter::SendAsync(pw::work_queue::WorkQueue& wq,
                                     const SensorData&) {
  wq.CheckPushWork(
      []() { work_queue_processor.HandleTransmitComplete(pw::OkStatus()); });
}

void WorkQueueTimer::Schedule(pw::work_queue::WorkQueue& wq,
                              pw::chrono::SystemClock::duration,
                              Type type) {
  wq.CheckPushWork([type]() {
    if (type == Type::kCycle) {
      work_queue_processor.RunCycle();
    } else {
      work_queue_processor.HandleTimeout();
    }
  });
}
// DOCSTAG: [pw_async2-examples-motivation-work-queue]

// ============================================================================
// Example 3: Synchronous multithreading
// ============================================================================

// DOCSTAG: [pw_async2-examples-motivation-threads]
class BlockingSensor {
 public:
  pw::Result<SensorData> ReadBlocking(
      pw::chrono::SystemClock::duration timeout);
};

class BlockingTransmitter {
 public:
  pw::Status SendBlocking(const SensorData& data);
};

class ThreadedSensorProcessor {
 public:
  ThreadedSensorProcessor() = default;

  ~ThreadedSensorProcessor() { Stop(); }

  void Init(BlockingSensor& s1, BlockingSensor& s2, BlockingTransmitter& tx) {
    s1_ = &s1;
    s2_ = &s2;
    tx_ = &tx;
  }

  void Start(const pw::thread::Options& options) {
    std::lock_guard guard(lock_);
    if (running_) {
      return;
    }
    running_ = true;
    cancelled_ = false;

    thread_ = pw::Thread(options, [this]() { Run(); });
  }

  // Stops the processor and joins the thread.
  void Stop() {
    {
      std::lock_guard guard(lock_);
      if (!running_) {
        return;
      }
      cancelled_ = true;
    }

    // If the worker thread is currently blocked on a timeout in `ReadBlocking`
    // or waiting for backpressure in `SendBlocking`, this `join()` call will
    // block the calling thread (whoever is requesting sleep) for the full
    // duration.
    thread_.join();

    std::lock_guard guard(lock_);
    running_ = false;
  }

 private:
  void Run() {
    while (true) {
      {
        std::lock_guard guard(lock_);
        if (cancelled_)
          break;
      }

      // NOTE: These reads are serialized. If we want to read the two sensors in
      // parallel (as they are independent), that cannot be done with a single
      // thread using blocking APIs. We would need to spin up additional worker
      // threads, further increasing stack usage.
      // Instead, we eat the latency cost of serialization, resulting in each
      // sampling cycle taking up to twice the timeout.

      // As noted in `Stop()`, if a sleep signal arrives while the thread is
      // blocked, it has to wait for the operation to complete or time out.
      auto r1 = s1_->ReadBlocking(std::chrono::milliseconds(10));
      if (!r1.ok()) {
        PW_LOG_WARN("Failed to read sensor 1");
        continue;
      }

      auto r2 = s2_->ReadBlocking(std::chrono::milliseconds(10));
      if (!r2.ok()) {
        PW_LOG_WARN("Failed to read sensor 2");
        continue;
      }

      SensorData combined;
      combined.value = r1->value + r2->value;

      // WARNING: If the transmitter is full, this blocks the thread until space
      // becomes available, which is dependent on the wider system and network.
      // Again, we are un-interruptible during this period.
      pw::Status status = tx_->SendBlocking(combined);
      if (!status.ok()) {
        // Handle send errors...
      }

      pw::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  BlockingSensor* s1_ = nullptr;
  BlockingSensor* s2_ = nullptr;
  BlockingTransmitter* tx_ = nullptr;

  pw::sync::Mutex lock_;
  bool running_ PW_GUARDED_BY(lock_) = false;
  bool cancelled_ PW_GUARDED_BY(lock_) = false;
  pw::Thread thread_;
};

namespace {

// The most noticeable issue with the threading model is that each thread
// requires a dedicated stack, massively increasing the system's RAM usage.
constexpr pw::ThreadAttrs kSensorThreadAttrs =
    pw::ThreadAttrs().set_name("SensorProcessor").set_stack_size_bytes(2048);

pw::ThreadContextFor<kSensorThreadAttrs> sensor_thread_context;

ThreadedSensorProcessor processor;

}  // namespace

// Begins the sensor processing thread.
void RunThreadedSensorProcessor(BlockingSensor& s1,
                                BlockingSensor& s2,
                                BlockingTransmitter& tx) {
  processor.Init(s1, s2, tx);
  processor.Start(pw::GetThreadOptions(sensor_thread_context));
}

// Signals the sensor processing thread to stop.
void SleepThreadedSensorProcessor() { processor.Stop(); }

// DOCSTAG: [pw_async2-examples-motivation-threads]

// ============================================================================
// Example 4: Event handlers / active objects
// ============================================================================

// DOCSTAG: [pw_async2-examples-motivation-event-handlers]
enum class EventType {
  kStartCycle,
  kSensorData,
  kSensorTimeout,
  kTransmitComplete,
  kSleepSignal,
};

enum class Sensor { kSensor1, kSensor2 };

struct Event {
  EventType type;
  Sensor sensor = Sensor::kSensor1;
  pw::Result<SensorData> sensor_data = pw::Status::Unavailable();
  pw::Status status = pw::OkStatus();

  static Event StartCycle() { return {EventType::kStartCycle}; }
  static Event SensorTimeout() { return {EventType::kSensorTimeout}; }
  static Event Sleep() { return {EventType::kSleepSignal}; }

  static Event SensorData(Sensor sensor, pw::Result<SensorData> res) {
    return {EventType::kSensorData, sensor, res};
  }

  static Event TransmitComplete(pw::Status status) {
    Event e;
    e.type = EventType::kTransmitComplete;
    e.status = status;
    return e;
  }
};

// All events in the system go through this centralized queue, which would be
// drained by a thread that forwards events into `HandleEvent`. The queue must
// outlive every possible asynchronous operation that posts events to it.
class EventQueue {
 public:
  void Post(Event event);
};

class EventSensorProcessor {
 public:
  EventSensorProcessor(EventQueue& queue,
                       CallbackSensor& s1,
                       CallbackSensor& s2,
                       CallbackTransmitter& tx,
                       CallbackTimer& timer)
      : queue_(queue), s1_(s1), s2_(s2), tx_(tx), timer_(timer) {}

  // Everything occurring in the system flows through this function as an event.
  // This model avoids locks by serializing events onto a single thread.
  // However, it suffers from severe logic fragmentation and loss of
  // composability.
  void HandleEvent(const Event& event) {
    switch (state_) {
      case State::kIdle:
        HandleIdleEvent(event);
        break;

      case State::kWaitingForSensors:
        HandleSensorsEvent(event);
        break;

      case State::kWaitingForTransmit:
        HandleTransmitEvent(event);
        break;
    }
  }

 private:
  enum class State {
    kIdle,
    kWaitingForSensors,
    kWaitingForTransmit,
  };

  void HandleIdleEvent(const Event& event) {
    switch (event.type) {
      case EventType::kStartCycle:
        StartCycle();
        break;

      // We use switch statements to ensure exhaustive handling, preventing bugs
      // from forgetting to handle a new event type. However, this results in a
      // massive increase in boilerplate and verbosity, as each state handler
      // must explicitly enumerate all events, even if it ignores most of them.
      case EventType::kSensorData:
      case EventType::kSensorTimeout:
      case EventType::kTransmitComplete:
      case EventType::kSleepSignal:
        // Ignored.
        break;
    }
  }

  void HandleSensorsEvent(const Event& event) {
    switch (event.type) {
      case EventType::kSensorTimeout:
        // Say we wanted to extend the behavior to able to handle retrying
        // before failing the cycle. Under this model, we could not easily
        // compose it here. We would require new states, new events, and
        // handling distributed across the entire class.
        PW_LOG_WARN("Timed out reading sensors");
        state_ = State::kIdle;
        ScheduleNext();
        break;

      case EventType::kSleepSignal:
        CancelAll();
        state_ = State::kIdle;
        break;

      case EventType::kSensorData:
        if (event.sensor == Sensor::kSensor1) {
          r1_ = event.sensor_data;
        } else {
          r2_ = event.sensor_data;
        }
        results_received_++;
        CheckSensorsComplete();
        break;

      case EventType::kStartCycle:
      case EventType::kTransmitComplete:
        // Ignored.
        break;
    }
  }

  void HandleTransmitEvent(const Event& event) {
    switch (event.type) {
      case EventType::kTransmitComplete:
        if (!event.status.ok()) {
          PW_LOG_WARN("Transmit failed");
        }
        state_ = State::kIdle;
        ScheduleNext();
        break;

      case EventType::kSleepSignal:
        // Under this model, we cannot easily cancel the pending `tx_.SendAsync`
        // operation unless the transmitter driver explicitly provides a
        // cancellation API. Instead, we transition to idle where the incoming
        // event will be ignored.
        state_ = State::kIdle;
        break;

      case EventType::kStartCycle:
      case EventType::kSensorData:
      case EventType::kSensorTimeout:
        // Ignored.
        break;
    }
  }

  void StartCycle() {
    state_ = State::kWaitingForSensors;
    results_received_ = 0;
    r1_ = pw::Status::Unavailable();
    r2_ = pw::Status::Unavailable();

    // Because callbacks post to the queue instead of directly modifying state,
    // the synchronization and lifetime issues from the original callback
    // example no longer apply.
    timer_.Schedule(std::chrono::milliseconds(10),
                    [this]() { queue_.Post(Event::SensorTimeout()); });

    s1_.ReadAsync([this](pw::Result<SensorData> res) {
      queue_.Post(Event::SensorData(Sensor::kSensor1, res));
    });
    s2_.ReadAsync([this](pw::Result<SensorData> res) {
      queue_.Post(Event::SensorData(Sensor::kSensor2, res));
    });
  }

  void CheckSensorsComplete() {
    if (results_received_ == 2) {
      timer_.Cancel();
      if (r1_.ok() && r2_.ok()) {
        SensorData combined;
        combined.value = r1_->value + r2_->value;
        state_ = State::kWaitingForTransmit;
        tx_.SendAsync(combined, [this](pw::Status status) {
          queue_.Post(Event::TransmitComplete(status));
        });
      } else {
        state_ = State::kIdle;
        ScheduleNext();
      }
    }
  }

  void ScheduleNext() {
    timer_.Schedule(std::chrono::milliseconds(100),
                    [this]() { queue_.Post(Event::StartCycle()); });
  }

  void CancelAll() { timer_.Cancel(); }

  EventQueue& queue_;
  CallbackSensor& s1_;
  CallbackSensor& s2_;
  CallbackTransmitter& tx_;
  CallbackTimer& timer_;

  State state_ = State::kIdle;
  int results_received_ = 0;
  pw::Result<SensorData> r1_;
  pw::Result<SensorData> r2_;
};
// DOCSTAG: [pw_async2-examples-motivation-event-handlers]

// ============================================================================
// Example 5: pw_async2 manual informed poll
// ============================================================================

// DOCSTAG: [pw_async2-examples-motivation-informed-poll]

class Async2Sensor {
 public:
  pw::async2::ValueFuture<pw::Result<SensorData>> ReadAsync() {
    return pw::async2::ValueFuture<pw::Result<SensorData>>::Resolved(
        SensorData{42});
  }
};

class Async2Transmitter {
 public:
  pw::async2::ValueFuture<pw::Status> SendAsync(const SensorData&) {
    return pw::async2::ValueFuture<pw::Status>::Resolved(pw::OkStatus());
  }
};

// The PolledSensorProcessor demonstrates the manual async2 Informed Poll model.
// All tasks run cooperatively on a shared dispatcher thread.
// Unlike callbacks, it requires no locks because the task owns its state and
// events are received via futures.
// Unlike threads, it does not require a dedicated stack for concurrency.
//
// However, writing this state machine manually can get quite complex and
// require boilerplate.
class PolledSensorProcessor : public pw::async2::Task {
 public:
  PolledSensorProcessor(Async2Sensor& s1,
                        Async2Sensor& s2,
                        pw::async2::Sender<SensorData> sender,
                        pw::async2::Notification& sleep_notification)
      : pw::async2::Task(PW_ASYNC_TASK_NAME("PolledSensorProcessor")),
        s1_(s1),
        s2_(s2),
        sender_(std::move(sender)),
        sleep_notification_(sleep_notification) {}

 private:
  // Entrypoint the task. Every time it is woken and runs, this function is
  // called from the top. We internally maintain our current state so we can
  // pick up from where we left off.
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
    // At the top level, always check for a sleep notification first. If it has
    // arrived, this short-circuits execution to exit the task.
    if (!sleep_future_.is_pendable()) {
      sleep_future_ = sleep_notification_.Wait();
    }
    if (sleep_future_.Pend(cx).IsReady()) {
      // Sleep requested. We reset futures to ensure the operations no longer
      // try to wake the task. There is no risk of dangling references or
      // invalid memory access, as the future providers will no longer attempt
      // to signal completion, and future destructors may additionally cancel
      // the underlying operation, if that's supported.
      //
      // Note that this is done assuming that the task is statically allocated
      // in some long-lived object. If the task was posted to the dispatcher
      // dynamically via `PostShared`, this would not be required as the
      // dispatcher would automatically destroy the task and its futures.
      s1_future_ = {};
      s2_future_ = {};
      tx_future_ = {};
      timer_future_ = {};
      return pw::async2::Ready();
    }

    while (true) {
      switch (state_) {
        case State::kIdle: {
          if (!timer_future_.is_pendable()) {
            timer_future_ = pw::async2::GetSystemTimeProvider().WaitFor(
                std::chrono::milliseconds(100));
          }

          // Wait for the timer to expire.
          PW_AWAIT(timer_future_, cx);

          // Begin a new cycle by starting the two sensor reads and a timeout.
          state_ = State::kReadingSensors;
          s1_future_ = s1_.ReadAsync();
          s1_result_ = pw::Status::Unknown();
          s2_future_ = s2_.ReadAsync();
          s2_result_ = pw::Status::Unknown();
          timer_future_ = pw::async2::GetSystemTimeProvider().WaitFor(
              std::chrono::milliseconds(10));
          break;
        }

        case State::kReadingSensors: {
          if (timer_future_.Pend(cx).IsReady()) {
            // Operation timed out. Cancel pending operations by resetting
            // their futures.
            s1_future_ = {};
            s2_future_ = {};
            PW_LOG_WARN("Timed out reading sensors");
            state_ = State::kIdle;
            break;
          }

          // By polling both futures separately, we achieve parallelism without
          // the stack overhead of dedicated threads.
          //
          // Note that this is being done manually here to demonstrate how
          // pw_async2 works internally. `pw::async2::Join` returns a
          // `JoinFuture` which handles this operation for you, though it
          // results in larger code size than writing it manually like this.
          if (s1_result_.status().IsUnknown()) {
            // Note that we don't use PW_AWAIT here because we always want to
            // pend both futures.
            auto r1 = s1_future_.Pend(cx);
            if (r1.IsReady()) {
              s1_result_ = r1.value();
            }
          }
          if (s2_result_.status().IsUnknown()) {
            auto r2 = s2_future_.Pend(cx);
            if (r2.IsReady()) {
              s2_result_ = r2.value();
            }
          }

          if (s1_result_.status().IsUnknown() ||
              s2_result_.status().IsUnknown()) {
            // Wait for both sensor reads to complete.
            return pw::async2::Pending();
          }

          // Cancel the timeout timer.
          timer_future_ = {};

          if (!s1_result_.ok() || !s2_result_.ok()) {
            PW_LOG_WARN("Failed to read sensors");
            state_ = State::kIdle;
            break;
          }

          // Start the send operation with the result.
          SensorData combined;
          combined.value = s1_result_->value + s2_result_->value;
          state_ = State::kTransmitting;
          tx_future_ = sender_.Send(combined);
          break;
        }

        case State::kTransmitting: {
          PW_AWAIT(bool sent, tx_future_, cx);
          if (!sent) {
            // If the channel closes, exit the task since there is nothing
            // further to do.
            PW_LOG_ERROR("Channel closed while transmitting");
            return pw::async2::Ready();
          }
          state_ = State::kIdle;
          break;
        }
      }
    }
  }

  enum class State {
    kIdle,
    kReadingSensors,
    kTransmitting,
  };

  Async2Sensor& s1_;
  Async2Sensor& s2_;
  pw::async2::Sender<SensorData> sender_;
  pw::async2::Notification& sleep_notification_;

  State state_ = State::kIdle;

  // Storing futures directly avoids the heap allocations often required
  // by callback chains to preserve state across yield points.
  pw::async2::ValueFuture<pw::Result<SensorData>> s1_future_;
  pw::Result<SensorData> s1_result_;
  pw::async2::ValueFuture<pw::Result<SensorData>> s2_future_;
  pw::Result<SensorData> s2_result_;
  pw::async2::SendFuture<SensorData> tx_future_;
  pw::async2::TimeFuture<pw::chrono::SystemClock> timer_future_;
  pw::async2::VoidFuture sleep_future_;
};

// DOCSTAG: [pw_async2-examples-motivation-informed-poll]

// ============================================================================
// Example 6: C++20 coroutines with pw_async2
// ============================================================================

// DOCSTAG: [pw_async2-examples-motivation-coro]
pw::async2::Coro<void> CoroSensorProcessor(
    pw::async2::CoroContext,
    Async2Sensor& s1,
    Async2Sensor& s2,
    pw::async2::Sender<SensorData> sender,
    pw::async2::Notification& sleep_notification) {
  while (true) {
    // Wait for the next cycle interval.
    co_await pw::async2::GetSystemTimeProvider().WaitFor(
        std::chrono::milliseconds(100));

    // Wait for the first of three events: the sleep signal, both sensor reads,
    // or a read timeout.
    auto results = co_await pw::async2::Select(
        sleep_notification.Wait(),
        pw::async2::GetSystemTimeProvider().WaitFor(
            std::chrono::milliseconds(10)),
        pw::async2::Join(s1.ReadAsync(), s2.ReadAsync()));

    if (results.template has_value<0>()) {
      PW_LOG_INFO("Sensor processor entering sleep mode");
      co_return;
    }

    if (results.template has_value<1>()) {
      PW_LOG_WARN("Timed out reading sensors");
      continue;
    }

    if (results.template has_value<2>()) {
      auto& [r1, r2] = results.template value<2>();

      if (r1.ok() && r2.ok()) {
        SensorData combined;
        combined.value = r1->value + r2->value;

        // ``Send`` on an async channel blocks until there is space for the
        // message, so backpressure is built in. However, this will stop the
        // task from continuing to sample sensors. There are alternative
        // approaches, such as continuing to poll sensors on a fixed interval
        // while calling ``ReserveSend`` or ``TrySend`` on the channel to send
        // the latest values when a slot is available.
        bool sent = co_await sender.Send(combined);
        if (!sent) {
          PW_LOG_ERROR("Channel closed while transmitting");
          co_return;
        }
      } else {
        PW_LOG_WARN("Failed to read sensors");
      }
    }
  }
}
// DOCSTAG: [pw_async2-examples-motivation-coro]

// Stub implementations so it compiles successfully
pw::Result<SensorData> BlockingSensor::ReadBlocking(
    pw::chrono::SystemClock::duration) {
  return pw::Status::Unimplemented();
}
pw::Status BlockingTransmitter::SendBlocking(const SensorData&) {
  return pw::Status::Unimplemented();
}
