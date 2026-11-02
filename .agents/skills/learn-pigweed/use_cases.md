# Example learning use cases

A catalog of realistic `/learn-pigweed` requests across Pigweed, used to
evaluate the skill (see [`TEST.md`](TEST.md)). Each case lists:
- the learner's opening;
- its **scope class**: scoped, large, vague, or lost;
- whether the session-size tripwire should fire;
- a good first session;
- a suggested sequence for multi-session cases;
- likely misconceptions, written so they can seed a simulated learner.

Paths point at where the material lives. The teaching agent must still read
and verify the source, because these notes can go stale.

Scope classes:

- **Scoped:** fits one session (about one page of notes). The tripwire must
  **not** fire and no planning ceremony should be added.
- **Large:** a legitimate goal that needs several sessions. The tripwire
  fires; Orient mode is optional.
- **Vague:** the goal is unclear. Clarify it; usually Orient.
- **Lost:** the learner has a real task but doesn't know which parts of
  Pigweed matter. Orient, anchored on the task.

## Scoped

### UC-01: pw_rpc server-side dispatch

- **Opening:** "How does pw_rpc dispatch an incoming packet from a channel to
  my service method?"
- **Tripwire:** no. **Mode:** Understand or Explore.
- **First session:** `Server::ProcessPacket` decodes the packet, looks up the
  channel, finds the service and method by numeric IDs, and invokes the method
  synchronously on the caller's thread under the RPC lock; streaming packets
  are routed to existing calls (`pw_rpc/server.cc`, `pw_rpc/design.rst`).
- **Misconceptions:** "Service and method names are sent as strings on the
  wire." "pw_rpc spawns a thread per request."

### UC-02: pw_tokenizer internals

- **Opening:** "How does PW_TOKENIZE_STRING turn a string into a number
  without storing the string on the device?"
- **Tripwire:** no. **Mode:** Understand or Explore.
- **First session:** compile-time 65599 hash, entries in a non-loaded
  `.pw_tokenizer.entries` ELF section, host-side database, argument encoding
  (`pw_tokenizer/public/pw_tokenizer/tokenize.h`,
  `pw_tokenizer/public/pw_tokenizer/hash.h`,
  `pw_tokenizer/token_databases.rst`).
- **Misconceptions:** "The device hashes the string at runtime." "Format
  strings are still in flash." "Hashes never collide."

### UC-03: pw::Status and pw::Result

- **Opening:** "Why does Pigweed use pw::Status and pw::Result instead of
  exceptions or errno?"
- **Tripwire:** no. **Mode:** Understand.
- **First session:** `Status` is a small `[[nodiscard]]` value; `PW_TRY` and
  `PW_TRY_ASSIGN` propagate errors; `Result<T>` holds a value or a status with
  no heap (`pw_status/public/pw_status/status.h`,
  `pw_status/public/pw_status/try.h`, `pw_result/public/pw_result/result.h`).
- **Misconceptions:** "Ignoring a returned Status is harmless." (It is
  `[[nodiscard]]`, a warning that Pigweed builds treat as an error.)
  "Result<T> throws if I access a value that isn't there."

### UC-04: pw::Function without a heap

- **Opening:** "Can I use pw::Function for callbacks on a microcontroller
  without blowing up my heap?"
- **Tripwire:** no. **Mode:** Understand.
- **First session:** inline callable storage, sized by
  `PW_FUNCTION_INLINE_CALLABLE_SIZE`; dynamic allocation off by default;
  captures that don't fit fail to compile; `InlineFunction`, `Closure`,
  `Callback` (`pw_function/public/pw_function/function.h`,
  `pw_function/public/pw_function/config.h`).
- **Misconceptions:** "pw::Function silently heap-allocates big captures."
  "pw::Function is just a function pointer."

### UC-05: Fixed-capacity and intrusive containers

- **Opening:** "I need to store a list of registered drivers without malloc;
  how do pw::Vector and IntrusiveList work?"
- **Tripwire:** no. **Mode:** Explore.
- **First session:** `Vector<T, N>` inline storage and the size-generic
  `Vector<T>&` base; intrusive lists embed links in the items, so insertion
  never allocates (`pw_containers/public/pw_containers/vector.h`,
  `pw_containers/public/pw_containers/intrusive_list.h`,
  `pw_containers/lists.rst`).
