.. _module-pw_status-guide:

====================
Get started & guides
====================
.. pigweed-module-subpage::
   :name: pw_status

.. _module-pw_status-get-started:

-----------
Get started
-----------
To deploy ``pw_status``, depend on the library:

.. tab-set::

   .. tab-item:: Bazel

      Add ``@pigweed//pw_status`` to the ``deps`` list in your Bazel target:

      .. code-block::

         cc_library("...") {
           # ...
           deps = [
             # ...
             "@pigweed//pw_status",
             # ...
           ]
         }

   .. tab-item:: GN

      Add ``$dir_pw_status`` to the ``deps`` list in your ``pw_executable()``
      build target:

      .. code-block::

         pw_executable("...") {
           # ...
           deps = [
             # ...
             "$dir_pw_status",
             # ...
           ]
         }

   .. tab-item:: CMake

      Add ``pw_status`` to your ``pw_add_library`` or similar CMake target:

      .. code-block::

         pw_add_library(my_library STATIC
           HEADERS
             ...
           PRIVATE_DEPS
             # ...
             pw_status
             # ...
         )

   .. tab-item:: Zephyr

      There are two ways to use ``pw_status`` from a Zephyr project:

      * Depend on ``pw_status`` in your CMake target (see CMake tab). This is
        the Pigweed team's suggested approach since it enables precise CMake
        dependency analysis.

      * Add ``CONFIG_PIGWEED_STATUS=y`` to the Zephyr project's configuration,
        which causes ``pw_status`` to become a global dependency and have the
        includes exposed to all targets. The Pigweed team does not recommend
        this approach, though it is the typical Zephyr solution.

Then use the status object or try macros:

.. code-block:: cpp

   #include "pw_status/status.h"
   #include "pw_status/try.h"

   pw::Status MyOperation() {
     PW_TRY(SubOp1());
     PW_TRY(SubOp2());
     // ...
     return pw::OkStatus();
   }

------
Guides
------

Tracking the first error encountered
====================================
In some contexts it is useful to track the first error encountered while
allowing execution to continue. Manually writing out ``if`` statements to check
and then assign quickly becomes verbose, and doesn't explicitly highlight the
intended behavior of "latching" to the first error.

.. admonition:: **No**: Track status manually across calls
   :class: error

   .. code-block:: cpp

      Status overall_status;
      for (Sector& sector : sectors) {
        Status erase_status = sector.Erase();
        if (!overall_status.ok()) {
          overall_status = erase_status;
        }

        if (erase_status.ok()) {
          Status header_write_status = sector.WriteHeader();
          if (!overall_status.ok()) {
            overall_status = header_write_status;
          }
        }
      }
      return overall_status;

:cc:`pw::Status` has a :cc:`pw::Status::Update <pw::StatusBase::Update>` helper
function that does exactly this to reduce visual clutter and succinctly
highlight the intended behavior.

.. admonition:: **Yes**: Track status with :cc:`pw::Status::Update <pw::StatusBase::Update>`
   :class: checkmark

   .. code-block:: cpp

      Status overall_status;
      for (Sector& sector : sectors) {
        Status erase_status = sector.Erase();
        overall_status.Update(erase_status);

        if (erase_status.ok()) {
          overall_status.Update(sector.WriteHeader());
        }
      }
      return overall_status;

.. _module-pw_status-guide-custom-status:

Defining custom status types
============================
Many drivers and subsystems have their own set of error codes. Rather than
mapping them onto the built-in :cc:`pw::Status` codes, define a custom status
type that wraps the domain-specific enum while retaining status semantics:
``ok()`` checks, ``[[nodiscard]]`` enforcement, and error propagation with
:cc:`PW_TRY` and :cc:`pw::Result`.

The recommended way to define a custom status type is with
:ref:`module-pw_enum`'s ``PW_ENUM_STATUS`` macro. Declare the status code enum
in a ``pw_cc_enum_library`` header, then register it with ``PW_ENUM_STATUS``.
The first argument is the fully qualified name of the status class to
generate. The second argument is the enumerator that represents success,
including its enum type (e.g. ``I2cStatusCode::kOk``); its namespace is
inferred from the status class if omitted.

.. literalinclude:: ../pw_enum/examples/private/enum_example/i2c_status.h
   :language: cpp
   :start-after: [pw_enum-examples-status-enum]
   :end-before: [pw_enum-examples-status-enum]

.. literalinclude:: ../pw_enum/examples/BUILD.bazel
   :language: python
   :start-after: [pw_enum-examples-status-enum-bazel]
   :end-before: [pw_enum-examples-status-enum-bazel]

This generates a ``[[nodiscard]]`` class named ``I2cStatus`` that inherits from
:cc:`pw::StatusBase`, with:

* Access to the underlying status code enum via ``I2cStatus::Code`` (e.g.
  ``I2cStatus::Code::kNack``).
