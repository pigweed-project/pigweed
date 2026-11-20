.. _module-pw_async2-contract:

========
Contract
========
.. pigweed-module-subpage::
   :name: pw_async2

This page defines the normative behavioral contract for ``pw_async2`` futures,
providers, wakers, tasks, and dispatchers.

The ``pw::async2::Future`` C++ concept checks only syntax (``value_type``,
``Pend()``, ``is_pendable()``, ``is_complete()``, default construction,
movability, and destruction). This page specifies the logical invariants that
all compliant implementations must uphold, independent of how they are
implemented.

-----------
Terminology
-----------
- **Provider**: The object that vends a future and later completes it, such as
  a driver, channel, timer service, or connection. A "provider" is a role; there
  is no universal class by that name.
- **Leaf future**: A future that is woken directly by an external event or
  provider. Because it is woken directly, it holds a
  :cc:`Waker <pw::async2::Waker>` and its provider must maintain some way to
  notify it when progress can be made.
- **Composite future**: A future built from one or more child futures. It is
  not woken directly; it delegates polling and waker registration transitively
  to its children.

.. _module-pw_async2-contract-states:

------------------
Future state space
------------------
At any point in its lifetime, a future is in one of five logical states:

.. list-table::
   :header-rows: 1

   * - State
     - ``is_pendable()``
     - ``is_complete()``
     - Meaning
   * - **Empty**
     - ``false``
     - ``false``
     - Default-constructed or moved-from. Represents no operation; calling
       ``Pend()`` is invalid. Dropping it has no effect.
   * - **Unstarted**
     - ``true``
     - ``false``
     - Created, but ``Pend()`` has not yet been called; no work has begun on
       any underlying resource. Dropping it has no effect on the resource.
   * - **Running**
     - ``true``
     - ``false``
     - ``Pend()`` has been called and returned
       :cc:`Pending <pw::async2::Pending>`; the operation or wait is active.
       Dropping it cancels the operation.
   * - **Ready to complete**
     - ``true``
     - ``false``
     - Complete-but-not-yet-consumed: the outcome is already determined, so the
       next ``Pend()`` call is guaranteed to return
       :cc:`Ready <pw::async2::Ready>` immediately. Depending on the
       implementation, the output may be stored inline in the future or pulled
       from its provider during that final ``Pend()`` (being woken does not by
       itself put a pull-based future into this state if another waiter may
       claim the output first). Dropping it does not consume the output;
       whether that output is discarded or remains available to other futures
       depends on the provider.
   * - **Complete**
     - ``false``
     - ``true``
     - ``Pend()`` has returned :cc:`Ready <pw::async2::Ready>` and the output
       has been consumed. Inert; calling ``Pend()`` again is invalid. Dropping
       it has no effect.

Callers observe only ``is_pendable()`` and ``is_complete()``; the distinctions
among unstarted, running, and ready to complete are internal to the future.

Moving a future transfers its state (including any active provider association
and stored waker) to the destination and leaves the moved-from future empty
(``!is_pendable() && !is_complete()``).

Not every future visits every state. For example, a factory function that
rejects invalid arguments returns a future directly in the ready-to-complete
state (never empty, unstarted, or running), and a future that completes
synchronously on its first ``Pend()`` moves straight from unstarted to
complete.

The empty state (default-constructed or moved-from) and drop transitions (see
:ref:`module-pw_async2-contract-dropping-cancels`) are omitted from the diagram
for clarity:

.. mermaid::

   stateDiagram-v2
      state "Ready to complete" as ReadyToComplete
      [*] --> Unstarted: factory
      [*] --> ReadyToComplete: immediate value or sync error
      Unstarted --> Running: first Pend returns Pending
      Unstarted --> ReadyToComplete: for example, provider closes before first Pend
      Unstarted --> Complete: first Pend returns Ready
      Running --> ReadyToComplete: outcome fixed before final Pend
      Running --> Complete: Pend returns Ready
      ReadyToComplete --> Complete: final Pend consumes output

.. _module-pw_async2-contract-rules:

-----
Rules
-----

.. _module-pw_async2-contract-futures-are-lazy:

