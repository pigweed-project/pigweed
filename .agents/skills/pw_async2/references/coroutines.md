# C++20 coroutines (`pw::async2::Coro`)

---

## 1. Defining a coroutine

A pw_async2 coroutine function must:
1. Return `pw::async2::Coro<T>` (e.g. `Coro<pw::Status>`,
   `Coro<pw::Result<int>>`, `Coro<void>`).
2. Accept `pw::async2::CoroContext` as its first parameter by value.
3. Use `co_await` on futures or sub-coroutines and `co_return` to return
   results.

```cpp
#include "pw_async2/coro.h"

pw::async2::Coro<pw::Status> ReadAndProcess(
    pw::async2::CoroContext coro_cx,
    Sensor& sensor) {
  PW_CO_TRY_ASSIGN(int raw_val, co_await sensor.Read());
  PW_CO_TRY(co_await sensor.Calibrate());
  co_return pw::OkStatus();
}
```

`CoroContext` is implicitly constructible from a `pw::Allocator&`.

---

## 2. Error propagation (`PW_CO_TRY` and `PW_CO_TRY_ASSIGN`)

Standard `PW_TRY` and `PW_TRY_ASSIGN` use `return`, which is invalid inside a
C++20 coroutine. Use `PW_CO_TRY` and `PW_CO_TRY_ASSIGN` to perform early
`co_return` on error:

```cpp
pw::async2::Coro<pw::Result<int>> ReadSensorDouble(
    pw::async2::CoroContext coro_cx, Sensor& sensor) {
  // Returns early via co_return if Read() resolves to a non-OK status
  PW_CO_TRY_ASSIGN(int raw_val, co_await sensor.Read());

  PW_CO_TRY(co_await sensor.Calibrate());

  co_return raw_val * 2;
}
```

---

## 3. Running coroutines (`CoroTask` and `FallibleCoroTask`)

Coroutines are executed inside tasks posted to a `Dispatcher`.

### Infallible task (`CoroTask`)

`CoroTask` wraps a `Coro<T>`. If coroutine frame allocation fails, pending a
`CoroTask` causes a `PW_CRASH`.

```cpp
#include "pw_async2/coro_task.h"

pw::async2::CoroTask task = ReadAndProcess(allocator, sensor);
dispatcher.Post(task);
```

### Fallible task (`FallibleCoroTask`)

`FallibleCoroTask` handles coroutine frame allocation failures by calling an
error handler closure instead of crashing.

```cpp
#include "pw_async2/fallible_coro_task.h"

pw::async2::FallibleCoroTask task(
    ReadAndProcess(allocator, sensor),
    [] { PW_LOG_ERROR("Coroutine frame allocation failed"); });
dispatcher.Post(task);
```

---

## 4. Asynchronous generators (`Generator<T>`)

`Generator<T>` produces a stream of values using `co_yield`. Consumers consume
values asynchronously using `co_await gen.Next()`.

```cpp
#include "pw_async2/coro.h"

pw::async2::Generator<int> CountUp(pw::async2::CoroContext coro_cx, int max) {
  for (int i = 0; i < max; ++i) {
    co_yield i;
  }
}

pw::async2::Coro<pw::Status> ConsumeCount(
    pw::async2::CoroContext coro_cx) {
  pw::async2::Generator<int> gen = CountUp(coro_cx, 5);
  while (true) {
    std::optional<int> val = co_await gen.Next();
    if (!val.has_value()) {
      break; // Generator finished
    }
    PW_LOG_INFO("Got: %d", *val);
  }
  co_return pw::OkStatus();
}
```

---

## 5. Memory model and allocation rules

1. **Coroutine Frame**: The compiler constructs a coroutine frame on the
   allocator passed via `CoroContext`.
2. **Implicit Allocation**: Even if `CoroTask` is stack-allocated, the
   underlying coroutine frame requires allocation via `CoroContext`.
3. **Allocation Verification**: Check `coro.ok()` to verify whether coroutine
   frame allocation succeeded.

