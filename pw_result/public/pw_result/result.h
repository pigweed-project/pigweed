// Copyright 2022 The Pigweed Authors
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
//
// -----------------------------------------------------------------------------
// File: result.h
// -----------------------------------------------------------------------------
//
// A `Result<T, StatusType>` represents a union of a `StatusType` object
// (defaulting to `pw::Status`) and an object of type `T`. The
// `Result<T, StatusType>` will either contain an object of type `T` (indicating
// a successful operation), or an error status explaining why such a value is
// not present.
//
// In general, check the success of an operation returning a `Result<T>` like
// you would a `pw::Status` by using the `ok()` member function.
//
// Example:
//
//   Result<Foo> result = Calculation();
//   if (result.ok()) {
//     result->DoSomethingCool();
//   } else {
//     PW_LOG_ERROR("Calculation failed: %s", result.status().str());
//   }
#pragma once

#include <exception>
#include <functional>
#include <initializer_list>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#include "lib/stdcompat/functional.h"
#include "pw_assert/assert.h"
#include "pw_preprocessor/compiler.h"
#include "pw_result/internal/result_internal.h"
#include "pw_status/status.h"
#include "pw_status/status_base.h"

namespace pw {

namespace internal {

template <typename T, typename StatusType>
constexpr T&& ConvertToValue(Result<T, StatusType>& result);

}  // namespace internal

/// @module{pw_result}

/// Error propagation primitive: value or error status.
///
/// The `pw::Result<T, StatusType>` class template is a union of a `StatusType`
/// object (`pw::Status` by default) and an object of type `T`.
/// `Result<T, StatusType>` models an object that is either a usable object, or
/// an error status (of type `StatusType`) explaining why such an object is not
/// present. It is typically the return value of a function which may fail.
///
/// To simplify its use in generic code and coroutines, `Result` is specialized
/// for `void`.
///
/// ### Checking for success
///
/// `Result<T, StatusType>` can never hold an OK error status; instead, the
/// presence of an object of type `T` indicates success (`status().ok() ==
/// true`). Use the `Result<T, StatusType>::ok()` member function to check
/// for success.
///
/// Example:
///
/// @code{.cpp}
///   Result<Foo> result = DoBigCalculationThatCouldFail();
///   if (result.ok()) {
///     result->DoSomethingCool();
///   } else {
///     PW_LOG_ERROR("Calculation failed: %s", result.status().str());
///   }
/// @endcode
///
/// ### Accessing objects
///
/// Accessing the object held by a `Result<T, StatusType>` should be performed
/// via `operator*` or `operator->`, after a call to `ok()` confirms that the
/// `Result<T, StatusType>` holds an object of type `T`:
///
/// Example:
///
/// @code{.cpp}
///   Result<int> i = GetCount();
///   if (i.ok()) {
///     updated_total += *i;
///   }
/// @endcode
///
/// Using `Result<T, StatusType>::value()` when no valid value is present will
/// trigger a `PW_ASSERT`.
///
/// Example:
///
/// @code{.cpp}
///   Result<Foo> result = DoBigCalculationThatCouldFail();
///   const Foo& foo = result.value();  // Asserts/crashes if no value present
///   foo.DoSomethingCool();
/// @endcode
///
/// ### Constructing result objects
///
/// A `Result<T*>` can be constructed from a null pointer like any other pointer
/// value, and the result will be that `ok()` returns `true` and `value()`
/// returns `nullptr`. Checking the value of a pointer in a `Result<T*>`
/// generally requires a bit more care, to ensure both that a value is present
/// and that value is not null:
///
/// @code{.cpp}
///   Result<Foo*> result = LookUpTheFoo(arg);
///   if (!result.ok()) {
///     PW_LOG_ERROR("Unable to look up the Foo: %s", result.status().str());
///   } else if (*result == nullptr) {
///     PW_LOG_ERROR("Unexpected null pointer");
///   } else {
///     (*result)->DoSomethingCool();
///   }
/// @endcode
///
/// Example factory implementation returning `Result<T>`:
///
/// @code{.cpp}
///   Result<Foo> FooFactory::MakeFoo(int arg) {
///     if (arg <= 0) {
///       return pw::Status::InvalidArgument();
///     }
///     return Foo(arg);
///   }
/// @endcode
///
/// ### `Result<Status>` and `Result<StatusType, StatusType>`
///
/// Prefer returning `StatusType` (or `Result<void, StatusType>` in generic
/// code) instead of `Result<StatusType, StatusType>` when no additional value
/// is needed. However, `Result<StatusType, StatusType>` is supported for
/// generic code that wraps functions returning `StatusType`.
///
/// Because the value type and the status type are the same in
/// `Result<StatusType, StatusType>`, constructing, assigning, or comparing with
/// a `StatusType` (or `StatusType::Code`) always operates on the **contained
/// value** (`ok() == true`), even if the provided status is not OK. To set an
/// error status on a `Result<StatusType, StatusType>`, call `reset()`. To check
/// or compare the status of the `Result` itself, use `ok()` or `status()`.
///
/// @example{pw_result/result_test.cc,pw_result-status-value}
///
/// @tparam T Type of the contained value (or `void`).
/// @tparam StatusType Status type that represents the error. Defaults to
///   `pw::Status`. Must be derived from `pw::StatusBase` and use an enum code.
template <typename T, typename StatusType>
class [[nodiscard]] Result final
    : private internal_result::ResultData<T, StatusType>,
      private internal_result::CopyCtorBase<T>,
      private internal_result::MoveCtorBase<T>,
      private internal_result::CopyAssignBase<T>,
      private internal_result::MoveAssignBase<T> {
  template <typename U, typename>
  friend class Result;

  template <typename U, typename S>
  friend constexpr U&& internal::ConvertToValue(Result<U, S>& result);

  using Base = internal_result::ResultData<T, StatusType>;

 public:
  /// Generic `value_type` member for use within generic programming. This usage
  /// is analogous to that of `std::optional::value_type`.
  using value_type = T;

  /// The status type used to represent the status of this `Result`. Defaults to
  /// `pw::Status`.
  using status_type = StatusType;

  static_assert(internal::is_status_v<status_type>,
                "The status type must derive from pw::StatusBase");
  static_assert(std::is_enum_v<typename status_type::Code>,
                "pw::Result only supports status types with enum codes");

  // Constructors

  /// Constructs a new `Result<T>` with a `pw::Status::Unknown()` status.
  ///
  /// This constructor is marked `explicit` to prevent usages in return values
  /// (e.g. `return {}`) under the misconception that `Result<std::vector<int>>`
  /// (for example) will be initialized with an empty vector.
  ///
  /// Only enabled when `StatusType` is `pw::Status`.
  template <typename S = StatusType,
            typename = std::enable_if_t<std::is_same_v<S, Status>>>
  explicit constexpr Result() : Base(PW_STATUS_UNKNOWN) {}

  /// `Result<T, StatusType>` is copy constructible if `T` is copy
  /// constructible.
  constexpr Result(const Result&) = default;
  /// `Result<T, StatusType>` is copy assignable if `T` is copy constructible
  /// and copy assignable.
  constexpr Result& operator=(const Result&) = default;

  /// `Result<T, StatusType>` is move constructible if `T` is move
  /// constructible.
  constexpr Result(Result&&) = default;
  /// `Result<T, StatusType>` is move assignable if `T` is move constructible
  /// and move assignable.
  constexpr Result& operator=(Result&&) = default;

  // Converting Constructors

  /// Constructs a new `Result<T, StatusType>` from a `Result<U, StatusType>`
  /// when `T` is constructible from `U`.
  ///
  /// To avoid ambiguity, these constructors are disabled if `T` is also
  /// constructible from `Result<U, StatusType>`. This constructor is explicit
  /// if and only if the corresponding construction of `T` from `U` is
  /// explicit. (This constructor inherits its explicitness from the underlying
  /// constructor.)
  template <typename U,
            std::enable_if_t<
                std::conjunction<
                    std::negation<std::is_same<T, U>>,
                    std::is_constructible<T, const U&>,
                    std::is_convertible<const U&, T>,
                    std::negation<
                        internal_result::IsConstructibleOrConvertibleFromResult<
                            T,
                            U,
                            StatusType>>>::value,
                int> = 0>
  constexpr Result(const Result<U, StatusType>& other)  // NOLINT
      : Base(static_cast<const typename Result<U, StatusType>::Base&>(other)) {}

  template <typename U,
            std::enable_if_t<
                std::conjunction<
                    std::negation<std::is_same<T, U>>,
                    std::is_constructible<T, const U&>,
                    std::negation<std::is_convertible<const U&, T>>,
                    std::negation<
                        internal_result::IsConstructibleOrConvertibleFromResult<
                            T,
                            U,
                            StatusType>>>::value,
                int> = 0>
  explicit constexpr Result(const Result<U, StatusType>& other)
      : Base(static_cast<const typename Result<U, StatusType>::Base&>(other)) {}

  template <typename U,
            std::enable_if_t<
                std::conjunction<
                    std::negation<std::is_same<T, U>>,
                    std::is_constructible<T, U&&>,
                    std::is_convertible<U&&, T>,
                    std::negation<
                        internal_result::IsConstructibleOrConvertibleFromResult<
                            T,
                            U,
                            StatusType>>>::value,
                int> = 0>
  constexpr Result(Result<U, StatusType>&& other)  // NOLINT
      : Base(static_cast<typename Result<U, StatusType>::Base&&>(other)) {}

  template <typename U,
            std::enable_if_t<
                std::conjunction<
                    std::negation<std::is_same<T, U>>,
                    std::is_constructible<T, U&&>,
                    std::negation<std::is_convertible<U&&, T>>,
                    std::negation<
                        internal_result::IsConstructibleOrConvertibleFromResult<
                            T,
                            U,
                            StatusType>>>::value,
                int> = 0>
  explicit constexpr Result(Result<U, StatusType>&& other)
      : Base(static_cast<typename Result<U, StatusType>::Base&&>(other)) {}

  // Converting Assignment Operators

  /// Assigns a `Result<T, StatusType>` from a `Result<U, StatusType>`.
  ///
  /// @pre These overloads only apply if `Result<T, StatusType>` is
  /// constructible and assignable from `Result<U, StatusType>` and `Result<T,
  /// StatusType>` cannot be directly assigned from `Result<U, StatusType>`.
  /// @pre If both `Result<T, StatusType>` and `Result<U, StatusType>` are OK,
  /// assigns `U` to `T` directly.
  /// @pre If `Result<T, StatusType>` is OK and `Result<U, StatusType>` contains
  /// an error code, destroys the value of `Result<T, StatusType>` and assigns
  /// the error status from `Result<U, StatusType>`.
  /// @pre If `Result<T, StatusType>` contains an error code and `Result<U,
  /// StatusType>` is OK, directly initializes `T` from `U`.
  /// @pre If both `Result<T, StatusType>` and `Result<U, StatusType>` contain
  /// an error code, assigns the status in `Result<U, StatusType>` to `Result<T,
  /// StatusType>`.
  template <typename U,
            std::enable_if_t<
                std::conjunction<
                    std::negation<std::is_same<T, U>>,
                    std::is_constructible<T, const U&>,
                    std::is_assignable<T, const U&>,
                    std::negation<
                        internal_result::
                            IsConstructibleOrConvertibleOrAssignableFromResult<
                                T,
                                U,
                                StatusType>>>::value,
                int> = 0>
  constexpr Result& operator=(const Result<U, StatusType>& other) {
    this->Assign(other);
    return *this;
  }

  template <typename U,
            std::enable_if_t<
                std::conjunction<
                    std::negation<std::is_same<T, U>>,
                    std::is_constructible<T, U&&>,
                    std::is_assignable<T, U&&>,
                    std::negation<
                        internal_result::
                            IsConstructibleOrConvertibleOrAssignableFromResult<
                                T,
                                U,
                                StatusType>>>::value,
                int> = 0>
  constexpr Result& operator=(Result<U, StatusType>&& other) {
    this->Assign(std::move(other));
    return *this;
  }

  /// Constructs a new `Result<T, StatusType>` with a non-OK status.
  ///
  /// After calling this constructor, `this->ok()` will be `false` and calls to
  /// `value()` will trigger a `PW_ASSERT`.
  ///
  /// The constructor takes any type `U` that is constructible/convertible to
  /// `status_type`. This constructor is explicit if and only if `U` is not
  /// implicitly convertible to `status_type`.
  ///
  /// Disabled when `T` is `StatusType` (in which case constructing from a
  /// status initializes the contained value; use `reset()` to set an error
  /// status).
  ///
  /// @pre `!status_type(std::forward<U>(v)).ok()`. This requirement is asserted
  /// via `PW_ASSERT`.
  template <
      typename U = status_type,
      std::enable_if_t<
          std::conjunction<
              std::negation<std::is_same<std::remove_cv_t<T>, status_type>>,
              std::is_convertible<U&&, status_type>,
              std::is_constructible<status_type, U&&>,
              std::negation<
                  std::is_same<std::decay_t<U>, Result<T, StatusType>>>,
              std::negation<std::is_same<std::decay_t<U>, std::remove_cv_t<T>>>,
              std::negation<std::is_same<std::decay_t<U>, std::in_place_t>>,
              std::negation<
                  internal_result::
                      HasConversionOperatorToResult<T, U&&, StatusType>>>::
              value,
          int> = 0>
  constexpr Result(U&& v)  // NOLINT
      : Base(static_cast<status_type>(std::forward<U>(v)).code()) {}

  template <
      typename U = status_type,
      std::enable_if_t<
          std::conjunction<
              std::negation<std::is_same<std::remove_cv_t<T>, status_type>>,
              std::negation<std::is_convertible<U&&, status_type>>,
              std::is_constructible<status_type, U&&>,
              std::negation<
                  std::is_same<std::decay_t<U>, Result<T, StatusType>>>,
              std::negation<std::is_same<std::decay_t<U>, std::remove_cv_t<T>>>,
              std::negation<std::is_same<std::decay_t<U>, std::in_place_t>>,
              std::negation<
                  internal_result::
                      HasConversionOperatorToResult<T, U&&, StatusType>>>::
              value,
          int> = 0>
  explicit constexpr Result(U&& v)
      : Base(static_cast<status_type>(std::forward<U>(v)).code()) {}

  /// Assigns a non-OK status to this `Result<T, StatusType>`, destroying any
  /// currently held value.
  ///
  /// Disabled when `T` is `StatusType` (in which case assigning a status
  /// updates the contained value; use `reset()` to set an error status).
  ///
  /// @pre `!status_type(std::forward<U>(v)).ok()`. This requirement is asserted
  /// via `PW_ASSERT`.
  template <
      typename U = status_type,
      std::enable_if_t<
          std::conjunction<
              std::negation<std::is_same<std::remove_cv_t<T>, status_type>>,
              std::is_convertible<U&&, status_type>,
              std::is_constructible<status_type, U&&>,
              std::negation<
                  std::is_same<std::decay_t<U>, Result<T, StatusType>>>,
              std::negation<std::is_same<std::decay_t<U>, std::remove_cv_t<T>>>,
              std::negation<std::is_same<std::decay_t<U>, std::in_place_t>>,
              std::negation<
                  internal_result::
                      HasConversionOperatorToResult<T, U&&, StatusType>>>::
              value,
          int> = 0>
  constexpr Result& operator=(U&& v) {
    this->AssignState(static_cast<status_type>(std::forward<U>(v)).code());
    return *this;
  }

  /// Perfect-forwarding value assignment operator.
  ///
  /// If `*this` contains a `T` value before the call, the contained value is
  /// assigned from `std::forward<U>(v)`. Otherwise, it is directly initialized
  /// from `std::forward<U>(v)`.
  ///
  /// @pre `std::is_constructible_v<T, U>` is true.
  /// @pre `std::is_assignable_v<T&, U>` is true.
  /// @pre `std::is_same_v<Result<T, StatusType>, std::remove_cvref_t<U>>` is
  /// false.
  /// @pre Assigning `U` to `T` is not ambiguous. If `U` is `Result<V,
  /// StatusType>` and `T` is constructible and assignable from both `Result<V,
  /// StatusType>` and `V`, the assignment is considered bug-prone and ambiguous
  /// and thus will fail to compile. For example:
  /// @code{.cpp}
  ///   Result<bool> s1 = true;  // s1.ok() && *s1 == true
  ///   Result<bool> s2 = false;  // s2.ok() && *s2 == false
  ///   s1 = s2;  // ambiguous, `s1 = *s2` or `s1 = bool(s2)`?
  /// @endcode
  template <
      typename U = T,
      typename = typename std::enable_if<std::conjunction<
          std::is_constructible<T, U&&>,
          std::is_assignable<T&, U&&>,
          std::disjunction<
              std::is_same<std::remove_cv_t<std::remove_reference_t<U>>,
                           std::remove_cv_t<T>>,
              std::conjunction<
                  std::disjunction<
                      std::is_same<std::remove_cv_t<T>, status_type>,
                      std::negation<std::is_convertible<U&&, status_type>>>,
                  std::negation<
                      internal_result::
                          HasConversionOperatorToResult<T, U&&, StatusType>>>>,
          internal_result::IsForwardingAssignmentValid<T, U&&, StatusType>>::
                                             value>::type>
  constexpr Result& operator=(U&& v) {
    this->Assign(std::forward<U>(v));
    return *this;
  }

  /// Constructs the inner value `T` in-place using the provided args, using the
  /// `T(args...)` constructor.
  template <typename... Args>
  explicit constexpr Result(std::in_place_t, Args&&... args)
      : Base(std::in_place, std::forward<Args>(args)...) {}

  template <typename U, typename... Args>
  explicit constexpr Result(std::in_place_t,
                            std::initializer_list<U> ilist,
                            Args&&... args)
      : Base(std::in_place, ilist, std::forward<Args>(args)...) {}

  /// Constructs the inner value `T` in-place using the provided arg, using the
  /// `T(U)` (direct-initialization) constructor.
  ///
  /// @pre This constructor is only valid if `T` can be constructed from a `U`.
  /// Can accept move or copy constructors.
  ///
  /// This constructor is explicit if `U` is not convertible to `T`. To avoid
  /// ambiguity, this constructor is disabled if `U` is a `Result<J,
  /// StatusType>`, where `J` is convertible to `T`.
  template <
      typename U = T,
      std::enable_if_t<
          std::conjunction<
              internal_result::IsDirectInitializationValid<T, U&&, StatusType>,
              std::is_constructible<T, U&&>,
              std::is_convertible<U&&, T>,
              std::disjunction<
                  std::is_same<std::remove_cv_t<std::remove_reference_t<U>>,
                               std::remove_cv_t<T>>,
                  std::conjunction<
                      std::disjunction<
                          std::is_same<std::remove_cv_t<T>, status_type>,
                          std::negation<std::is_convertible<U&&, status_type>>>,
                      std::negation<
                          internal_result::HasConversionOperatorToResult<
                              T,
                              U&&,
                              StatusType>>>>>::value,
          int> = 0>
  constexpr Result(U&& u)  // NOLINT
      : Base(std::in_place, std::forward<U>(u)) {}

  template <
      typename U = T,
      std::enable_if_t<
          std::conjunction<
              internal_result::IsDirectInitializationValid<T, U&&, StatusType>,
              std::disjunction<
                  std::is_same<std::remove_cv_t<std::remove_reference_t<U>>,
                               std::remove_cv_t<T>>,
                  std::conjunction<
                      std::disjunction<
                          std::is_same<std::remove_cv_t<T>, status_type>,
                          std::negation<
                              std::is_constructible<status_type, U&&>>>,
                      std::negation<
                          internal_result::HasConversionOperatorToResult<
                              T,
                              U&&,
                              StatusType>>>>,
              std::is_constructible<T, U&&>,
              std::negation<std::is_convertible<U&&, T>>>::value,
          int> = 0>
  explicit constexpr Result(U&& u)  // NOLINT
      : Base(std::in_place, std::forward<U>(u)) {}

  /// @returns Whether or not this `Result<T, StatusType>` holds a `T` value.
  /// This member function is analogous to `Status::ok()` and should be used
  /// similarly to check the status of return values.
  ///
  /// Example:
  ///
  /// @code{.cpp}
  ///   Result<Foo> result = DoBigCalculationThatCouldFail();
  ///   if (result.ok()) {
  ///     // Handle result
  ///   } else {
  ///     // Handle error
  ///   }
  /// @endcode
  [[nodiscard]] constexpr bool ok() const {
    return this->state_ == Base::kHasValueState;
  }

  /// @returns The current `status_type` contained within the
  /// `Result<T, StatusType>`. If `Result<T, StatusType>` contains a `T`, then
  /// this function returns an OK status (`ok() == true`).
  constexpr status_type status() const { return status_type(this->state_); }

  /// Destroys the contained value, if any, and sets the status. The status must
  /// not be OK. To avoid a runtime check, use the `reset<kCode>()`
  /// function template whenever possible.
  constexpr void reset(status_type status) { this->AssignState(status.code()); }

  /// @copydoc reset
  template <
      typename C,
      std::enable_if_t<std::is_same_v<C, typename status_type::Code>, int> = 0>
  constexpr void reset(C code) {
    this->AssignState(code);
  }

  /// @copydoc reset
  template <typename status_type::Code kCode>
  constexpr void reset() {
    this->template AssignState<kCode>();
  }

  /// @returns A reference to the held value if `this->ok()`. Otherwise,
  /// terminates the process via `PW_ASSERT`.
  ///
  /// If you have already checked the status using `this->ok()`, you probably
  /// want to use `operator*()` or `operator->()` to access the value instead of
  /// `value`.
  ///
  /// For value types that are cheap to copy, prefer simple code:
  ///
  /// @code{.cpp}
  ///   T value = result.value();
  /// @endcode
  ///
  /// Otherwise, if the value type is expensive to copy, but can be left
  /// in the `Result<T, StatusType>`, simply assign to a reference:
  ///
  /// @code{.cpp}
  ///   T& value = result.value();  // or `const T&`
  /// @endcode
  ///
  /// Otherwise, if the value type supports an efficient move, it can be
  /// used as follows:
  ///
  /// @code{.cpp}
  ///   T value = std::move(result).value();
  /// @endcode
  ///
  /// The `std::move` on `result` instead of on the whole expression enables
  /// warnings about possible uses of the result object after the move.
  constexpr const T& value() const& PW_ATTRIBUTE_LIFETIME_BOUND {
    PW_ASSERT(ok());
    return this->data_;
  }
  /// @copydoc value
  constexpr T& value() & PW_ATTRIBUTE_LIFETIME_BOUND {
    PW_ASSERT(ok());
    return this->data_;
  }
  /// @copydoc value
  constexpr const T&& value() const&& PW_ATTRIBUTE_LIFETIME_BOUND {
    PW_ASSERT(ok());
    return std::move(this->data_);
  }
  /// @copydoc value
  constexpr T&& value() && PW_ATTRIBUTE_LIFETIME_BOUND {
    PW_ASSERT(ok());
    return std::move(this->data_);
  }

  /// @returns A reference to the current value. Only checks if a value is
  /// present if `PW_ASSERT_ENABLE_DEBUG` is enabled (via `PW_DASSERT`).
  ///
  /// @pre `this->ok() == true`, otherwise the behavior is undefined.
  ///
  /// Use `this->ok()` to verify that there is a current value within the
  /// `Result<T, StatusType>`. Alternatively, see the `value()` member function
  /// for a similar API that guarantees crashing if there is no current value.
  constexpr const T& operator*() const& PW_ATTRIBUTE_LIFETIME_BOUND {
    PW_DASSERT(ok());
    return this->data_;
  }
  /// @copydoc operator*
  constexpr T& operator*() & PW_ATTRIBUTE_LIFETIME_BOUND {
    PW_DASSERT(ok());
    return this->data_;
  }
  /// @copydoc operator*
  constexpr const T&& operator*() const&& PW_ATTRIBUTE_LIFETIME_BOUND {
    PW_DASSERT(ok());
    return std::move(this->data_);
  }
  /// @copydoc operator*
  constexpr T&& operator*() && PW_ATTRIBUTE_LIFETIME_BOUND {
    PW_DASSERT(ok());
    return std::move(this->data_);
  }

  /// @returns A pointer to the current value. Only checks if a value is
  /// present if `PW_ASSERT_ENABLE_DEBUG` is enabled (via `PW_DASSERT`).
  ///
  /// @pre `this->ok() == true`, otherwise the behavior is undefined.
  ///
  /// Use `this->ok()` to verify that there is a current value.
  constexpr const T* operator->() const PW_ATTRIBUTE_LIFETIME_BOUND {
    PW_DASSERT(ok());
    return &this->data_;
  }
  /// @copydoc operator->
  constexpr T* operator->() PW_ATTRIBUTE_LIFETIME_BOUND {
    PW_DASSERT(ok());
    return &this->data_;
  }

  /// @returns The current value if `this->ok() == true`. Otherwise constructs a
  /// value using the provided `default_value`.
  ///
  /// Unlike `value`, this function returns by value, copying the current value
  /// if necessary. If the value type supports an efficient move, it can be used
  /// as follows:
  ///
  /// @code{.cpp}
  ///   T value = std::move(result).value_or(def);
  /// @endcode
  ///
  /// Unlike with `value`, calling `std::move()` on the result of `value_or`
  /// will still trigger a copy.
  template <typename U>
  constexpr T value_or(U&& default_value) const& {
    if (ok()) {
      return this->data_;
    }
    return std::forward<U>(default_value);
  }
  /// @copydoc value_or
  template <typename U>
  constexpr T value_or(U&& default_value) && {
    if (ok()) {
      return std::move(this->data_);
    }
    return std::forward<U>(default_value);
  }

  /// Ignores any errors. This method does nothing except potentially suppress
  /// complaints from any tools that are checking that errors are not dropped.
  constexpr void IgnoreError() const {}

  /// Reconstructs the inner value `T` in-place using the provided args, using
  /// the `T(args...)` constructor.
  ///
  /// @returns A reference to the reconstructed `T`.
  template <typename... Args>
  constexpr T& emplace(Args&&... args) {
    this->Clear();
    this->MakeValue(std::forward<Args>(args)...);
    this->state_ = Base::kHasValueState;
    return this->data_;
  }

  /// @copydoc emplace
  template <
      typename U,
      typename... Args,
      std::enable_if_t<
          std::is_constructible<T, std::initializer_list<U>&, Args&&...>::value,
          int> = 0>
  constexpr T& emplace(std::initializer_list<U> ilist, Args&&... args) {
    this->Clear();
    this->MakeValue(ilist, std::forward<Args>(args)...);
    this->state_ = Base::kHasValueState;
    return this->data_;
  }

  // Monadic operations

  /// @returns The `Result` from invoking `func()` on the held value if `ok()`.
  /// Otherwise, returns a `Result` with the contained error status.
  template <typename Fn,
            typename Ret = internal_result::InvokeResultType<Fn, T&>,
            std::enable_if_t<std::is_copy_constructible_v<Ret>, int> = 0>
  constexpr Ret and_then(Fn&& function) & {
    static_assert(internal_result::IsResult<Ret>,
                  "Fn must return a pw::Result");
    if constexpr (internal_result::IsResult<Ret>) {
      static_assert(std::is_same_v<typename Ret::status_type, status_type>,
                    "Fn must return a pw::Result with the same status_type");
    }
    return ok() ? cpp20::invoke(std::forward<Fn>(function), this->data_)
                : Ret(internal_result::kErrorTag, this->state_);
  }

  /// @copydoc and_then
  template <typename Fn,
            typename Ret = internal_result::InvokeResultType<Fn, T&&>,
            std::enable_if_t<std::is_move_constructible_v<Ret>, int> = 0>
  constexpr Ret and_then(Fn&& function) && {
    static_assert(internal_result::IsResult<Ret>,
                  "Fn must return a pw::Result");
    if constexpr (internal_result::IsResult<Ret>) {
      static_assert(std::is_same_v<typename Ret::status_type, status_type>,
                    "Fn must return a pw::Result with the same status_type");
    }
    return ok() ? cpp20::invoke(std::forward<Fn>(function),
                                std::move(this->data_))
                : Ret(internal_result::kErrorTag, this->state_);
  }

  /// @copydoc and_then
  template <typename Fn,
            typename Ret = internal_result::InvokeResultType<Fn, const T&>,
            std::enable_if_t<std::is_copy_constructible_v<Ret>, int> = 0>
  constexpr Ret and_then(Fn&& function) const& {
    static_assert(internal_result::IsResult<Ret>,
                  "Fn must return a pw::Result");
    if constexpr (internal_result::IsResult<Ret>) {
      static_assert(std::is_same_v<typename Ret::status_type, status_type>,
                    "Fn must return a pw::Result with the same status_type");
    }
    return ok() ? cpp20::invoke(std::forward<Fn>(function), this->data_)
                : Ret(internal_result::kErrorTag, this->state_);
  }

  /// @copydoc and_then
  template <typename Fn,
            typename Ret = internal_result::InvokeResultType<Fn, const T&&>,
            std::enable_if_t<std::is_move_constructible_v<Ret>, int> = 0>
  constexpr Ret and_then(Fn&& function) const&& {
    static_assert(internal_result::IsResult<Ret>,
                  "Fn must return a pw::Result");
    if constexpr (internal_result::IsResult<Ret>) {
      static_assert(std::is_same_v<typename Ret::status_type, status_type>,
                    "Fn must return a pw::Result with the same status_type");
    }
    return ok() ? cpp20::invoke(std::forward<Fn>(function),
                                std::move(this->data_))
                : Ret(internal_result::kErrorTag, this->state_);
  }

  /// @returns This `Result<T, StatusType>` if it has a value, otherwise invokes
  /// the given `function` with `status()`. The function must return a type
  /// convertible to `Result<T, StatusType>` or `void`.
  template <
      typename Fn,
      typename Ret = internal_result::InvokeResultType<Fn, const status_type&>,
      std::enable_if_t<!std::is_void_v<Ret>, int> = 0>
  constexpr Result<T, StatusType> or_else(Fn&& function) const& {
    static_assert(std::is_convertible_v<Ret, Result<T, StatusType>>,
                  "Fn must be convertible to a pw::Result");
    return ok() ? *this : cpp20::invoke(std::forward<Fn>(function), status());
  }

  /// @copydoc or_else
  template <
      typename Fn,
      typename Ret = internal_result::InvokeResultType<Fn, const status_type&>,
      std::enable_if_t<std::is_void_v<Ret>, int> = 0>
  constexpr Result<T, StatusType> or_else(Fn&& function) const& {
    if (ok()) {
      return *this;
    }
    cpp20::invoke(std::forward<Fn>(function), status());
    return *this;
  }

  /// @copydoc or_else
  template <typename Fn,
            typename Ret = internal_result::InvokeResultType<Fn, status_type&&>,
            std::enable_if_t<!std::is_void_v<Ret>, int> = 0>
  constexpr Result<T, StatusType> or_else(Fn&& function) && {
    static_assert(std::is_convertible_v<Ret, Result<T, StatusType>>,
                  "Fn must be convertible to a pw::Result");
    return ok() ? std::move(*this)
                : cpp20::invoke(std::forward<Fn>(function), status());
  }

  /// @copydoc or_else
  template <typename Fn,
            typename Ret = internal_result::InvokeResultType<Fn, status_type&&>,
            std::enable_if_t<std::is_void_v<Ret>, int> = 0>
  constexpr Result<T, StatusType> or_else(Fn&& function) && {
    if (ok()) {
      return std::move(*this);
    }
    cpp20::invoke(std::forward<Fn>(function), status());
    return std::move(*this);
  }

  /// @returns A `Result<U, StatusType>` containing the result of invoking
  /// `function` on the held value if `*this` contains a value. Otherwise,
  /// returns a `Result<U, StatusType>` with the same status as `*this`.
  template <
      typename Fn,
      typename Ret = internal_result::InvokeResultType<Fn, T&>,
      std::enable_if_t<std::is_void_v<Ret> || std::is_copy_constructible_v<Ret>,
                       int> = 0>
  constexpr Result<Ret, StatusType> transform(Fn&& function) & {
    if (!ok()) {
      return Result<Ret, StatusType>(internal_result::kErrorTag, this->state_);
    }
    if constexpr (std::is_void_v<Ret>) {
      cpp20::invoke(std::forward<Fn>(function), this->data_);
      return Result<void, StatusType>(std::in_place);
    } else {
      return cpp20::invoke(std::forward<Fn>(function), this->data_);
    }
  }

  /// @copydoc transform
  template <
      typename Fn,
      typename Ret = internal_result::InvokeResultType<Fn, T&&>,
      std::enable_if_t<std::is_void_v<Ret> || std::is_move_constructible_v<Ret>,
                       int> = 0>
  constexpr Result<Ret, StatusType> transform(Fn&& function) && {
    if (!ok()) {
      return Result<Ret, StatusType>(internal_result::kErrorTag, this->state_);
    }
    if constexpr (std::is_void_v<Ret>) {
      cpp20::invoke(std::forward<Fn>(function), std::move(this->data_));
      return Result<void, StatusType>(std::in_place);
    } else {
      return cpp20::invoke(std::forward<Fn>(function), std::move(this->data_));
    }
  }

  /// @copydoc transform
  template <
      typename Fn,
      typename Ret = internal_result::InvokeResultType<Fn, const T&>,
      std::enable_if_t<std::is_void_v<Ret> || std::is_copy_constructible_v<Ret>,
                       int> = 0>
  constexpr Result<Ret, StatusType> transform(Fn&& function) const& {
    if (!ok()) {
      return Result<Ret, StatusType>(internal_result::kErrorTag, this->state_);
    }
    if constexpr (std::is_void_v<Ret>) {
      cpp20::invoke(std::forward<Fn>(function), this->data_);
      return Result<void, StatusType>(std::in_place);
    } else {
      return cpp20::invoke(std::forward<Fn>(function), this->data_);
    }
  }

  /// @copydoc transform
  template <
      typename Fn,
      typename Ret = internal_result::InvokeResultType<Fn, const T&&>,
      std::enable_if_t<std::is_void_v<Ret> || std::is_move_constructible_v<Ret>,
                       int> = 0>
  constexpr Result<Ret, StatusType> transform(Fn&& function) const&& {
    if (!ok()) {
      return Result<Ret, StatusType>(internal_result::kErrorTag, this->state_);
    }
    if constexpr (std::is_void_v<Ret>) {
      cpp20::invoke(std::forward<Fn>(function), std::move(this->data_));
      return Result<void, StatusType>(std::in_place);
    } else {
      return cpp20::invoke(std::forward<Fn>(function), std::move(this->data_));
    }
  }

  /// Checks the equality of two `Result<T, StatusType>` objects. Only enabled
  /// if `T` is equality comparable.
  template <typename U = T,
            std::enable_if_t<internal_result::IsEqualityComparable<U, U>::value,
                             int> = 0>
  friend constexpr bool operator==(const Result& lhs, const Result& rhs) {
    if (lhs.ok() && rhs.ok()) {
      return lhs.data_ == rhs.data_;
    }
    return lhs.status() == rhs.status();
  }
  /// @copydoc operator==(const Result&, const Result&)
  template <typename U = T,
            std::enable_if_t<internal_result::IsEqualityComparable<U, U>::value,
                             int> = 0>
  friend constexpr bool operator!=(const Result& lhs, const Result& rhs) {
    return !(lhs == rhs);
  }

  /// Compares the status of a `Result<T, StatusType>` with a status. A `Result`
  /// holding a value compares equal to an OK status.
  ///
  /// Disabled when `T` is `StatusType` (in which case `operator==` compares
  /// against the held `StatusType` value).
  template <typename U = T,
            std::enable_if_t<!std::is_same_v<std::remove_cv_t<U>, status_type>,
                             int> = 0>
  friend constexpr bool operator==(const Result& lhs, status_type rhs) {
    return lhs.status() == rhs;
  }
  /// @copydoc operator==(const Result&, status_type)
  template <typename U = T,
            std::enable_if_t<!std::is_same_v<std::remove_cv_t<U>, status_type>,
                             int> = 0>
  friend constexpr bool operator==(status_type lhs, const Result& rhs) {
    return lhs == rhs.status();
  }
  /// @copydoc operator==(const Result&, status_type)
  template <typename U = T,
            std::enable_if_t<!std::is_same_v<std::remove_cv_t<U>, status_type>,
                             int> = 0>
  friend constexpr bool operator!=(const Result& lhs, status_type rhs) {
    return lhs.status() != rhs;
  }
  /// @copydoc operator==(const Result&, status_type)
  template <typename U = T,
            std::enable_if_t<!std::is_same_v<std::remove_cv_t<U>, status_type>,
                             int> = 0>
  friend constexpr bool operator!=(status_type lhs, const Result& rhs) {
    return lhs != rhs.status();
  }

  /// Compares the status of a `Result<T, StatusType>` with a status code.
  ///
  /// Disabled when `T` is `StatusType` or `StatusType::Code` (in which case
  /// `operator==` compares against the held value).
  template <
      typename U = T,
      std::enable_if_t<
          !std::is_same_v<std::remove_cv_t<U>, status_type> &&
              !std::is_same_v<std::remove_cv_t<U>, typename status_type::Code>,
          int> = 0>
  friend constexpr bool operator==(const Result& lhs,
                                   typename status_type::Code rhs) {
    return lhs.status() == rhs;
  }
  /// @copydoc operator==(const Result&, typename status_type::Code)
  template <
      typename U = T,
      std::enable_if_t<
          !std::is_same_v<std::remove_cv_t<U>, status_type> &&
              !std::is_same_v<std::remove_cv_t<U>, typename status_type::Code>,
          int> = 0>
  friend constexpr bool operator==(typename status_type::Code lhs,
                                   const Result& rhs) {
    return lhs == rhs.status();
  }
  /// @copydoc operator==(const Result&, typename status_type::Code)
  template <
      typename U = T,
      std::enable_if_t<
          !std::is_same_v<std::remove_cv_t<U>, status_type> &&
              !std::is_same_v<std::remove_cv_t<U>, typename status_type::Code>,
          int> = 0>
  friend constexpr bool operator!=(const Result& lhs,
                                   typename status_type::Code rhs) {
    return lhs.status() != rhs;
  }
  /// @copydoc operator==(const Result&, typename status_type::Code)
  template <
      typename U = T,
      std::enable_if_t<
          !std::is_same_v<std::remove_cv_t<U>, status_type> &&
              !std::is_same_v<std::remove_cv_t<U>, typename status_type::Code>,
          int> = 0>
  friend constexpr bool operator!=(typename status_type::Code lhs,
                                   const Result& rhs) {
    return lhs != rhs.status();
  }

  /// Checks equality of a `Result<T, StatusType>` and a value. A `Result`
  /// without a value never compares equal to a value. This overload is only
  /// enabled if `T` is equality comparable with `U`, and `U` is not a `Result`,
  /// status, or status code (unless `T` itself is `StatusType` or
  /// `StatusType::Code`).
  template <
      typename U,
      std::enable_if_t<
          internal_result::IsValueComparisonValid<T, U, StatusType>::value,
          int> = 0>
  friend constexpr bool operator==(const Result& lhs, const U& rhs) {
    return lhs.ok() && lhs.data_ == rhs;
  }
  /// @copydoc operator==(const Result&, const U&)
  template <
      typename U,
      std::enable_if_t<
          internal_result::IsValueComparisonValid<T, U, StatusType>::value,
          int> = 0>
  friend constexpr bool operator==(const U& lhs, const Result& rhs) {
    return rhs.ok() && lhs == rhs.data_;
  }
  /// @copydoc operator==(const Result&, const U&)
  template <
      typename U,
      std::enable_if_t<
          internal_result::IsValueComparisonValid<T, U, StatusType>::value,
          int> = 0>
  friend constexpr bool operator!=(const Result& lhs, const U& rhs) {
    return !(lhs == rhs);
  }
  /// @copydoc operator==(const Result&, const U&)
  template <
      typename U,
      std::enable_if_t<
          internal_result::IsValueComparisonValid<T, U, StatusType>::value,
          int> = 0>
  friend constexpr bool operator!=(const U& lhs, const Result& rhs) {
    return !(lhs == rhs);
  }

 private:
  explicit constexpr Result(internal_result::ErrorTag,
                            typename status_type::Code code)
      : Base(code) {}

  using Base::Assign;
  template <typename U>
  constexpr void Assign(const Result<U, StatusType>& other) {
    if (other.ok()) {
      this->Assign(other.data_);
    } else {
      this->AssignState(other.status().code());
    }
  }
  template <typename U>
  constexpr void Assign(Result<U, StatusType>&& other) {
    if (other.ok()) {
      this->Assign(std::move(other.data_));
    } else {
      this->AssignState(other.status().code());
    }
  }
};

/// The `void` specialization of `Result` is provided to simplify generic and
/// template code. `Result<void, StatusType>` wraps `StatusType` with the
/// `Result` interface (`ok()`, `status()`, `value()`, `emplace()`, `reset()`,
/// and monadic operations).
///
/// Unlike `Result<StatusType, StatusType>` (which contains a `StatusType` value
/// AND a separate `StatusType` error state), `Result<void, StatusType>` holds
/// only a single `StatusType` state: constructing or assigning from an OK
/// `StatusType` (or `std::in_place`) sets `ok() == true`, while constructing or
/// assigning from a non-OK `StatusType` (or calling `reset()`) sets
/// `ok() == false`.
///
/// @example{pw_result/result_test.cc,pw_result-void}
///
/// @tparam StatusType Status type that represents the error. Defaults to
///   `pw::Status`.
template <typename StatusType>
class [[nodiscard]] Result<void, StatusType> final {
  template <typename U, typename>
  friend class Result;