1. Futures are lazy
===================
Creating or dropping a future before its first ``Pend()`` has no observable
effect on any underlying resource. Work on a resource --- such as starting I/O,
arming a hardware timer, or consuming a buffered item from a queue --- begins
only when ``Pend()`` is first called.

- **Creation-time parameters and observation windows**: Parameters (such as
  request arguments or a relative timer's deadline) are fixed at creation.
  Similarly, an unbuffered event source (such as a notification) documents when
  its observation window opens --- either at creation (events between creation
  and the first ``Pend()`` satisfy the future) or on the first ``Pend()``
  (earlier events are ignored).
- **Synchronous validation**: A factory function may validate arguments
  synchronously and return a future already ready to complete with an error.
- **Internal bookkeeping**: Internal provider bookkeeping at creation (such as
  taking a reference, snapshotting a counter, or tracking the future for
  lifetime safety) is an implementation detail and is permitted as long as no
  work starts on the underlying resource. Dropping an unstarted future releases
  any internal handle or endpoint reference it held.
- **Deferred polling**: Because creation starts no work, callers may create and
  hold an unstarted future before deciding whether or when to poll it (for
  example, passing it into a combinator or conditional branch), noting that any
  parameters or observation windows fixed at creation still apply.

.. _module-pw_async2-contract-dropping-cancels:

2. Dropping a future cancels it
===============================
Dropping a future --- either by destroying it or by overwriting it via
move-assignment (for example, ``some_running_future_ = {}``) --- cancels any
active operation synchronously:

- **Synchronous completion**: Cancellation must finish before the destructor or
  move-assignment operator returns, rather than being deferred asynchronously.
  Once it returns, no provider, ISR, background thread, or DMA transfer may
  still access the future or any memory borrowed by the operation (such as a
  caller-provided buffer).
- **Empty, unstarted, and complete futures**: Dropping an empty or complete
  future has no effect on any provider; dropping an unstarted future has no
  effect on the underlying resource (releasing only any internal handle or
  endpoint reference the future held).
- **Ready-to-complete futures**: Dropping a future that is ready to complete
  does not consume its output (whether that output is discarded or remains
  available to other futures depends on the provider) and releases any
  remaining association with its provider.
- **Running futures**: Dropping a running future disassociates it from its
  provider so the provider will never access or wake it again, clears any
  stored waker, and aborts the underlying operation if feasible. If the
  operation cannot be aborted synchronously, the provider lets it finish safely
  and discards its result. When hardware or another thread cannot be quiesced
  before the drop returns, the API must take owned or reference-counted buffers
  rather than borrowing caller memory.
- **No cross-delivery of late completions**: A late completion from a cancelled
  operation must never be delivered to a different future.

.. _module-pw_async2-contract-provider-never-strands:

3. A provider never strands its futures
=======================================
A provider chooses and documents one of two lifetime contracts for running
futures, and documents whether unstarted futures may outlive the provider:

- **Provider outlives its futures**: Every running future (and any future that
  is ready to complete while still referencing the provider) must complete or
  be dropped before the provider is destroyed; the provider asserts this
  invariant in its destructor. Unstarted futures must not outlive the provider.
- **Provider or resource may go away**: When the underlying resource fails or
  the provider is destroyed, every running future is woken and its next
  ``Pend()`` returns :cc:`Ready <pw::async2::Ready>` with an error or closed
  outcome. In this case, the future's ``value_type`` must be fallible (for
  example, :cc:`pw::Result`, :cc:`pw::Status`, ``std::optional``, or ``bool``).
  Providers backed by shared state give unstarted futures the same guarantee;
  providers that do not track unstarted futures document that unstarted futures
  must not outlive the provider.

A complete future (see :ref:`module-pw_async2-contract-inert-after-ready`) is
already decoupled from its provider and may safely outlive it under either
contract; whether a future that is ready to complete may outlive its provider
depends on whether it still references the provider.

.. _module-pw_async2-contract-inert-after-ready:

4. A future is inert once it returns ``Ready``
==============================================
Before returning :cc:`Ready <pw::async2::Ready>`, a future transitions to the
complete state (``!is_pendable() && is_complete()``) and releases any
association with its provider (references, registrations, and stored wakers).

