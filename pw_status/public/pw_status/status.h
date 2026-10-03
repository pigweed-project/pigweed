// Copyright 2020 The Pigweed Authors
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

#include "pw_status/internal/config.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/// @module{pw_status}

// This is the pw_Status enum. pw_Status is used to return the status from an
// operation.
//
// In C++, use the pw::Status class instead of the pw_Status enum. pw_Status and
// Status implicitly convert to one another and can be passed cleanly between C
// and C++ APIs.
//
// pw_Status uses the canonical Google error codes. The following enum was based
// on Abseil's status/status.h. The values are all-caps and prefixed with
// PW_STATUS_ instead of using C++ constant style.
//
// The status codes are described at
// https://pigweed.dev/pw_status/reference.html. Consult that
// guide when deciding which status code to use.

/// C API for status codes. In C++, use the pw::Status class instead.
typedef enum {
  /// @see @OK
  PW_STATUS_OK = 0,  // Use OkStatus() in C++
  /// @see @CANCELLED
  PW_STATUS_CANCELLED = 1,  // Use Status::Cancelled() in C++
  /// @see @UNKNOWN
  PW_STATUS_UNKNOWN = 2,  // Use Status::Unknown() in C++
  /// @see @INVALID_ARGUMENT
  PW_STATUS_INVALID_ARGUMENT = 3,  // Use Status::InvalidArgument() in C++
  /// @see @DEADLINE_EXCEEDED
  PW_STATUS_DEADLINE_EXCEEDED = 4,  // Use Status::DeadlineExceeded() in C++
  /// @see @NOT_FOUND
  PW_STATUS_NOT_FOUND = 5,  // Use Status::NotFound() in C++
  /// @see @ALREADY_EXISTS
  PW_STATUS_ALREADY_EXISTS = 6,  // Use Status::AlreadyExists() in C++
  /// @see @PERMISSION_DENIED
  PW_STATUS_PERMISSION_DENIED = 7,  // Use Status::PermissionDenied() in C++
  /// @see @RESOURCE_EXHAUSTED
  PW_STATUS_RESOURCE_EXHAUSTED = 8,  // Use Status::ResourceExhausted() in C++
  /// @see @FAILED_PRECONDITION
  PW_STATUS_FAILED_PRECONDITION = 9,  // Use Status::FailedPrecondition() in C++
  /// @see @ABORTED
  PW_STATUS_ABORTED = 10,  // Use Status::Aborted() in C++
  /// @see @OUT_OF_RANGE
  PW_STATUS_OUT_OF_RANGE = 11,  // Use Status::OutOfRange() in C++
  /// @see @UNIMPLEMENTED
  PW_STATUS_UNIMPLEMENTED = 12,  // Use Status::Unimplemented() in C++
  /// @see @INTERNAL
  PW_STATUS_INTERNAL = 13,  // Use Status::Internal() in C++
  /// @see @UNAVAILABLE
  PW_STATUS_UNAVAILABLE = 14,  // Use Status::Unavailable() in C++
  /// @see @DATA_LOSS
  PW_STATUS_DATA_LOSS = 15,  // Use Status::DataLoss() in C++
  /// @see @UNAUTHENTICATED
  PW_STATUS_UNAUTHENTICATED = 16,  // Use Status::Unauthenticated() in C++

  // Warning: This error code entry should not be used and you should not rely
  // on its value, which may change.
  //
  // The purpose of this enumerated value is to force people who handle status
  // codes with `switch` statements to *not* simply enumerate all possible
  // values, but instead provide a `default` case. Providing such a default
  // case ensures that code will compile when new codes are added.
  PW_STATUS_DO_NOT_USE_RESERVED_FOR_FUTURE_EXPANSION_USE_DEFAULT_IN_SWITCH_INSTEAD_,
} pw_Status;  // Use pw::Status in C++

/// @returns A null-terminated string representation of the pw_Status.
const char* pw_StatusString(pw_Status status);

