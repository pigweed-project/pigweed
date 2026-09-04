# Top-Down Design and the Coroutine Mental Model

---

## 1. Top-Down vs. Bottom-Up Design

When designing asynchronous systems in `pw_async2`, always design **top-down**
from the caller's perspective rather than **bottom-up** from low-level hardware,
interrupts, or wakers.

### The Bottom-Up Trap

Starting from low-level transport or hardware register polling usually leads to
over-engineered and tightly-coupled architectures:

- **Waker obsession**: Assuming that every asynchronous operation requires a
  custom leaf future with `FutureCore`, intrusive lists, and manual `Waker`
  management. In reality, the vast majority of operations are composite futures
  or can be built using existing primitives (`Channel`, `ValueFuture`,
  `TimeFuture`).
- **Pendable function sprawl**: Creating loose `Poll<T>(Context&, ...)` helper
  functions instead of concrete future types. This scatters state across
  classes, makes lifecycle tracking opaque, and breaks RAII cancellation and
  composability (`PW_AWAIT`, `co_await`, `Select`, `Join`).
- **Task-per-operation proliferation**: Turning every sub-operation into a
  `pw::async2::Task` posted to the dispatcher (like RTOS threads). This creates
  unnecessary synchronization overhead, shared pointers, and scheduling churn.
- **Leaking hardware details upward**: Forcing callers to manage driver-level
  buffer lifetimes, retry loops, and hardware status flags rather than
  interacting with a clean domain-level API.

### The Top-Down Approach

Starting from the sequential caller experience produces simpler, zero-overhead
designs:

- **Design the high-level flow first**: Focus on what the caller needs to
  achieve sequentially, what operations can run concurrently, and what state
  must survive across pauses.
- **Model business logic as composite futures**: Intermediate operations become
  pure C++ value objects returned by factory functions that compose child
  futures inline without extra wakers, mutexes, or dynamic allocation.
- **Deterministic RAII cleanup**: Child subfutures are owned directly within the
  composite future struct, ensuring immediate synchronous cancellation when
  dropped.
- **Introduce leaf futures only at true boundaries**: Custom leaf futures with
  `FutureCore` are reserved strictly for the physical edges of the system
  (direct hardware registers or custom ISR handlers).

---

## 2. The Mental Model: Coroutine to Composite Future

Manual `pw_async2::Future`s provide deterministic, stack-allocated storage
without unwanted dynamic allocations, eliminating opaque, toolchain-dependent heap
allocations, fragmentation risks, and runtime exhaustion paths inherent to C++20
coroutine frames.

The most effective way to design a manual future is to **draft the API as a
coroutine first**, then mechanically map each construct into a factory function
and manual state machine.

### Mental Mapping Reference

```
Mental Coroutine Construct               Composite Future Equivalent
─────────────────────────────────────────────────────────────────────────────
Coroutine signature & parameters     ──> Public factory function returning Future by value
Pre-suspension argument validation   ──> Synchronous validation in factory function
Local variables across `co_await`    ──> Class member fields (persistent state)
`co_await sub_operation()`           ──> Child subfuture member + State enum value
Disjoint / non-overlapping variables ──> Grouped into `std::variant` (transient state)
Sequential statement block           ──> `switch (state_) case` in `Pend(cx)`
`co_return value;`                   ──> `return pw::async2::Ready(value);`
`PW_CO_TRY(...)` / error return      ──> `return pw::async2::Ready(error_status);`
Coroutine frame destruction          ──> Automatic RAII destruction of subfutures
Uninitialized / default state        ──> Default constructor setting `kUninitialized`
```

---

## 3. Step-by-Step Transformation Workflow

### Step 1: Draft the Mental Coroutine

Draft the linear asynchronous flow using `co_await`, loops, and early returns:

```cpp
// 1. Conceptual Coroutine (Mental or prototype)
Coro<pw::Result<SensorData>> ReadAndProcess(
    CoroContext, Sensor& sensor, Processor& proc, int max_retries = 3) {
  // Synchronous pre-validation
  if (max_retries <= 0) {
    co_return pw::Status::InvalidArgument();
  }

  // Suspension Point 1: Initialize sensor
  PW_CO_TRY(co_await sensor.Init());

  // Local state preserved across suspension points
  int attempts = 0;
  while (attempts < max_retries) {
    // Suspension Point 2: Read sensor
    pw::Result<RawData> raw = co_await sensor.Read();
    if (raw.ok()) {
      // Suspension Point 3: Process data asynchronously
      pw::Result<SensorData> processed = co_await proc.Process(*raw);
      if (processed.ok()) {
        co_return processed;
      }
    }
    attempts++;
  }
  co_return pw::Status::DeadlineExceeded();
}
```

---

### Step 2: Define the Future Class Members

From the mental coroutine, identify:

