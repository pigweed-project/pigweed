.. _module-pw_status:

=========
pw_status
=========
.. pigweed-module::
   :name: pw_status

   - **Easy**: Simple to understand, includes convenient macro
     :cc:`PW_TRY`
   - **Efficient**: No memory allocation, no exceptions
   - **Established**: Just like ``absl::Status``, deployed extensively at Google
   - **Customizable**: Create custom status types for domain-specific enums or
     codes with :cc:`pw::StatusBase`

   :cc:`pw::Status` is Pigweed's error propagation primitive, enabling
   exception-free error handling. The primary feature of ``pw_status`` is the
   :cc:`pw::Status` class, a simple, zero-overhead status object that
   wraps a status code, and the ``PW_TRY`` macro. For example:

   .. code-block:: cpp

      #include "pw_status/status.h"

      pw::Status ImuEnable() {
        if (!device_has_imu) {
          return Status::FailedPrecondition();
        }
        PW_TRY(ImuSpiSendEnable());  // Propagates failure on non-OK status.
        return pw::OkStatus();
      }

      void Initialize() {
        if (auto status = ImuEnable(); status.ok()) {
          PW_LOG_INFO("Imu initialized successfully")
        } else {
          if (status.IsFailedPrecondition()) {
            PW_LOG_WARNING("No IMU present");
          } else {
            PW_LOG_ERROR("Unknown error initializing IMU: %d", status.code());
          }
        }
      }

``pw_status`` provides an implementation of status in every supported
Pigweed language, including C, Rust, TypeScript, Java, and Python.

Pigweed's status matches Google's standard status codes (see the `Google APIs
repository
<https://github.com/googleapis/googleapis/blob/HEAD/google/rpc/code.proto>`_).
These codes are used extensively in Google projects including `Abseil
<https://abseil.io>`_ (`status/status.h
<https://cs.opensource.google/abseil/abseil-cpp/+/HEAD:absl/status/status.h>`_)
and `gRPC <https://grpc.io>`_ (`doc/statuscodes.md
<https://github.com/grpc/grpc/blob/HEAD/doc/statuscodes.md>`_).

.. grid:: 2

   .. grid-item-card:: :octicon:`rocket` Get Started & Guides
      :link: module-pw_status-get-started
      :link-type: ref
      :class-item: sales-pitch-cta-primary

      Integrate pw_status into your project, see common uses

   .. grid-item-card:: :octicon:`code-square` API Reference
      :link: module-pw_status-reference
      :link-type: ref
      :class-item: sales-pitch-cta-secondary

      Detailed description of pw_status's methods.

.. _module-pw_status-quickref:

---------------
Quick reference
---------------
See :ref:`module-pw_status-codes` for the precise semantics of each error, as
well as how to spell the status in each of our supported languages.  Click on
the status names below to jump directly to that error's reference.

.. list-table::
   :widths: 35 5 60
   :header-rows: 1

   * - Status
     - Code
     - Description
   * - :c:enumerator:`OK`
     - 0
     - Operation succeeded
   * - :c:enumerator:`CANCELLED`
     - 1
     - Operation was cancelled, typically by the caller
   * - :c:enumerator:`UNKNOWN`
     - 2
     - Unknown error occurred. Avoid this code when possible.
   * - :c:enumerator:`INVALID_ARGUMENT`
     - 3
     - Argument was malformed; e.g. invalid characters when parsing integer
   * - :c:enumerator:`DEADLINE_EXCEEDED`
     - 4
     - Deadline passed before operation completed
   * - :c:enumerator:`NOT_FOUND`
     - 5
     - The entity that the caller requested (e.g. file or directory) is not
       found
   * - :c:enumerator:`ALREADY_EXISTS`
     - 6
     - The entity that the caller requested to create is already present
   * - :c:enumerator:`PERMISSION_DENIED`
     - 7
     - Caller lacks permission to execute action
   * - :c:enumerator:`RESOURCE_EXHAUSTED`
     - 8
     - Insufficient resources to complete operation; e.g. supplied buffer is too
       small
   * - :c:enumerator:`FAILED_PRECONDITION`
     - 9
     - System isn't in the required state; e.g. deleting a non-empty directory
   * - :c:enumerator:`ABORTED`
     - 10
     - Operation aborted due to e.g. concurrency issue or failed transaction
   * - :c:enumerator:`OUT_OF_RANGE`
     - 11
     - Operation attempted out of range; e.g. seeking past end of file
   * - :c:enumerator:`UNIMPLEMENTED`
     - 12
     - Operation isn't implemented or supported
   * - :c:enumerator:`INTERNAL`
     - 13
     - Internal error occurred; e.g. system invariants were violated
   * - :c:enumerator:`UNAVAILABLE`
     - 14
     - Requested operation can't finish now, but may at a later time
   * - :c:enumerator:`DATA_LOSS`
     - 15
     - Unrecoverable data loss occurred while completing the requested operation
   * - :c:enumerator:`UNAUTHENTICATED`
     - 16
     - Caller does not have valid authentication credentials for the operation

