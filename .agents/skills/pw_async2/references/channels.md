# Channels (`pw::async2::Channel`)

`pw::async2::Channel` provides thread-safe, multi-producer/multi-consumer
asynchronous message passing between tasks on the same dispatcher, across
dispatchers, or between asynchronous tasks and synchronous threads/ISRs.

---

## 1. Topologies and channel creation

Channels support SPSC, MPSC, SPMC, and MPMC topologies (up to 255 producers or
consumers). Channels use either static storage (`ChannelStorage<T, N>`) or
dynamic allocation (`pw::allocator::Allocator`).

### Static storage (`ChannelStorage`)

```cpp
#include "pw_async2/channel.h"

// 1. SPSC: returns [channel_handle, sender, receiver]
pw::async2::ChannelStorage<int, 10> spsc_storage;
auto [spsc_handle, sender, receiver] =
    pw::async2::CreateSpscChannel(spsc_storage);
spsc_handle.Release();

// 2. MPSC: returns [channel_handle, receiver]
pw::async2::ChannelStorage<int, 10> mpsc_storage;
auto [mpsc_handle, receiver] = pw::async2::CreateMpscChannel(mpsc_storage);
auto sender1 = mpsc_handle.CreateSender();
auto sender2 = mpsc_handle.CreateSender();
mpsc_handle.Release();

// 3. SPMC: returns [channel_handle, sender]
pw::async2::ChannelStorage<int, 10> spmc_storage;
auto [spmc_handle, sender] = pw::async2::CreateSpmcChannel(spmc_storage);
auto receiver1 = spmc_handle.CreateReceiver();
auto receiver2 = spmc_handle.CreateReceiver();
spmc_handle.Release();

// 4. MPMC: returns MpmcChannelHandle
pw::async2::ChannelStorage<int, 10> mpmc_storage;
auto mpmc_handle = pw::async2::CreateMpmcChannel(mpmc_storage);
auto sender = mpmc_handle.CreateSender();
auto receiver = mpmc_handle.CreateReceiver();
mpmc_handle.Release();
```

### Dynamic allocation (`pw::allocator::Allocator`)

Dynamic channel creation functions return `std::optional<...>` containing the
handle/endpoints tuple. If allocation fails, the optional is empty
(`std::nullopt`).

```cpp
auto result = pw::async2::CreateSpscChannel<int>(allocator, 10);
if (!result.has_value()) {
  // Allocation failed
  return;
}
auto [handle, sender, receiver] = std::move(*result);
handle.Release();
```

---

## 2. Channel lifetime and handle ownership

A channel remains open as long as:
1. At least one `ChannelHandle` is active, **OR**
2. At least one `Sender` AND at least one `Receiver` are active.

### Handle release

Calling `handle.Release()` drops the handle reference while leaving existing
senders and receivers active.

- If handles are **not** released, the channel remains open indefinitely even
  if all senders or receivers are destroyed.
- Always call `handle.Release()` after creating the required senders and
  receivers unless the handle is intentionally retained to call
  `handle.Close()`.

### Channel closing

- **Senders dropped (0 active senders, 0 active handles)**: Channel closes.
  Receivers can continue to drain buffered items. Once empty,
  `receiver.Receive()` resolves to `std::nullopt`.
- **Receivers dropped (0 active receivers, 0 active handles)**: Channel closes
  immediately. Any pending or future `sender.Send()` resolves to `false`.
- **Explicit Close (`handle.Close()`)**: Closes channel immediately regardless
  of active endpoint counts.

---

## 3. Asynchronous operations (`Send` and `Receive`)

### Sending (`Sender::Send`)

`sender.Send(value)` returns a `SendFuture<T>`.

- Resolves to `bool`: `true` if sent; `false` if the channel is closed.
- If full, the future waits until space becomes available.

```cpp
// In Task::DoPend(Context& cx)
if (!send_future_.is_pendable()) {
  send_future_ = sender_.Send(42);
}
PW_AWAIT(bool sent, send_future_, cx);
if (!sent) {
  // Channel closed
}
```

### Receiving (`Receiver::Receive`)

`receiver.Receive()` returns a `ReceiveFuture<T>`.

- Resolves to `std::optional<T>`: contains the item when received, or
  `std::nullopt` if closed and empty.

