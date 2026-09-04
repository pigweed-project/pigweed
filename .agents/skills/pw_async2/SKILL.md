---
name: pw_async2
description: >-
  Best practices for pw_async2 development and testing, covering
  futures, values, tasks, dispatchers, polling, and coroutines.
---

# pw_async2 development instructions and skill guide

Use this skill when writing, reviewing, refactoring, or testing code that uses
Pigweed's pw_async2 cooperative asynchronous framework.

---

## 1. System topology and the 3-tier execution graph

Structure pw_async2 systems using a strict 3-tier execution graph:

1. **Top level: Tasks (posted to dispatcher)**
   - Inherits from `pw::async2::Task` or uses `CoroTask`/`FuncTask`.
   - Registered with `Dispatcher`; holds task lifecycle state.
2. **Middle level: Composite futures and async helper functions**
   - Pure C++ value objects returned directly on the stack.
   - NO `FutureCore`, NO `Waker`s, NO providers, NO dynamic allocation.
   - Composes subfutures inline, transitively passing `Context&`.
3. **Leaf level: Primitive wakeable futures**
   - `ValueFuture<T>`, `TimeFuture<Clock>`, `Channel` Receiver/Sender.
   - Uses `FutureCore` / `Waker` / Provider; interacts with ISRs/timers.

### Directives

- **Top-level tasks**: Use `pw::async2::Task` **only** for top-level entry
  points (background worker loops, main event loops). Do NOT make mid-level
  operations `Task`s.
- **Mid-level logic**: Implement mid-level logic as **composite futures**
  returned by **factory functions**. Store child futures inline as member
  variables.
- **Leaf futures**: Use primitive futures (`ValueFuture`, `TimeFuture`,
  `Channel`) or `FutureCore` **only** when interfacing directly with hardware,
  timers, or event providers.

### Top-down design and the coroutine mental model

Design asynchronous code **top-down** from the caller's perspective rather than
**bottom-up** from low-level wakers or hardware polling.

The recommended mental model for writing a composite future is:

1. **Draft the sequential API as a coroutine** (mental or prototype) using
   `co_await`, loops, and early returns.
2. **Derive the factory function and manual state machine**:
   - Coroutine signature & validation -> Public factory function returning the Future by value.
   - Variables surviving across `co_await` -> class member fields.
   - `co_await` suspension points -> state enum and child subfuture members.
   - Non-overlapping transient state / futures -> group into `std::variant` to optimize memory layout.
   - Sequential control flow -> `while (true) switch (state_)` loop in `Pend()`.
   - `PW_CO_TRY` and return values -> `Ready()` returns.

> [!TIP]
> For a detailed guide on top-down design, the step-by-step transformation
> workflow, and bottom-up pitfalls to avoid, view
> [references/top_down_design.md](./references/top_down_design.md).

---

## 2. What is a future? (Concept and invariants)

A `Future` is a stack-allocated state machine representing an asynchronous
operation that produces a `value_type` upon completion.

### The `Future` C++ concept

Futures do **NOT** derive from a common polymorphic base class. Instead, they
satisfy the `pw::async2::Future` concept defined in `pw_async2/future.h`. Any
concrete type `F` is a `Future` if it exposes:

- `typename F::value_type`: Result type (e.g. `pw::Result<int>`, `pw::Status`,
  `void`).
- `Poll<value_type> Pend(Context& cx)`: Advances the operation. Returns
  `Ready(T)` when complete or `Pending()` when blocked.
- `bool is_pendable() const`: Returns `true` if active/pollable (`false` when
  default-constructed / uninitialized or completed).
- `bool is_complete() const`: Returns `true` if completed.
- Special member functions: Default-constructible (initializes to an empty,
  non-pendable state), destructible, movable.

> [!TIP]
>
> **ALWAYS verify custom future types with `static_assert`**: Immediately after
> defining any custom future class, add a static assertion to enforce compliance
> with `pw::async2::Future` at compile time:
>
> ```cpp
> static_assert(pw::async2::Future<MyCustomFuture>);
> ```

### Fundamental invariants

1. **Empty default state**: Default-constructing a future initializes it to an
   empty, uninitialized state where `is_pendable()` and `is_complete()` return
   `false`. Polling an uninitialized future is an error (`PW_CRASH`).
2. **Lazy execution**: Creating a future executes no work. Work occurs only when
   polled via `Pend(Context& cx)`.