/// Indicates the status code with the highest valid value.
#define PW_STATUS_LAST PW_STATUS_UNAUTHENTICATED

/// @endmodule

#ifdef __cplusplus

}  // extern "C"

#include "pw_enum/to_string.h"
#include "pw_status/status_base.h"

/// Returns a string representation of a given `pw_Status` enumerator.
///
/// This implements the FTADLE extension point for `pw::EnumToString`.
inline const char* PwEnumToString(pw_Status status) {
  return pw_StatusString(status);
}

/// The Pigweed namespace
namespace pw {

/// @module{pw_status}

/// `Status` is a thin, zero-cost abstraction around the `pw_Status` enum. It
/// initializes to `pw::OkStatus()` by default and adds `ok()` and `str()`
/// methods. Implicit conversions are permitted between `pw_Status` and
/// `pw::Status`.
///
/// An OK status is created by the `pw::OkStatus()` function or by the default
/// `Status` constructor. Non-OK `Status` is created with a static member
/// function that corresponds with the status code. Example:
///
/// @code{.cpp}
///   using pw::Status;
///
///   Status OperationThatFails() {
///      // …
///      return Status::DataLoss();
///   }
/// @endcode
class _PW_STATUS_NO_DISCARD Status final
    : public StatusBase<Status, PW_STATUS_OK> {
 public:
  using Code = pw_Status;

  // Functions that create a Status with the specified code.
  //
  // The status codes are described at
  // https://pigweed.dev/pw_status/reference.html. Consult that guide when
  // deciding which status code to use.
  // clang-format off

  /// Operation was cancelled, typically by the caller.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`CANCELLED`). The C++ API for this code is `Status::Cancelled()`
  /// (or @ref Status::IsCancelled() "status.IsCancelled()") and the C API is
  /// `PW_STATUS_CANCELLED`. See
  /// [CANCELLED](../../pw_status/reference.html#c.CANCELLED) for details on
  /// interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status Cancelled() {
    return PW_STATUS_CANCELLED;
  }