 public:
  /// Generic `value_type` member (`void`).
  using value_type = void;

  /// The status type used to represent the status of this `Result`. Defaults to
  /// `pw::Status`.
  using status_type = StatusType;

  static_assert(internal::is_status_v<status_type>,
                "The status type must derive from pw::StatusBase");
  static_assert(std::is_enum_v<typename status_type::Code>,
                "pw::Result only supports status types with enum codes");

  /// Constructs a new `Result<void>` with a `pw::Status::Unknown()` status.
  ///
  /// Only enabled when `StatusType` is `pw::Status`.
  template <typename S = StatusType,
            typename = std::enable_if_t<std::is_same_v<S, Status>>>
  explicit constexpr Result() : state_(PW_STATUS_UNKNOWN) {}

  /// Copy constructor.
  constexpr Result(const Result&) = default;
  /// Move constructor.
  constexpr Result(Result&&) = default;
  /// Copy assignment operator.
  constexpr Result& operator=(const Result&) = default;
  /// Move assignment operator.
  constexpr Result& operator=(Result&&) = default;

  /// Constructs a `Result<void, StatusType>` from a status value (either OK or
  /// non-OK).
  constexpr Result(status_type status)  // NOLINT
      : state_(status.code()) {}

  /// Constructs a `Result<void, StatusType>` from any `U` constructible to
  /// `status_type`. This constructor is explicit if and only if `U` is not
  /// implicitly convertible to `status_type`.
  template <
      typename U = status_type,
      std::enable_if_t<
          std::conjunction<
              std::is_convertible<U&&, status_type>,
              std::is_constructible<status_type, U&&>,
              std::negation<std::is_same<std::decay_t<U>, Result>>,
              std::negation<std::is_same<std::decay_t<U>, status_type>>,
              std::negation<std::is_same<std::decay_t<U>, std::in_place_t>>,
              std::negation<
                  internal_result::
                      HasConversionOperatorToResult<void, U&&, StatusType>>>::
              value,
          int> = 0>
  constexpr Result(U&& v)
      : state_(static_cast<status_type>(std::forward<U>(v)).code()) {}