```cpp
// In Task::DoPend(Context& cx)
if (!receive_future_.is_pendable()) {
  receive_future_ = receiver_.Receive();
}
PW_AWAIT(std::optional<int> item, receive_future_, cx);
if (!item.has_value()) {
  // Channel closed and empty
} else {
  int value = *item;
}
```

---

## 4. Deferred emplace (`ReserveSend`)

`Sender::ReserveSend()` reserves capacity in the channel before constructing
the value. This avoids unnecessary copies or moves for expensive types.

- Returns `ReserveSendFuture<T>`, resolving to
  `std::optional<SendReservation<T>>`.
- `reservation->Commit(args...)`: Constructs the value in-place in the channel
  buffer.
- If `SendReservation` is destroyed without calling `Commit()`, the
  reservation is canceled and capacity is released.

```cpp
if (!reserve_future_.is_pendable()) {
  reserve_future_ = sender_.ReserveSend();
}
PW_AWAIT(std::optional<pw::async2::SendReservation<MyStruct>> reservation,
         reserve_future_, cx);
if (reservation.has_value()) {
  reservation->Commit(arg1, arg2);
}
```

---

## 5. Synchronous APIs (`Try` and `Blocking`)

Use synchronous APIs when interacting with channels from ISRs, non-blocking
callbacks, or synchronous OS threads.

### Non-blocking / ISR-safe (`TrySend` / `TryReceive` / `TryReserveSend`)

- `sender.TrySend(value)` -> `pw::Status`:
  - `OkStatus()`: Value enqueued.
  - `Status::Unavailable()`: Channel full.
  - `Status::FailedPrecondition()`: Channel closed.
- `receiver.TryReceive()` -> `pw::Result<T>`:
  - `OkStatus()`: Value returned in `pw::Result<T>`.
  - `Status::Unavailable()`: Channel empty.
  - `Status::FailedPrecondition()`: Channel closed and empty.
- `sender.TryReserveSend()` -> `pw::Result<SendReservation<T>>`:
  - Returns `SendReservation` if space is available.

### Thread-blocking (`BlockingSend` / `BlockingReceive`)

Blocks the calling OS thread until the operation completes or timeout elapses.
Requires passing a `Dispatcher&`.

```cpp
// On a synchronous OS thread:
pw::Status status = sender.BlockingSend(dispatcher, 42, timeout);

pw::Result<int> result = receiver.BlockingReceive(dispatcher, timeout);
```

---

## 6. Notification channels (`Channel<void>`)

`Channel<void>` is optimized for signaling events without payload data. It
stores a single integer counter instead of a queue, reducing memory overhead.

### API differences for `Channel<void>`

- **`Send()`**:
  - `Channel<T>`: `Send(T value)` -> `SendFuture<T>` (resolves to `bool`)
  - `Channel<void>`: `Send()` -> `SendFuture<void>` (resolves to `bool`)
- **`TrySend()`**:
  - `Channel<T>`: `TrySend(T value)` -> `pw::Status`
  - `Channel<void>`: `TrySend()` -> `pw::Status` (no argument)
- **`BlockingSend()`**:
  - `Channel<T>`: `BlockingSend(dispatcher, T, [timeout])` -> `pw::Status`
  - `Channel<void>`: `BlockingSend(dispatcher, [timeout])` -> `pw::Status`
- **`reservation.Commit()`**:
  - `Channel<T>`: `reservation.Commit(args...)`
  - `Channel<void>`: `reservation.CommitNotification()`
- **`TryReceive()`**:
  - `Channel<T>`: `TryReceive()` -> `pw::Result<T>`
  - `Channel<void>`: `TryReceive()` -> `pw::Status`
- **`BlockingReceive()`**:
  - `Channel<T>`: `BlockingReceive(dispatcher)` -> `pw::Result<T>`
  - `Channel<void>`: `BlockingReceive(dispatcher)` -> `pw::Status`
- **`Receive()`**:
  - `Channel<T>`: `Receive()` -> `ReceiveFuture<T>` (resolves to
    `std::optional<T>`)
  - `Channel<void>`: `Receive()` -> `ReceiveFuture<void>` (resolves to `bool`)

---

## 7. Capacity and status queries

- `sender.capacity()` / `receiver.capacity()`: Total channel buffer capacity.
- `sender.remaining_capacity()` / `receiver.remaining_capacity()`: Available
  buffer slots (accounting for active reservations).
- `sender.is_open()` / `receiver.is_open()`: Returns `true` if channel is open.

