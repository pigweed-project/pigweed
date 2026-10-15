.. _docs-blog-05-coroutines:

=========================================================
Pigweed Blog #5: C++20 coroutines without heap allocation
=========================================================
*Published on 2024-10-07 by Taylor Cramer*

Pigweed now provides support for coroutines on embedded
targets!

.. literalinclude:: ../pw_async2/examples/coro_blinky_loop.cc
   :language: cpp
   :linenos:
   :start-after: [pw_async2-examples-coro-blinky-loop]
   :end-before: [pw_async2-examples-coro-blinky-loop]

`Click here for more examples. <https://pigweed.googlesource.com/pigweed/pigweed/+/refs/heads/main/pw_async2/examples>`_

Notably, Pigweed's :cc:`pw::async2::Coro` API does not require heap
allocation. This post details how we accompilished this, what challenges
we encountered, and how you can get started using coroutines on embedded.

If you're:

* a C++ embedded developer looking for an ergonomic way to write async code
* a Rust ``async`` / ``await`` user curious about C++
* a C++ committee member who wants to make embedded developers happy

read on!

--------------------------------
Why use coroutines for embedded?
--------------------------------
Coroutines allow for multiple functions to run concurrently.
Compared to other concurrency options, coroutines have:

* No need for thread stacks
* No OS/RTOS-dependent context switching
* No complex, manually written state machines

The `coroutines API introduced in C++20
<https://en.cppreference.com/w/cpp/language/coroutines>`_
allows you to write
`straight-line code <https://en.wikipedia.org/wiki/Straight-line_program>`_
that can yield to the caller and resume at some later point. This can be used
similarly to ``async`` / ``await`` from languages like Rust, C#, JavaScript,
Python, and Swift.

When a typical blocking function waits for a long-running operation to
complete, the whole thread (and any resources associated with it) become
blocked until the operation is done. By contrast, coroutine functions can
yield to their caller and only resume once an asynchronous operation
completes. The caller, often some kind of scheduler, is then free to run
other code while the coroutine function waits for its result.

This allows for executing many operations concurrently without paying the
performance cost of threading or the cognitive cost of callbacks or manual
state machines.

---------------------------------------------
Why C++ coroutines require dynamic allocation
---------------------------------------------
Unfortunately for embedded developers, the C++ coroutine API uses heap
allocation by default. *Heap allocation* (e.g. ``new`` and ``delete`` or
``malloc`` and ``free``) isn't available on many embedded platforms.
Fortunately, heap allocation is not necessary to use C++ coroutines.

However, C++20 coroutines do require *dynamic allocation*
(memory that is allocated at runtime). Pigweed's
:cc:`pw::async2::Coro` API allocates memory using an
:cc:`pw::Allocator`. This avoids the need for a platform-provided
heap allocator, but users may still encounter some of the costs associated
with dynamic allocation, such as:

* Heap fragmentation
* Runtime memory management size and performance overhead
* Runtime out-of-memory conditions
* The need to "right-size" a heap

This dynamic allocation is done in order to store the "coroutine frame", an
object which holds the coroutine's state.

The coroutine frame
===================
Despite not using a native thread stack, C++20 coroutines do need somewhere to
store the "stack" of paused functions. For example, in our ``Blinky`` function
from above:

.. literalinclude:: ../pw_async2/examples/coro_blinky_loop.cc
   :language: cpp
   :linenos:
   :start-after: [pw_async2-examples-coro-blinky-loop]
   :end-before: [pw_async2-examples-coro-blinky-loop]

When we reach a ``co_await`` statement, the function is paused and control
is returned to the scheduler until ``500ms`` passes. When paused, we still
need to store the function's state, including the loop variable
``i``, the function arguments, and a pointer to where we were in the function
when it paused.

This coroutine state (hereafter referred to as a "coroutine frame") typically
requires much less memory than a full thread stack, as it can allocate exactly
the amount of memory the function needs for variables that are "live" across
``co_await`` points.