* Static factory functions for non-OK ``kEnumName``-style enumerators (e.g.
  ``I2cStatus::Nack()``, ``I2cStatus::Timeout()``). No factory is generated for
  the OK code; default construct (``I2cStatus()``) for OK.
* ``Is*()`` query functions (e.g. ``status.IsNack()``). ``IsOk()`` is omitted
  for codes named OK, since ``ok()`` serves that purpose.
* Explicit construction from ``I2cStatus::Code`` (e.g.
  ``I2cStatus(I2cStatus::Code::kNack)``).
* Stringification via ``.str()`` and :cc:`pw::EnumToString`.
* Native support in :cc:`PW_TRY` and :ref:`module-pw_log`.

.. literalinclude:: ../pw_enum/examples/status_enum_test.cc
   :language: cpp
   :start-after: [pw_enum-examples-status-enum-usage]
   :end-before: [pw_enum-examples-status-enum-usage]

See :ref:`module-pw_enum` for details.

.. _module-pw_status-guide-custom-status-type:

Alternative: PW_STATUS_TYPE
---------------------------
If the status code enum cannot be declared in a ``pw_cc_enum_library``,
:cc:`PW_STATUS_TYPE` wraps an existing enum with a :cc:`pw::StatusBase`-derived
class. It does not generate factory functions, and ``str()`` requires the enum
to support :cc:`pw::EnumToString` (e.g. by registering it with ``PW_ENUM``).

.. literalinclude:: status_base_test.cc
   :language: cpp
   :start-after: [pw_status-status_base-custom_enum]
   :end-before: [pw_status-status_base-custom_enum]

.. _module-pw_status-guide-custom-status-manual:

Alternative: Manual definition
------------------------------
To add domain-specific member functions, subclass :cc:`pw::StatusBase`
directly. Pass the derived class and the OK code as template arguments. Declare
the class ``[[nodiscard]]`` and define constructors that forward to
``StatusBase``, whose constructors are protected.

.. literalinclude:: status_base_test.cc
   :language: cpp
   :start-after: [pw_status-status_base-subclass]
   :end-before: [pw_status-status_base-subclass]

.. _module-pw_status-guide-status-with-size:

----------------------------------
Jointly reporting status with size
----------------------------------
:cc:`pw::StatusWithSize` (``pw_status/status_with_size.h``) is a
convenient, efficient class for reporting a status along with an unsigned
integer value.  It is similar to the ``pw::Result<T>`` class, but it stores both
a size and a status, regardless of the status value, and only supports a limited
range (27 bits).

``pw::StatusWithSize`` values may be created with functions similar to
``pw::Status``. For example:

.. code-block:: cpp

   #include "pw_status/status_with_size.h"

   // An OK StatusWithSize with a size of 123.
   StatusWithSize(123);

   // A NOT_FOUND StatusWithSize with a size of 0.
   StatusWithSize::NotFound();

   // A RESOURCE_EXHAUSTED StatusWithSize with a size of 10.
   StatusWithSize::ResourceExhausted(10);

``pw::StatusWithSize`` is useful for cases where an operation may partially
complete - for example read operations may read some number of bytes into an
output buffer, but not all.

-----------------------------------
Reducing error handling boilerplate
-----------------------------------
Manual error handling through return codes is easy to understand and
straightforward to write, but leads to verbose code. To reduce boilerplate,
Pigweed has the :cc:`PW_TRY` (``pw_status/try.h``) macro, easing
development of functions checking or returning :cc:`pw::Status` and
:cc:`pw::StatusWithSize` objects. The ``PW_TRY`` and
:cc:`PW_TRY_WITH_SIZE` macros call a function and do an early return if
the function's return status is not :c:enumerator:`OK`.

Example:

.. code-block:: cpp

   Status PwTryExample() {
     PW_TRY(FunctionThatReturnsStatus());
     PW_TRY(FunctionThatReturnsStatusWithSize());

     // Do something, only executed if both functions above return OK.
   }

   StatusWithSize PwTryWithSizeExample() {
     PW_TRY_WITH_SIZE(FunctionThatReturnsStatus());
     PW_TRY_WITH_SIZE(FunctionThatReturnsStatusWithSize());

     // Do something, only executed if both functions above return OK.
   }

:cc:`PW_TRY_ASSIGN` is for working with ``pw::StatusWithSize`` objects in
functions that return Status. It is similar to ``PW_TRY`` with the addition of
assigning the size from the ``pw::StatusWithSize`` on ok.

.. code-block:: cpp

   Status PwTryAssignExample() {
     size_t size_value PW_TRY_ASSIGN(size_value,
                                     FunctionThatReturnsStatusWithSize());

     // Do something that uses size_value. size_value is only assigned and this
     // following code executed if the PW_TRY_ASSIGN function above returns OK.
   }