  template <
      typename U = status_type,
      std::enable_if_t<
          std::conjunction<
              std::negation<std::is_convertible<U&&, status_type>>,
              std::is_constructible<status_type, U&&>,
              std::negation<std::is_same<std::decay_t<U>, Result>>,
              std::negation<std::is_same<std::decay_t<U>, status_type>>,
              std::negation<std::is_same<std::decay_t<U>, std::in_place_t>>,
              std::negation<
                  internal_result::
                      HasConversionOperatorToResult<void, U&&, StatusType>>>::
              value,
          int> = 0>
  explicit constexpr Result(U&& v)
      : state_(static_cast<status_type>(std::forward<U>(v)).code()) {}

  /// Assigns a status value to this `Result<void, StatusType>`.
  template <
      typename U = status_type,
      std::enable_if_t<
          std::conjunction<
              std::is_convertible<U&&, status_type>,
              std::is_constructible<status_type, U&&>,
              std::negation<std::is_same<std::decay_t<U>, Result>>,
              std::negation<std::is_same<std::decay_t<U>, std::in_place_t>>,
              std::negation<
                  internal_result::
                      HasConversionOperatorToResult<void, U&&, StatusType>>>::
              value,
          int> = 0>
  constexpr Result& operator=(U&& v) {
    state_ = static_cast<status_type>(std::forward<U>(v)).code();
    return *this;
  }

