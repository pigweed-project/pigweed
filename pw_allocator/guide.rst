.. _module-pw_allocator-guides:

======
Guides
======
.. pigweed-module-subpage::
   :name: pw_allocator

.. _module-pw_allocator-get-started:

-----------
Get started
-----------
.. tab-set::

   .. tab-item:: Bazel

      Add ``@pigweed//pw_allocator`` to the ``deps`` list in your Bazel target:

      .. code-block::

         cc_library("...") {
           # ...
           deps = [
             # ...
             "@pigweed//pw_allocator",
             # ...
           ]
         }

      For a narrower dependency, depend on subtargets, e.g.
      ``@pigweed//pw_allocator:tracking_allocator``.

   .. tab-item:: GN

      Add ``dir_pw_allocator`` to the ``deps`` list in your build target:

      .. code-block::

         pw_executable("...") {
           # ...
           deps = [
             # ...
             dir_pw_allocator,
             # ...
           ]
         }

      For a narrower dependency, depend on subtargets, e.g.
      ``"$dir_pw_allocator:tracking_allocator"``.

      If the build target is a dependency of other targets and includes
      allocators in its public interface, e.g. its header file, use
      ``public_deps`` instead of ``deps``.

   .. tab-item:: CMake

      Add ``pw_allocator`` to your ``pw_add_library`` or similar CMake target:

      .. code-block::

         pw_add_library(my_library STATIC
           HEADERS
             ...
           PRIVATE_DEPS
             # ...
             pw_allocator
             # ...
         )

      For a narrower dependency, depend on subtargets, e.g.
      ``pw_allocator.tracking_allocator``, etc.

      If the build target is a dependency of other targets and includes
      allocators in its public interface, e.g. its header file, use
      ``PUBLIC_DEPS`` instead of ``PRIVATE_DEPS``.

.. _module-pw_allocator-module-configuration:

--------------------
Module configuration
--------------------
This module has configuration options that globally affect the behavior of
``pw_allocator`` via compile-time configuration of this module, see the
:ref:`module documentation <module-structure-compile-time-configuration>` for
more details.

Module configuration options include:

- :cc:`PW_ALLOCATOR_BLOCK_POISON_INTERVAL` determines
  how frequently blocks that implemented the :cc:`PoisonableBlock
  <pw::allocator::PoisonableBlock>` mix-in should apply the poison pattern on
  deallocation.
- :ref:`` allows you to set how many
  validation checks are enabled. Additional checks can detect more errors at the
  cost of performance and code size.
- :cc:`PW_ALLOCATOR_SUPPRESS_DEPRECATED_WARNINGS`
  allows you to silence warnings about deprecated interfaces. This is a
  temporary measure. It is strongly advised to migrate away from deprecated
  interfaces as soon as possible as they will eventually be removed.

-----------------
Inject allocators
-----------------
Routines that need to allocate memory dynamically should do so using the generic
:cc:`pw::Allocator` interface. By using dependency
injection, memory users can be kept decoupled from the details of how memory is
provided. This yields the most flexibility for modifying or replacing the
specific allocators.

Use the ``Allocator`` interface
===============================
Write or refactor your memory-using routines to take
a pointer or reference to such an object and use the ``Allocate``,
``Deallocate``, ``Reallocate``, and ``Resize`` methods to manage memory. For
example:

.. literalinclude:: examples/basic.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-basic-allocate]
   :end-before: [pw_allocator-examples-basic-allocate]

.. literalinclude:: examples/basic.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-basic-deallocate]
   :end-before: [pw_allocator-examples-basic-deallocate]

To invoke methods or objects that inject allocators now requires an allocator to
have been constructed. The exact location where allocators should be
instantiated will vary from project to project, but it's likely to be early in a
program's lifecycle and in a device-specific location of the source tree.

For initial testing on :ref:`target-host`, a simple allocator such as
:cc:`LibCAllocator <pw::allocator::LibCAllocator>` can be used. This
allocator is trivally constructed and simply wraps ``malloc`` and ``free``.