1. **Default Constructibility**: Futures must default-construct into an empty,
   non-pendable state (e.g. `State::kUninitialized`).
2. **The State Enum**: `kUninitialized`, each `co_await` point, and `kDone`.
3. **Preserved State**: References, arguments, and local variables that live
   across `co_await` calls.
4. **Child Subfutures**: Member variables for the futures returned by
   sub-operations.
5. **Private Constructor**: Restrict direct instantiation and friend the factory
   function.

```cpp
class ReadAndProcessFuture {
 public:
  using value_type = pw::Result<SensorData>;

  // Futures must be default-constructible to an empty non-pendable state and movable.
  ReadAndProcessFuture() = default;
  ReadAndProcessFuture(ReadAndProcessFuture&&) = default;
  ReadAndProcessFuture& operator=(ReadAndProcessFuture&&) = default;

  // Concept requirement: is_pendable() is false when uninitialized or complete.
  bool is_pendable() const {
    return state_ != State::kUninitialized && state_ != State::kDone;
  }

  // Concept requirement: is_complete() is true only after completion.
  bool is_complete() const { return state_ == State::kDone; }

  Poll<value_type> Pend(Context& cx);

 private:
  // Friend the factory function to enforce construction via validation
  friend ReadAndProcessFuture ReadAndProcess(Sensor& sensor,
                                             Processor& proc,
                                             int max_retries);

  ReadAndProcessFuture(Sensor& sensor, Processor& proc, int max_retries)
      : state_(State::kInit),
        sensor_(&sensor),
        proc_(&proc),
        max_retries_(max_retries) {}

  // Constructor for immediate synchronous failure (e.g. invalid arguments)
  explicit ReadAndProcessFuture(pw::Status error_status)
      : state_(State::kInit), early_error_(error_status) {}

  enum class State {
    kUninitialized,
    kInit,
    kReading,
    kProcessing,
    kDone,
  };

  // Default-initialized to kUninitialized (is_pendable() == false)
  State state_ = State::kUninitialized;
  pw::Status early_error_ = pw::OkStatus();

  // Preserved state
  Sensor* sensor_ = nullptr;
  Processor* proc_ = nullptr;
  int max_retries_ = 0;
  int attempts_ = 0;

  // Subfutures
  InitFuture init_future_;
  ReadFuture read_future_;
  ProcessFuture process_future_;
};

static_assert(pw::async2::Future<ReadAndProcessFuture>);
```

---

### Step 3: Implement `Pend()` with `while (true) switch`

Translate the sequential coroutine control flow into a state machine loop:

- Handle `State::kUninitialized` and `State::kDone` as invariant violations
  (`PW_CRASH`).
- Check for early synchronous failures first.
- Use `PW_AWAIT` to poll child subfutures.
- On completion, update preserved state, advance `state_`, and `continue` the
  loop immediately.
- On error/completion, set `state_ = State::kDone` and return `Ready()`.

```cpp
Poll<pw::Result<SensorData>> ReadAndProcessFuture::Pend(Context& cx) {
  while (true) {
    switch (state_) {
      case State::kUninitialized:
        PW_CRASH("Polled an uninitialized ReadAndProcessFuture");

      case State::kInit: {
        if (!early_error_.ok()) {
          state_ = State::kDone;
          return pw::async2::Ready(early_error_);
        }
        if (!init_future_.is_pendable()) {
          init_future_ = sensor_->Init();
        }
        PW_AWAIT(pw::Status status, init_future_, cx);
        if (!status.ok()) {
          state_ = State::kDone;
          return pw::async2::Ready(status);
        }
        state_ = State::kReading;
        continue;  // Advance immediately to pend the read_future_
      }

      case State::kReading: {
        if (!read_future_.is_pendable()) {
          read_future_ = sensor_->Read();
        }
        PW_AWAIT(pw::Result<RawData> raw, read_future_, cx);
        if (!raw.ok()) {
          if (++attempts_ >= max_retries_) {
            state_ = State::kDone;
            return pw::async2::Ready(pw::Status::DeadlineExceeded());
          }
          read_future_ = {};  // Reset for retry
          continue;
        }
        // Start processing the raw data
        process_future_ = proc_->Process(*raw);
        state_ = State::kProcessing;
        continue;
      }

      case State::kProcessing: {
        PW_AWAIT(pw::Result<SensorData> data, process_future_, cx);
        if (data.ok()) {
          state_ = State::kDone;
          return pw::async2::Ready(data);
        }
        if (++attempts_ >= max_retries_) {
          state_ = State::kDone;
          return pw::async2::Ready(pw::Status::DeadlineExceeded());
        }
        read_future_ = {};     // Reset read future for next attempt
        process_future_ = {};  // Reset process future
        state_ = State::kReading;
        continue;
      }

      case State::kDone:
        PW_CRASH("Polled a completed ReadAndProcessFuture");
    }
  }
}
```