  /// Constructs a `Result<void, StatusType>` indicating success (`ok() ==
  /// true`).
  explicit constexpr Result(std::in_place_t) : state_(kHasValueState) {}

  /// Sets the state to indicate success (`ok() == true`).
  constexpr void emplace() { state_ = kHasValueState; }

  /// Sets the status of the `Result` to an error status.
  ///
  /// The status must not be OK (`!status.ok()`). To avoid a runtime check, use
  /// the `reset<kCode>()` function template whenever possible.
  constexpr void reset(status_type status) {
    PW_ASSERT(!status.ok());
    state_ = status.code();
  }

  /// @copydoc reset
  template <
      typename C,
      std::enable_if_t<std::is_same_v<C, typename status_type::Code>, int> = 0>
  constexpr void reset(C code) {
    PW_ASSERT(code != kHasValueState);
    state_ = code;
  }

  /// @copydoc reset
  template <typename status_type::Code kCode>
  constexpr void reset() {
    static_assert(
        kCode != kHasValueState,
        "Cannot set status to the has-value state; use emplace() instead");
    state_ = kCode;
  }

  /// @returns `true` if the `Result` represents success (`ok() == true`).
  [[nodiscard]] constexpr bool ok() const { return state_ == kHasValueState; }

  /// Asserts that `ok()` is `true` via `PW_ASSERT`.
  constexpr void value() const { PW_ASSERT(ok()); }