- Calling ``Pend()`` on an empty or complete future is a precondition violation
  (undefined behavior); implementations should assert (via ``PW_ASSERT`` or
  ``PW_DASSERT``).
- The value returned inside ``Ready(...)`` must not hold pointers or references
  into the future's internal storage.
- A complete future may be destroyed immediately upon returning ``Ready`` (as
  :cc:`SelectFuture <pw::async2::SelectFuture>`,
  :cc:`FutureOrValue <pw::async2::FutureOrValue>`,
  :cc:`BoxedFuture <pw::async2::BoxedFuture>`, and
  :cc:`Coro <pw::async2::Coro>` do), destroyed later, or destroyed after its
  provider is gone, with no side effects.

.. _module-pw_async2-contract-composites-cancel-children:

5. Composites cancel unfinished children when they return ``Ready``
===================================================================
When a composite future returns :cc:`Ready <pw::async2::Ready>`, it must
immediately destroy (and thereby cancel) every child future that has not
returned ``Ready`` (as :cc:`SelectFuture <pw::async2::SelectFuture>` and
:cc:`FutureWithTimeout <pw::async2::FutureWithTimeout>` do).

Children that already returned ``Ready`` are complete and inert (see
:ref:`module-pw_async2-contract-inert-after-ready`). A composite may destroy
complete children immediately (preferred to reclaim storage, as in
:cc:`FutureOrValue <pw::async2::FutureOrValue>`) or retain them until the
composite itself is destroyed (as in
:cc:`JoinFuture <pw::async2::JoinFuture>`).

.. _module-pw_async2-contract-waker-conservation:

6. Conservation of pends and wakers
===================================
Wakes in ``pw_async2`` coalesce rather than accumulate: multiple wake events
before a task runs produce a single poll, and spurious polls may occur.
Preventing both permanently stalled tasks and leaked wakes is a shared invariant
whose concrete obligations depend on your role:

Composite futures and tasks driving futures
-------------------------------------------
Code that drives futures never interacts with :cc:`Waker <pw::async2::Waker>`
directly. Follow three rules when polling child futures, and the underlying
waker invariants are satisfied automatically:

- **Pend every child you are waiting on before returning Pending**:
  Constructing a child future does not register a waker. Before returning
  :cc:`Pending <pw::async2::Pending>`, poll every child whose progress you are
  waiting on (in a manual state machine, loop straight into the next state's
  ``Pend()``). Leaving a child unpolled means it cannot wake the task --- and
  if no other waker is registered, returning ``Pending`` crashes.
- **Re-pend or drop every previously pended child that is still pendable**:
  When polled again, any child you previously pended that is still pendable
  (whether still running or already ready to complete) may have been the one
  that woke the task. Before returning ``Pending``, every such child must
  either be polled again or dropped. This is why in-place selection
  (:cc:`PW_SELECT`, :cc:`PW_CO_SELECT`) pends every pendable branch without
  short-circuiting.
- **Check poll results rather than assuming readiness**: Because wakes coalesce
  and spurious polls may occur, never assume that being polled means a
  particular child is ready.

Leaf futures
------------
Leaf futures are the layer that interacts directly with
:cc:`Waker <pw::async2::Waker>`:

- **Store a waker whenever returning Pending**: Before returning
  :cc:`Pending <pw::async2::Pending>`, store a waker from the provided
  :cc:`Context <pw::async2::Context>` such that the provider or external event
  is guaranteed to call :cc:`Waker::Wake <pw::async2::Waker::Wake>` when
  progress can be made, with no race window between checking readiness and
  storing the waker.
- **Clear the waker on completion or cancellation**: Clear or destroy any
  stored waker before returning :cc:`Ready <pw::async2::Ready>` and when
  dropped (destroyed or overwritten).
- **Handle spurious polls idempotently**: Additional ``Pend()`` calls before
  the event occurs must re-check readiness, re-store the waker, and return
  ``Pending`` without restarting work or corrupting state.
