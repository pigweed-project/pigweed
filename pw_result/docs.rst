.. _module-pw_result:

=========
pw_result
=========
.. pigweed-module::
   :name: pw_result

   - **Easy**: Minimal boilerplate via with ``PW_TRY_ASSIGN`` macro and chaining
   - **Reliable**: Propagate errors consistently for less bugs
   - **Battle-tested**: Just like ``absl::StatusOr``, deployed extensively

``pw::Result<T>`` is a union of an error (:cpp:class:`pw::Status`) and a value
(``T``), in a type-safe and convenient wrapper. This enables operations to
return either a value on success, or an error code on failure, in one type.
Propagating errors with this one type is less burdensome than other methods,
reducing the temptation to write code that crashes or fails silently in error
cases. Take the following lengthy code for example:

.. code-block:: cpp

   #include "pw_log/log.h"
   #include "pw_result/result.h"
   #include "pw_status/try.h"

   pw::Result<int> GetBatteryVoltageMillivolts();  // Can fail

   pw::Status UpdateChargerDisplay() {
     const pw::Result<int> battery_mv = GetBatteryVoltageMillivolts();
     if (!battery_mv.ok()) {
       // Voltage read failed; propagate the status to callers.
       return battery_mv.status();
     }
     PW_LOG_INFO("Battery voltage: %d mV", *battery_mv);
     SetDisplayBatteryVoltage(*battery_mv);
     return pw::OkStatus();
   }

The ``PW_TRY_ASSIGN`` macro enables shortening the above to:

.. code-block:: cpp

   pw::Status UpdateChargerDisplay() {
     PW_TRY_ASSIGN(const int battery_mv, GetBatteryVoltageMillivolts());
     PW_LOG_INFO("Battery voltage: %d mV", battery_mv);
     SetDisplayBatteryVoltage(battery_mv);
     return pw::OkStatus();
   }

The ``pw::Result<T>`` class is based on Abseil's ``absl::StatusOr<T>`` class.
See Abseil's `documentation
<https://abseil.io/docs/cpp/guides/status#returning-a-status-or-a-value>`_ and
`usage tips <https://abseil.io/tips/181>`_ for extra guidance.

-----------
Get started
-----------
To deploy ``pw_result``, depend on the library:

.. tab-set::

   .. tab-item:: Bazel

      Add ``@pigweed//pw_result`` to the ``deps`` list in your Bazel target:

      .. code-block::

         cc_library("...") {
           # ...
           deps = [
             # ...
             "@pigweed//pw_result",
             # ...
           ]
         }

   .. tab-item:: GN

      Add ``$dir_pw_result`` to the ``deps`` list in your ``pw_executable()``
      build target:

      .. code-block::

         pw_executable("...") {
           # ...
           deps = [
             # ...
             "$dir_pw_result",
             # ...
           ]
         }

   .. tab-item:: CMake

      Add ``pw_result`` to your ``pw_add_library`` or similar CMake target:

      .. code-block::

         pw_add_library(my_library STATIC
           HEADERS
             ...
           PRIVATE_DEPS
             # ...
             pw_result
             # ...
         )

   .. tab-item:: Zephyr

      There are two ways to use ``pw_result`` from a Zephyr project:

      * Depend on ``pw_result`` in your CMake target (see CMake tab). This is
        the Pigweed team's suggested approach since it enables precise CMake
        dependency analysis.

      * Add ``CONFIG_PIGWEED_RESULT=y`` to the Zephyr project's configuration,
        which causes ``pw_result`` to become a global dependency and have the
        includes exposed to all targets. The Pigweed team does not recommend
        this approach, though it is the typical Zephyr solution.

------
Guides
------

Overview
========
``pw::Result<T>`` objects represent either:

* A value (accessed with ``operator*`` or ``operator->``) and an OK status
* An error (accessed with ``.status()``) and no value

If ``result.ok()`` returns ``true`` the instance contains a valid value (of type
``T``). Otherwise, it does not contain a valid value, and attempting to access
the value is an error.

A ``pw::Result<T>`` instance can never contain both an OK status and a value. In
most cases, this is the appropriate choice. For cases where both a size and a
status must be returned, see :ref:`module-pw_status-guide-status-with-size`.

.. warning::

  Be careful not to use large value types in ``pw::Result`` objects, since they
  are embedded by value. This can quickly consume stack.

Returning a result from a function
==================================
To return a result from a function, return either a value (implicitly indicating
success), or a :ref:`non-OK pw::Status <module-pw_status-codes>` object
otherwise (to indicate failure).

.. code-block:: cpp

   pw::Result<float> GetAirPressureSensor() {
     uint32_t sensor_value;
     if (!vendor_raw_sensor_read(&sensor_value)) {
       return pw::Status::Unavailable();  // Converted to error Result
     }
     return sensor_value / 216.5;  // Converted to OK Result with float
   }