.. admonition:: Layout of coroutine frames

   The actual structure of this object is a compiler implementation
   detail, but you can read more about how Clang structures it in
   `The structure of coroutine frames <https://clang.llvm.org/docs/DebuggingCoroutines.html#coroutine-frame>`_.

Static vs. dynamic allocation of the coroutine frame
====================================================
When a coroutine is first created, the C++ coroutine API dynamically allocates
space to store the coroutine frame and returns a pointer to it called a
`coroutine handle <https://en.cppreference.com/w/cpp/coroutine/coroutine_handle>`_.
By contrast, `Rust's async functions
<https://rust-lang.github.io/async-book/01_getting_started/04_async_await_primer.html>`_
directly return coroutine frames inline, allowing for static allocation.
Why did C++ make the choice to dynamically allocate? Some key reasons are that
returning a pointer to dynamically allocated memory keeps the coroutine
function's return value small, movable, and most importantly ABI-stable.

ABI stability
=============
ABI stability is a particular challenge when designing coroutines. The coroutine
frame contains the stack of the coroutine function, so the coroutine frame's
size is dependent upon the number and size of the variables used in the function
implementation.

For example, two functions with this declaration can have differently sized
coroutine frames:

.. code-block:: c++

   Coro<int> GimmeANumber();

An implementation like this would have a very small coroutine frame:

.. code-block:: c++

   Coro<int> GimmeANumber() { co_return 5; }

While this function would need a very large coroutine frame in order
to store its temporary state:

.. code-block:: c++

   Coro<int> GimmeANumber() {
     ABigValue a = co_await DownloadAllOfReddit();
     AnotherOne b = co_await DownloadAllOfHackerNews();
     co_return a.size() + b.size();
   }

If we were to inline the coroutine frame in the resulting ``Coro<int>`` object,
that would mean that ``Coro<int>`` would have to be a different size depending
on the function implementation. The two functions would therefore have different
ABIs, and we would no longer be able to refer to ``Coro<int>`` as a single
statically sized type.

This would also make it impossible to call a coroutine function from a different
translation unit without knowing the details of its implementation. Without
additional compiler features, inlined coroutine frames would require that public
coroutine functions be implemented in headers (similar to templated or
``constexpr`` functions).

Similarly, inlined coroutine frames would prevent coroutine functions from
being ``virtual``, as different overrides would have different ABIs.

Rust bites the bullet
=====================
Rust inlines the coroutine frame and runs headfirst into these challenges.
The coroutine-style object returned by a Rust ``async fn`` is:

* Immovable once the coroutine has started. This is necessary in order to
  ensure that pointers to variables in the coroutine stack are not invalidated.
* Dependent upon the implementation of the ``async`` function. Adding a new
  variable in the body of the ``async`` function will require more "stack"
  space, so the size and structure of the returned state machine will change
  as the implementation of the ``async`` function changes.
