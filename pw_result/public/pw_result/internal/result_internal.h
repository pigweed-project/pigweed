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
#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_preprocessor/compiler.h"
#include "pw_status/status.h"
#include "pw_status/status_base.h"

namespace pw {

template <typename T, typename StatusType = Status>
class [[nodiscard]] Result;

namespace internal_result {

// Helper base class to hold the data and all operations. We move all this to a
// base class to allow mixing with the appropriate TraitsBase specialization.
template <typename T,
          typename StatusType,
          bool = std::is_trivially_destructible<T>::value>
class ResultData;

#define PW_RESULT_DATA_IMPL                                                \
  template <typename U, typename S, bool b>                                \
  friend class ResultData;                                                 \
                                                                           \
 public:                                                                   \
  using status_type = StatusType;                                          \
  using State = typename status_type::Code;                                \
  static constexpr State kHasValueState = status_type().code();            \
                                                                           \
  ResultData() = delete;                                                   \
                                                                           \
  PW_MODIFY_DIAGNOSTICS_PUSH();                                            \
  PW_MODIFY_DIAGNOSTIC_GCC(ignored, "-Wmaybe-uninitialized");              \
                                                                           \
  constexpr ResultData(const ResultData& other)                            \
      : empty_(), state_(other.state_) {                                   \
    if (other.state_ == kHasValueState) {                                  \
      MakeValue(other.data_);                                              \
    }                                                                      \
  }                                                                        \
                                                                           \
  constexpr ResultData(ResultData&& other) noexcept(                       \
      std::is_nothrow_move_constructible<T>::value)                        \
      : empty_(), state_(other.state_) {                                   \
    if (other.state_ == kHasValueState) {                                  \
      MakeValue(std::move(other.data_));                                   \
    }                                                                      \
  }                                                                        \
                                                                           \
  template <typename U, bool b>                                            \
  explicit constexpr ResultData(const ResultData<U, StatusType, b>& other) \
      : empty_() {                                                         \
    if (other.state_ == kHasValueState) {                                  \
      MakeValue(other.data_);                                              \
      state_ = kHasValueState;                                             \
    } else {                                                               \
      state_ = other.state_;                                               \
    }                                                                      \
  }                                                                        \
                                                                           \
  template <typename U, bool b>                                            \
  explicit constexpr ResultData(ResultData<U, StatusType, b>&& other)      \
      : empty_() {                                                         \
    if (other.state_ == kHasValueState) {                                  \
      MakeValue(std::move(other.data_));                                   \
      state_ = kHasValueState;                                             \
    } else {                                                               \
      state_ = other.state_;                                               \
    }                                                                      \
  }                                                                        \
                                                                           \
  template <typename... Args>                                              \
  explicit constexpr ResultData(std::in_place_t, Args&&... args)           \
      : data_(std::forward<Args>(args)...), state_(kHasValueState) {}      \
                                                                           \
  explicit constexpr ResultData(State no_value_state)                      \
      : empty_(), state_(no_value_state) {                                 \
    PW_ASSERT(no_value_state != kHasValueState);                           \
  }                                                                        \
                                                                           \
  constexpr ResultData& operator=(const ResultData& other) {               \
    if (this == &other) {                                                  \
      return *this;                                                        \
    }                                                                      \
    if (other.state_ == kHasValueState) {                                  \
      Assign(other.data_);                                                 \
    } else {                                                               \
      AssignState(other.state_);                                           \
    }                                                                      \
    return *this;                                                          \
  }                                                                        \
                                                                           \
  constexpr ResultData& operator=(ResultData&& other) noexcept(            \
      std::is_nothrow_move_assignable<T>::value &&                         \
      std::is_nothrow_move_constructible<T>::value) {                      \
    if (this == &other) {                                                  \
      return *this;                                                        \
    }                                                                      \
    if (other.state_ == kHasValueState) {                                  \
      Assign(std::move(other.data_));                                      \
    } else {                                                               \
      AssignState(other.state_);                                           \
    }                                                                      \
    return *this;                                                          \
  }                                                                        \
                                                                           \
  template <typename U>                                                    \
  constexpr void Assign(U&& value) {                                       \
    if (state_ == kHasValueState) {                                        \
      data_ = std::forward<U>(value);                                      \
    } else {                                                               \
      MakeValue(std::forward<U>(value));                                   \
      state_ = kHasValueState;                                             \
    }                                                                      \
  }                                                                        \
                                                                           \
  constexpr void AssignState(State no_value_state) {                       \
    Clear();                                                               \
    state_ = no_value_state;                                               \
    PW_ASSERT(no_value_state != kHasValueState);                           \
  }                                                                        \
                                                                           \
  template <State kState>                                                  \
  constexpr void AssignState() {                                           \
    Clear();                                                               \
    state_ = kState;                                                       \
    static_assert(                                                         \
        kState != kHasValueState,                                          \
        "Cannot set state to the has-value state; set the value instead"); \
  }                                                                        \
                                                                           \
 protected:                                                                \
  struct Empty {};                                                         \
  union {                                                                  \
    Empty empty_;                                                          \
    std::remove_const_t<T> data_;                                          \
  };                                                                       \
  State state_;                                                            \
                                                                           \
  constexpr void Clear() {                                                 \
    if constexpr (!std::is_trivially_destructible_v<T>) {                  \
      if (state_ == kHasValueState) {                                      \
        data_.~T();                                                        \
      }                                                                    \
    }                                                                      \
  }                                                                        \
  template <typename... Arg>                                               \
  constexpr void MakeValue(Arg&&... arg) {                                 \
    new (&data_) T(std::forward<Arg>(arg)...);                             \
  }                                                                        \
  PW_MODIFY_DIAGNOSTICS_POP();                                             \
  static_assert(true, "Macros must be terminated with a semicolon")

template <typename T, typename StatusType>
class ResultData<T, StatusType, true> {
  PW_RESULT_DATA_IMPL;
};

template <typename T, typename StatusType>
class ResultData<T, StatusType, false> {
  PW_RESULT_DATA_IMPL;