  /// Unknown error occurred. Avoid this code when possible.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`UNKNOWN`). The C++ API for this code is `Status::Unknown()` (or
  /// @ref Status::IsUnknown() "status.IsUnknown()") and the C API is
  /// `PW_STATUS_UNKNOWN`. See
  /// [UNKNOWN](../../pw_status/reference.html#c.UNKNOWN) for details on
  /// interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status Unknown() {
    return PW_STATUS_UNKNOWN;
  }

  /// Argument was malformed; e.g. invalid characters when parsing integer.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`INVALID_ARGUMENT`). The C++ API for this code is
  /// `Status::InvalidArgument()` (or
  /// @ref pw::Status::IsInvalidArgument() "status.IsInvalidArgument()") and the
  /// C API is `PW_STATUS_INVALID_ARGUMENT`. See
  /// [INVALID_ARGUMENT](../../pw_status/reference.html#c.INVALID_ARGUMENT) for
  /// details on interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status InvalidArgument() {
    return PW_STATUS_INVALID_ARGUMENT;
  }

  /// Deadline passed before operation completed.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`DEADLINE_EXCEEDED`). The C++ API for this code is
  /// `Status::DeadlineExceeded()` (or
  /// @ref pw::Status::IsDeadlineExceeded() "status.IsDeadlineExceeded()") and
  /// the C API is `PW_STATUS_DEADLINE_EXCEEDED`. See
  /// [DEADLINE_EXCEEDED](../../pw_status/reference.html#c.DEADLINE_EXCEEDED)
  /// for details on interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status DeadlineExceeded() {
    return PW_STATUS_DEADLINE_EXCEEDED;
  }

  /// The entity that the caller requested (e.g. file or directory) is not
  /// found.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`NOT_FOUND`). The C++ API for this code is `Status::NotFound()` (or
  /// @ref pw::Status::IsNotFound() "status.IsNotFound()") and the C API is
  /// `PW_STATUS_NOT_FOUND`. See
  /// [NOT_FOUND](../../pw_status/reference.html#c.NOT_FOUND) for details on
  /// interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status NotFound() {
    return PW_STATUS_NOT_FOUND;
  }

  /// The entity that the caller requested to create is already present.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`ALREADY_EXISTS`). The C++ API for this code is
  /// `Status::AlreadyExists()` (or
  /// @ref pw::Status::IsAlreadyExists() "status.IsAlreadyExists()") and the
  /// C API is `PW_STATUS_ALREADY_EXISTS`. See
  /// [ALREADY_EXISTS](../../pw_status/reference.html#c.ALREADY_EXISTS) for
  /// details on interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status AlreadyExists() {
    return PW_STATUS_ALREADY_EXISTS;
  }

  /// Caller lacks permission to execute action.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`PERMISSION_DENIED`). The C++ API for this code is
  /// `Status::PermissionDenied()` (or
  /// @ref pw::Status::IsPermissionDenied() "status.IsPermissionDenied()") and
  /// the C API is `PW_STATUS_PERMISSION_DENIED`. See
  /// [PERMISSION_DENIED](../../pw_status/reference.html#c.PERMISSION_DENIED)
  /// for details on interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status PermissionDenied() {
    return PW_STATUS_PERMISSION_DENIED;
  }

  /// Insufficient resources to complete operation; e.g. supplied buffer is too
  /// small.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`RESOURCE_EXHAUSTED`). The C++ API for this code is
  /// `Status::ResourceExhausted()` (or
  /// @ref pw::Status::IsResourceExhausted() "status.IsResourceExhausted()") and
  /// the C API is `PW_STATUS_RESOURCE_EXHAUSTED`. See
  /// [RESOURCE_EXHAUSTED](../../pw_status/reference.html#c.RESOURCE_EXHAUSTED)
  /// for details on interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status ResourceExhausted() {
    return PW_STATUS_RESOURCE_EXHAUSTED;
  }

  /// System isn’t in the required state; e.g. deleting a non-empty directory.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`FAILED_PRECONDITION`). The C++ API for this code is
  /// `Status::FailedPrecondition()` (or
  /// @ref pw::Status::IsFailedPrecondition() "status.IsFailedPrecondition()")
  /// and the C API is `PW_STATUS_PERMISSION_DENIED`. See
  /// [PERMISSION_DENIED](../../pw_status/reference.html#c.PERMISSION_DENIED)
  /// for details on interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status FailedPrecondition() {
    return PW_STATUS_FAILED_PRECONDITION;
  }

  /// Operation aborted due to e.g. concurrency issue or failed transaction.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`ABORTED`). The C++ API for this code is `Status::Aborted()` (or
  /// @ref pw::Status::IsAborted() "status.IsAborted()") and the C API is
  /// `PW_STATUS_ABORTED`. See
  /// [ABORTED](../../pw_status/reference.html#c.ABORTED) for details on
  /// interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status Aborted() {
    return PW_STATUS_ABORTED;
  }

  /// Operation attempted out of range; e.g. seeking past end of file.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`OUT_OF_RANGE`). The C++ API for this code is `Status::OutOfRange()`
  /// (or @ref pw::Status::IsOutOfRange() "status.IsOutOfRange()") and the C API
  /// is `PW_STATUS_OUT_OF_RANGE`. See
  /// [OUT_OF_RANGE](../../pw_status/reference.html#c.OUT_OF_RANGE) for details
  /// on interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status OutOfRange() {
    return PW_STATUS_OUT_OF_RANGE;
  }

  /// Operation isn’t implemented or supported.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`UNIMPLEMENTED`). The C++ API for this code is
  /// `Status::Unimplemented()` (or
  /// @ref pw::Status::IsUnimplemented() "status.IsUnimplemented()") and the
  /// C API is `PW_STATUS_UNIMPLEMENTED`. See
  /// [UNIMPLEMENTED](../../pw_status/reference.html#c.UNIMPLEMENTED) for
  /// details on interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status Unimplemented() {
    return PW_STATUS_UNIMPLEMENTED;
  }

  /// Internal error occurred; e.g. system invariants were violated.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`INTERNAL`). The C++ API for this code is `Status::Internal()` (or
  /// @ref pw::Status::IsInternal() "status.IsInternal()") and the C API is
  /// `PW_STATUS_INTERNAL`. See
  /// [INTERNAL](../../pw_status/reference.html#c.INTERNAL) for details on
  /// interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status Internal() {
    return PW_STATUS_INTERNAL;
  }

  /// Requested operation can’t finish now, but may at a later time.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`UNAVAILABLE`). The C++ API for this code is `Status::Unavailable()`
  /// (or @ref pw::Status::IsUnavailable() "status.IsUnavailable()") and the C
  /// API is `PW_STATUS_UNAVAILABLE`.
  /// See [UNAVAILABLE](../../pw_status/reference.html#c.UNAVAILABLE) for
  /// details on interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status Unavailable() {
    return PW_STATUS_UNAVAILABLE;
  }

  /// Unrecoverable data loss occurred while completing the requested operation.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`DATA_LOSS`). The C++ API for this code is `Status::DataLoss()` (or
  /// @ref pw::Status::IsDataLoss() "status.IsDataLoss()") and the C API is
  /// `PW_STATUS_DATA_LOSS`. See
  /// [DATA_LOSS](../../pw_status/reference.html#c.DATA_LOSS) for details on
  /// interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status DataLoss() {
    return PW_STATUS_DATA_LOSS;
  }

  /// Caller does not have valid authentication credentials for the operation.
  ///
  /// @note In the Pigweed docs we refer to this code by its generic, canonical
  /// name (`UNAUTHENTICATED`). The C++ API for this code is
  /// `Status::Unauthenticated()` (or
  /// @ref pw::Status::IsUnauthenticated() "status.IsUnauthenticated()") and the
  /// C API is `PW_STATUS_UNAUTHENTICATED`. See
  /// [UNAUTHENTICATED](../../pw_status/reference.html#c.UNAUTHENTICATED) for
  /// details on interacting with this status code in other languages.
  [[nodiscard]] static constexpr Status Unauthenticated() {
    return PW_STATUS_UNAUTHENTICATED;
  }
  // clang-format on

  /// Statuses are created with a `Status::Code`.
  // C/C++ interoperability: allow implicit conversion from pw_Status
  constexpr Status(Code code = PW_STATUS_OK) : StatusBase(code) {}

  constexpr Status(const Status&) = default;
  constexpr Status& operator=(const Status&) = default;
  constexpr Status(Status&&) = default;
  constexpr Status& operator=(Status&&) = default;

  /// @returns `true` if the status of this instance is @CANCELLED.
  [[nodiscard]] constexpr bool IsCancelled() const {
    return code() == PW_STATUS_CANCELLED;
  }
  /// @returns `true` if the status of this instance is @UNKNOWN.
  [[nodiscard]] constexpr bool IsUnknown() const {
    return code() == PW_STATUS_UNKNOWN;
  }
  /// @returns `true` if the status of this instance is @INVALID_ARGUMENT.
  [[nodiscard]] constexpr bool IsInvalidArgument() const {
    return code() == PW_STATUS_INVALID_ARGUMENT;
  }
  /// @returns `true` if the status of this instance is @DEADLINE_EXCEEDED.
  [[nodiscard]] constexpr bool IsDeadlineExceeded() const {
    return code() == PW_STATUS_DEADLINE_EXCEEDED;
  }
  /// @returns `true` if the status of this instance is @NOT_FOUND.
  [[nodiscard]] constexpr bool IsNotFound() const {
    return code() == PW_STATUS_NOT_FOUND;
  }
  /// @returns `true` if the status of this instance is @ALREADY_EXISTS.
  [[nodiscard]] constexpr bool IsAlreadyExists() const {
    return code() == PW_STATUS_ALREADY_EXISTS;
  }
  /// @returns `true` if the status of this instance is @PERMISSION_DENIED.
  [[nodiscard]] constexpr bool IsPermissionDenied() const {
    return code() == PW_STATUS_PERMISSION_DENIED;
  }
  /// @returns `true` if the status of this instance is @RESOURCE_EXHAUSTED.
  [[nodiscard]] constexpr bool IsResourceExhausted() const {
    return code() == PW_STATUS_RESOURCE_EXHAUSTED;
  }
  /// @returns `true` if the status of this instance is @FAILED_PRECONDITION.
  [[nodiscard]] constexpr bool IsFailedPrecondition() const {
    return code() == PW_STATUS_FAILED_PRECONDITION;
  }
  /// @returns `true` if the status of this instance is @ABORTED.
  [[nodiscard]] constexpr bool IsAborted() const {
    return code() == PW_STATUS_ABORTED;
  }
  /// @returns `true` if the status of this instance is @OUT_OF_RANGE.
  [[nodiscard]] constexpr bool IsOutOfRange() const {
    return code() == PW_STATUS_OUT_OF_RANGE;
  }
  /// @returns `true` if the status of this instance is @UNIMPLEMENTED.
  [[nodiscard]] constexpr bool IsUnimplemented() const {
    return code() == PW_STATUS_UNIMPLEMENTED;
  }
  /// @returns `true` if the status of this instance is @INTERNAL.
  [[nodiscard]] constexpr bool IsInternal() const {
    return code() == PW_STATUS_INTERNAL;
  }
  /// @returns `true` if the status of this instance is @UNAVAILABLE.
  [[nodiscard]] constexpr bool IsUnavailable() const {
    return code() == PW_STATUS_UNAVAILABLE;
  }
  /// @returns `true` if the status of this instance is @DATA_LOSS.
  [[nodiscard]] constexpr bool IsDataLoss() const {
    return code() == PW_STATUS_DATA_LOSS;
  }
  /// @returns `true` if the status of this instance is @UNAUTHENTICATED.
  [[nodiscard]] constexpr bool IsUnauthenticated() const {
    return code() == PW_STATUS_UNAUTHENTICATED;
  }
};

