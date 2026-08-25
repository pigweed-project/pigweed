# Testing pw_async2 code

This guide covers unit testing pw_async2 tasks, futures, channels, and
time-dependent code using `pw::async2::DispatcherForTest` and
`pw::async2::SimulatedTimeProvider`.

---

## 1. Build setup and headers

To test async code, include `dispatcher_for_test.h`:

```cpp
#include "pw_async2/dispatcher_for_test.h"
#include "pw_unit_test/framework.h"

// Optional: include if testing time-dependent logic
#include "pw_async2/simulated_time_provider.h"
```

Build target dependencies:
- **GN**: Add `$dir_pw_async2:dispatcher_for_test` to `deps` (and
  `$dir_pw_async2:simulated_time_provider` for time tests).
- **Bazel**: Add `//pw_async2:dispatcher_for_test` to `deps` (and
  `//pw_async2:simulated_time_provider` for time tests).

---

## 2. Test Dispatcher (`DispatcherForTest`)

`DispatcherForTest` is a `RunnableDispatcher` implementation that executes tasks
synchronously on the calling thread without real-time delays.

### Execution methods

- `dispatcher.Post(task)`: Registers a `Task` with the test dispatcher.
- `dispatcher.RunUntilStalled()`: Runs registered tasks until all tasks return
  `Pending()` or complete.
  - Returns `true` if sleeping or pending tasks remain registered on the
    dispatcher.
  - Returns `false` if all tasks ran to completion.
- `dispatcher.RunToCompletion()`: Runs tasks until all tasks complete. If a task
  blocks waiting for a wake from another thread, `dispatcher.AllowBlocking()`
  must be called first; otherwise attempting to block triggers a debug
  assertion failure.
- `dispatcher.AllowBlocking()`: Explicitly permits `DispatcherForTest` to block
  the test thread when waiting for tasks to be woken (e.g., from background
  threads or ISR handlers).
- `dispatcher.RunInTaskUntilStalled(future)`: Wraps `future` in an internal
  `FutureTask`, posts it to the dispatcher, executes `RunUntilStalled()`,
  deregisters the task, and returns `Poll<value_type>`. This allows polling a
  future directly without instantiating a `Task` or `FuncTask`.

### Inspection and metrics

- `dispatcher.tasks_polled()`: Total number of times `DoPend()` was called
  across all tasks.
- `dispatcher.tasks_completed()`: Total number of tasks that finished with
  `Ready()`.
- `dispatcher.wake_count()`: Total number of waker triggers.

---

## 3. Testing futures directly (`RunInTaskUntilStalled`)

When testing a future returned by a method or factory function, pass the future
directly to `dispatcher.RunInTaskUntilStalled(future)`:

```cpp
TEST(MyDeviceTest, ConnectResolvesAsync) {
  pw::async2::DispatcherForTest dispatcher;
  MyDevice device;

  auto future = device.Connect();

  // 1. Verify future is pending initially
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(future), pw::async2::Pending());

  // 2. Trigger mock event or state transition
  device.SimulateConnectionComplete();

  // 3. Re-poll future directly
  auto result = dispatcher.RunInTaskUntilStalled(future);
  ASSERT_TRUE(result.IsReady());
  EXPECT_EQ(*result, pw::OkStatus());
}
```

---

## 4. Mocking inputs with value providers (optional)

When building mock dependencies that return futures, `ValueProvider<T>` allows
resolving futures manually during a test:

- `provider.Get()`: Returns a `ValueFuture<T>` to return from mock methods.
- `provider.Resolve(value)`: Resolves pending futures with `value` and wakes
  their tasks.
- `provider.ResolveWithResult(status_or_value)`: Resolves futures returning
  `pw::Result<T>`.

---

## 5. Task testing patterns

### A. Lambda tasks (`FuncTask`)

`FuncTask` wraps an inline lambda for testing async operations without
defining a `Task` subclass:

```cpp
TEST(MyAsyncCodeTest, InlineLambdaTask) {
  pw::async2::DispatcherForTest dispatcher;

  int result_val = 0;
  pw::async2::FuncTask task([&](pw::async2::Context& cx) -> pw::async2::Poll<> {
    result_val = 100;
    return pw::async2::Ready();
  });

  dispatcher.Post(task);
  dispatcher.RunToCompletion();
  EXPECT_EQ(result_val, 100);
}
```

### B. Stateful custom tasks

Subclass `Task` when test logic requires state across polls or internal
counters:

```cpp
class TestSensorTask : public pw::async2::Task {
 public:
  TestSensorTask(Sensor& sensor) : sensor_(sensor) {}

  int poll_count = 0;
  pw::Result<int> result = pw::Status::Unknown();

 private:
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override {
    ++poll_count;
    if (!read_future_.is_pendable()) {
      read_future_ = sensor_.Read();
    }
    PW_AWAIT(result, read_future_, cx);
    return pw::async2::Ready();
  }

  Sensor& sensor_;
  SensorReadFuture read_future_;
};
```

### C. Coroutines (`CoroTask` / `FallibleCoroTask`)

To test C++20 coroutines (`Coro<T>`), pass `CoroContext` with an allocator (such
as `AllocatorForTest`) and post a `CoroTask`:

```cpp
TEST(CoroTest, ExecutesCoroToCompletion) {
  pw::allocator::test::AllocatorForTest<2048> alloc;
  pw::async2::CoroTask task = MyCoroFunction(alloc, arg1, arg2);

  pw::async2::DispatcherForTest dispatcher;
  dispatcher.Post(task);
  dispatcher.RunToCompletion();

  EXPECT_EQ(task.Wait(), pw::OkStatus());
}
```

### D. Cancellable tasks (`CancellableTask`)

Test task cancellation by calling `task.Cancel()`:

```cpp
TEST(CancellableTaskTest, CancelsPendingTask) {
  pw::async2::CancellableTask<MyTask> task;
  pw::async2::DispatcherForTest dispatcher;

  dispatcher.Post(task);
  EXPECT_TRUE(dispatcher.RunUntilStalled());  // Task is pending

  task.Cancel();
  dispatcher.RunToCompletion();
  EXPECT_FALSE(task.IsRegistered());
}
```

### E. Task deregistration requirement

Every posted task must reach completion or call `task.Deregister()` before
destruction. Destroying a registered task results in a debug assertion
failure.

---

## 6. Time and timer testing (`SimulatedTimeProvider`)

Inject `TimeProvider<Clock>&` into time-dependent code instead of using
`SystemClock::now()` or thread sleeping.

In tests, inject `SimulatedTimeProvider<SystemClock>`:

- `time_provider.AdvanceTime(duration)`: Advances simulated clock by
  `duration`.
- `time_provider.AdvanceUntilNextExpiration()`: Advances simulated clock
  directly to the next scheduled timer expiration. Returns `true` if a timer
  expired, `false` if no pending timers remain.
- `time_provider.SetTime(time_point)`: Sets simulated clock to an exact
  timestamp.
- `time_provider.TimeUntilNextExpiration()`: Returns `std::optional<duration>`
  indicating delay until next timer.

### Example: Delays and timeouts

```cpp
TEST(TimeoutTest, AdvancesTimeUntilTimerExpires) {
  pw::async2::DispatcherForTest dispatcher;
  pw::async2::SimulatedTimeProvider<pw::chrono::SystemClock> time_provider;

  // Code under test schedules a 1-hour delay
  auto timer_future = time_provider.WaitFor(std::chrono::hours(1));

  // Initially pending
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(timer_future),
            pw::async2::Pending());

  // Advance time partially (30 mins) -> still pending
  time_provider.AdvanceTime(std::chrono::minutes(30));
  EXPECT_EQ(dispatcher.RunInTaskUntilStalled(timer_future),
            pw::async2::Pending());

  // Advance directly to next expiration -> now ready
  EXPECT_TRUE(time_provider.AdvanceUntilNextExpiration());
  EXPECT_TRUE(dispatcher.RunInTaskUntilStalled(timer_future).IsReady());

  // No further timers pending
  EXPECT_FALSE(time_provider.AdvanceUntilNextExpiration());
}
```