3. **Explicit single ownership**: The caller owns the `Future` by value on the
   stack or inline inside a parent struct. There is no `std::shared_ptr` or
   hidden heap allocation.
4. **Zero-cost RAII cancellation**: Destructing a future cancels the operation
   immediately and synchronously. Leaf futures unlist from providers and drop
   wakers; composite futures recursively destruct child subfutures inline.
5. **Single-use completion**: Once `Pend()` returns `Ready()`, the operation is
   final. **Do NOT poll a completed future again.**

---

## 3. Futures vs. pendable functions

Avoid writing **pendable functions**—helper methods or standalone functions that
accept `Context& cx` and return `Poll<T>` directly:

```cpp
// AVOID: Pendable helper function
Poll<pw::Result<int>> PendReadSensor(Context& cx,
                                     Sensor& sensor,
                                     int& retries);
```

Pendable functions operate within pw_async2's informed poll model, but they
make the asynchronous contract bespoke and inscrutable:

- **Opaque state and ownership**: It is impossible to tell from the signature
  what state the function maintains, where that state is stored, or who
  currently owns it. State easily leaks across parent Task member variables or
  lambda captures, creating brittle, error-prone state management.
- **No self-describing lifecycle**: A pendable function cannot report whether it
  is pendable or complete (`is_pendable()`, `is_complete()`), forcing callers
  to maintain external flags to track execution state.
- **No RAII cancellation**: A pendable function cannot be cleanly canceled by
  dropping a value object. Destructing a task with active pendable
  sub-operations risks leaving underlying resources or timers in indeterminate
  states.
- **Incompatible with combinators and coroutines**: You cannot `co_await`,
  `PW_AWAIT`, `Select`, or `Join` an arbitrary `Poll<T>` function. All
  pw_async2 combinators, macros, and coroutines require a concrete type
  fulfilling the `Future` concept.

### Exceptions (when pendable functions are acceptable)

Pendable functions should be avoided for general application logic and
public/module APIs. They are acceptable **only** for very low-level internal
helper functions that are strictly private to a single class or file and
consumed from a single caller by design.

In all other cases, wrap mid-level async operations in a concrete `Future` type
returned by value.

---

## 4. Implementing composite and leaf futures

### A. Leaf futures (external event signals)

Leaf futures interact directly with event providers (hardware interrupts,
timers, etc.) and manage waker registration.

> [!NOTE] > **Prefer pre-built primitives**: In most cases, do NOT write a custom leaf
> future from scratch. Use existing primitives like `ValueFuture<T>` (with
> `ValueProvider` or `ValueListProvider`), `Notification`, or `Channel`.
> Pre-built primitives handle synchronization, thread safety, and waker
> management automatically.

When a custom leaf future **is** required (e.g. interfacing with a low-level
hardware driver):

1. Store a `pw::async2::FutureCore` member to manage wakers, intrusive list
   membership, and state tracking.
2. **Movability and intrusive relocation**: Leaf futures must be
   move-constructible and move-assignable (`ButtonFuture(ButtonFuture&&) =
default;`). `FutureCore` handles movability automatically: when a future is
   moved by value (e.g. returned from a factory function or stored in a task
   member), `FutureCore` automatically updates its intrusive list pointers in
   the provider's `FutureList` and transfers the `Waker` to the new memory
   location. Note that `FutureCore` itself is lock-free; synchronizing access
   across threads or ISR contexts is the responsibility of the user (e.g. via
   spinlocks, mutexes, or global locks).
3. Delegate public `Pend(cx)` to `core_.DoPend(*this, cx)`.
4. Implement a private `DoPend(Context& cx)` callback friended by `FutureCore`.
5. Store pending futures in a `pw::async2::FutureList` inside the provider
   class.

For full implementation patterns, see:

- Documentation:
  [pw_async2/futures.rst](../../../pw_async2/futures.rst) (Section:
  _Implementing a future_)
- Complete Example:
  [custom_future.cc](../../../pw_async2/examples/custom_future.cc)

### B. Composite futures (mid-level operations)

Composite futures encapsulate multi-step asynchronous operations without using
`FutureCore` or registering wakers:

- **Do NOT** inherit from `FutureCore` or use `Waker`s.
- Store child subfutures inline as member variables.
- Pass `Context& cx` down transitively into subfutures via `PW_AWAIT` or
  `subfuture_.Pend(cx)`.