Use New and Delete
==================
In addition to managing raw memory, an ``Allocator`` can also be used to create
and destroy objects:

.. literalinclude:: examples/basic.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-basic-new_delete]
   :end-before: [pw_allocator-examples-basic-new_delete]

Use smart pointers
==================
Where possible, using `RAII`_ is a recommended approach for making memory
management easier and less error-prone.
:cc:`UniquePtr <pw::UniquePtr>` is a smart pointer that resembles
``std::unique_ptr`` and makes allocating and deallocating memory more
transparent:

.. literalinclude:: examples/basic.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-basic-make_unique]
   :end-before: [pw_allocator-examples-basic-make_unique]

In a similar fashion, :cc:`SharedPtr <pw::SharedPtr>` and
:cc:`WeakPtr <pw::WeakPtr>` are smart pointers that resemble
``std::shared_ptr`` and ``std::weak_ptr``, respectively, and share ownership of
allocated memory.

Determine an allocation's Layout
================================
Several of the :cc:`pw::Allocator` methods take a parameter of the
:cc:`Layout <pw::allocator::Layout>` type. This type combines the size
and alignment requirements of an allocation. It can be constructed directly, or
if allocating memory for a specific type, by using a templated static method:

.. literalinclude:: examples/block_allocator.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-block_allocator-layout_of]
   :end-before: [pw_allocator-examples-block_allocator-layout_of]

As stated above, you should generally try to keep allocator implementation
details abstracted behind the :cc:`pw::Allocator`
interface. One exception to this guidance is when integrating allocators into
existing code that assumes ``malloc`` and ``free`` semantics. Notably, ``free``
does not take any parameters beyond a pointer describing the memory to be freed.

.. literalinclude:: examples/block_allocator.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-block_allocator-malloc_free]
   :end-before: [pw_allocator-examples-block_allocator-malloc_free]

.. _module-pw_allocator-use-standard-library-containers:

Use standard library containers
===============================
All of C++'s standard library containers are `AllocatorAwareContainers`_, with
the exception of ``std::array``. These types include a template parameter used
to specify an allocator type, and a constructor which takes a reference to an
object of this type.

While there are
:ref:`module-pw_allocator-design-differences-with-polymorphic-allocators`, an
:cc:`pw::Allocator` can be used with these containers by
wrapping them with a PMR adapter type,
:cc:`PmrAllocator <pw::allocator::PmrAllocator>`:

.. literalinclude:: examples/pmr.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-pmr]
   :end-before: [pw_allocator-examples-pmr]

.. Warning::
   Some of the standard library containers may add a significant amount of
   additional code size and/or memory overhead. In particular, implementations
   of ``std::deque`` are known to preallocate significant memory in order to
   meet its complexity requirements, e.g. O(1) insertion at the front of the
   deque.

.. Warning::
   The standard library containers expect their allocators to throw an exception
   on allocation failure, and do not check for failure themselves. If exceptions
   are disabled, :cc:`PmrAllocator <pw::allocator::PmrAllocator>` instead
   **asserts** that allocation succeeded. Care must be taken in this case to
   ensure that memory is not exhausted.

--------------------------
Choose the right allocator
--------------------------
Once one or more routines are using allocators, you can consider how best to
implement memory allocation for each particular scenario.

.. TODO(b/378549332): Add a decision tree for selecting an allocator.

Concrete allocator implementations
==================================
This module provides several allocator implementations. The following is an
overview. Consult the :cc:`API reference <pw_allocator>` for additional
details.

- :cc:`LibCAllocator <pw::allocator::LibCAllocator>`: Uses ``malloc``,
  ``realloc``, and ``free``. This should only be used if the ``libc`` in use
  provides those functions. This allocator is a stateless singleton that may be
  referenced using ``GetLibCAllocator()``.
