.. _module-pw_status-reference:

=========
Reference
=========
.. pigweed-module-subpage::
   :name: pw_status

.. _module-pw_status-codes:

------------
Status codes
------------
.. c:enumerator:: OK = 0

   :c:enumerator:`OK` indicates that the operation completede successfully.  It
   is typical to check for this value before proceeding on any given call across
   an API or RPC boundary. To check this value, use the ``ok()`` member function
   rather than inspecting the raw code.

   .. list-table::

      * - C++
        - :cc:`pw::OkStatus()`
      * - C
        - :cc:`PW_STATUS_OK`
      * - Python / Java / TypeScript
        - ``Status.OK``
      * - Rust
        - ``Ok(val)``

.. c:enumerator:: CANCELLED = 1

   :c:enumerator:`CANCELLED` indicates the operation was cancelled, typically by
   the caller.

   .. list-table::

      * - C++
        - :cc:`pw::Status::Cancelled()`
      * - C
        - :cc:`PW_STATUS_CANCELLED`
      * - Python / Java / TypeScript
        - ``Status.CANCELLED``
      * - Rust
        - ``Error::Cancelled``

.. c:enumerator:: UNKNOWN = 2

   :c:enumerator:`UNKNOWN` indicates an unknown error occurred. In general, more
   specific errors should be raised, if possible. Errors raised by APIs that do
   not return enough error information may be converted to this error.

   .. list-table::

      * - C++
        - :cc:`pw::Status::Unknown()`
      * - C
        - :cc:`PW_STATUS_UNKNOWN`
      * - Python / Java / TypeScript
        - ``Status.UNKNOWN``
      * - Rust
        - ``Error::Unknown``

.. c:enumerator:: INVALID_ARGUMENT = 3

   :c:enumerator:`INVALID_ARGUMENT` indicates the caller specified an invalid
   argument, such as a malformed filename. Note that use of such errors should
   be narrowly limited to indicate the invalid nature of the arguments
   themselves. Errors with validly formed arguments that may cause errors with
   the state of the receiving system should be denoted with
   :c:enumerator:`FAILED_PRECONDITION` instead.

   .. list-table::

      * - C++
        - :cc:`pw::Status::InvalidArgument()`
      * - C
        - :cc:`PW_STATUS_INVALID_ARGUMENT`
      * - Python / Java / TypeScript
        - ``Status.INVALID_ARGUMENT``
      * - Rust
        - ``Error::InvalidArgument``

.. c:enumerator:: DEADLINE_EXCEEDED = 4

   :c:enumerator:`DEADLINE_EXCEEDED` indicates a deadline expired before the
   operation could complete. For operations that may change state within a
   system, this error may be returned even if the operation has completed
   successfully. For example, a successful response from a server could have
   been delayed long enough for the deadline to expire.

   .. list-table::

      * - C++
        - :cc:`pw::Status::DeadlineExceeded()`
      * - C
        - :cc:`PW_STATUS_DEADLINE_EXCEEDED`
      * - Python / Java / TypeScript
        - ``Status.DEADLINE_EXCEEDED``
      * - Rust
        - ``Error::DeadlineExceeded``

.. c:enumerator:: NOT_FOUND = 5

   :c:enumerator:`NOT_FOUND` indicates some requested entity (such as a file or
   directory) was not found.

   :c:enumerator:`NOT_FOUND` is useful if a request should be denied for an
   entire class of users, such as during a gradual feature rollout or
   undocumented allowlist. If a request should be denied for specific sets of
   users, such as through user-based access control, use
   :c:enumerator:`PERMISSION_DENIED` instead.

   .. list-table::

      * - C++
        - :cc:`pw::Status::NotFound()`
      * - C
        - :cc:`PW_STATUS_NOT_FOUND`
      * - Python / Java / TypeScript
        - ``Status.NOT_FOUND``
      * - Rust
        - ``Error::NotFound``

.. c:enumerator:: ALREADY_EXISTS = 6

   :c:enumerator:`ALREADY_EXISTS` indicates that the entity a caller attempted
   to create (such as a file or directory) is already present.

   .. list-table::

      * - C++
        - :cc:`pw::Status::AlreadyExists()`
      * - C
        - :cc:`PW_STATUS_ALREADY_EXISTS`
      * - Python / Java / TypeScript
        - ``Status.ALREADY_EXISTS``
      * - Rust
        - ``Error::AlreadyExists``