  /// @returns The current `status_type`.
  constexpr status_type status() const { return status_type(state_); }

  /// Ignores any errors. This method does nothing except potentially suppress
  /// complaints from any tools that are checking that errors are not dropped.
  constexpr void IgnoreError() const {}

  // Monadic operations

  /// @returns The `Result` from invoking `function()` if `ok()`. Otherwise,
  /// returns a `Result` with the contained error status.
  template <typename Fn,
            typename Ret = internal_result::InvokeResultType<Fn>,
            std::enable_if_t<std::is_copy_constructible_v<Ret>, int> = 0>
  constexpr Ret and_then(Fn&& function) const& {
    static_assert(internal_result::IsResult<Ret>,
                  "Fn must return a pw::Result");
    if constexpr (internal_result::IsResult<Ret>) {
      static_assert(std::is_same_v<typename Ret::status_type, status_type>,
                    "Fn must return a pw::Result with the same status_type");
    }
    return ok() ? cpp20::invoke(std::forward<Fn>(function))
                : Ret(internal_result::kErrorTag, state_);
  }

  /// @copydoc and_then
  template <typename Fn,
            typename Ret = internal_result::InvokeResultType<Fn>,
            std::enable_if_t<std::is_move_constructible_v<Ret>, int> = 0>
  constexpr Ret and_then(Fn&& function) && {
    static_assert(internal_result::IsResult<Ret>,
                  "Fn must return a pw::Result");
    if constexpr (internal_result::IsResult<Ret>) {
      static_assert(std::is_same_v<typename Ret::status_type, status_type>,
                    "Fn must return a pw::Result with the same status_type");
    }
    return ok() ? cpp20::invoke(std::forward<Fn>(function))
                : Ret(internal_result::kErrorTag, state_);
  }