.. _module-pw_status-status-base:

-----------------------------------
Custom status codes with StatusBase
-----------------------------------
:cc:`pw::StatusBase` is a base class template that provides Pigweed status
semantics for custom code types, such as domain-specific error enums or integer
codes. A designated code value represents success (``ok() == true``).

Pigweed's standard :cc:`pw::Status` is itself implemented as a concrete class
inheriting from ``StatusBase<Status, PW_STATUS_OK>``.

Custom status types can be defined using the :c:macro:`PW_STATUS_TYPE` macro
or by directly subclassing :cc:`pw::StatusBase`:

* **Macro**: ``PW_STATUS_TYPE(MyStatus, MyEnum::kOk)`` defines a final status
  class with ``[[nodiscard]]``. The status code must be an enum.
* **Subclass**: Subclassing ``pw::StatusBase<MyStatus, MyEnum::kOk>`` directly
  (using the Curiously Recurring Template Pattern / CRTP) allows
  customizing the status class with additional domain-specific member functions.
  Derived classes should be declared ``[[nodiscard]]`` and define constructors
  forwarding to ``StatusBase``, whose constructors are protected.

Custom status types are constructed from a status code (e.g.
``MyStatus(MyStatus::Code::kFailed)``). They default construct to the OK code.
The code type is available as ``MyStatus::Code``.

Custom error enums
==================
Many drivers and hardware interfaces define their own error enumerations.
``StatusBase`` allows using these enumerations while retaining status semantics,
including ``.ok()`` checks, ``[[nodiscard]]`` enforcement, and error propagation
with :cc:`PW_TRY`:

.. literalinclude:: status_base_test.cc
   :language: cpp
   :start-after: [pw_status-status_base-custom_enum]
   :end-before: [pw_status-status_base-custom_enum]

Callers inspect the status or compare it directly against the underlying enum:

.. literalinclude:: status_base_test.cc
   :language: cpp
   :dedent:
   :start-after: [pw_status-status_base-caller]
   :end-before: [pw_status-status_base-caller]

Non-enum status codes
=====================
Enums are recommended for status codes because they prevent arbitrary or unknown
values and allow the compiler to warn about unhandled cases in ``switch``
statements. However, ``StatusBase`` also supports non-enum types such as
integers when interfacing with existing protocols or numeric error codes. For
example, HTTP status codes can be represented with ``200`` designating success:

.. literalinclude:: status_base_test.cc
   :language: cpp
   :start-after: [pw_status-status_base-non_enum]
   :end-before: [pw_status-status_base-non_enum]

Callers handle non-enum statuses in the same way:

.. literalinclude:: status_base_test.cc
   :language: cpp
   :dedent:
   :start-after: [pw_status-status_base-non_enum_caller]
   :end-before: [pw_status-status_base-non_enum_caller]

Should I use a custom status?
=============================
Custom status types offer significant advantages, but are not a fit for every
scenario or project.

**Advantages**

* **Resolve ambiguity**: Each distinct error can map to its own code. No more
  wondering which resource was exhausted for :c:enumerator:`RESOURCE_EXHAUSTED`.
* **Better API contracts for callers**: APIs only produce codes that make sense.
  Callers do not have consider codes that will never be returned.
* **Better API contracts for implementers**: API implementers are constrained to
  defined codes and cannot return codes that do not apply. No need to check that
  an implementation returned a supported code.
* **Thoughtful layering of status domains**: Prevent users from mindlessly
  forwarding errors between API layers with :cc:`PW_TRY`, which loses meaning
  and specificity.

**Disadvantages**

* **Cognitive load**: Users have to learn different status codes for different
  APIs.
* **Enum proliferation**: Custom statuses could lead to a proliferation of
  enums, even when fewer status types would be better.

Whether and how to use custom status types is ultimately a project decision.

.. toctree::
   :hidden:
   :maxdepth: 1

   guide
   reference