.. c:enumerator:: PERMISSION_DENIED = 7

   :c:enumerator:`PERMISSION_DENIED` indicates that the caller does not have
   permission to execute the specified operation. Note that this error is
   different than an error due to an unauthenticated user. This error code does
   not imply the request is valid or the requested entity exists or satisfies
   any other pre-conditions.

   :c:enumerator:`PERMISSION_DENIED` must not be used for rejections caused by
   exhausting some resource. Instead, use :c:enumerator:`RESOURCE_EXHAUSTED` for
   those errors.  :c:enumerator:`PERMISSION_DENIED` must not be used if the
   caller cannot be identified.  Instead, use :c:enumerator:`UNAUTHENTICATED`
   for those errors.

   .. list-table::

      * - C++
        - :cc:`pw::Status::PermissionDenied()`
      * - C
        - :cc:`PW_STATUS_PERMISSION_DENIED`
      * - Python / Java / TypeScript
        - ``Status.PERMISSION_DENIED``
      * - Rust
        - ``Error::PermissionDenied``

.. c:enumerator:: RESOURCE_EXHAUSTED = 8

   :c:enumerator:`RESOURCE_EXHAUSTED` indicates some resource has been
   exhausted, perhaps a per-user quota, or perhaps the entire file system is out
   of space.

   .. list-table::

      * - C++
        - :cc:`pw::Status::ResourceExhausted()`
      * - C
        - :cc:`PW_STATUS_RESOURCE_EXHAUSTED`
      * - Python / Java / TypeScript
        - ``Status.RESOURCE_EXHAUSTED``
      * - Rust
        - ``Error::ResourceExhausted``

.. c:enumerator:: FAILED_PRECONDITION = 9

   :c:enumerator:`FAILED_PRECONDITION` indicates that the operation was rejected
   because the system is not in a state required for the operation's execution.
   For example, a directory to be deleted may be non-empty, an ``rmdir``
   operation is applied to a non-directory, etc.

   .. _module-pw_status-guidelines:

   Some guidelines that may help a service implementer in deciding between
   :c:enumerator:`FAILED_PRECONDITION`, :c:enumerator:`ABORTED`, and
   :c:enumerator:`UNAVAILABLE`:

   a. Use :c:enumerator:`UNAVAILABLE` if the client can retry just the failing
      call.
   b. Use :c:enumerator:`ABORTED` if the client should retry at a higher
      transaction level (such as when a client-specified test-and-set fails,
      indicating the client should restart a read-modify-write sequence).
   c. Use :c:enumerator:`FAILED_PRECONDITION` if the client should not retry
      until the system state has been explicitly fixed. For example, if a
      ``rmdir`` fails because the directory is non-empty,
      :c:enumerator:`FAILED_PRECONDITION` should be returned since the client
      should not retry unless the files are deleted from the directory.

   .. list-table::

      * - C++
        - :cc:`pw::Status::FailedPrecondition()`
      * - C
        - :cc:`PW_STATUS_FAILED_PRECONDITION`
      * - Python / Java / TypeScript
        - ``Status.FAILED_PRECONDITION``
      * - Rust
        - ``Error::FailedPrecondition``

.. c:enumerator:: ABORTED = 10

   :c:enumerator:`ABORTED` indicates the operation was aborted, typically due to
   a concurrency issue such as a sequencer check failure or a failed
   transaction.

   See the :ref:`guidelines <module-pw_status-guidelines>` above for deciding
   between :c:enumerator:`FAILED_PRECONDITION`, :c:enumerator:`ABORTED`, and
   :c:enumerator:`UNAVAILABLE`.

   .. list-table::

      * - C++
        - :cc:`pw::Status::Aborted()`
      * - C
        - :cc:`PW_STATUS_ABORTED`
      * - Python / Java / TypeScript
        - ``Status.ABORTED``
      * - Rust
        - ``Error::Aborted``

.. c:enumerator:: OUT_OF_RANGE = 11

   :c:enumerator:`OUT_OF_RANGE` indicates the operation was attempted past the
   valid range, such as seeking or reading past an end-of-file.

   Unlike :c:enumerator:`INVALID_ARGUMENT`, this error indicates a problem that
   may be fixed if the system state changes. For example, a 32-bit file system
   will generate :c:enumerator:`INVALID_ARGUMENT` if asked to read at an offset
   that is not in the range [0,2^32-1], but it will generate
   :c:enumerator:`OUT_OF_RANGE` if asked to read from an offset past the current
   file size.

   There is a fair bit of overlap between :c:enumerator:`FAILED_PRECONDITION`
   and :c:enumerator:`OUT_OF_RANGE`. Use :c:enumerator:`OUT_OF_RANGE` (the more
   specific error) when it applies so that callers who are iterating through a
   space can easily look for an :c:enumerator:`OUT_OF_RANGE` error to detect
   when they are done.

   .. list-table::

      * - C++
        - :cc:`pw::Status::OutOfRange()`
      * - C
        - :cc:`PW_STATUS_OUT_OF_RANGE`
      * - Python / Java / TypeScript
        - ``Status.OUT_OF_RANGE``
      * - Rust
        - ``Error::OutOfRange``