- :cc:`NullAllocator <pw::allocator::NullAllocator>`: Always fails. This
  may be useful if allocations should be disallowed under specific
  circumstances. This allocator is a stateless singleton that may be referenced
  using ``GetNullAllocator()``.
- :cc:`BumpAllocator <pw::allocator::BumpAllocator>`: Allocates objects
  out of a region of memory and only frees them all at once when the allocator
  is destroyed.
- :cc:`BuddyAllocator <pw::allocator::BuddyAllocator>`: Allocates objects
  out of a blocks with sizes that are powers of two. Blocks are split evenly for
  smaller allocations and merged on free.
- :cc:`BlockAllocator <pw::allocator::BlockAllocator>`: Tracks memory
  using the :cc:`Block API <pw_allocator_block>`. Derived types use
  specific strategies for how to choose a block to use to satisfy a request.
  See also :ref:`module-pw_allocator-design-blocks`. Derived types include:

  - :cc:`FirstFitAllocator <pw::allocator::FirstFitAllocator>`: Chooses
    the first block that's large enough to satisfy a request. This strategy is
    very fast, but may increase fragmentation.
  - :cc:`BestFitAllocator <pw::allocator::BestFitAllocator>`: Chooses the
    smallest block that's large enough to satisfy a request. This strategy
    maximizes the avilable space for large allocations, but may increase
    fragmentation and is slower.
  - :cc:`WorstFitAllocator <pw::allocator::WorstFitAllocator>`: Chooses
    the largest block if it's large enough to satisfy a request. This strategy
    minimizes the amount of memory in unusably small blocks, but is slower.
  - :cc:`BucketAllocator <pw::allocator::BucketAllocator>`:
    Sorts and stores each free blocks in a :cc:`Bucket
    <pw_allocator_bucket>` with a given maximum block inner size.

- :cc:`TypedPool <pw::allocator::TypedPool>`: Efficiently creates and
  destroys objects of a single given type.

Forwarding allocator implementations
====================================
This module provides several "forwarding" allocators, as described in
:ref:`module-pw_allocator-design-forwarding`. The following is an overview.
Consult the :cc:`API reference <pw_allocator>` for additional details.

- :cc:`FallbackAllocator <pw::allocator::FallbackAllocator>`: Dispatches
  first to a primary allocator, and, if that fails, to a secondary allocator.
- :cc:`PmrAllocator <pw::allocator::PmrAllocator>`: Adapts an allocator to
  be a ``std::pmr::polymorphic_allocator``, which can be used with standard
  library containers that `use allocators`_, such as ``std::pmr::vector<T>``.
- :cc:`GuardedAllocator <pw::allocator::GuardedAllocator>`: Inserts guard
  values before and after allocations, and provides a thread-safe way to check
  them in order to detect heap overflows.
- :cc:`SynchronizedAllocator <pw::allocator::SynchronizedAllocator>`:
  Synchronizes access to another allocator, allowing it to be used by multiple
  threads.
- :cc:`TrackingAllocator <pw::allocator::TrackingAllocator>`: Wraps
  another allocator and records its usage.

.. _module-pw_allocator-guide-custom_allocator:

Custom allocator implementations
================================
If none of the allocator implementations provided by this module meet your
needs, you can implement your allocator and pass it into any routine that uses
the generic interface.

:cc:`pw::Allocator` uses an `NVI`_ pattern. To add a custom
allocator implementation, you must at a miniumum implement the ``DoAllocate``
and ``DoDeallocate`` methods.

For example, the following is a forwarding allocator that simply writes to the
log whenever a threshold is exceeded:

.. literalinclude:: examples/public/examples/custom_allocator.h
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-custom_allocator]

.. literalinclude:: examples/custom_allocator.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-custom_allocator]

There are also several optional methods you can provide:

- If an implementation of ``DoResize`` isn't provided, then ``Resize`` will
  always return false.
