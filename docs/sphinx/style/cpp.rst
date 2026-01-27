.. _docs-pw-style-cpp:

=========
C++ style
=========
.. _Google C++ Style Guide: https://google.github.io/styleguide/cppguide.html

The Pigweed C++ style guide is closely based on the `Google C++ Style Guide`_.
The Google C++ Style Guide applies to Pigweed except as described in this
document.

The Pigweed style guide only applies to Pigweed itself. It does not apply to
projects that use Pigweed or to the third-party code included with Pigweed.
Non-Pigweed code is free to use features restricted by Pigweed, such as dynamic
memory allocation and the entirety of the C++ Standard Library.

Recommendations in the :ref:`docs-embedded-cpp` are considered part of the
Pigweed style guide, but are separated out since it covers more general
embedded development beyond just C++ style.

C++ standard
============
Pigweed C++ code must compile with ``-std=c++17`` in Clang and GCC. C++20 or
newer features may be used as long as the code still compiles unmodified with
C++17 or the features are restricted to specific libraries (e.g.
``//pw_async2:coro``). See ``pw_polyfill/language_feature_macros.h`` for macros
that provide newer C++ features when supported.

Compiler extensions should not be used unless wrapped in a macro or properly
guarded in the preprocessor. See ``pw_processor/compiler.h`` for macros that
wrap compiler-specific features.

Automatic formatting
====================
Pigweed uses `clang-format <https://clang.llvm.org/docs/ClangFormat.html>`_ to
automatically format Pigweed source code. A ``.clang-format`` configuration is
provided with the Pigweed repository.  Within an upstream Pigweed environment, the
`pw format` tool can be used to automatically format code.

Automatic formatting is essential to facilitate large-scale, automated changes
in Pigweed. Therefore, all code in Pigweed is expected to be formatted with
``clang-format`` prior to submission. Existing code may be reformatted at any
time.

If ``clang-format`` formats code in an undesirable or incorrect way, it can be
disabled for the affected lines by adding ``// clang-format off``.
``clang-format`` must then be re-enabled with a ``// clang-format on`` comment.

.. code-block:: cpp

   // clang-format off
   constexpr int kMyMatrix[] = {
       100,  23,   0,
         0, 542,  38,
         1,   2, 201,
   };
   // clang-format on

C Standard Library
==================
In C++ headers, always use the C++ versions of C Standard Library headers (e.g.
``<cstdlib>`` instead of ``<stdlib.h>``). If the header is used by both C and
C++ code, only the C header should be used.

In C++ code, it is preferred to use C functions from the ``std`` namespace. For
example, use ``std::memcpy`` instead of ``memcpy``. The C++ standard does not
require the global namespace versions of the functions to be provided. Using
``std::`` is more consistent with the C++ Standard Library and makes it easier
to distinguish Pigweed functions from library functions.

Within core Pigweed, do not use C standard library functions that allocate
memory, such as ``std::malloc``. There are exceptions to this for when dynamic
allocation is enabled for a system; Pigweed modules are allowed to add extra
functionality when a heap is present; but this must be optional.

C++ Standard Library
====================
Much of the C++ Standard Library is not a good fit for embedded software. Many
of the classes and functions were not designed with the RAM, flash, and
performance constraints of a microcontroller in mind. For example, simply
adding the line ``#include <iostream>`` can increase the binary size by 150 KB!
This is larger than many microcontrollers' entire internal storage.

However, with appropriate caution, a limited set of standard C++ libraries can
be used to great effect. Developers can leverage familiar, well-tested
abstractions instead of writing their own. C++ library algorithms and classes
can give equivalent or better performance than hand-written C code.

A limited subset of the C++ Standard Library is permitted in Pigweed. To keep
Pigweed small, flexible, and portable, functions that allocate dynamic memory
must be avoided. Care must be exercised when using multiple instantiations of a
template function, which can lead to code bloat.

Permitted Headers
-----------------
.. admonition:: The following C++ Standard Library headers are always permitted:
   :class: checkmark

   * ``<array>``
   * ``<complex>``
   * ``<initializer_list>``
   * ``<iterator>``
   * ``<limits>``
   * ``<optional>``
   * ``<random>``
   * ``<ratio>``
   * ``<string_view>``
   * ``<tuple>``
   * ``<type_traits>``
   * ``<utility>``
   * ``<variant>``
   * C Standard Library headers (``<c*>``)

.. admonition:: With caution, parts of the following headers can be used:
   :class: warning

   * ``<algorithm>`` -- be wary of potential memory allocation
   * ``<atomic>`` -- not all MCUs natively support atomic operations
   * ``<bitset>`` -- conversions to or from strings are disallowed
   * ``<functional>`` -- do **not** use ``std::function``; use
     :ref:`module-pw_function`
   * ``<mutex>`` -- can use ``std::lock_guard``, use :ref:`module-pw_sync` for
     mutexes
   * ``<new>`` -- for placement new
   * ``<numeric>`` -- be wary of code size with multiple template instantiations

.. admonition:: Never use any of these headers:
   :class: error

   * Dynamic containers (``<list>``, ``<map>``, ``<set>``, ``<vector>``, etc.)
   * Streams (``<iostream>``, ``<ostream>``, ``<fstream>``, ``<sstream>`` etc.)
     -- in some cases :ref:`module-pw_stream` can work instead
   * ``<span>`` -- use :ref:`module-pw_span` instead. Downstream projects may
     want to directly use ``std::span`` if it is available, but upstream must
     use the ``pw::span`` version for compatability
   * ``<string>`` -- can use :ref:`module-pw_string`
   * ``<thread>`` -- can use :ref:`module-pw_thread`
   * ``<future>`` -- eventually :ref:`module-pw_async2` will offer this
   * ``<exception>``, ``<stdexcept>`` -- no exceptions
   * ``<memory>``, ``<scoped_allocator>`` -- no allocations
   * ``<regex>``
   * ``<valarray>``

Headers not listed here should be carefully evaluated before they are used.

These restrictions do not apply to third party code or to projects that use
Pigweed.

Combining C and C++
===================
Prefer to write C++ code over C code, using ``extern "C"`` for symbols that must
have C linkage. ``extern "C"`` functions should be defined within C++
namespaces to simplify referring to other code.

Declarations in ``.h`` files for ``extern "C"`` functions with no parameters
must include ``void`` in their parameter lists to prevent being called with
parameters erroneously in C code. For consistency with the header file, use
``(void)`` in the C++ definition also.

.. code-block:: cpp
   :caption: example.h

   #include "pw_preprocessor/util.h"

   PW_EXTERN_C_START

   // Ensure (void) is used here for parameter-less functions.
   int pw_ExampleOne(void);
   int pw_ExampleTwo(void);

   PW_EXTERN_C_END

.. code-block:: cpp
   :caption: example.cc

   namespace pw {

   bool ThisIsACppFunction() { return true; }

   // Both of these example functions are C++ functions with C linkage.
   // They can call C++ code and use C++ constructs.
   // (void) is not technically necessary, but included for consistency.
   extern "C" int pw_ExampleOne(void) { return -1; }

   extern "C" {

   int pw_ExampleTwo(void) {
     return ThisIsACppFunction() ? 100 : 0;
   }

   }  // extern "C"

   }  // namespace pw

.. _docs-pw-style-cpp-comments:

Comments
========
Prefer C++-style (``//``) comments over C-style comments (``/* */``). C-style
comments should only be used for inline comments.

.. code-block:: cpp

   // Use C++-style comments, except where C-style comments are necessary.
   // This returns a random number using an algorithm I found on the internet.
   #define RANDOM_NUMBER()                      \
     [] {                                       \
       return 4; /* chosen by fair dice roll */ \
     }()

Indent code in comments with two additional spaces, making a total of three
spaces after the ``//``. All code blocks must begin and end with an empty
comment line, even if the blank comment line is the last line in the block.

.. code-block:: cpp

   // Here is an example of code in comments.
   //
   //   int indentation_spaces = 2;
   //   int total_spaces = 3;
   //
   //   engine_1.thrust = RANDOM_NUMBER() * indentation_spaces + total_spaces;
   //
   bool SomeFunction();

Passing move-only or expensive-to-copy arguments
================================================
C++ offers a number of ways to pass arguments arguments to functions.
When taking move-only or expensive-to-copy arguments, use the following table
to determine which argument type to use:

.. list-table:: C++ argument type choices
   :widths: 30 20 10
   :header-rows: 1

   * - Use-case
     - Name
     - Syntax
   * - If read-only
     - By const reference
     - ``const T&``
   * - If mutating
     - By reference
     - ``T&``
   * - If consuming
     - By rvalue reference
     - ``T&&``
   * - If conditionally consuming
     - By value
     - ``T``

Why rvalue references
---------------------
When a function consumes or moves such an argument, it should accept an rvalue
reference (``T&&``) rather than taking the argument by-value (``T``). An rvalue
reference forces the caller to ``std::move`` when passing a preexisting
variable, which makes the transfer of ownership explicit.

Compared with accepting arguments by-value, rvalue references prevent
unnecessary object instances and extra calls to move constructors. This has been
shown to significantly impact code size and stack usage for Pigweed users.

This is especially important when using ``pw::Function``. For more information
about accepting ``pw::Function`` arguments, see
:ref:`module-pw_function-move-semantics`.

.. admonition:: **Yes**: Accept move-only or expensive-to-copy values by rvalue:
   :class: checkmark

   .. code-block:: cpp

      void FrobulateVector(pw::Vector<T>&& vector) { Frobulate(std::move(vector)); }

.. admonition:: **No**: Accepts move-only or expensive-to-copy values by value:
   :class: error

   .. code-block:: cpp

      void FrobulateVector(pw::Vector<T> vector) { Frobulate(std::move(vector)); }

This guidance overrides the standard `Google style guidance on rvalues
<https://google.github.io/styleguide/cppguide.html#Rvalue_references>`_.

Conditionally moving values
---------------------------
An exception to the rule above is when a move-only or expensive-to-copy value
is only conditionally consumed by the body of the function, for example:

.. admonition:: **No**: Conditionally consumes ``vector``:
   :class: error

   .. code-block:: cpp

      void PossiblyFrobulate(bool should_frob, pw::Vector<T>&& vector) {
        if (should_frob) {
          Frobulate(std::move(vector));
        }
      }

Because ``PossiblyFrobulate`` above will only consume ``vector`` in some code
paths, the original ``vector`` passed by the user will outlive the call to
``PossiblyFrobulate``:

.. code-block:: cpp

   pw::Vector<T> my_vec = /* ... */;

   // ``my_vec`` looks to be moved here, but the resulting ``rvalue`` is never
   // consumed by ``PossiblyFrobulate``.
   PossiblyFrobulate(false, std::move(my_vec));

   // some other long-running work ...

   // ``my_vec`` is still alive here, possibly causing excess memory usage,
   // deadlocks, or even undefined behavior!

When conditionally consuming an argument, prefer instead to either accept
the argument by-value or ensure that it is consumed by all control paths:

.. admonition:: **Yes**: Conditionally consumes by-value ``vector``:
   :class: checkmark

   .. code-block:: cpp

      void PossiblyFrobulate(bool should_frob, pw::Vector<T> vector) {
        if (should_frob) {
          Frobulate(std::move(vector));
        }
      }

.. admonition:: **Yes**: Consumes ``vector`` on all control paths:
   :class: checkmark

   .. code-block:: cpp

      void PossiblyFrobulate(bool should_frob, pw::Vector<T>&& vector) {
        if (should_frob) {
          Frobulate(std::move(vector));
        } else {
          [[maybe_unused]] auto to_discard = std::move(vector);
        }
      }

Control statements
==================

Loops and conditionals
----------------------
All loops and conditional statements must use braces, and be on their own line.

.. admonition:: **Yes**: Always use braces for line conditionals and loops:
   :class: checkmark

   .. code-block:: cpp

      while (SomeCondition()) {
        x += 2;
      }
      if (OtherCondition()) {
        DoTheThing();
      }

.. admonition:: **No**: Missing braces
   :class: error

   .. code-block:: cpp

      while (SomeCondition())
        x += 2;
      if (OtherCondition())
        DoTheThing();

.. admonition:: **No**: Statement on same line as condition
   :class: error

   .. code-block:: cpp

      while (SomeCondition()) {
        x += 2;
      }
      if (OtherCondition()) {
        DoTheThing();
      }

The syntax ``while (true)`` is preferred over ``for (;;)`` for infinite loops.

.. admonition:: **Yes**:
   :class: checkmark

   .. code-block:: cpp

      while (true) {
        DoSomethingForever();
      }

.. admonition:: **No**:
   :class: error

   .. code-block:: cpp

      for (;;) {
        DoSomethingForever();
      }

Do not declare empty infinite loops, which are undefined behavior. Instead, call
:cc:`pw::BusyWaitForever`.

.. admonition:: **Yes**:
   :class: checkmark

   .. code-block:: cpp

      pw::BusyWaitForever();

.. admonition:: **No**:
   :class: error

   .. code-block:: cpp

      while (true) {
        // The compiler may optimize out this loop!
      }

Prefer early exit with ``return`` and ``continue``
--------------------------------------------------
Prefer to exit early from functions and loops to simplify code. This is the
same same conventions as `LLVM
<https://llvm.org/docs/CodingStandards.html#use-early-exits-and-continue-to-simplify-code>`_.
We find this approach is superior to the "one return per function" style for a
multitude of reasons:

* **Visually**, the code is easier to follow, and takes less horizontal screen
  space.
* It makes it clear what part of the code is the **"main business" versus "edge
  case handling"**.
* For **functions**, parameter checking is in its own section at the top of the
  function, rather than scattered around in the function body.
* For **loops**, element checking is in its own section at the top of the loop,
  rather than scattered around in the loop body.
* Commit **deltas are simpler to follow** in code reviews; since adding a new
  parameter check or loop element condition doesn't cause an indentation change
  in the rest of the function.

The guidance applies in two cases:

* **Function early exit** - Early exits are for function parameter checking
  and edge case checking at the top. The main functionality follows.
* **Loop early exit** - Early exits in loops are for skipping an iteration
  due to some edge case with an item getting iterated over. Loops may also
  contain function exits, which should be structured the same way (see example
  below).

.. admonition:: **Yes**: Exit early from functions; keeping the main handling
   at the bottom and de-dentend.
   :class: checkmark

   .. code-block:: cpp

      Status DoSomething(Parameter parameter) {
        // Parameter validation first; detecting incoming use errors.
        PW_CHECK_INT_EQ(parameter.property(), 3, "Programmer error: frobnitz");

        // Error case: Not in correct state.
        if (parameter.other() == MyEnum::kBrokenState) {
          LOG_ERROR("Device in strange state: %s", parametr.state_str());
          return Status::InvalidPrecondition();
        }

        // Error case: Not in low power mode; shouldn't do anything.
        if (parameter.power() != MyEnum::kLowPower) {
          LOG_ERROR("Not in low power mode");
          return Status::InvalidPrecondition();
        }

        // Main business for the function here.
        MainBody();
        MoreMainBodyStuff();
      }

.. admonition:: **No**: Main body of function is buried and right creeping.
   Even though this is shorter than the version preferred by Pigweed due to
   factoring the return statement, the logical structure is less obvious. A
   function in Pigweed containing **nested conditionals indicates that
   something complicated is happening with the flow**; otherwise it would have
   the early bail structure; so pay close attention.
   :class: error

   .. code-block:: cpp

      Status DoSomething(Parameter parameter) {
        // Parameter validation first; detecting incoming use errors.
        PW_CHECK_INT_EQ(parameter.property(), 3, "Programmer error: frobnitz");

        // Error case: Not in correct state.
        if (parameter.other() != MyEnum::kBrokenState) {
          // Error case: Not in low power mode; shouldn't do anything.
          if (parameter.power() == MyEnum::kLowPower) {
            // Main business for the function here.
            MainBody();
            MoreMainBodyStuff();
          } else {
            LOG_ERROR("Not in low power mode");
          }
        } else {
          LOG_ERROR("Device in strange state: %s", parametr.state_str());
        }
        return Status::InvalidPrecondition();
      }

.. admonition:: **Yes**: Bail early from loops; keeping the main handling at
   the bottom and de-dentend.
   :class: checkmark

   .. code-block:: cpp

      for (int i = 0; i < LoopSize(); ++i) {
        // Early skip of item based on edge condition.
        if (!CommonCase()) {
          continue;
        }
        // Early exit of function based on error case.
        int my_measurement = GetSomeMeasurement();
        if (my_measurement < 10) {
          LOG_ERROR("Found something strange; bailing");
          return Status::Unknown();
        }

        // Main body of the loop.
        ProcessItem(my_items[i], my_measurement);
        ProcessItemMore(my_items[i], my_measurement, other_details);
        // ...
      }

.. admonition:: **No**: Right-creeping code with the main body buried inside
   some nested conditional. This makes it harder to understand what is the
   main purpose of the loop versus what is edge case handling.
   :class: error

   .. code-block:: cpp

      for (int i = 0; i < LoopSize(); ++i) {
        if (CommonCase()) {
          int my_measurement = GetSomeMeasurement();
          if (my_measurement >= 10) {
            // Main body of the loop.
            ProcessItem(my_items[i], my_measurement);
            ProcessItemMore(my_items[i], my_measurement, other_details);
            // ...
          } else {
            LOG_ERROR("Found something strange; bailing");
            return Status::Unknown();
          }
        }
      }

There are cases where this structure doesn't work, and in those cases, it is
fine to structure the code differently.

No ``else`` after ``return`` or ``continue``
--------------------------------------------
Do not put unnecessary ``} else {`` blocks after blocks that terminate with a
return, since this causes unnecessary rightward indentation creep. This
guidance pairs with the preference for early exits to reduce code duplication
and standardize loop/function structure.

.. admonition:: **Yes**: No else after return or continue
   :class: checkmark

   .. code-block:: cpp

      // Note lack of else block due to return.
      if (Failure()) {
        DoTheThing();
        return Status::ResourceExausted();
      }

      // Note lack of else block due to continue.
      while (MyCondition()) {
        if (SomeEarlyBail()) {
          continue;
        }
        // Main handling of item
        // ...
      }

      DoOtherThing();
      return OkStatus();

.. admonition:: **No**: Else after return needlessly creeps right
   :class: error

   .. code-block:: cpp

      if (Failure()) {
        DoTheThing();
        return Status::ResourceExausted();
      } else {
        while (MyCondition()) {
          if (SomeEarlyBail()) {
            continue;
          } else {
            // Main handling of item
            // ...
          }
        }
        DoOtherThing();
        return OkStatus();
      }

Error handling
==============
Historically, exceptions have been avoided in embedded C++ as well as in general
C++ code written at Google. Instead, assertions and error codes are used to
communicate errors with less overhead.

Signal and propagate non-fatal errors with ``pw::Status`` and ``pw::Result``,
and assert/check for fatal errors.

Add log statements to help with error tracking. See
:ref:`guidance below <docs-pw-style-cpp-logging>` on how to craft high-value,
low-noise logs.

.. note:

Like Google's C++ style guide, Pigweed does not use exceptions. The case for
avoiding exceptions on embedded is primarily due to reducing code size.

Recoverable errors
------------------
Use the following to report non-fatal failures from subroutines:

- :cpp:type:`pw::Status`: Zero-overhead type that wraps a
  :ref:`status code <module-pw_status-quickref>`.
- :ref:`pw::Result <module-pw_result>`: Union of a status code and a value.
- :ref:`pw::StatusWithSize <module-pw_status-guide-status-with-size>`: A status
  combined with a size. Especially useful for operations which may partially
  succeed, such as a write that sent some bytes before failing.

Fatal errors
------------
Use :c:macro:`PW_ASSERT` and the :c:macro:`PW_CHECK` family of macros to halt
execution on a fatal error.

- These are appropriate when the security of the device is compromised.

  - Example: memory corruption is detected.

.. admonition:: **Yes**
   :class: checkmark

   .. code-block:: cpp

      PW_CHECK_NOTNULL(item->next);
      PW_CHECK_PTR_EQ(item, item->next->prev);

- These may be appropriate for instances of unambiguous programmer error.

  - Example: a caller passed a null pointer to a routine that explicitly
    requires non-null pointers.

.. warning::

   Be very careful about introducing new assertions into existing code, or in
   code paths that are not exhaustively tested, or any other scenario that may
   result in crashes in fielded devices.

.. admonition:: **No**: May cause a runtime crash.
   :class: error

   .. code-block:: cpp

      StatusWithSize sws = kvs.Get("some-key", &out);
      PW_CHECK_OK(sws.status());

   The key may be missing from the :ref:`KVS <module-pw_kvs>` for a number of
   reasons. It is likely better to surface this error to a higher level that can
   decide how to handle a missing value.

Include guards
==============
The first non-comment line of every header file must be ``#pragma once``. Do
not use traditional macro include guards. The ``#pragma once`` should come
directly after the Pigweed copyright block, with no blank line, followed by a
blank, like this:

.. code-block:: cpp

   // Copyright 2021 The Pigweed Authors
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
   #pragma once

   // Header file-level comment goes here...

.. _docs-pw-style-cpp-logging:

Logging
=======
Good logging can be incredibly useful in detecting and debugging errors. Log
quality is determined by the amount of useful information relative to overall
amount of logs.

Log in the right spot
---------------------
Limiting logs to only the most relevant sections of code can guide developers to
areas that require debugging.

- **Log errors as soon as they can be unambiguously determined to be errors.**
  An unambiguous error is one that will be reported to the caller of the module
  or component. Avoid logging errors that are handled internally by the module
  or component.

  - Example: A task manager would not log a failure to schedule a specific
    worker from a pool, but may log the failure to find *any* worker in the
    pool.

    .. admonition:: **No**: May log errors even if the call eventually succeeds.
       :class: error

       .. code-block:: cpp

          Status TaskManager::AssignToWorker(Task& task) {
            for (auto& worker : pool_) {
              if (worker.AssignTask(task).ok()) {
                return OkStatus();
              }
            }
            return Status::ResourceExhausted();
          }

          Status Worker::Assign(Task& task) {
            if (busy_) {
              PW_LOG_DEBUG("failed to assign task to worker %zu", id_);
              return Status::FailedPrecondition();
            }
            // ...
          }

    .. admonition:: **Yes**: Only logs when an actual failure has occurred.
       :class: checkmark

       .. code-block:: cpp

          Status TaskManager::AssignToWorker(Task& task) {
            for (auto& worker : pool_) {
              if (worker.AssignTask(task).ok()) {
                return OkStatus();
              }
            }
            PW_LOG_DEBUG("failed to find a worker to handle the task");
            return Status::ResourceExhausted();
          }

          Status Worker::Assign(Task& task) {
            if (busy_) {
              return Status::FailedPrecondition();
            }
            // ...
          }

- **Log failures of an overall workflow at the level that it was initiated** to
  provide context in which an error occurred.

  - Example: A widget may log that it could not perform a user-scheduled task
    because the task manager returned an error.

- **Limit the use of informational logs of non-failure conditions.** These
  "heartbeat" logs can quickly accrue and become noise. If needed, keep the
  frequency of these logs low, e.g. not more than once per second.
  :c:macro:`PW_LOG_EVERY_N` and :c:macro:`PW_LOG_EVERY_N_DURATION` can be used
  to rate-limit such logs.

  .. admonition:: **No**: May spew a large number of logs.
     :class: error

     .. code-block:: cpp

        void OnChunk(const Chunk& chunk) {
          ++count_;
          total_ += chunk.size();
          PW_LOG_DEBUG("Processed %zu chunks totaling %zu bytes", count_, total_);
          // ...
        }

  .. admonition:: **Yes**: Only logs once every 10 seconds.
     :class: checkmark

     .. code-block:: cpp

        void OnChunk(const Packet& packet) {
          static constexpr auto kLogInterval =
              chrono::SystemClock::for_at_least(std::chrono::seconds(10));
          ++count_;
          total_ += packet.size();
          PW_LOG_EVERY_N_DURATION(PW_LOG_LEVEL_DEBUG,
                                  kLogInterval,
                                  "Processed %zu chunks totaling %zu bytes",
                                  count_,
                                  total_);
        }

Log at the correct level
------------------------
:ref:`Log levels <module-pw_log-levels>` indicate the seriousness of a message
and provide a simple mechanism for conditional logging and for log filtering.

- **Downstream projects should use less filtered log levels**, as
  project-specific errors more likely indicate an actionable failure.

  - Use :c:macro:`PW_LOG_CRITICAL` for failures that compromise the entire
    device and will imminently halt or crash the device.
  - Use :c:macro:`PW_LOG_ERROR` for failures that are more serious or harder to
    recover from.
  - Use :c:macro:`PW_LOG_WARN` for failures that are less serious or easier to
    recover from.
  - Use :c:macro:`PW_LOG_INFO` for informational logs of non-failure conditions.

- **Libraries and upstream code should allow configurable logging.** Downstream
  projects may want to disable library and module logging to save on code size,
  or enable it to aid in debugging.

  - Use :c:macro:`PW_LOG_DEBUG` to log specific errors that the caller is
    expected to handle.

    .. admonition:: **Yes**
       :class: checkmark

       .. code-block:: cpp

          if (stream.IsClosed()) {
            PW_LOG_DEBUG("Stream closed unexpectedly");
            return Status::OutOfRange();
          }

  - Use :c:macro:`PW_LOG_INFO` and :c:macro:`PW_LOG_WARN` to communicate error
    conditions that may not have a caller that can handle them.

    .. admonition:: **Yes**
       :class: checkmark

       .. code-block:: cpp

          while (!task_queue_.empty()) {
            Task task = std::move(task_queue_.back());
            task_queue_.pop_back();
            if (task.HasExpired()) {
              PW_LOG_INFO("Task %zu expired before being scheduled", task.id());
              continue;
            }
            Schedule(std::move(task));
            // ...
          }

  - Set a :c:macro:`PW_LOG_LEVEL`. If logging in a module with a
    :ref:`module configuration <module-structure-compile-time-configuration>`,
    include a logging option and set :c:macro:`PW_LOG_LEVEL` to it.

    .. admonition:: **Yes**
       :class: checkmark

       .. code-block:: cpp

          // In my_module's config.h. Can be overridden at compile time.
          #ifndef MY_MODULE_LOG_LEVEL
          #define MY_MODULE_LOG_LEVEL PW_LOG_LEVEL_WARN
          #endif MY_MODULE_LOG_LEVEL

          // In my_module's source files.
          #include "my_module/config.h"
          #define PW_LOG_LEVEL MY_MODULE_LOG_LEVEL

Log the right information
-------------------------
Logging the most useful information requires considering what may be relevant to
an error and cannot be obtained another way.

- **Include relevant context**, such as function parameters.
- **Capitalize your log message, but do not end with punctuation.** Log backends
  typically combine your log message with additional information and format
  them.

.. admonition:: **No**
   :class: error

   .. code-block:: cpp

      PW_LOG_DEBUG("the operation did not complete normally.");

.. admonition:: **Yes**
   :class: checkmark

   .. code-block:: cpp

      PW_LOG_DEBUG("The operation completed normally");

- **Set** :c:macro:`PW_LOG_MODULE_NAME` to include a
  module name that you can filter on.

.. admonition:: **Yes**
   :class: checkmark

   .. code-block:: cpp

      #define PW_LOG_MODULE_NAME "my_module"

- **Do not include source location details.** The log backend can be configured
  to add various :ref:`module-pw_log-logging_attributes` automatically.

.. admonition:: **No**
   :class: error

   .. code-block:: cpp

      PW_LOG_DEBUG("%s:%d: %s called", __FILE__, __LINE__, __PRETTY_FUNCTION__);

- **Do not log** :cpp:type:`pw::Status` **details.** If you are logging and
  returning an error as a result of a subroutine that returned an error, it is
  likely that a log statement can be added closer to where that error was
  detected.

.. admonition:: **No**
   :class: error

   .. code-block:: cpp

      Result<Message> ReadAndDecode(Stream& stream) {
        Result<EncodedMessage> result = ReadEncodedMessage(stream);
        if (!result.ok()) {
          Status status = result.status();
          PW_LOG_DEBUG("Failed to read message: %s", pw_StatusString(status.code));
          return status;
        }
        // ...
      }

Memory allocation
=================
Dynamic memory allocation can be problematic. Heap allocations and deallocations
occupy valuable CPU cycles. Memory usage becomes nondeterministic, which can
result in a system crashing without a clear culprit.

To keep Pigweed portable, core Pigweed code is not permitted to dynamically
(heap) allocate memory, such as with ``malloc`` or ``new``. All memory should be
allocated with automatic (stack) or static (global) storage duration. Pigweed
must not use C++ libraries that use dynamic allocation.

Projects that use Pigweed are free to use dynamic allocation, provided they
have selected a target that enables the heap.

Naming
======
Entities shall be named according to the `Google style guide
<https://google.github.io/styleguide/cppguide.html>`_, with the following
additional requirements.

C++ code
--------
* All Pigweed C++ code must be in the ``pw`` namespace. Namespaces for modules
  should be nested under ``pw``. For example, ``pw::string::Format()``.
* Whenever possible, private code should be in a source (.cc) file and placed in
  anonymous namespace nested under ``pw``. Unit tests must be declared in an
  anonymous namespace to avoid potential linking issues when building multiple
  tests in one binary.
* If private code must be exposed in a header file, it must be in a namespace
  nested under ``pw``. The namespace may be named for its subsystem or use a
  name that designates it as private, such as ``internal``.
* Template arguments for non-type names (e.g. ``template <int kFooBar>``) should
  follow the constexpr and const variable Google naming convention, which means
  k prefixed camel case (e.g.  ``kCamelCase``). This matches the Google C++
  style for variable naming, however the wording in the official style guide
  isn't explicit for template arguments and could be interpreted to use
  ``foo_bar`` style naming.  For consistency with other variables whose value is
  always fixed for the duration of the program, the naming convention is
  ``kCamelCase``, and so that is the style we use in Pigweed.
* Trivial member accessors should be named with ``snake_case()``. The Google
  C++ style allows either ``snake_case()`` or ``CapsCase()``, but Pigweed
  always uses ``snake_case()``.
* Abstract base classes should be named generically, with derived types named
  specifically. For example, ``Stream`` is an abstract base, and
  ``SocketStream`` and ``StdioStream`` are an implementations of that
  interface.  Any prefix or postfix indicating whether something is abstract or
  concrete is not permitted; for example, ``IStream`` or ``SocketStreamImpl``
  are both not permitted. These pre-/post-fixes add additional visual noise and
  are irrelevant to consumers of these interfaces.

C code
------
In general, C symbols should be prefixed with the module name. If the symbol is
not associated with a module, use just ``pw`` as the module name. Facade
backends may chose to prefix symbols with the facade's name to help reduce the
length of the prefix.

* Public names used by C code must be prefixed with the module name (e.g.
  ``pw_tokenizer_*``).
* If private code must be exposed in a header, private names used by C code must
  be prefixed with an underscore followed by the module name (e.g.
  ``_pw_assert_*``).
* Avoid writing C source (.c) files in Pigweed. Prefer to write C++ code with C
  linkage using ``extern "C"``. Within C source, private C functions and
  variables must be named with the ``_pw_my_module_*`` prefix and should be
  declared ``static`` whenever possible; for example,
  ``_pw_my_module_MyPrivateFunction``.
* The C prefix rules apply to

  * C functions (``int pw_foo_FunctionName(void);``),
  * variables used by C code (``int pw_foo_variable_name;``),
  * constant variables used by C code (``const int pw_foo_kConstantName;``),
  * structs used by C code (``typedef struct {} pw_foo_StructName;``), and
  * all of the above for ``extern "C"`` names in C++ code.

  The prefix does not apply to struct members, which use normal Google style.

Preprocessor macros
-------------------
* Public Pigweed macros must be prefixed with the module name (e.g.
  ``PW_MY_MODULE_*``).
* Private Pigweed macros must be prefixed with an underscore followed by the
  module name (e.g. ``_PW_MY_MODULE_*``). (This style may change, see
  `b/234886184 <https://issuetracker.google.com/issues/234886184>`_).

**Example**

.. code-block:: cpp

   namespace pw::my_module {
   namespace nested_namespace {

   // C++ names (types, variables, functions) must be in the pw namespace.
   // They are named according to the Google style guide.
   constexpr int kGlobalConstant = 123;

   // Prefer using functions over extern global variables.
   extern int global_variable;

   class Class {};

   void Function();

   extern "C" {

   // Public Pigweed code used from C must be prefixed with pw_.
   extern const int pw_my_module_kGlobalConstant;

   extern int pw_my_module_global_variable;

   void pw_my_module_Function(void);

   typedef struct {
     int member_variable;
   } pw_my_module_Struct;

   // Private Pigweed code used from C must be prefixed with _pw_.
   extern const int _pw_my_module_kPrivateGlobalConstant;

   extern int _pw_my_module_private_global_variable;

   void _pw_my_module_PrivateFunction(void);

   typedef struct {
     int member_variable;
   } _pw_my_module_PrivateStruct;

   }  // extern "C"

   // Public macros must be prefixed with PW_.
   #define PW_MY_MODULE_PUBLIC_MACRO(arg) arg

   // Private macros must be prefixed with _PW_.
   #define _PW_MY_MODULE_PRIVATE_MACRO(arg) arg

   }  // namespace nested_namespace
   }  // namespace pw::my_module

See :ref:`docs-pw-style-macros` for details about macro usage.

Namespace scope formatting
==========================
All non-indented blocks (namespaces, ``extern "C"`` blocks, and preprocessor
conditionals) must have a comment on their closing line with the
contents of the starting line.

All nested namespaces should be declared together with no blank lines between
them.

.. code-block:: cpp

   #include "some/header.h"

   namespace pw::nested {
   namespace {

   constexpr int kAnonConstantGoesHere = 0;

   }  // namespace

   namespace other {

   const char* SomeClass::yes = "no";

   bool ThisIsAFunction() {
   #if PW_CONFIG_IS_SET
     return true;
   #else
     return false;
   #endif  // PW_CONFIG_IS_SET
   }

   extern "C" {

   const int pw_kSomeConstant = 10;
   int pw_some_global_variable = 600;

   void pw_CFunction() { ... }

   }  // extern "C"

   }  // namespace other
   }  // namespace pw::nested

Using directives for literals
=============================
`Using-directives
<https://en.cppreference.com/w/cpp/language/namespace#Using-directives>`_ (e.g.
``using namespace ...``) are permitted in implementation files only for the
purposes of importing literals such as ``std::chrono_literals`` or
``pw::bytes::unit_literals``. Namespaces that contain any symbols other than
literals are not permitted in a using-directive. This guidance also has no
impact on `using-declarations
<https://en.cppreference.com/w/cpp/language/namespace#Using-declarations>`_
(e.g. ``using foo::Bar;``).

Rationale: Literals improve code readability, making units clearer at the point
of definition.

.. code-block:: cpp

   using namespace std::chrono;                     // Not allowed
   using namespace std::literals::chrono_literals;  // Allowed

   constexpr std::chrono::duration delay = 250ms;

Pointers and references
=======================
For pointer and reference types, place the asterisk or ampersand next to the
type.

.. code-block:: cpp

   int* const number = &that_thing;
   constexpr const char* kString = "theory!";

   bool FindTheOneRing(const Region& where_to_look) {
     // ...
   }

Prefer storing references over storing pointers. Pointers are required when the
pointer can change its target or may be ``nullptr``. Otherwise, a reference or
const reference should be used.

.. _docs-pw-style-macros:

Preprocessor macros
===================
Macros should only be used when they significantly improve upon the C++ code
they replace. Macros should make code more readable, robust, and safe, or
provide features not possible with standard C++, such as stringification, line
number capturing, or conditional compilation. When possible, use C++ constructs
like constexpr variables in place of macros. Never use macros as constants,
except when a string literal is needed or the value must be used by C code.

When macros are needed, the macros should be accompanied with extensive tests
to ensure the macros are hard to use wrong.

Stand-alone statement macros
----------------------------
Macros that are standalone statements must require the caller to terminate the
macro invocation with a semicolon (see `Swalling the Semicolon
<https://gcc.gnu.org/onlinedocs/cpp/Swallowing-the-Semicolon.html>`_). For
example, the following does *not* conform to Pigweed's macro style:

.. code-block:: cpp

   // BAD! Definition has built-in semicolon.
   #define PW_LOG_IF_BAD(mj) CallSomeFunction(mj);

   // BAD! Compiles without error; semicolon is missing.
   PW_LOG_IF_BAD("foo")

Here's how to do this instead:

.. code-block:: cpp

   // GOOD; requires semicolon to compile.
   #define PW_LOG_IF_BAD(mj) CallSomeFunction(mj)

   // GOOD; fails to compile due to lacking semicolon.
   PW_LOG_IF_BAD("foo")

For macros in function scope that do not already require a semicolon, the
contents can be placed in a ``do { ... } while (0)`` loop.

.. code-block:: cpp

   #define PW_LOG_IF_BAD(mj) \
     do {                    \
       if (mj.Bad()) {       \
         Log(#mj " is bad"); \
       }                     \
     } while (0)

Standalone macros at global scope that do not already require a semicolon can
add a ``static_assert`` declaration statement as their last line.

.. code-block:: cpp

   #define PW_NEAT_THING(thing)             \
     bool IsNeat_##thing() { return true; } \
     static_assert(true, "Macros must be terminated with a semicolon")

Private macros in public headers
--------------------------------
Private macros in public headers must be prefixed with ``_PW_``, even if they
are undefined after use; this prevents collisions with downstream users. For
example:

.. code-block:: cpp

   #define _PW_MY_SPECIAL_MACRO(op) ...
   // ...
   // Code that uses _PW_MY_SPECIAL_MACRO()
   //...
   #undef _PW_MY_SPECIAL_MACRO

Macros in private implementation files (.cc)
--------------------------------------------
Macros within .cc files that should only be used within one file should be
undefined after their last use; for example:

.. code-block:: cpp

   #define DEFINE_OPERATOR(op)                   \
     T operator##op(T x, T y) { return x op y; } \
     static_assert(true, "Macros must be terminated with a semicolon")

   DEFINE_OPERATOR(+);
   DEFINE_OPERATOR(-);
   DEFINE_OPERATOR(/);
   DEFINE_OPERATOR(*);

   #undef DEFINE_OPERATOR

Preprocessor conditional statements
===================================
When using macros for conditional compilation, prefer to use ``#if`` over
``#ifdef``. This checks the value of the macro rather than whether it exists.

* ``#if`` handles undefined macros equivalently to ``#ifdef``. Undefined
  macros expand to 0 in preprocessor conditional statements.
* ``#if`` evaluates false for macros defined as 0, while ``#ifdef`` evaluates
  true.
* Macros defined using compiler flags have a default value of 1 in GCC and
  Clang, so they work equivalently for ``#if`` and ``#ifdef``.
* Macros defined to an empty statement cause compile-time errors in ``#if``
  statements, which avoids ambiguity about how the macro should be used.

All ``#endif`` statements should be commented with the expression from their
corresponding ``#if``. Do not indent within preprocessor conditional statements.

.. code-block:: cpp

   #if USE_64_BIT_WORD
   using Word = uint64_t;
   #else
   using Word = uint32_t;
   #endif  // USE_64_BIT_WORD

Unsigned integers
=================
Unsigned integers are permitted in Pigweed. Aim for consistency with existing
code and the C++ Standard Library. Be very careful mixing signed and unsigned
integers.

Features not in the C++ standard
================================
Avoid features not available in standard C++. This includes compiler extensions
and features from other standards like POSIX.

For example, use ``ptrdiff_t`` instead of POSIX's ``ssize_t``, unless
interacting with a POSIX API in intentionally non-portable code. Never use
POSIX functions with suitable standard or Pigweed alternatives, such as
``strnlen`` (use ``pw::string::NullTerminatedLength`` instead).