  /// @returns This `Result<void, StatusType>` if `ok()`, otherwise invokes
  /// `function` with `status()`. The function must return a type convertible
  /// to `Result<void, StatusType>` or `void`.
  template <
      typename Fn,
      typename Ret = internal_result::InvokeResultType<Fn, const status_type&>,
      std::enable_if_t<!std::is_void_v<Ret>, int> = 0>
  constexpr Result<void, StatusType> or_else(Fn&& function) const& {
    static_assert(std::is_convertible_v<Ret, Result<void, StatusType>>,
                  "Fn must be convertible to a pw::Result");
    return ok() ? *this : cpp20::invoke(std::forward<Fn>(function), status());
  }

  /// @copydoc or_else
  template <
      typename Fn,
      typename Ret = internal_result::InvokeResultType<Fn, const status_type&>,
      std::enable_if_t<std::is_void_v<Ret>, int> = 0>
  constexpr Result<void, StatusType> or_else(Fn&& function) const& {
    if (ok()) {
      return *this;
    }
    cpp20::invoke(std::forward<Fn>(function), status());
    return *this;
  }

  /// @copydoc or_else
  template <typename Fn,
            typename Ret = internal_result::InvokeResultType<Fn, status_type&&>,
            std::enable_if_t<!std::is_void_v<Ret>, int> = 0>
  constexpr Result<void, StatusType> or_else(Fn&& function) && {
    static_assert(std::is_convertible_v<Ret, Result<void, StatusType>>,
                  "Fn must be convertible to a pw::Result");
    return ok() ? std::move(*this)
                : cpp20::invoke(std::forward<Fn>(function), status());
  }