- **Single-task affinity while running**: A running leaf future's stored waker
  is bound to one :cc:`Task <pw::async2::Task>`. Polling an active leaf future
  from a different task without first cancelling or clearing its waker asserts
  in :cc:`PW_ASYNC_STORE_WAKER`.

Tasks
-----
A bare :cc:`Task <pw::async2::Task>` that stores :cc:`Waker <pw::async2::Waker>`
objects directly instead of or alongside driving futures bears the raw runtime
requirements directly:

- Never return :cc:`Pending <pw::async2::Pending>` without at least one
  registered waker, unless explicitly unscheduling the task via
  :cc:`Context::Unschedule <pw::async2::Context::Unschedule>`.
- Before returning ``Pending``, re-store, clear, or destroy every waker that
  fired during the wake cycle. When :cc:`PW_ASYNC2_CONFIG_ASSERT_LEAKED_WAKES`
  is enabled, returning ``Pending`` with an unhandled woken waker crashes.

Dispatcher implementers
-----------------------
The base :cc:`Dispatcher <pw::async2::Dispatcher>` handles task wake latching
and ``DoWake()`` coalescing internally, invoking ``DoWake()`` once per
idle-to-woken transition.

Subclasses must drain the ready queue before sleeping for the next ``DoWake()``
signal --- either by calling
:cc:`PopAndRunAllReadyTasks <pw::async2::Dispatcher::PopAndRunAllReadyTasks>`,
or by looping :cc:`PopTaskToRun <pw::async2::Dispatcher::PopTaskToRun>` until
it returns ``nullptr``. To run a single task without draining, use
:cc:`PopSingleTaskForThisWake
<pw::async2::Dispatcher::PopSingleTaskForThisWake>`.

.. _module-pw_async2-contract-single-threaded:

7. Futures and dispatchers are single-threaded
==============================================
A :cc:`Dispatcher <pw::async2::Dispatcher>` runs on a single thread; polling
one dispatcher concurrently from multiple threads is undefined.

- **Exclusive access and single-threaded pending**: A future is only ever
  pended from a single dispatcher thread --- never concurrently from multiple
  threads or across dispatchers (subsystems may verify this with
  :cc:`Context::IsSameDispatcher <pw::async2::Context::IsSameDispatcher>`).
  Creating, moving, or destroying a future from another thread is permitted
  only when it cannot race with either pending the future or the dispatcher
  thread accessing an unsynchronized provider --- for example, when the
  future's task has completed or been deregistered (so the future cannot be
  pended concurrently) and either the provider is thread-safe or the dispatcher
  is no longer running.
- **Provider APIs are single-dispatcher by default**: Unless a provider
  explicitly documents that an operation is thread-safe or ISR-safe, callers
  may invoke its API only from a single dispatcher thread. Whether the provider
  synchronizes internally (for example, with an ISR or background thread that
  completes its futures) is an implementation detail of the provider.
- **Cross thread and dispatcher boundaries through primitives, not futures**:
  Multiple dispatchers may run on separate threads, and hardware interrupts or
  blocking threads may interact with async code, but communication across those
  boundaries goes through explicitly synchronized primitives --- such as
  :ref:`channels <module-pw_async2-channels>` (:cc:`Sender <pw::async2::Sender>`
  and :cc:`Receiver <pw::async2::Receiver>`),
  :cc:`Notification <pw::async2::Notification>`, or
  :cc:`Waker <pw::async2::Waker>` --- rather than by sharing futures across
  threads. Each future vended by such a primitive is still pended only on the
  single dispatcher thread that polls it.

.. _module-pw_async2-contract-teardown:

8. Tasks are deregistered and dispatchers are terminated before destruction
===========================================================================
- Every posted :cc:`Task <pw::async2::Task>` must complete or be deregistered
  via :cc:`Task::Deregister <pw::async2::Task::Deregister>` (or awaited via
  :cc:`Task::BlockingJoin <pw::async2::Task::BlockingJoin>`) before it is
  destroyed. Never call ``Deregister()`` from inside the task's own ``DoPend()``
  or while holding a lock that ``DoPend()`` acquires. Destroying a deregistered
  task from another thread while the dispatcher is still running is safe only
  if its member futures belong to thread-safe providers or are already empty or
  complete.