- **Misconceptions:** "One object can be in two IntrusiveLists at once."
  "Functions taking a Vector must template on its capacity."

### UC-06: How PW_LOG reaches a backend

- **Opening:** "How does PW_LOG() end up on my serial console?"
- **Tripwire:** no. **Mode:** Understand or Explore.
- **First session:** `PW_LOG` expands to the backend's `PW_HANDLE_LOG`
  through the facade's backend header; the backend is chosen by the build
  (`pw_log/public/pw_log/log.h`, `pw_log/backends.rst`,
  `docs/sphinx/facades.rst`).
- **Misconceptions:** "PW_LOG calls a virtual method on a logger singleton."
  "Log strings always end up in flash." (Not with `pw_log_tokenized`.)

### UC-07: How one facade picks its backend in Bazel

- **Opening:** "In Bazel, how does pw_log know which backend to use for my
  target?"
- **Tripwire:** no. This is a scoped slice of the facade topic; do **not**
  turn it into a facade curriculum. **Mode:** Explore.
- **First session:** the `label_flag` backend in `pw_log/BUILD.bazel`, how a
  platform or `.bazelrc` sets it, and why the facade target depends on the
  flag rather than on a backend (`docs/sphinx/facades.rst`,
  `pw_build/bazel.rst`).
- **Misconceptions:** "My code has to depend on the backend directly."
  "Backends are chosen at runtime."

## Large

### UC-08: pw_kernel from the ground up

- **Opening:** "Teach me how pw_kernel works from the ground up."
- **Tripwire:** yes. **Mode:** Orient (or ask for the task, then propose
  units).
- **Sequence:**
  1. Static architecture and the object/handle model.
  2. Signals, waiting, and wait groups.
  3. Channels and IPC.
  4. The scheduler and threads.
  5. Syscalls and architecture ports.
  (`pw_kernel/tour.rst`, `pw_kernel/design.rst`, `pw_kernel/kernel/object.rs`,
  `pw_kernel/kernel/syscall.rs`)
- **Misconceptions:** "Kernel objects are heap-allocated at runtime."
  "Handles are global IDs any process can use."

### UC-09: pw_async2

- **Opening:** "Teach me pw_async2, I want to use it for concurrent sensor
  sampling."
- **Tripwire:** yes. **Mode:** Orient.
- **Sequence:**
  1. The informed-poll model and `Poll`.
  2. Tasks, the dispatcher, and wakers.
  3. Futures and composition.
  4. Coroutines.
  5. Channels.
  (`pw_async2/docs.rst`, `pw_async2/informed_poll.rst`,
  `pw_async2/tasks.rst`, `pw_async2/dispatcher.rst`, `pw_async2/futures.rst`,
  `pw_async2/coroutines.rst`)
- **Misconceptions:** "The dispatcher busy-polls every task." "Each task needs
  its own RTOS thread." "Returning Pending without arranging a wake is fine."

### UC-10: pw_allocator

- **Opening:** "Teach me how pw_allocator works and how to set up memory pools
  for embedded targets."
- **Tripwire:** yes. **Mode:** Orient or Understand with a proposed split.
- **Sequence:**
  1. The `Allocator` interface and `Layout`.
  2. Block allocators and fragmentation.
  3. Forwarding allocators: tracking and synchronization.
  (`pw_allocator/docs.rst`, `pw_allocator/design.rst`,
  `pw_allocator/public/pw_allocator/allocator.h`,
  `pw_allocator/public/pw_allocator/block_allocator.h`)
- **Misconceptions:** "pw_allocator needs malloc underneath." "Allocators are
  thread-safe by default."

### UC-11: pw_protobuf

- **Opening:** "I need to understand pw_protobuf: how does it serialize and
  deserialize protos on tiny devices?"
- **Tripwire:** yes. **Mode:** Understand with a proposed split.
- **Sequence:**
  1. Wire format and `StreamEncoder`.
  2. Decoding with `StreamDecoder`.
  3. Generated message structs.
  (`pw_protobuf/docs.rst`, `pw_protobuf/public/pw_protobuf/encoder.h`,
  `pw_protobuf/public/pw_protobuf/stream_decoder.h`)
- **Misconceptions:** "It needs big generated classes like protoc's C++
  output." "Decoding needs the whole message in RAM."

### UC-12: An explicit list that doesn't fit (silent-truncation check)

- **Opening:** "I have an hour. Teach me pw_async2: the poll model, tasks,
  wakers, the dispatcher, futures, coroutines, and channels."