---

### Step 4: Implement the Public Factory Function

The coroutine's signature directly becomes the public factory function:

- Perform synchronous argument validation.
- Return the `Future` object directly by value (never `pw::Result<Future>`).
- If validation fails, return an immediately resolving error future.

```cpp
inline ReadAndProcessFuture ReadAndProcess(Sensor& sensor,
                                           Processor& proc,
                                           int max_retries = 3) {
  if (max_retries <= 0) {
    return ReadAndProcessFuture(pw::Status::InvalidArgument());
  }
  return ReadAndProcessFuture(sensor, proc, max_retries);
}
```

---

## 4. Memory Footprint Optimization: Managing Disjoint Lifetimes

In a compiler-generated coroutine frame, the compiler performs lifetime analysis:
variables with non-overlapping lifetimes share the same memory in an internal
union.

When manually transforming a coroutine into a struct, a naive mapping makes
every transient future and intermediate variable a separate member field. In
multi-stage operations (e.g. handshake -> authenticate -> request -> transfer),
this creates an oversized class where only 1 or 2 members are active at a time.

### Solution: Group Disjoint State in `std::variant`

To achieve the same optimal memory layout as a compiler-generated coroutine
frame:

1. **Partition variables**:
   - **Persistent state**: Member variables needed across multiple states
     (e.g. device pointers, overall retry count, final result buffer).
   - **State-specific transient state**: Subfutures and intermediate buffers
     needed only during a single state.
2. Store transient state in a `std::variant`.

```cpp
class WorkflowFuture {
 public:
  using value_type = pw::Result<Response>;

  // Default constructible into empty, uninitialized state
  WorkflowFuture() = default;
  WorkflowFuture(WorkflowFuture&&) = default;
  WorkflowFuture& operator=(WorkflowFuture&&) = default;

  bool is_pendable() const {
    return !std::holds_alternative<std::monostate>(phase_) &&
           !std::holds_alternative<DonePhase>(phase_);
  }
  bool is_complete() const {
    return std::holds_alternative<DonePhase>(phase_);
  }

  Poll<value_type> Pend(Context& cx);

 private:
  // Persistent state (lives across all states)
  Device* device_ = nullptr;
  int retries_ = 0;

  // Disjoint phase state payloads
  struct HandshakePhase {
    HandshakeFuture future;
  };
  struct AuthPhase {
    AuthFuture future;
    AuthCredentials credentials;
  };
  struct TransferPhase {
    TransferFuture future;
    std::array<std::byte, 64> chunk_buffer;
  };
  struct DonePhase {};

  // Monostate represents uninitialized.
  std::variant<std::monostate,
               HandshakePhase,
               AuthPhase,
               TransferPhase,
               DonePhase> phase_;
};

static_assert(pw::async2::Future<WorkflowFuture>);
```

### Transitioning with `emplace`

When advancing states in `Pend()`, using `phase_.emplace<NextPhase>(...)`
automatically:

1. Destructs the previous phase, canceling outstanding futures and releasing
   any underlying resources/wakers via RAII.
2. Constructs the new phase with its futures.
3. Limits memory usage to the size of the largest phase.

```cpp
// Example state transition inside Pend(Context& cx)
if (auto* handshake = std::get_if<HandshakePhase>(&phase_)) {
  PW_AWAIT(pw::Status status, handshake->future, cx);
  if (!status.ok()) {
    phase_.emplace<DonePhase>();
    return pw::async2::Ready(status);
  }
  // Transition to auth phase: destroys HandshakePhase and constructs AuthPhase
  phase_.emplace<AuthPhase>(device_->Authenticate(credentials));
  continue;
}
```

---

## 5. Best Practices

1. **Default constructible to non-pendable**:
   Every `Future` must be default constructible to an empty, uninitialized state
   where `is_pendable() == false` and `is_complete() == false`.
2. **Factory function entry point**:
   Async APIs are invoked via public factory functions that validate inputs
   synchronously and return composite futures by value.
3. **Composite futures are mid-level building blocks**:
   Mid-level orchestration should almost always be a composite future returned by
   value, **not** a `Task`.
4. **Immediate loop-through (`continue`)**:
   Always advance state machines using `continue` inside `while (true) switch`.
   Returning `Pending()` between state transitions prevents registering the next
   subfuture's waker and can cause the system to hang.
5. **Partition persistent vs. transient state**:
   Avoid storing 10 flat subfuture members in a single struct. Use `std::variant`
   to overlap disjoint subfutures and intermediate buffers, keeping future size
   minimal.
6. **Lazy subfuture allocation**:
   Provision child subfutures lazily inside `Pend()` via `if (!subfuture_.is_pendable())`,
   or upon transitioning into that state.