- If an implementation of ``DoReallocate`` isn't provided, then ``Reallocate``
  will try to ``Resize``, and, if unsuccessful, try to ``Allocate``, copy, and
  ``Deallocate``.
- If an implementation of ``DoGetInfo`` isn't provided, then ``GetInfo``
  will always return ``pw::Status::Unimplmented``.

Custom allocators can indicate which optional methods they implement and what
optional behaviors they want from the base class by specifying
:cc:`Capabilities <pw::allocator::Capabilities>` when invoking the base
class constructor.

.. TODO: b/328076428 - Make Deallocate optional once traits supporting
   MonotonicAllocator are added.

--------------------
Measure memory usage
--------------------
You can observe how much memory is being used for a particular use case using a
:cc:`TrackingAllocator <pw::allocator::TrackingAllocator>`.

.. literalinclude:: examples/metrics.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-metrics-custom_metrics1]
   :end-before: [pw_allocator-examples-metrics-custom_metrics1]

.. literalinclude:: examples/metrics.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-metrics-custom_metrics2]
   :end-before: [pw_allocator-examples-metrics-custom_metrics2]

Metric data can be retrieved according to the steps described in
:ref:`module-pw_metric-exporting`, or by using the ``Dump`` method of
:ref:`module-pw_metric-group`:

.. literalinclude:: examples/metrics.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-metrics-dump]
   :end-before: [pw_allocator-examples-metrics-dump]


The ``CustomMetrics`` type used in the example above is a struct provided by the
developer. You can create your own metrics structs that enable zero or more of
the following metrics:

- **requested_bytes**: The number of bytes currently requested from this
  allocator.
- **peak_requested_bytes**: The most bytes requested from this allocator at any
  one time.
- **cumulative_requested_bytes**: The total number of bytes that have been
  requested from this allocator across its lifetime.
- **allocated_bytes**: The number of bytes currently allocated by this
  allocator.
- **peak_allocated_bytes**: The most bytes allocated by this allocator at any
  one time.
- **cumulative_allocated_bytes**: The total number of bytes that have been
  allocated by this allocator across its lifetime.
- **num_allocations**: The number of allocation requests this allocator has
  successfully completed.
- **num_deallocations**: The number of deallocation requests this allocator has
  successfully completed.
- **num_resizes**: The number of resize requests this allocator has successfully
  completed.
- **num_reallocations**: The number of reallocation requests this allocator has
  successfully completed.
- **num_failures**: The number of requests this allocator has failed to
  complete.

If you only want a subset of these metrics, you can implement your own metrics
struct. For example:

.. literalinclude:: examples/metrics.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-metrics-custom_metrics1]
   :end-before: [pw_allocator-examples-metrics-custom_metrics1]

If you have multiple routines that share an underlying allocator that you want
to track separately, you can create multiple tracking allocators that forward to
the same underlying allocator:

.. literalinclude:: examples/metrics.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-metrics-multiple_trackers]
   :end-before: [pw_allocator-examples-metrics-multiple_trackers]

Measure fragmentation
=====================
If you are using a :cc:`BlockAllocator <pw::allocator::BlockAllocator>`,
you can use the ``MeasureFragmentation`` method to examine how fragmented the
heap is. This method returns a :cc:`Fragmentation
<pw::allocator::Fragmentation>` struct, which includes the "sum of squares" and
the sum of the inner sizes of the current free blocks. On a platform or host
with floating point support, you can divide the square root of the sum of
squares by the sum to obtain a number that ranges from 0 to 1 to indicate
maximal and minimal fragmenation, respectively. Subtracting this number from 1
can give a more intuitive "fragmenation score".

For example, consider a heap consisting of the following blocks:

- 100 bytes in use.
- 200 bytes free.
- 50 bytes in use.
- 10 bytes free.
- 200 bytes in use.
- 300 bytes free.

For such a heap, ``MeasureFragmentation`` will return 130100 and 510. The above
calculation gives a fragmentation score of ``1 - sqrt(130100) / 510``, which is
approximately ``0.29``.