Accessing the contained value
=============================
Accessing the value held by a ``pw::Result<T>`` should be performed via
``operator*`` or ``operator->``, after a call to ``ok()`` has verified that the
value is present.

Accessing the value via ``operator->`` avoids introducing an extra local
variable to capture the result.

.. code-block:: cpp

   #include "pw_result/result.h"

   void Example() {
     if (pw::Result<Foo> foo = TryCreateFoo(); foo.ok()) {
       foo->DoBar();  // Note direct access to value inside the Result
     }
   }

Propagating errors from results
===============================
``pw::Result`` instances are compatible with the ``PW_TRY`` and
``PW_TRY_ASSIGN`` macros, which enable concise propagation of errors for
falliable operations that return values:

.. code-block:: cpp

   #include "pw_result/result.h"
   #include "pw_status/try.h"

   pw::Result<int> GetAnswer();  // Example function.

   pw::Status UseAnswerWithTry() {
     const pw::Result<int> answer = GetAnswer();
     PW_TRY(answer.status());
     if (answer.value() == 42) {
       WhatWasTheUltimateQuestion();
     }
     return pw::OkStatus();
   }

With the ``PW_TRY_ASSIGN`` macro, you can combine declaring the result with the
return:

.. code-block:: cpp

   pw::Status UseAnswerWithTryAssign() {
     PW_TRY_ASSIGN(const int answer, GetAnswer());
     PW_LOG_INFO("Got answer: %d", static_cast<int>(answer));
     return pw::OkStatus();
   }

Chaining results
================
``pw::Result<T>`` also supports chained operations, similar to the additions
made to ``std::optional<T>`` in C++23. These operations allow functions to be
applied to a ``pw::Result<T>`` that would perform additional computation.

These operations do not incur any additional FLASH or RAM cost compared to a
traditional if/else ladder, as can be seen in the `Code size analysis`_.

Without chaining or ``PW_TRY_ASSIGN``, invoking multiple falliable operations is
verbose:

.. code-block:: cpp

   pw::Result<Image> GetCuteCat(const Image& img) {
     pw::Result<Image> cropped = CropToCat(img);
     if (!cropped.ok()) {
       return cropped.status();
     }
     pw::Result<Image> with_tie = AddBowTie(*cropped);
     if (!with_tie.ok()) {
       return with_tie.status();
     }
     pw::Result<Image> with_sparkles = MakeEyesSparkle(*with_tie);
     if (!with_sparkles.ok()) {
       return with_parkes.status();
     }
     return AddRainbow(MakeSmaller(*with_sparkles));
   }

Leveraging ``PW_TRY_ASSIGN`` reduces the verbosity:

.. code-block:: cpp

   // Without chaining but using PW_TRY_ASSIGN.
   pw::Result<Image> GetCuteCat(const Image& img) {
     PW_TRY_ASSIGN(Image cropped, CropToCat(img));
     PW_TRY_ASSIGN(Image with_tie, AddBowTie(*cropped));
     PW_TRY_ASSIGN(Image with_sparkles, MakeEyesSparkle(*with_tie));
     return AddRainbow(MakeSmaller(*with_sparkles));
   }

With chaining we can reduce the code even further:

.. code-block:: cpp

   pw::Result<Image> GetCuteCat(const Image& img) {
     return CropToCat(img)
         .and_then(AddBoeTie)
         .and_then(MakeEyesSparkle)
         .transform(MakeSmaller)
         .transform(AddRainbow);
   }

``pw::Result<T>::and_then``
---------------------------
The ``pw::Result<T>::and_then`` member function will return the result of the
invocation of the provided function on the contained value if it exists.
Otherwise, returns the contained status in a ``pw::Result<U>``, which is the
return type of provided function.

.. code-block:: cpp

   // Expositional prototype of and_then:
   template <typename T>
   class Result {
     template <typename U>
     Result<U> and_then(Function<Result<U>(T)> func);
   };

   Result<Foo> CreateFoo();
   Result<Bar> CreateBarFromFoo(const Foo& foo);

   Result<Bar> bar = CreateFoo().and_then(CreateBarFromFoo);

``pw::Result<T>::or_else``
--------------------------
The ``pw::Result<T>::or_else`` member function will return ``*this`` if it
contains a value. Otherwise, it will return the result of the provided function.
The function must return a type convertible to a ``pw::Result<T>`` or ``void``.
This is particularly useful for handling errors.