---

## 7. Testing channels (`pw::async2::Channel`)

Test channel senders and receivers using `DispatcherForTest` or synchronous
`Try`/`Blocking` methods.

### Async send and receive

```cpp
TEST(ChannelTest, SendAndReceiveAsync) {
  pw::allocator::test::AllocatorForTest<2048> alloc;
  auto maybe_channel = pw::async2::CreateSpscChannel<int>(alloc, 5);
  ASSERT_TRUE(maybe_channel.ok());
  auto [channel, sender, receiver] = *maybe_channel;
  channel.Release();

  pw::async2::DispatcherForTest dispatcher;

  // Send a value
  auto send_fut = sender.Send(42);
  EXPECT_TRUE(dispatcher.RunInTaskUntilStalled(send_fut).IsReady());

  // Receive the value
  auto recv_fut = receiver.Receive();
  auto result = dispatcher.RunInTaskUntilStalled(recv_fut);
  ASSERT_TRUE(result.IsReady());
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ(**result, 42);
}
```

### Channel closure

When senders are dropped or `channel.Release()` is called,
`receiver.Receive()` resolves to `std::nullopt`:

```cpp
TEST(ChannelTest, ReceiveReturnsNulloptOnClose) {
  pw::allocator::test::AllocatorForTest<2048> alloc;
  auto maybe_channel = pw::async2::CreateSpscChannel<int>(alloc, 5);
  ASSERT_TRUE(maybe_channel.ok());
  auto [channel, sender, receiver] = *maybe_channel;
  channel.Release();

  pw::async2::DispatcherForTest dispatcher;

  // Drop sender to close channel
  { pw::async2::Sender<int> move_sender = std::move(sender); }

  auto recv_fut = receiver.Receive();
  auto result = dispatcher.RunInTaskUntilStalled(recv_fut);
  ASSERT_TRUE(result.IsReady());
  EXPECT_FALSE(result->has_value());  // std::nullopt indicates EOF / closed
}
```

---

## 8. Multi-step integration test pattern

Pattern for multi-step unit tests with mock events and time steps:

```cpp
TEST(SensorWorkflowTest, RetriesAndSucceeds) {
  pw::async2::DispatcherForTest dispatcher;
  pw::async2::SimulatedTimeProvider<pw::chrono::SystemClock> time_provider;
  MockSensor sensor;

  SensorReaderTask task(sensor, time_provider);
  dispatcher.Post(task);

  // 1. Initial poll -> task starts read operation
  dispatcher.RunUntilStalled();

  // 2. Mock sensor fails 1st read
  sensor.ResolveWithResult(pw::Status::Unavailable());

  // 3. Task receives failure, schedules retry timer
  dispatcher.RunUntilStalled();

  // 4. Advance time to trigger retry timer
  EXPECT_TRUE(time_provider.AdvanceUntilNextExpiration());

  // 5. Task wakes up, starts 2nd read operation
  dispatcher.RunUntilStalled();

  // 6. Mock sensor succeeds 2nd read
  sensor.ResolveWithResult(99);

  // 7. Drive task to completion and verify final result
  dispatcher.RunToCompletion();
  EXPECT_TRUE(task.result().ok());
  EXPECT_EQ(*task.result(), 99);
}
```

---

## 9. Rules summary

- **No wall-clock sleeps**: Inject `SimulatedTimeProvider` instead of sleeping
  or reading system wall clocks directly.
- **Use `RunInTaskUntilStalled`**: Pass standalone futures directly to
  `dispatcher.RunInTaskUntilStalled(future)`.
- **Step execution deterministically**: Alternate between `RunUntilStalled()`,
  triggering events/time advancements, and asserting state.
- **Task deregistration**: Ensure tasks complete or call `task.Deregister()`
  before test destruction.
- **Concept validation**: Add `static_assert(pw::async2::Future<MyFuture>);`
  for custom futures.