.. TODO: b/328648868 - Add guide for heap-viewer and link to cli.rst.

------------------------
Detect memory corruption
------------------------
The :ref:`module-pw_allocator-design-blocks` provide a few different mechanisms
to help detect memory corruptions when they happen. On every deallocation they
will check the integrity of the block header and assert if it has been modified.

Additionally, you can enable poisoning to detect additional memory corruptions
such as use-after-frees. The :cc:`configuration <pw_allocator_config>`
for ``pw_allocator`` includes the
:cc:`PW_ALLOCATOR_BLOCK_POISON_INTERVAL` option. If a block derives from
:cc:`PoisonableBlock <pw::allocator::PoisonableBlock>`, the allocator
will "poison" every N-th block it frees. Allocators "poison" blocks by writing
a set pattern to the usable memory, and later check on allocation that the
pattern is intact. If it is not, something has illegally modified unallocated
memory.

----------------------
Test custom allocators
----------------------
If you create your own allocator implementation, it's strongly recommended that
you test it as well. If you're creating a forwarding allocator, you can use
:cc:`AllocatorForTest <pw::allocator::test::AllocatorForTest>`. This
simple allocator provides its own backing storage and automatically frees any
outstanding allocations when it goes out of scope. It also tracks the most
recent values provided as parameters to the interface methods.

For example, the following tests the custom allocator from
:ref:`module-pw_allocator-guide-custom_allocator`:

.. literalinclude:: examples/custom_allocator_test.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-custom_allocator-unit_test]
   :end-before: [pw_allocator-examples-custom_allocator-unit_test]

You can also extend the :cc:`TestHarness
<pw::allocator::test::TestHarness>` to perform pseudorandom sequences of
allocations and deallocations, e.g. as part of a performance test:

.. literalinclude:: examples/public/examples/custom_allocator_test_harness.h
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-custom_allocator-test_harness]

.. literalinclude:: examples/custom_allocator_perf_test.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-custom_allocator-perf_test]

Even better, you can easily add fuzz tests for your allocator. This module uses
the :cc:`TestHarness <pw::allocator::test::TestHarness>` to integrate
with :ref:`module-pw_fuzzer` and provide :cc:`FuzzTest support
<pw_allocator_impl_test_fuzz>`.

.. literalinclude:: examples/custom_allocator_test.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-custom_allocator-fuzz_test]
   :end-before: [pw_allocator-examples-custom_allocator-fuzz_test]

-----------------------------
Measure custom allocator size
-----------------------------
If you create your own allocator implementation, you may wish to measure its
code size, similar to measurements in the module's own
:ref:`module-pw_allocator-size-reports`. You can use ``pw_bloat`` and the
:cc:`size reports API <pw_allocator_impl_size>` to create size reports as
described in :ref:`bloat-howto`.

For example, the C++ code for a size report binary might look like:

.. literalinclude:: examples/size_report.cc
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-size_report]

The resulting binary could be compared with the binary produced from
pw_allocator/size_report/first_fit.cc to identify just the code added in this
case by ``CustomAllocator``.

For example, the Bazel build rules to generate a size report might look like:

.. literalinclude:: examples/BUILD.bazel
   :language: cpp
   :linenos:
   :start-after: [pw_allocator-examples-size_report-bazel]
   :end-before: [pw_allocator-examples-size_report-bazel]

The size report produced by this rule would render as:

.. include:: examples/custom_allocator_size_report

.. _AllocatorAwareContainers: https://en.cppreference.com/w/cpp/named_req/AllocatorAwareContainer
.. _NVI: https://en.wikipedia.org/wiki/Non-virtual_interface_pattern
.. _RAII: https://en.cppreference.com/w/cpp/language/raii
.. _repository: https://bazel.build/concepts/build-ref#repositories
.. _use allocators: https://en.cppreference.com/w/cpp/memory/uses_allocator