.. c:enumerator:: UNIMPLEMENTED = 12

   :c:enumerator:`UNIMPLEMENTED` indicates the operation is not implemented or
   supported in this service. In this case, the operation should not be
   re-attempted.

   .. list-table::

      * - C++
        - :cc:`pw::Status::Unimplemented()`
      * - C
        - :cc:`PW_STATUS_UNIMPLEMENTED`
      * - Python / Java / TypeScript
        - ``Status.UNIMPLEMENTED``
      * - Rust
        - ``Error::Unimplemented``

.. c:enumerator:: INTERNAL = 13

   :c:enumerator:`INTERNAL` indicates an internal error has occurred and some
   invariants expected by the underlying system have not been satisfied. This
   error code is reserved for serious errors.

   .. list-table::

      * - C++
        - :cc:`pw::Status::Internal()`
      * - C
        - :cc:`PW_STATUS_INTERNAL`
      * - Python / Java / TypeScript
        - ``Status.INTERNAL``
      * - Rust
        - ``Error::Internal``

.. c:enumerator:: UNAVAILABLE = 14

   :c:enumerator:`UNAVAILABLE` indicates the service is currently unavailable
   and that this is most likely a transient condition. An error such as this can
   be corrected by retrying with a backoff scheme. Note that it is not always
   safe to retry non-idempotent operations.

   See the :ref:`guidelines <module-pw_status-guidelines>` above for deciding
   between :c:enumerator:`FAILED_PRECONDITION`, :c:enumerator:`ABORTED`, and
   :c:enumerator:`UNAVAILABLE`.

   .. list-table::

      * - C++
        - :cc:`pw::Status::Unavailable()`
      * - C
        - :cc:`PW_STATUS_UNAVAILABLE`
      * - Python / Java / TypeScript
        - ``Status.UNAVAILABLE``
      * - Rust
        - ``Error::Unavailable``

.. c:enumerator:: DATA_LOSS = 15

   :c:enumerator:`DATA_LOSS` indicates that unrecoverable data loss or
   corruption has occurred. As this error is serious, proper alerting should be
   attached to errors such as this.

   .. list-table::

      * - C++
        - :cc:`pw::Status::DataLoss()`
      * - C
        - :cc:`PW_STATUS_DATA_LOSS`
      * - Python / Java / TypeScript
        - ``Status.DATA_LOSS``
      * - Rust
        - ``Error::DataLoss``

.. c:enumerator:: UNAUTHENTICATED = 16

   :c:enumerator:`UNAUTHENTICATED` indicates that the request does not have
   valid authentication credentials for the operation. Correct the
   authentication and try again.

   .. list-table::

      * - C++
        - :cc:`pw::Status::Unauthenticated()`
      * - C
        - :cc:`PW_STATUS_UNAUTHENTICATED`
      * - Python / Java / TypeScript
        - ``Status.UNAUTHENTICATED``
      * - Rust
        - ``Error::Unauthenticated``

-------
C++ API
-------
Moved: :cc:`pw_status`

.. _module-pw_status-reference-unused:

Unused result warnings
----------------------
If the :cc:`PW_STATUS_CFG_CHECK_IF_USED` option is enabled,
:cc:`pw::Status` objects returned from function calls must be used or it
is a compilation error. To silence these warnings call ``IgnoreError()`` on the
returned status object.

:cc:`PW_STATUS_CFG_CHECK_IF_USED` defaults to ``false`` in GN and CMake,
but ``true`` in Bazel. Pigweed compiles with this option enabled, but projects
that use Pigweed will need to be updated to compile with this option. After all
projects have migrated, unused result warnings will be enabled unconditionally.

-----
C API
-----
``pw_status`` provides the C-compatible :cc:`pw_Status` enum for the status
codes.  For ease of use, :cc:`pw::Status` implicitly converts to and from
:cc:`pw_Status`.  However, the :cc:`pw_Status` enum should never be used
in C++; instead use the :cc:`pw::Status` class.

--------
Rust API
--------
``pw_status``'s Rust API is documented in our
`rustdoc API docs </rustdoc/pw_status/>`_.