- Every :cc:`Dispatcher <pw::async2::Dispatcher>` must have no registered tasks
  when destroyed. Call
  :cc:`Dispatcher::Terminate <pw::async2::Dispatcher::Terminate>` before
  destruction (dispatcher subclasses that support destruction with active tasks
  must call ``Terminate()`` in their own destructor).
- **Teardown order**: Always destroy tasks (cancelling their member futures)
  before any providers whose futures cannot outlive them. Two teardown patterns
  satisfy the task and dispatcher requirements above:

  - **Explicit completion or deregistration** (works with any dispatcher):
    ensure all tasks have completed or been deregistered via
    :cc:`Task::Deregister <pw::async2::Task::Deregister>`,
    :cc:`Task::BlockingJoin <pw::async2::Task::BlockingJoin>`, or
    :cc:`Dispatcher::Terminate <pw::async2::Dispatcher::Terminate>` before
    destroying the tasks and the dispatcher.
  - **Dispatcher-terminated teardown** (for concrete dispatchers whose
    destructor calls ``Terminate()``, such as
    :cc:`BasicDispatcher <pw::async2::BasicDispatcher>`,
    :cc:`NotifiedDispatcher <pw::async2::NotifiedDispatcher>`, and
    :cc:`EpollDispatcher <pw::async2::EpollDispatcher>`): declare providers
    first, then tasks, then the dispatcher last so C++ reverse destruction
    order terminates the dispatcher first, then destroys tasks, then destroys
    providers.

.. _module-pw_async2-contract-exceptions:

------------------------
Known current exceptions
------------------------
A few existing ``pw_async2`` APIs do not yet satisfy all rules above:

- :cc:`ValueFuture <pw::async2::ValueFuture>` and its provider classes
  (:cc:`ValueProvider <pw::async2::ValueProvider>`,
  :cc:`BroadcastValueProvider <pw::async2::BroadcastValueProvider>`,
  :cc:`ValueListProvider <pw::async2::ValueListProvider>`,
  :cc:`DerivedValueProvider <pw::async2::DerivedValueProvider>`) provide
  passive event or value delivery rather than lazy operations. Two common usage
  patterns violate the rules above:

  - Returning a ``ValueFuture`` for an operation whose work starts eagerly in
    the factory function violates
    :ref:`module-pw_async2-contract-futures-are-lazy` and
    :ref:`module-pw_async2-contract-dropping-cancels` (work begins before
    ``Pend()`` and dropping the future cannot abort it).
  - Reusing a multi-shot provider
    (:cc:`ValueProvider <pw::async2::ValueProvider>` or
    :cc:`ValueListProvider <pw::async2::ValueListProvider>`) across a cancelled
    operation can violate :ref:`module-pw_async2-contract-dropping-cancels`: if
    a second future is vended before a late completion from the cancelled first
    operation arrives, ``Resolve()`` delivers that late completion to the
    second future.

  *Plan*: Replace these classes with a dedicated oneshot primitive and a
  standalone :cc:`Notification <pw::async2::Notification>` future, with no
  generic value-provider classes. New operations should implement custom leaf
  futures directly instead of using ``ValueFuture``.
- :cc:`TimeFuture <pw::async2::TimeFuture>` and
  :cc:`FutureWithTimeout <pw::async2::FutureWithTimeout>` currently register
  with :cc:`TimeProvider <pw::async2::TimeProvider>` and arm the backend timer
  eagerly at construction rather than on the first ``Pend()``.

  *Plan*: Defer registration and backend timer arming to the first ``Pend()``,
  while keeping the deadline fixed at creation.

.. _module-pw_async2-contract-guidance:

-----------------------
Implementation guidance
-----------------------
The choices below are implementation details that depend on product
requirements rather than the logical contract above.

Composite vs. leaf futures
==========================
Most asynchronous code should be written as **composite futures** --- value
types that store child futures inline and forward
:cc:`Context <pw::async2::Context>` without managing wakers, provider tracking,
or locks.