/// Operation succeeded.
///
/// Equivalent to `Status()` or `Status(PW_STATUS_OK)`. This function is used
/// instead of a `Status::Ok()` function, which would be too similar to
/// `Status::ok()`.
///
/// @note In the Pigweed docs we refer to this code by its generic, canonical
/// name (`OK`). The C++ API for this code is `OkStatus()` (or `Status::ok()`)
/// and the C API is `PW_STATUS_OK`. See
/// [OK](../../pw_status/reference.html#c.OK) for details on interacting with
/// this status code in other languages.
[[nodiscard]] constexpr Status OkStatus() { return Status(); }

namespace internal {

// Converts to pw::Status for use by internal macros like PW_TRY and
// PW_CHECK_OK. This accepts pw::Status, the C pw_Status enum, and any other
// type that implicitly converts to pw::Status. Custom status types are handled
// by the generic ConvertToStatus template in status_base.h.
constexpr Status ConvertToStatus(Status status) { return status; }

}  // namespace internal

/// @endmodule

}  // namespace pw

/// @module{pw_status}

/// Creates a C++ overload of `pw_StatusString` so that it supports `pw::Status`
/// in addition to `pw_Status`.
inline const char* pw_StatusString(pw::Status status) {
  return pw_StatusString(status.code());
}

/// @endmodule

#endif  // __cplusplus