 public:
  ~ResultData() { Clear(); }
};

#undef PW_RESULT_DATA_IMPL

// Helper base classes to allow implicitly deleted constructors and assignment
// operators in `Result`.
template <typename T, bool = std::is_copy_constructible<T>::value>
struct CopyCtorBase {
  CopyCtorBase() = default;
  CopyCtorBase(const CopyCtorBase&) = default;
  CopyCtorBase(CopyCtorBase&&) = default;
  CopyCtorBase& operator=(const CopyCtorBase&) = default;
  CopyCtorBase& operator=(CopyCtorBase&&) = default;
};

template <typename T>
struct CopyCtorBase<T, false> {
  CopyCtorBase() = default;
  CopyCtorBase(const CopyCtorBase&) = delete;
  CopyCtorBase(CopyCtorBase&&) = default;
  CopyCtorBase& operator=(const CopyCtorBase&) = default;
  CopyCtorBase& operator=(CopyCtorBase&&) = default;
};

template <typename T, bool = std::is_move_constructible<T>::value>
struct MoveCtorBase {
  MoveCtorBase() = default;
  MoveCtorBase(const MoveCtorBase&) = default;
  MoveCtorBase(MoveCtorBase&&) = default;
  MoveCtorBase& operator=(const MoveCtorBase&) = default;
  MoveCtorBase& operator=(MoveCtorBase&&) = default;
};

template <typename T>
struct MoveCtorBase<T, false> {
  MoveCtorBase() = default;
  MoveCtorBase(const MoveCtorBase&) = default;
  MoveCtorBase(MoveCtorBase&&) = delete;
  MoveCtorBase& operator=(const MoveCtorBase&) = default;
  MoveCtorBase& operator=(MoveCtorBase&&) = default;
};

template <typename T,
          bool = std::is_copy_constructible<T>::value &&
                 std::is_copy_assignable<T>::value>
struct CopyAssignBase {
  CopyAssignBase() = default;
  CopyAssignBase(const CopyAssignBase&) = default;
  CopyAssignBase(CopyAssignBase&&) = default;
  CopyAssignBase& operator=(const CopyAssignBase&) = default;
  CopyAssignBase& operator=(CopyAssignBase&&) = default;
};

template <typename T>
struct CopyAssignBase<T, false> {
  CopyAssignBase() = default;
  CopyAssignBase(const CopyAssignBase&) = default;
  CopyAssignBase(CopyAssignBase&&) = default;
  CopyAssignBase& operator=(const CopyAssignBase&) = delete;
  CopyAssignBase& operator=(CopyAssignBase&&) = default;
};

template <typename T,
          bool = std::is_move_constructible<T>::value &&
                 std::is_move_assignable<T>::value>
struct MoveAssignBase {
  MoveAssignBase() = default;
  MoveAssignBase(const MoveAssignBase&) = default;
  MoveAssignBase(MoveAssignBase&&) = default;
  MoveAssignBase& operator=(const MoveAssignBase&) = default;
  MoveAssignBase& operator=(MoveAssignBase&&) = default;
};

template <typename T>
struct MoveAssignBase<T, false> {
  MoveAssignBase() = default;
  MoveAssignBase(const MoveAssignBase&) = default;
  MoveAssignBase(MoveAssignBase&&) = default;
  MoveAssignBase& operator=(const MoveAssignBase&) = default;
  MoveAssignBase& operator=(MoveAssignBase&&) = delete;
};

// Detects whether `U` has conversion operator to `Result<T, StatusType>`, i.e.
// `operator Result<T, StatusType>()`.
template <typename T, typename U, typename StatusType, typename = void>
struct HasConversionOperatorToResult : std::false_type {};

template <typename T, typename U, typename StatusType>
void test_result_conv(
    char (*)[sizeof(std::declval<U>().operator Result<T, StatusType>())]);

template <typename T, typename U, typename StatusType>
struct HasConversionOperatorToResult<
    T,
    U,
    StatusType,
    decltype(test_result_conv<T, U, StatusType>(0))> : std::true_type {};

// Detects whether `T` is constructible or convertible from `Result<U,
// StatusType>`.
template <typename T, typename U, typename StatusType>
using IsConstructibleOrConvertibleFromResult =
    std::disjunction<std::is_constructible<T, Result<U, StatusType>&>,
                     std::is_constructible<T, const Result<U, StatusType>&>,
                     std::is_constructible<T, Result<U, StatusType>&&>,
                     std::is_constructible<T, const Result<U, StatusType>&&>,
                     std::is_convertible<Result<U, StatusType>&, T>,
                     std::is_convertible<const Result<U, StatusType>&, T>,
                     std::is_convertible<Result<U, StatusType>&&, T>,
                     std::is_convertible<const Result<U, StatusType>&&, T>>;

// Detects whether `T` is constructible or convertible or assignable from
// `Result<U, StatusType>`.
template <typename T, typename U, typename StatusType>
using IsConstructibleOrConvertibleOrAssignableFromResult =
    std::disjunction<IsConstructibleOrConvertibleFromResult<T, U, StatusType>,
                     std::is_assignable<T&, Result<U, StatusType>&>,
                     std::is_assignable<T&, const Result<U, StatusType>&>,
                     std::is_assignable<T&, Result<U, StatusType>&&>,
                     std::is_assignable<T&, const Result<U, StatusType>&&>>;

// Detects whether direct initializing `Result<T, StatusType>` from `U` is
// ambiguous, i.e. when `U` is `Result<V, StatusType>` and `T` is constructible
// or convertible from `V`.
template <typename T, typename U, typename StatusType>
struct IsDirectInitializationAmbiguous
    : public std::conditional_t<
          std::is_same<std::remove_cv_t<std::remove_reference_t<U>>, U>::value,
          std::false_type,
          IsDirectInitializationAmbiguous<
              T,
              std::remove_cv_t<std::remove_reference_t<U>>,
              StatusType>> {};

template <typename T, typename V, typename StatusType>
struct IsDirectInitializationAmbiguous<T, Result<V, StatusType>, StatusType>
    : public IsConstructibleOrConvertibleFromResult<T, V, StatusType> {};

struct ErrorTag {};
inline constexpr ErrorTag kErrorTag{};

// Checks against the constraints of the direct initialization, i.e. when
// `Result<T, StatusType>::Result(U&&)` should participate in overload
// resolution.
template <typename T, typename U, typename StatusType>
using IsDirectInitializationValid = std::disjunction<
    // Short circuits if T is basically U.
    std::is_same<std::remove_cv_t<T>,
                 std::remove_cv_t<std::remove_reference_t<U>>>,
    std::negation<std::disjunction<
        std::is_same<Result<T, StatusType>,
                     std::remove_cv_t<std::remove_reference_t<U>>>,
        std::conjunction<
            std::negation<std::is_same<std::remove_cv_t<T>, StatusType>>,
            std::disjunction<
                std::is_same<StatusType,
                             std::remove_cv_t<std::remove_reference_t<U>>>,
                std::is_same<typename StatusType::Code,
                             std::remove_cv_t<std::remove_reference_t<U>>>>>,
        std::is_same<std::in_place_t,
                     std::remove_cv_t<std::remove_reference_t<U>>>,
        IsDirectInitializationAmbiguous<T, U, StatusType>>>>;

// This trait detects whether `Result<T, StatusType>::operator=(U&&)` is
// ambiguous.
template <typename T, typename U, typename StatusType>
struct IsForwardingAssignmentAmbiguous
    : public std::conditional_t<
          std::is_same<std::remove_cv_t<std::remove_reference_t<U>>, U>::value,
          std::false_type,
          IsForwardingAssignmentAmbiguous<
              T,
              std::remove_cv_t<std::remove_reference_t<U>>,
              StatusType>> {};

template <typename T, typename U, typename StatusType>
struct IsForwardingAssignmentAmbiguous<T, Result<U, StatusType>, StatusType>
    : public IsConstructibleOrConvertibleOrAssignableFromResult<T,
                                                                U,
                                                                StatusType> {};

// Checks against the constraints of the forwarding assignment, i.e. whether
// `Result<T, StatusType>::operator=(U&&)` should participate in overload
// resolution.
template <typename T, typename U, typename StatusType>
using IsForwardingAssignmentValid = std::disjunction<
    // Short circuits if T is basically U.
    std::is_same<std::remove_cv_t<T>,
                 std::remove_cv_t<std::remove_reference_t<U>>>,
    std::negation<std::disjunction<
        std::is_same<Result<T, StatusType>,
                     std::remove_cv_t<std::remove_reference_t<U>>>,
        std::conjunction<
            std::negation<std::is_same<std::remove_cv_t<T>, StatusType>>,
            std::disjunction<
                std::is_same<StatusType,
                             std::remove_cv_t<std::remove_reference_t<U>>>,
                std::is_same<typename StatusType::Code,
                             std::remove_cv_t<std::remove_reference_t<U>>>>>,
        std::is_same<std::in_place_t,
                     std::remove_cv_t<std::remove_reference_t<U>>>,
        IsForwardingAssignmentAmbiguous<T, U, StatusType>>>>;

// This trait is for determining if a given type is a Result.
template <typename T>
constexpr bool IsResult = false;
template <typename T, typename StatusType>
constexpr bool IsResult<Result<T, StatusType>> = true;

// Detects whether `const T& == const U&` is a valid expression.
template <typename T, typename U, typename = void>
struct IsEqualityComparable : std::false_type {};

template <typename T, typename U>
struct IsEqualityComparable<
    T,
    U,
    std::void_t<decltype(std::declval<const T&>() == std::declval<const U&>())>>
    : std::true_type {};

// Checks whether `Result<T, StatusType> == U` should compare `U` against the
// held value. `Result`, status, and status code operands are handled by
// dedicated overloads and are excluded here, except when `T` itself is
// `StatusType` or `StatusType::Code` (in which case comparisons with `T`
// compare the held value).
template <typename T, typename U, typename StatusType>
using IsValueComparisonValid = std::conjunction<
    std::bool_constant<!IsResult<U>>,
    std::disjunction<std::is_same<std::remove_cv_t<T>, StatusType>,
                     std::negation<std::is_same<U, StatusType>>>,
    std::disjunction<
        std::is_same<std::remove_cv_t<T>, StatusType>,
        std::is_same<std::remove_cv_t<T>, typename StatusType::Code>,
        std::negation<std::is_same<U, typename StatusType::Code>>>,
    IsEqualityComparable<T, U>>;

// This trait determines the return type of a given function without const,
// volatile or reference qualifiers.
template <typename Fn, typename... Args>
using InvokeResultType = std::remove_cv_t<
    std::remove_reference_t<std::invoke_result_t<Fn, Args...>>>;

}  // namespace internal_result

}  // namespace pw