  /// @copydoc or_else
  template <typename Fn,
            typename Ret = internal_result::InvokeResultType<Fn, status_type&&>,
            std::enable_if_t<std::is_void_v<Ret>, int> = 0>
  constexpr Result<void, StatusType> or_else(Fn&& function) && {
    if (ok()) {
      return std::move(*this);
    }
    cpp20::invoke(std::forward<Fn>(function), status());
    return std::move(*this);
  }

  /// @returns A `Result<U, StatusType>` containing the result of invoking
  /// `function()` if `ok()`. Otherwise, returns a `Result<U, StatusType>` with
  /// the same error status as `*this`.
  template <
      typename Fn,
      typename Ret = internal_result::InvokeResultType<Fn>,
      std::enable_if_t<std::is_void_v<Ret> || std::is_copy_constructible_v<Ret>,
                       int> = 0>
  constexpr Result<Ret, StatusType> transform(Fn&& function) const& {
    if (!ok()) {
      return Result<Ret, StatusType>(internal_result::kErrorTag, state_);
    }
    if constexpr (std::is_void_v<Ret>) {
      cpp20::invoke(std::forward<Fn>(function));
      return Result<void, StatusType>(std::in_place);
    } else {
      return cpp20::invoke(std::forward<Fn>(function));
    }
  }