Reserve **leaf futures** for boundaries that wait directly on hardware
interrupts, timers, queues, or other external event sources. ``pw_async2``
provides :cc:`FutureState <pw::async2::FutureState>`,
:cc:`FutureCore <pw::async2::FutureCore>`, and
:cc:`FutureList <pw::async2::FutureList>` as optional helpers to reduce
boilerplate for intrusive leaf futures, but implementations are free to use any
internal representation (such as a ``std::variant``, a single waiter pointer,
or reference-counted state) that upholds the rules above.

How a leaf future survives its provider
=======================================
When implementing a leaf future, choose a lifetime and synchronization strategy
based on (a) whether the provider can be destroyed while futures exist and (b)
whether completions occur exclusively from tasks on the same dispatcher thread
or from another execution context (such as an ISR or another thread):

.. list-table::
   :header-rows: 1

   * - Strategy
     - How it works
     - Locking & trade-offs
     - In-tree example
   * - **Provider outlives its futures**
     - Future holds a raw provider pointer and registers with the provider
       (for example, via a single slot or an intrusive list) while running.
       Provider destructor asserts no futures remain.
     - No lock if completed only on the same dispatcher thread; per-provider
       lock if completed from an ISR or another thread. Cheapest, but relies on
       system structure.
     - :cc:`TimeProvider <pw::async2::TimeProvider>`
   * - **Provider-tracked detach**
     - On destruction, the provider walks its tracked futures, marks each ready
       to complete with an error/closed outcome, clears its back-pointer, and
       wakes it. Futures never dereference the provider once detached.
     - Zero allocation. No lock if both provider destruction and completion
       happen only on the same dispatcher thread. If either can happen from an
       ISR or another thread, requires a lock that **outlives both** the
       provider and the future (a lock stored inside the provider causes
       use-after-free in ``~Future()``), adding contention and subtle
       concurrency.
     - :cc:`OptionalValueProvider <pw::async2::OptionalValueProvider>`,
       ``pw_rpc2`` ``ControlFuture``
   * - **Shared reference-counted state**
     - Provider handles and futures hold a counted reference to shared state
       (allocated or statically stored). Dropping the provider handle closes the
       state and wakes active futures; the state is reclaimed when the last
       reference drops.
     - Simplest to reason about and handles both unstarted and running futures
       uniformly. Uses a non-atomic refcount with no lock if accessed only from
       the same dispatcher thread, or an atomic refcount with a per-object lock
       across threads/ISRs.
     - :ref:`pw_async2 channels <module-pw_async2-channels>`,
       ``pw_transport`` sockets, ``pw_rpc2`` ``Call``

.. note::

   **Laziness and unstarted futures**: In both non-refcounted strategies above,
   an unstarted lazy future holds a pointer to its provider without yet being
   registered as an active waiter.

   - Under *Provider outlives its futures*, asserting that no active waiters
     remain in ``~Provider()`` catches running futures; optionally maintain a
     debug-only outstanding-future count if you also want to catch dangling
     unstarted futures.
   - Under *Provider-tracked detach*, an unstarted future cannot be notified on
     provider destruction unless it registers at creation (deferring only the
     work); otherwise document that unstarted futures must not outlive the
     provider.
   - *Shared reference-counted state* avoids this wrinkle automatically because
     even an unstarted future holds a reference to the shared state.

What virtual async interfaces return
====================================
Virtual C++ interfaces must name a concrete return type rather than returning an
arbitrary implementation-defined future. Two patterns preserve the contract:

.. list-table::
   :header-rows: 1

   * - Pattern
     - How it works
     - Trade-offs
   * - **Named concrete handle future**
     - The interface defines a concrete future type (for example,
       ``ReadFuture``) holding a handle or pointer to the interface and
       delegating ``Pend()`` and cancellation to virtual ``DoPend*`` /
       ``DoCancel*`` methods (as ``pw_transport`` sockets do).
     - Zero per-operation heap allocation; supports laziness and real
       cancellation. Fixes the future's storage and lifetime mechanism in the
       interface.
   * - :cc:`BoxedFuture <pw::async2::BoxedFuture>`
     - Each implementation returns its own concrete future type, type-erased via
       a :cc:`pw::Allocator`.
     - Maximum implementation flexibility; requires dynamic allocation and
       allocation-failure handling.