.. code-block:: cpp

   // Expositional prototype of or_else:
   template <typename T>
   class Result {
     template <typename U>
       requires std::is_convertible_v<U, Result<T>>
     Result<T> or_else(Function<U(Status)> func);

     Result<T> or_else(Function<void(Status)> func);
   };

   // Without or_else:
   Result<Image> GetCuteCat(const Image& image) {
     Result<Image> cropped = CropToCat(image);
     if (!cropped.ok()) {
       PW_LOG_ERROR("Failed to crop cat: %d", cropped.status().code());
       return cropped.status();
     }
     return cropped;
   }

   // With or_else:
   Result<Image> GetCuteCat(const Image& image) {
     return CropToCat(image).or_else(
         [](Status s) { PW_LOG_ERROR("Failed to crop cat: %d", s.code()); });
   }

Another useful scenario for ``pw::Result<T>::or_else`` is providing a default
value that is expensive to compute. Typically, default values are provided by
using ``pw::Result<T>::value_or``, but that requires the default value to be
constructed regardless of whether you actually need it.

.. code-block:: cpp

   // With value_or:
   Image GetCuteCat(const Image& image) {
     // GenerateCuteCat() must execute regardless of the success of CropToCat
     return CropToCat(image).value_or(GenerateCuteCat());
   }

   // With or_else:
   Image GetCuteCat(const Image& image) {
     // GenerateCuteCat() only executes if CropToCat fails.
     return *CropToCat(image).or_else([](Status) { return GenerateCuteCat(); });
   }

``pw::Result<T>::transform``
----------------------------
The ``pw::Result<T>::transform`` member method will return a ``pw::Result<U>``
which contains the result of the invocation of the given function if ``*this``
contains a value. Otherwise, it returns a ``pw::Result<U>`` with the same
``pw::Status`` value as ``*this``.

The monadic methods for ``and_then`` and ``transform`` are fairly similar. The
primary difference is that ``and_then`` requires the provided function to return
a ``pw::Result``, whereas ``transform`` functions can return any type. Users
should be aware that if they provide a function that returns a ``pw::Result`` to
``transform``, this will return a ``pw::Result<pw::Result<U>>``.

.. code-block:: cpp

   // Expositional prototype of transform:
   template <typename T>
   class Result {
     template <typename U>
     Result<U> transform(Function<U(T)> func);
   };

   Result<int> ConvertStringToInteger(std::string_view);
   int MultiplyByTwo(int x);

   Result<int> x = ConvertStringToInteger("42").transform(MultiplyByTwo);

Results with custom status types
================================
``pw::Result<T, StatusType>`` accepts an optional second template parameter
for the status type. It defaults to ``pw::Status``, but can be any
:ref:`custom status type <module-pw_status-status-base>` derived from
:cc:`pw::StatusBase` with an enum code.

.. literalinclude:: result_test.cc
   :language: c++
   :start-after: [pw_result-custom-status]
   :end-before: [pw_result-custom-status]

``PW_TRY``, ``PW_TRY_ASSIGN``, and ``PW_CHECK_OK`` work with results that use
custom status types. Unlike ``pw::Result<T>``, results with custom status types
are not default constructible.

``pw::Result<void>``
====================
``pw::Result<void, StatusType>`` provides the ``pw::Result`` interface
(``ok()``, ``status()``, ``value()``, ``emplace()``, ``reset()``, equality
comparisons, and the monadic operations) without a contained value. It wraps a
single ``StatusType`` state and is provided to simplify generic code and
coroutines that handle both value-returning and ``void``-returning operations.

Unlike ``pw::Result<T>``, ``pw::Result<void, StatusType>`` may be constructed
or assigned from either an OK or a non-OK status, or constructed in the OK
state with ``std::in_place``:

.. literalinclude:: result_test.cc
   :language: c++
   :start-after: [pw_result-void]
   :end-before: [pw_result-void]

For non-generic functions that only return a status, prefer returning
``pw::Status`` (or ``StatusType``) directly instead of ``pw::Result<void>``.

``pw::Result<Status>`` and ``pw::Result<StatusType, StatusType>``
=================================================================
When no additional value is returned, prefer ``StatusType`` or
``pw::Result<void, StatusType>`` (in generic code). However,
``pw::Result<StatusType, StatusType>`` (such as ``pw::Result<pw::Status>``) is
supported so that generic code and class template argument deduction (CTAD) can
uniformly wrap functions that return ``StatusType``.

Unlike ``pw::Result<void, StatusType>``, which has a single ``StatusType``
state, ``pw::Result<StatusType, StatusType>`` has **two** independent statuses:

* The ``Result``'s own status (inspected via ``result.ok()`` and
  ``result.status()``), which indicates whether a value is present.
* The contained ``StatusType`` value (accessed via ``*result``, ``result->``,
  or ``result.value()`` when ``result.ok()`` is ``true``), which may itself be
  OK or non-OK.

To avoid ambiguity when the value type ``T`` and ``StatusType`` are the same,
``pw::Result<StatusType, StatusType>`` follows these rules:

* **Construction and assignment set the value**: Constructing or assigning a
  ``pw::Result<StatusType, StatusType>`` from a ``StatusType`` or
  ``StatusType::Code`` always initializes or updates the **contained value**
  (``result.ok() == true``), even when the status is not OK.
* **Set errors with** ``reset()``: To put a ``pw::Result<StatusType,
  StatusType>`` into an error state (``result.ok() == false``), call
  ``result.reset(status)`` or ``result.reset<kCode>()`` (or use the default
  constructor for ``pw::Result<pw::Status>``, which initializes the error
  status to ``pw::Status::Unknown()``).
* **Comparisons check the value**: Comparing a ``pw::Result<StatusType,
  StatusType>`` with a ``StatusType`` or ``StatusType::Code`` using ``==`` or
  ``!=`` compares against the **contained value**
  (``result.ok() && *result == rhs``). An error ``Result`` without a value never
  compares equal to a ``StatusType`` value. To compare the ``Result``'s own
  status, use ``result.status() == status`` or ``result.ok()``.
* ``PW_TRY`` **and** ``PW_CHECK_OK``: Passing a ``pw::Result<StatusType,
  StatusType>`` directly to ``PW_TRY``, ``PW_TRY_ASSIGN``, ``PW_CHECK_OK``, or
  ``PW_ASSERT_OK`` is disallowed (via ``static_assert``) because it is
  ambiguous whether the macro should check ``result.ok()`` or ``result->ok()``;
  pass ``result.status()`` or ``*result`` instead. Additionally, avoid using
  ``PW_TRY`` or ``PW_TRY_ASSIGN`` on other expressions inside a function whose
  return type is ``pw::Result<StatusType, StatusType>``: ``PW_TRY`` returns a
  ``StatusType`` on error, which constructs a **contained value**
  (``ok() == true``, ``*result == error_status``) rather than propagating an
  error state (``ok() == false``). Prefer ``and_then()`` / ``transform()`` or
  explicit ``reset()``.

.. literalinclude:: result_test.cc
   :language: c++
   :dedent:
   :start-after: [pw_result-status-value]
   :end-before: [pw_result-status-value]

Resetting a result
==================
``reset()`` destroys the contained value, if any, and sets an error status. The
status may be provided as a non-OK status object, a non-OK status code, or a
template argument (``result.reset<kCode>()``), which checks at compile time that
the code is not OK.

Results with custom error types: ``pw::expected``
=================================================
Most error codes can fit into one of the status codes supported by
``pw::Status``. However, custom error codes are occasionally needed for
interfacing with other libraries, or other special situations. This module
includes the ``pw::expected`` type for these situtaions.

``pw::expected`` is either an alias for ``std::expected`` or a polyfill for that
type if it is not available. This type has a similar use case to ``pw::Result``,
in that it either returns a type ``T`` or an error, but the error may be any
type ``E``, not just ``pw::Status``.  The ``PW_TRY`` and ``PW_TRY_ASSIGN``
macros do not work with ``pw::expected`` but it should be usable in any place
that ``std::expected`` from the ``C++23`` standard could be used.

.. code-block:: cpp

   #include "pw_result/expected.h"

   pw::expected<float, const char*> ReadBatteryVoltageOrError();

   void TrySensorRead() {
     pw::expected<float, const char*> voltage = ReadBatteryVoltageOrError();
     if (!voltage.has_value()) {
       PW_LOG_ERROR("Couldn't read battery: %s", voltage.error());
       return;
     }
     PW_LOG_ERROR("Battery: %f", voltage.value());
   }

For more information, see the `standard library reference
<https://en.cppreference.com/w/cpp/utility/expected>`_.

------
Design
------
.. inclusive-language: disable

``pw::Result<T>``'s implementation is closely based on Abseil's `StatusOr<T>
class <https://github.com/abseil/abseil-cpp/blob/master/absl/status/statusor.h>`_.
There are a few differences:

.. inclusive-language: enable

* ``pw::Result<T>`` objects represent their status code with a ``pw::Status``
  member rather than an ``absl::Status``. The ``pw::Status`` objects are less
  sophisticated but smaller than ``absl::Status`` objects. In particular,
  ``pw::Status`` objects do not support string errors, and are limited to the
  canonical error codes.
* ``pw::Result<T>`` objects are usable in ``constexpr`` statements if the value
  type ``T`` is trivially destructible.

-------
Roadmap
-------
This module is stable.

------------------
Code size analysis
------------------
The table below showcases the difference in size between functions returning a
``pw::Status`` with an output pointer, and functions returning a Result, in
various situations.

Note that these are simplified examples which do not necessarily reflect the
usage of ``pw::Result`` in real code. Make sure to always run your own size
reports to check if ``pw::Result`` is suitable for you.

.. include:: result_size