  /// @copydoc transform
  template <
      typename Fn,
      typename Ret = internal_result::InvokeResultType<Fn>,
      std::enable_if_t<std::is_void_v<Ret> || std::is_move_constructible_v<Ret>,
                       int> = 0>
  constexpr Result<Ret, StatusType> transform(Fn&& function) && {
    if (!ok()) {
      return Result<Ret, StatusType>(internal_result::kErrorTag, state_);
    }
    if constexpr (std::is_void_v<Ret>) {
      cpp20::invoke(std::forward<Fn>(function));
      return Result<void, StatusType>(std::in_place);
    } else {
      return cpp20::invoke(std::forward<Fn>(function));
    }
  }

  /// Checks the equality of two `Result<void, StatusType>` objects.
  friend constexpr bool operator==(const Result& lhs, const Result& rhs) {
    return lhs.status() == rhs.status();
  }
  /// Checks the inequality of two `Result<void, StatusType>` objects.
  friend constexpr bool operator!=(const Result& lhs, const Result& rhs) {
    return lhs.status() != rhs.status();
  }

  /// Compares the status of a `Result<void, StatusType>` with a status.
  friend constexpr bool operator==(const Result& lhs, status_type rhs) {
    return lhs.status() == rhs;
  }
  /// @copydoc operator==(const Result&, status_type)
  friend constexpr bool operator==(status_type lhs, const Result& rhs) {
    return lhs == rhs.status();
  }
  /// @copydoc operator==(const Result&, status_type)
  friend constexpr bool operator!=(const Result& lhs, status_type rhs) {
    return lhs.status() != rhs;
  }
  /// @copydoc operator==(const Result&, status_type)
  friend constexpr bool operator!=(status_type lhs, const Result& rhs) {
    return lhs != rhs.status();
  }

  /// Compares the status of a `Result<void, StatusType>` with a status code.
  friend constexpr bool operator==(const Result& lhs,
                                   typename status_type::Code rhs) {
    return lhs.status() == rhs;
  }
  /// @copydoc operator==(const Result&, typename status_type::Code)
  friend constexpr bool operator==(typename status_type::Code lhs,
                                   const Result& rhs) {
    return lhs == rhs.status();
  }
  /// @copydoc operator==(const Result&, typename status_type::Code)
  friend constexpr bool operator!=(const Result& lhs,
                                   typename status_type::Code rhs) {
    return lhs.status() != rhs;
  }
  /// @copydoc operator==(const Result&, typename status_type::Code)
  friend constexpr bool operator!=(typename status_type::Code lhs,
                                   const Result& rhs) {
    return lhs != rhs.status();
  }

 private:
  explicit constexpr Result(internal_result::ErrorTag,
                            typename status_type::Code code)
      : state_(code) {
    PW_ASSERT(code != kHasValueState);
  }

  static constexpr typename status_type::Code kHasValueState =
      status_type().code();

  typename status_type::Code state_;
};

/// Deduction guide to allow `Result(v)` rather than `Result<T>(v)`.
template <typename T>
Result(T value) -> Result<T>;

/// @endmodule

namespace internal {

template <typename T, typename StatusType>
constexpr StatusType ConvertToStatus(const Result<T, StatusType>& result) {
  static_assert(
      !std::is_same_v<std::remove_cv_t<T>, StatusType>,
      "PW_TRY, PW_CHECK_OK, and PW_ASSERT_OK cannot be used directly on "
      "pw::Result<StatusType, StatusType> because both the Result and its "
      "contained value have a status; pass result.status() or *result instead");
  return result.status();
}

template <typename T, typename StatusType>
constexpr T&& ConvertToValue(Result<T, StatusType>& result) {
  return std::move(result.data_);
}

}  // namespace internal

}  // namespace pw