- **Tripwire:** yes. The agent must say up front that this is several
  sessions and let the learner pick. If it tries anyway, it must not silently
  drop the later topics or cram them into a rushed closing.
- **Misconceptions:** as UC-09.

## Vague

### UC-13: Facades and backends in general

- **Opening:** "I keep seeing facades and backends all over Pigweed; what's
  the deal with them?"
- **Tripwire:** yes (clarify first). **Mode:** Orient.
- **Sequence:**
  1. The facade contract and backend header overrides.
  2. Backend selection in Bazel and GN.
  3. Writing a small backend.
  (`docs/sphinx/facades.rst`, `pw_build/facade.gni`, `pw_build/bazel.rst`)
- **Misconceptions:** "Facades are virtual interfaces resolved at runtime."
  "Application code depends on the backend target."

### UC-14: pw_multibuf

- **Opening:** "What is multibuf and why does Pigweed need another buffer
  thing?"
- **Tripwire:** depends on the goal; clarify first. **Mode:** Orient or
  Understand.
- **Sequence:**
  1. Motivation: zero-copy, discontiguous data.
  2. Chunks and ownership.
  3. Using a MultiBuf through a driver or protocol stack.
  (`pw_multibuf/docs.rst`, `pw_multibuf/concepts.rst`,
  `pw_multibuf/design.rst`)
- **Misconceptions:** "MultiBuf is a vector of bytes." "Changing a MultiBuf
  means reallocating."

### UC-15: Testing on host vs. device

- **Opening:** "How does Pigweed run tests on host vs target hardware?"
- **Tripwire:** depends; clarify whether they mean writing tests or running
  them on hardware. **Mode:** Orient or Understand.
- **Sequence:**
  1. `pw_unit_test` and its GoogleTest-compatible subset.
  2. Running on device and collecting results.
  3. Build integration.
  (`pw_unit_test/docs.rst`)
- **Misconceptions:** "Running tests on an MCU means porting all of
  GoogleTest." "On-device tests can't report structured results."

## Lost

### UC-16: Portable driver across FreeRTOS and host

- **Opening:** "I need to make my sensor driver work on both FreeRTOS and
  host; where do I even start?"
- **Tripwire:** yes. **Mode:** Orient, anchored on what the driver needs
  (locks, sleeping, time).
- **Sequence:**
  1. The OS abstraction facades: `pw_sync`, `pw_thread`, `pw_chrono`.
  2. Selecting FreeRTOS vs. host backends (`pw_sync_freertos`, `pw_sync_stl`).
  3. Threads and stacks.
  4. Interrupt-safe primitives.
  (`pw_sync/docs.rst`, `pw_sync/public/pw_sync/mutex.h`, `pw_thread/docs.rst`,
  `pw_chrono/docs.rst`)
- **Misconceptions:** "I'll need `#ifdef FREERTOS` in my driver." "I have to
  understand the FreeRTOS scheduler first."

### UC-17: From UART bytes to RPC calls

- **Opening:** "I have UART data coming from another chip and need to turn it
  into RPC calls without losing bytes or blowing my RAM; what modules do I
  use?"
- **Tripwire:** yes. **Mode:** Orient.
- **Sequence:**
  1. The pipeline: stream, HDLC framing, RPC server.
  2. Streams and buffering.
  3. HDLC decoding.
  4. Feeding frames to `Server::ProcessPacket`.
  (`pw_stream/docs.rst`, `pw_hdlc/docs.rst`, `pw_rpc/docs.rst`,
  `pw_rpc/server.cc`)
- **Misconceptions:** "I need to buffer the whole UART stream before
  decoding." "HDLC needs its own thread."

### UC-18: Board bring-up with logging and a console

- **Opening:** "I have a new STM32 board on my desk; how do I get logging and
  a console working with Pigweed?"
- **Tripwire:** yes. **Mode:** Orient.
- **Sequence:**
  1. The bring-up ladder: `pw_sys_io`, then `pw_log`, then `pw_system`.
  2. A `pw_sys_io` backend for the UART.
  3. `pw_system` wiring.
  4. The host console.
  (`pw_sys_io/docs.rst`, `pw_system/docs.rst`, `pw_log/docs.rst`)
- **Misconceptions:** "I need an RTOS and RPC before my first log line."
  "pw_system is mandatory."