* Not usable with dynamic dispatch (Rust's ``virtual``) without an
  intermediate layer which dynamically allocates the coroutine frame
  (see the `async_trait <https://docs.rs/async-trait/latest/async_trait/>`_
  library for one implementation of this pattern).
* Challenging to compile. Since the size of the coroutine object is
  dependent on the function implementation, code which uses the coroutine
  function can't be compiled until the compiler has first determined the
  size of the coroutine object.

However, this tradeoff also means that Rust can statically allocate space
for coroutines, avoiding all the pitfalls of dynamic allocation. The current
C++ coroutine API doesn't allow for this.

---------------------
What Pigweed provides
---------------------
Despite the need for dynamic allocation, it is still possible to use coroutines
on embedded devices. Today, Pigweed is using coroutines in the :ref:`showcase-sense`
showcase for the Raspberry Pi RP2040 and (new!) RP2350 microcontrollers.

In order to accomplish this, we needed the Pigweed :cc:`pw::async2::Coro`
API to:

* Avoid heap allocation.
* Allow recovering from allocation failure without using exceptions.
* Allow using custom, context-specific
  :cc:`pw::Allocator` implementations on a per-coroutine basis.

If you want to skip ahead, you can :cs:`see the Coro implementation here.
<main:pw_async2/public/pw_async2/coro.h>`

--------------------
Eliminating the heap
--------------------
As I said before, the C++ coroutine API defaults to heap allocation.
Fortunately for us embedded devs, the coroutine ``promise_type`` can customize
the behavior of this allocation by implementing the following functions:

* ``static void* operator new(std::size_t, Args...) noexcept``
* ``static MyReturnObjectType get_return_object_on_allocation_failure()``
* ``static void operator delete(void*)``

Allocating memory
=================
Implementing ``operator new`` lets us control how coroutine memory is allocated.
Our custom ``operator new`` takes two parameters: a ``std::size_t`` and an
``Args...`` parameter pack. Let's explore how these are handled.

The ``size`` parameter
----------------------
Unlike regular ``operator new`` or `placement new
<https://en.cppreference.com/w/cpp/language/new#Placement_new>`_,
the ``promise_type`` ``operator new`` function allocates space not just for
the ``promise_type`` itself, but also for the compiler-generated coroutine
stack. The size of this coroutine stack is determined by the compiler and is
optimization-dependent, so it is not statically observable and is only available
at runtime.

The ``Args...`` parameter pack
------------------------------
The arguments to coroutine functions are also passed into the ``operator new``
of the corresponding ``promise_type``. This allows ``operator new`` to "intercept"
arguments passed to the coroutine function.

This means that if we have some
coroutine function:

.. code-block:: c++

   MyCoroutineType some_func(int a)

and ``MyCoroutineType::promise_type`` has an ``operator new`` like:

.. code-block:: c++

   static void* operator new(std::size_t n, int a) noexcept

then the argument ``a`` will be copied into the invocation of ``operator new``.

:cc:`pw::async2::Coro` uses this tool to allow the caller of a coroutine
function to specify which memory allocator to use for the coroutine frame.

Different coroutines may want to use memory provided by different allocators
with various memory locations or allocation strategies. When allocating a
regular type ``T``, we'd typically achieve this by pre-allocating ``sizeof(T)``
with our custom allocator and then passing the resulting memory into the
`placement new
<https://en.cppreference.com/w/cpp/language/new#Placement_new>`_
function of ``T``.

However, unlike regular types, coroutine constructors or ``operator new`` are not
directly available to the caller. Instead, the coroutine types are constructed
by the compiler when the coroutine function is called.

Furthermore, C++20 doesn't provide a way to know ahead of time how much memory
needs to be allocated to store the coroutine state—that information is only
available once our custom ``operator new`` is invoked with a ``size`` value.

Instead, :cc:`pw::async2::Coro` allows custom per-coroutine ``Allocator``
values to be provided by intercepting an ``Allocator`` argument to the coroutine
function. One can pass an allocator as the first argument of all coroutine
functions. We can then use an ``Args...`` variadic to discard all other
arguments to the coroutine function:

.. code-block:: c++

   template <typename... Args>
   static void* operator new(std::size_t n,
                             Allocator& alloc,
                             const Args&...) noexcept {
     return alloc.Allocate(n);
   }

The user's coroutine functions will then look like this:

.. code-block:: c++

   MyCoroutineType some_func(Allocator&, int a) { ... }

The ``operator new`` implementation will then receive the caller-provided ``Allocator``
reference and can use it to allocate the coroutine state.

.. admonition:: ``CoroContext``

   The allocator argument to :cc:`pw::async2::Coro` is actually packaged inside a
   :cc:`pw::async2::CoroContext` in order to allow for greater evolvability, but
   today this type is a simple wrapper over a :cc:`pw::Allocator`.

Handling allocation failure
===========================
Many embedded systems both disable exceptions and wish to recover gracefully
from allocation failure. If this is the case, ``promise_type::operator new`` must
be marked ``noexcept`` and return ``nullptr`` on allocation failure.

Note that our coroutine function, however, doesn't have an obvious way to
return ``nullptr``:

.. code-block:: c++

   MyCoroutineType some_func(Allocator&, int a) { ... }

It *must* produce a value of type ``MyCoroutineType``.

This is made possible by
``promise_type::get_return_object_on_allocation_failure``. When ``operator new``
returns ``nullptr``, C++ will invoke ``get_return_object_on_allocation_failure``
to produce an "empty" ``MyCoroutineType``. Coroutine return types must therefore
decide on a sentinel representation to use in order to indicate allocation
failure.

Fortunately, most coroutine return types are simple wrappers around
``std::coroutine_handle<my_promise_type>``, which is nullable, so we can use the
``nullptr`` representation to indicate allocation failure.

The ``delete`` function is missing critical pieces. What now?
=============================================================
Just as we can control allocation with our custom ``promise_type::operator new``,
we can control deallocation with ``promise_type::operator delete``!

But unlike ``operator new``, ``delete`` cannot intercept function arguments,
nor can it access properties of the coroutine frame. By the time ``delete``
is called, the coroutine frame has already been destroyed. Other parts of
C++ use the ``destroying_delete`` API to allow accessing an object as part
of its deletion, but the coroutine ``promise_type`` doesn't include such an
API.

This means that ``delete`` has no way to get access to the ``Allocator``
instance from which the memory was allocated. Pigweed's
:cc:`pw::Allocator` API does not provide a way to free memory from only
a pointer to the allocated memory; a reference to the ``Allocator`` object is
required (particular ``Allocator`` instances may support this, but the generic
interface does not). :cc:`pw::async2::Coro` solves this by specifying
``operator delete`` to be a no-op. Instead, the coroutine memory is freed by
the coroutine handle wrapper type (``Coro``) *after* running
``operator delete``:

.. literalinclude:: ../pw_async2/public/pw_async2/coro.h
   :language: cpp
   :linenos:
   :start-after: [pw_async2-coro-release]
   :end-before: [pw_async2-coro-release]

.. admonition:: Promise handle address

   The standard doesn't appear to clarify whether or not this usage is
   supported. I couldn't find any documentation specifying that the promise
   handle address must point to the *start* of the allocated memory (rather
   than, say, the allocated memory plus eight bytes or something).

   In practice, this seems to work just fine: the Pigweed API works in both
   GCC and Clang, even with sanitizers enabled.

   In the future, a guarantee here would be super useful!

----------------------------------------------------------------
Still to go: improvements needed from C++ standard and compilers
----------------------------------------------------------------
One concern that may hamper adoption of coroutines in embedded applications is
the total size of coroutine frames. While these frames are frequently much
smaller than the stack sizes of threads, there may be many more of them.

Especially for users who would otherwise write manual state-machine-based or
callback-based code, the current size overhead of coroutines may be a limiting
factor. Some opportunities for improvement here are discussed below.

Reusing stack space of expired temporaries
==========================================
Currently, Clang does not use stack space of temporaries after their
storage duration ends. This means that all variables and temporaries
in a coroutine function must have dedicated storage space in the coroutine frame.
This is unnecessary: many variables are not alive at the same time, and others
may not ever be alive across a ``co_await`` point! In the former case, storage
for these variables could be overlapped, and in the latter, they need not be
stored in the coroutine frame at all.

This results in having a coroutine frame of 88 bytes:

.. code-block:: c++

   Coro<Status> SimplestCoroWithAwait(CoroContext& cx) {
     co_return co_await SimplestCoro(cx);
   }

And this code having a coroutine frame of 408 bytes:

.. code-block:: c++

   Coro<Status> SimplestCoroWithTwoAwaits(CoroContext& cx) {
     co_await SimplestCoro(cx);
     co_await SimplestCoro(cx);
     co_await SimplestCoro(cx);
     co_await SimplestCoro(cx);
     co_await SimplestCoro(cx);
     co_await SimplestCoro(cx);
     co_await SimplestCoro(cx);
     co_await SimplestCoro(cx);
     co_await SimplestCoro(cx);
     co_await SimplestCoro(cx);
     co_return co_await SimplestCoro(cx);
   }

Despite both requiring the same amount of temporary storage.

Discussion on this topic continues in
`this LLVM issue regarding the missing lifetime markers on temporaries <https://github.com/llvm/llvm-project/issues/43598>`_.

A similar issue in Rust was solved thanks to tmandry's efforts culminating
in `this PR <https://github.com/rust-lang/rust/pull/60187>`_.

Passing values in and out of coroutines
=======================================
Once a coroutine function has been invoked, it can only be advanced
(and completed) via a series of calls to
`resume <https://en.cppreference.com/w/cpp/coroutine/coroutine_handle/resume>`_.
Annoyingly, ``resume`` does not accept any arguments and returns ``void``,
so it does not offer a built-in way to pass arguments into our coroutine
function, nor does it give us a way to return values from our coroutine function.

:cc:`pw::async2::Coro` solves this by storing a pointer to input
and output storage inside the ``promise_type`` before invoking ``resume``.

.. literalinclude:: ../pw_async2/public/pw_async2/coro.h
   :language: cpp
   :linenos:
   :start-after: [pw_async2-coro-resume]
   :end-before: [pw_async2-coro-resume]

This means that every coroutine frame must additionally include space for this
input/output pointer. This wasted overhead seems fairly easy to eliminate if
C++ evolved to allow ``promise_type`` to have associated ``resume_arg_type``
and ``return_arg_type`` values.

Avoiding dynamic allocation for statically known coroutines
===========================================================
Although the steps described in this blog will allow you to avoid heap
allocation, they still rely on *dynamic* allocation via a
:cc:`pw::Allocator`. This means that our application can potentially
fail at runtime by running out of memory for our coroutines.

For code which only instantiates a single known coroutine or fixed set of
known coroutines defined within the same translation unit, this is an
unnecessary cost. Dynamic allocation creates both runtime inefficiency,
developer uncertainty, and risk: it's no longer possible to statically ensure
that your program will be able to run on-device.

It would be fantastic if future C++ standards would make it possible to access
the coroutine frame size of specific, known coroutines so that space for them
could be statically allocated.

.. admonition:: Heap allocation elision optimization (HALO)

   `Heap allocation elision optimization <https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0981r0.html>`_
   can omit the calls to ``new`` and ``delete`` entirely in some cases.
   However, this relies on the inlining of several independent functions
   to the top-level scope which encompasses the full lifetime of the
   coroutine. This isn't possible in the vast majority of the relevant
   embedded use cases where it is common to store a coroutine as part of a
   long-lived (often ``static``) object.

   Furthermore, relying on such an optimization would be quite fragile, as
   the compiler's choice to inline (or not) is dependent upon a variety of
   heuristics outside the programmer's control.

   Note, too, that when using custom allocator implementations one has
   to take care or elision will not be possible: the allocation and
   deallocation functions must be tagged with
   `the appropriate attributes <https://discourse.llvm.org/t/optimize-away-memory-allocations/65587/4>`_
   to tell the compiler that they are safe to optimize away.

-----------------------------------
Try out Pigweed's coroutine API now
-----------------------------------
To experiment with Pigweed's :cc:`pw::async2::Coro` API, you can get
started by `cloning our Quickstart repo <https://cs.opensource.google/pigweed/quickstart/bazel>`_
for a bare-bones example
or trying out the :ref:`Sense tutorial <showcase-sense-tutorial-intro>`
for a full tour of a Pigweed product codebase.