- Loop state transitions immediately (`while (true) switch (state_)`) to pend
  newly created subfutures within the same `Pend(cx)` call. Returning
  `Pending()` before pending a new subfuture prevents waker registration,
  causing the task to stall indefinitely.

For step-by-step guidance on transforming a sequential coroutine mental model
into a manual composite future, see
[references/top_down_design.md](./references/top_down_design.md).

For full implementation patterns, see:

- Documentation:
  [pw_async2/futures.rst](../../../pw_async2/futures.rst) (Section:
  _Implementing a composite future_)
- Complete Example:
  [composite_future.cc](../../../pw_async2/examples/composite_future.cc)

---

## 5. Async API design rules

### 1. Direct return of futures

Async functions **MUST** return `Future` objects directly by value. Do **NOT**
wrap futures in `pw::Result<Future>` or `std::optional<Future>`.

**Why:**

- **Composability**: Returning a future directly allows callers to seamlessly
  pass the returned future to combinators (`Select`, `Join`), macros
  (`PW_AWAIT`), or coroutines (`co_await`). Wrapping the future in a
  `pw::Result` or `std::optional` breaks composition, forcing callers to
  perform awkward nested unwrapping before polling.
- **Fallible operations**: If an operation can fail (or fail to produce a
  value), move the `pw::Result<T>`, `pw::Status`, or `std::optional<T>`
  **inside** the future's result type (`value_type`), e.g.,
  `ReadSensorFuture::value_type` is `pw::Result<int>`.
- **Synchronous failures**: If an operation fails synchronously during factory
  argument validation, return a future that immediately resolves to that error
  state rather than failing out-of-band.

```cpp
// ✅ CORRECT: Returns Future directly; fallibility lives inside value_type
// (pw::Result<int>)
ReadSensorWithRetryFuture ReadSensorWithRetry(Sensor& sensor, ...);

// ❌ WRONG: Wrapping the Future itself breaks composability and co_await
pw::Result<ReadSensorWithRetryFuture> ReadSensorWithRetry(Sensor& sensor);  // ❌
std::optional<ReadSensorWithRetryFuture> TryReadSensor(Sensor& sensor);     // ❌
```

### 2. Synchronous argument validation

Validate arguments synchronously in the factory function before future
construction. Return an immediately-resolving error future if validation
fails:

```cpp
inline ReadSensorWithRetryFuture ReadSensorWithRetry(
    MockSensor& sensor,
    SimulatedTimeProvider<SystemClock>& time_provider,
    int max_retries = 3) {
  if (max_retries <= 0) {
    return ReadSensorWithRetryFuture(pw::Status::InvalidArgument());
  }
  return ReadSensorWithRetryFuture(sensor, time_provider, max_retries);
}
```

### 3. Enforce factory construction

Make composite future constructors `private` and friend the factory function to
prevent callers from bypassing validation.

### 4. Strict naming conventions

- **Future-returning functions**: Name for the operation (`Read()`, `Write()`).
  **NEVER** use `Async` in function names (`AsyncRead()` ❌) or name after
  future types (`GetReadFuture()` ❌).
- **Non-blocking immediate operations**: Prefix with `Try` (`std::optional<T>
TryRead()`).
- **Thread-blocking operations**: Prefix with `Blocking` (`pw::Result<T>
BlockingRead()`).

---

## 6. Tasks and lifecycle management

### Creating tasks

Subclass `pw::async2::Task` and implement `DoPend(Context& cx)`:

```cpp
class SensorReaderTask : public pw::async2::Task {
 public:
  SensorReaderTask(MockSensor& sensor,
                   SimulatedTimeProvider<SystemClock>& time_provider)
      : sensor_(&sensor), time_provider_(&time_provider) {}

  pw::Result<int> result() const { return result_; }

 private:
  Poll<> DoPend(Context& cx) override {
    // Provision futures on first poll
    if (!read_retry_future_.is_pendable()) {
      read_retry_future_ = ReadSensorWithRetry(*sensor_, *time_provider_, 2);
    }
    PW_AWAIT(auto res, read_retry_future_, cx);
    result_ = res;
    return pw::async2::Ready();
  }

  MockSensor* sensor_ = nullptr;
  SimulatedTimeProvider<SystemClock>* time_provider_ = nullptr;
  ReadSensorWithRetryFuture read_retry_future_;
  pw::Result<int> result_ = pw::Status::Unknown();
};
```

> [!IMPORTANT] > **Lazy Future Provisioning**: Do **NOT** construct futures in a `Task`'s
> constructor. Constructing futures eagerly in constructors can trigger side
> effects (e.g. initiating hardware transactions or timer registrations) before
> the task is posted to or polled by a dispatcher, defeating lazy execution
> invariants. Always provision subfutures lazily inside `DoPend(Context& cx)`
> when `!subfuture_.is_pendable()`.

### Task deregistration requirement (CRITICAL)

Every posted task **MUST** be explicitly deregistered before destruction:

```cpp
SensorReaderTask task(sensor, time_provider);
dispatcher.Post(task);

// ... run dispatcher ...

// MUST call Deregister() or Join() before destruction!
task.Deregister();
```

---

## 7. C++20 coroutines (`pw::async2::Coro`)

> [!NOTE]
> Prefer manual state machines using composite futures and `PW_AWAIT` by default.
> C++20 coroutines require dynamic allocation for coroutine frames via
> `CoroContext` / `pw::Allocator`, introducing a opaque allocations whose sizes
> vary by toolchain, causing heap fragmentation risks and runtime allocation
> failure paths. Handwritten futures provide deterministic inline storage.
>
> However, **always use the coroutine mental model** to design the control flow
> before writing the manual future (see
> [references/top_down_design.md](./references/top_down_design.md)).
> Use `Coro<T>` when coroutines are explicitly requested or already established
> in the codebase.

If working with coroutines, view
[references/coroutines.md](./references/coroutines.md) for detailed patterns,
error handling with `PW_CO_TRY` / `PW_CO_TRY_ASSIGN`, and memory guidelines.

---

## 8. Channels (`pw::async2::Channel`)

If working with channels for message passing or sync-to-async bridging, view
[references/channels.md](./references/channels.md) for channel creation
(SPSC/MPSC/SPMC/MPMC), `Send`/`Receive` futures, and non-blocking
`Try`/`Blocking` APIs.

---

## 9. Testing async code

If writing unit tests for pw_async2 tasks, futures, channels, or
time-dependent operations, view [references/testing.md](./references/testing.md)
for deterministic unit testing patterns with `DispatcherForTest` and
`SimulatedTimeProvider`.

---

## 10. Rules and anti-pattern checklist

- [ ] **Top-down design & memory layout**: Draft APIs top-down using the
      coroutine mental model before implementing manual composite futures; group
      disjoint transient subfutures in `std::variant` to minimize struct size.
- [ ] **Verify Future concept**: Always verify custom future types with
      `static_assert(pw::async2::Future<MyCustomFuture>);`.
- [ ] **Prefer manual state machines**: Use composite futures and `PW_AWAIT` by
      default to avoid opaque dynamic coroutine frame allocations. Only use C++20
      coroutines (`Coro<T>`) if explicitly requested or already established in the
      codebase.
- [ ] **Avoid pendable functions**: Do not write functions accepting `Context&`
      and returning `Poll<T>` for general application logic. Construct and return a
      concrete `Future` type by value instead (except for low-level internal
      helpers private to a single caller).
- [ ] **No implicit dynamic allocation**: Core futures and tasks must be
      stack/inline allocated; channels support both allocator-backed
      (`pw::allocator::Allocator`) and static (`ChannelStorage`) allocation.
- [ ] **No `Async` naming**: Name functions `Read()`, not `AsyncRead()`.
- [ ] **Direct future return**: Return `Future<T>` directly by value, not
      `Result<Future>`.
- [ ] **Task deregistration**: Call `task.Deregister()` or `task.Join()` before
      destroying a posted task.
- [ ] **Lazy future provisioning**: Provision subfutures lazily inside
      `DoPend()` (when `!future.is_pendable()`), never eagerly in constructors.
- [ ] **Completed future invariant**: Never call `Pend()` on a future after it
      returns `Ready()`.
- [ ] **State machine loop-through**: Loop state transitions in `Pend()`
      immediately (`while (true) switch`) to ensure newly created subfutures are
      pended immediately so their wakers get registered.
- [ ] **TimeProvider injection**: Inject `TimeProvider<Clock>&` for time
      operations; never use static wall-clock sleeps.
- [ ] **Deterministic tests**: Use `DispatcherForTest::RunUntilStalled()` and
      `SimulatedTimeProvider::AdvanceUntilNextExpiration()`.
