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

#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include "lib/stdcompat/functional.h"
#include "pw_assert/assert.h"
#include "pw_polyfill/language_feature_macros.h"
#include "pw_preprocessor/compiler.h"
#include "pw_status/status.h"

// C++20 `std::construct_at` enables `constexpr` in-place construction (unlike
// placement `new`). On freestanding libstdc++ 13+, `std::construct_at` is
// available via `__cpp_constexpr_dynamic_alloc` even though
// `__cpp_lib_constexpr_dynamic_alloc` is omitted.
#if __cplusplus >= 202002L &&                                \
    (defined(__cpp_lib_constexpr_dynamic_alloc) ||           \
     (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE >= 13 && \
      defined(__cpp_constexpr_dynamic_alloc)))
#define _PW_RESULT_CONSTEXPR_CONSTRUCT_AT 1
#else
#define _PW_RESULT_CONSTEXPR_CONSTRUCT_AT 0
#endif

namespace pw {

class [[nodiscard]] HasValue;

template <typename T, typename StatusType = Status>
class [[nodiscard]] Result;

namespace internal_result {

enum class HasValueCode : bool {
  kEmpty = false,
  kHasValue = true,
};

constexpr const char* PwEnumToString(HasValueCode code) noexcept {
  return code == HasValueCode::kHasValue ? "HAS_VALUE" : "EMPTY";
}

struct Empty {};

struct EmptyTag {};
inline constexpr EmptyTag kEmptyTag{};

struct ErrorTag {};
inline constexpr ErrorTag kErrorTag{};

struct InvokeTag {};
inline constexpr InvokeTag kInvokeTag{};

struct FromResultTag {};
inline constexpr FromResultTag kFromResultTag{};

// Casts `value` to `const Val&` if `Owner` is an lvalue reference, or `Val&&`
// otherwise (preserving `const` if `Val` itself is `const`).
template <typename Owner, typename Val>
constexpr decltype(auto) CopyOrMove(Val& value) noexcept {
  if constexpr (std::is_lvalue_reference_v<Owner>) {
    return static_cast<const Val&>(value);
  } else {
    return static_cast<Val&&>(value);
  }
}

// Guard union member type so invalid `T` (references, arrays, functions) fails
// cleanly at `Result`'s `static_assert`s without secondary union errors.
template <typename T>
using StoredType =
    std::conditional_t<std::is_object_v<T> && !std::is_array_v<T>,
                       std::remove_const_t<T>,
                       Empty>;

PW_MODIFY_DIAGNOSTICS_PUSH();
PW_MODIFY_DIAGNOSTIC_GCC(ignored, "-Wmaybe-uninitialized");

#if defined(__GNUC__) && !defined(__clang__)
// GCC's `-Wmaybe-uninitialized` flags trivial copies/moves of an empty `Result`
// at the caller (GCC PR80635), outside this header's diagnostic pragmas. Zero
// empty storage when `Result` has a trivial copy or move (which also requires
// trivial destruction, so `Storage<StoredT, false>` is unaffected).
template <typename StoredT>
inline constexpr bool kZeroEmptyStorage =
    std::is_trivially_copy_constructible_v<StoredT> ||
    std::is_trivially_move_constructible_v<StoredT>;
#endif  // defined(__GNUC__) && !defined(__clang__)

// Named storage union specialized on trivial destructibility. In C++20, a union
// with a non-trivially-destructible variant member requires a user-provided
// `constexpr` destructor so the enclosing class can have a `constexpr`
// destructor.
//
// Pigweed always builds with exceptions disabled (-fno-exceptions), so
// `noexcept` qualifiers in `pw_result` are unconditional.
template <typename StoredT, bool = std::is_trivially_destructible_v<StoredT>>
union Storage {
  Empty empty;
  StoredT data;

#if defined(__GNUC__) && !defined(__clang__)
  unsigned char bytes[sizeof(StoredT)];

  template <typename U = StoredT,
            std::enable_if_t<kZeroEmptyStorage<U>, int> = 0>
  constexpr Storage() noexcept : bytes{} {}

  template <typename U = StoredT,
            std::enable_if_t<!kZeroEmptyStorage<U>, int> = 0>
  constexpr Storage() noexcept : empty() {}
#else
  constexpr Storage() noexcept : empty() {}
#endif  // defined(__GNUC__) && !defined(__clang__)

  template <typename... Args>
  explicit constexpr Storage(std::in_place_t, Args&&... args) noexcept
      : data(std::forward<Args>(args)...) {}

  template <typename Fn, typename... Args>
  explicit constexpr Storage(InvokeTag, Fn&& fn, Args&&... args) noexcept
      : data(cpp20::invoke(std::forward<Fn>(fn), std::forward<Args>(args)...)) {
  }
};

template <typename StoredT>
union Storage<StoredT, false> {
  Empty empty;
  StoredT data;

  constexpr Storage() noexcept : empty() {}

  template <typename... Args>
  explicit constexpr Storage(std::in_place_t, Args&&... args) noexcept
      : data(std::forward<Args>(args)...) {}

  template <typename Fn, typename... Args>
  explicit constexpr Storage(InvokeTag, Fn&& fn, Args&&... args) noexcept
      : data(cpp20::invoke(std::forward<Fn>(fn), std::forward<Args>(args)...)) {
  }

  PW_CONSTEXPR_CPP20 ~Storage() noexcept {}
  Storage(const Storage&) = delete;
  Storage(Storage&&) = delete;
  Storage& operator=(const Storage&) = delete;
  Storage& operator=(Storage&&) = delete;
};

// Holds `storage` and `status` and provides the constructors shared by both
// `ResultBase` specializations.
template <typename T, typename StatusType>
struct ResultMembers {
  using status_type = StatusType;

  Storage<StoredType<T>> storage;
  status_type status;

  ResultMembers() = delete;

  constexpr ResultMembers(EmptyTag, status_type initial_status) noexcept
      : storage(), status(std::move(initial_status)) {}

  explicit constexpr ResultMembers(status_type error_status)
      : storage(), status(std::move(error_status)) {
    PW_ASSERT(!this->status.ok());
  }

  template <typename... Args>
  explicit constexpr ResultMembers(std::in_place_t, Args&&... args) noexcept
      : storage(std::in_place, std::forward<Args>(args)...), status() {}

  template <typename Fn, typename... Args>
  explicit constexpr ResultMembers(InvokeTag, Fn&& fn, Args&&... args) noexcept
      : storage(kInvokeTag, std::forward<Fn>(fn), std::forward<Args>(args)...),
        status() {}

  // Destroys the value, if any, without updating the status.
  constexpr void Clear() noexcept {
    if constexpr (!std::is_trivially_destructible_v<StoredType<T>>) {
      if (this->status.ok()) {
        this->storage.data.~T();
      }
    }
  }
};

// Provides a trivial destructor if `T` and `StatusType` are trivially
// destructible, or a C++20-constexpr destructor that destroys the value.
template <typename T,
          typename StatusType,
          bool = std::is_trivially_destructible_v<T> &&
                 std::is_trivially_destructible_v<StatusType>>
struct ResultBase : ResultMembers<T, StatusType> {
  using ResultMembers<T, StatusType>::ResultMembers;
};

template <typename T, typename StatusType>
struct ResultBase<T, StatusType, false> : ResultMembers<T, StatusType> {
  using ResultMembers<T, StatusType>::ResultMembers;

  ResultBase(const ResultBase&) = delete;
  ResultBase(ResultBase&&) = delete;
  ResultBase& operator=(const ResultBase&) = delete;
  ResultBase& operator=(ResultBase&&) = delete;

  PW_CONSTEXPR_CPP20 ~ResultBase() noexcept { this->Clear(); }
};

// Shared mutation helpers and cross-type converting constructors/assignments.
template <typename T, typename StatusType>
struct ResultStorageBase : ResultBase<T, StatusType> {
  using Base = ResultBase<T, StatusType>;
  using status_type = StatusType;
  using Base::Base;

  ResultStorageBase() = delete;

  constexpr T& data() noexcept { return this->storage.data; }
  constexpr const T& data() const noexcept { return this->storage.data; }

  // Copies, moves, or converts from another `Result`'s storage. Implements the
  // non-trivial copy and move constructors and the converting constructors.
  template <typename Other>
  explicit constexpr ResultStorageBase(FromResultTag, Other&& other) noexcept
      : Base(kEmptyTag, CopyOrMove<Other>(other.status)) {
    if (this->status.ok()) {
      MakeValue(CopyOrMove<Other>(other.data()));
    }
  }

  template <typename... Args>
  constexpr void MakeValue(Args&&... args) noexcept {
#if _PW_RESULT_CONSTEXPR_CONSTRUCT_AT
    if constexpr (!std::is_volatile_v<T>) {
      std::construct_at(std::addressof(this->storage.data),
                        std::forward<Args>(args)...);
      return;
    }
#endif  // _PW_RESULT_CONSTEXPR_CONSTRUCT_AT
    ::new (&this->storage) StoredType<T>(std::forward<Args>(args)...);
  }

  template <typename U>
  constexpr void Assign(U&& value) noexcept {
    if (this->status.ok()) {
      this->storage.data = std::forward<U>(value);
    } else {
      MakeValue(std::forward<U>(value));
      this->status = status_type();
    }
  }

  template <typename Other>
  constexpr void AssignFrom(Other&& other) noexcept {
    if (other.status.ok()) {
      Assign(CopyOrMove<Other>(other.data()));
    } else {
      status_type error_status = CopyOrMove<Other>(other.status);
      this->Clear();
      this->status = std::move(error_status);
    }
  }

  constexpr void AssignStatus(status_type error_status) {
    this->Clear();
    this->status = std::move(error_status);
    PW_ASSERT(!this->status.ok());
  }

  template <typename status_type::Code kCode>
  constexpr void AssignStatus() noexcept {
    static_assert(!status_type(kCode).ok(),
                  "Cannot set status to an OK status; set the value instead");
    this->Clear();
    this->status = status_type(kCode);
  }
};

enum class SpecialMember {
  kTrivial,
  kNonTrivial,
  kDeleted,
};

template <bool kSupported, bool kTrivial>
inline constexpr SpecialMember kSpecialMember =
    kSupported
        ? (kTrivial ? SpecialMember::kTrivial : SpecialMember::kNonTrivial)
        : SpecialMember::kDeleted;

template <typename T, typename StatusType>
inline constexpr SpecialMember kCopyCtorKind =
    kSpecialMember<std::is_copy_constructible_v<T> &&
                       std::is_copy_constructible_v<StatusType>,
                   std::is_trivially_copy_constructible_v<T> &&
                       std::is_trivially_copy_constructible_v<StatusType>>;

template <typename T, typename StatusType>
inline constexpr SpecialMember kMoveCtorKind =
    kSpecialMember<std::is_move_constructible_v<T> &&
                       std::is_move_constructible_v<StatusType>,
                   std::is_trivially_move_constructible_v<T> &&
                       std::is_trivially_move_constructible_v<StatusType>>;

template <typename T, typename StatusType>
inline constexpr SpecialMember kCopyAssignKind =
    kSpecialMember<std::is_copy_constructible_v<T> &&
                       std::is_copy_assignable_v<T> &&
                       std::is_copy_constructible_v<StatusType> &&
                       std::is_copy_assignable_v<StatusType>,
                   std::is_trivially_destructible_v<T> &&
                       std::is_trivially_copy_constructible_v<T> &&
                       std::is_trivially_copy_assignable_v<T> &&
                       std::is_trivially_destructible_v<StatusType> &&
                       std::is_trivially_copy_constructible_v<StatusType> &&
                       std::is_trivially_copy_assignable_v<StatusType>>;

template <typename T, typename StatusType>
inline constexpr SpecialMember kMoveAssignKind =
    kSpecialMember<std::is_move_constructible_v<T> &&
                       std::is_move_assignable_v<T> &&
                       std::is_move_constructible_v<StatusType> &&
                       std::is_move_assignable_v<StatusType>,
                   std::is_trivially_destructible_v<T> &&
                       std::is_trivially_move_constructible_v<T> &&
                       std::is_trivially_move_assignable_v<T> &&
                       std::is_trivially_destructible_v<StatusType> &&
                       std::is_trivially_move_constructible_v<StatusType> &&
                       std::is_trivially_move_assignable_v<StatusType>>;

// Copy bases precede move bases so non-const copy signatures on `T` (e.g.
// `T(T&) = delete`) are normalized to `const` before derived classes `=
// default` copy operations in C++17.
template <typename T,
          typename StatusType,
          SpecialMember = kCopyCtorKind<T, StatusType>>
struct ResultCopyCtorBase : ResultStorageBase<T, StatusType> {
  using Base = ResultStorageBase<T, StatusType>;
  using Base::Base;

  ResultCopyCtorBase() = delete;
};

template <typename T, typename StatusType>
struct ResultCopyCtorBase<T, StatusType, SpecialMember::kNonTrivial>
    : ResultStorageBase<T, StatusType> {
  using Base = ResultStorageBase<T, StatusType>;
  using Base::Base;

  ResultCopyCtorBase() = delete;

  constexpr ResultCopyCtorBase(const ResultCopyCtorBase& other) noexcept
      : Base(kFromResultTag, other) {}

  ResultCopyCtorBase(ResultCopyCtorBase&&) = default;
  ResultCopyCtorBase& operator=(ResultCopyCtorBase&&) = default;
};

template <typename T, typename StatusType>
struct ResultCopyCtorBase<T, StatusType, SpecialMember::kDeleted>
    : ResultStorageBase<T, StatusType> {
  using Base = ResultStorageBase<T, StatusType>;
  using Base::Base;

  ResultCopyCtorBase() = delete;
  ResultCopyCtorBase(const ResultCopyCtorBase&) = delete;
  ResultCopyCtorBase(ResultCopyCtorBase&&) = default;
  ResultCopyCtorBase& operator=(ResultCopyCtorBase&&) = default;
};

template <typename T,
          typename StatusType,
          SpecialMember = kCopyAssignKind<T, StatusType>>
struct ResultCopyAssignBase : ResultCopyCtorBase<T, StatusType> {
  using Base = ResultCopyCtorBase<T, StatusType>;
  using Base::Base;

  ResultCopyAssignBase() = delete;
};

template <typename T, typename StatusType>
struct ResultCopyAssignBase<T, StatusType, SpecialMember::kNonTrivial>
    : ResultCopyCtorBase<T, StatusType> {
  using Base = ResultCopyCtorBase<T, StatusType>;
  using Base::Base;

  ResultCopyAssignBase() = delete;
  ResultCopyAssignBase(const ResultCopyAssignBase&) = default;
  ResultCopyAssignBase(ResultCopyAssignBase&&) = default;

  constexpr ResultCopyAssignBase& operator=(
      const ResultCopyAssignBase& other) noexcept {
    if (this != &other) {
      this->AssignFrom(other);
    }
    return *this;
  }

  ResultCopyAssignBase& operator=(ResultCopyAssignBase&&) = default;
};

template <typename T, typename StatusType>
struct ResultCopyAssignBase<T, StatusType, SpecialMember::kDeleted>
    : ResultCopyCtorBase<T, StatusType> {
  using Base = ResultCopyCtorBase<T, StatusType>;
  using Base::Base;

  ResultCopyAssignBase() = delete;
  ResultCopyAssignBase(const ResultCopyAssignBase&) = default;
  ResultCopyAssignBase(ResultCopyAssignBase&&) = default;
  ResultCopyAssignBase& operator=(const ResultCopyAssignBase&) = delete;
  ResultCopyAssignBase& operator=(ResultCopyAssignBase&&) = default;
};

template <typename T,
          typename StatusType,
          SpecialMember = kMoveCtorKind<T, StatusType>>
struct ResultMoveCtorBase : ResultCopyAssignBase<T, StatusType> {
  using Base = ResultCopyAssignBase<T, StatusType>;
  using Base::Base;

  ResultMoveCtorBase() = delete;
};

template <typename T, typename StatusType>
struct ResultMoveCtorBase<T, StatusType, SpecialMember::kNonTrivial>
    : ResultCopyAssignBase<T, StatusType> {
  using Base = ResultCopyAssignBase<T, StatusType>;
  using Base::Base;

  ResultMoveCtorBase() = delete;
  ResultMoveCtorBase(const ResultMoveCtorBase&) = default;

  constexpr ResultMoveCtorBase(ResultMoveCtorBase&& other) noexcept
      : Base(kFromResultTag, std::move(other)) {}

  ResultMoveCtorBase& operator=(const ResultMoveCtorBase&) = default;
  ResultMoveCtorBase& operator=(ResultMoveCtorBase&&) = default;
};

template <typename T, typename StatusType>
struct ResultMoveCtorBase<T, StatusType, SpecialMember::kDeleted>
    : ResultCopyAssignBase<T, StatusType> {
  using Base = ResultCopyAssignBase<T, StatusType>;
  using Base::Base;

  ResultMoveCtorBase() = delete;
  ResultMoveCtorBase(const ResultMoveCtorBase&) = default;
  ResultMoveCtorBase(ResultMoveCtorBase&&) = delete;
  ResultMoveCtorBase& operator=(const ResultMoveCtorBase&) = default;
  ResultMoveCtorBase& operator=(ResultMoveCtorBase&&) = default;
};

template <typename T,
          typename StatusType,
          SpecialMember = kMoveAssignKind<T, StatusType>>
struct ResultMoveAssignBase : ResultMoveCtorBase<T, StatusType> {
  using Base = ResultMoveCtorBase<T, StatusType>;
  using Base::Base;

  ResultMoveAssignBase() = delete;
};

template <typename T, typename StatusType>
struct ResultMoveAssignBase<T, StatusType, SpecialMember::kNonTrivial>
    : ResultMoveCtorBase<T, StatusType> {
  using Base = ResultMoveCtorBase<T, StatusType>;
  using Base::Base;

  ResultMoveAssignBase() = delete;
  ResultMoveAssignBase(const ResultMoveAssignBase&) = default;
  ResultMoveAssignBase(ResultMoveAssignBase&&) = default;
  ResultMoveAssignBase& operator=(const ResultMoveAssignBase&) = default;

  constexpr ResultMoveAssignBase& operator=(
      ResultMoveAssignBase&& other) noexcept {
    if (this != &other) {
      this->AssignFrom(std::move(other));
    }
    return *this;
  }
};

template <typename T, typename StatusType>
struct ResultMoveAssignBase<T, StatusType, SpecialMember::kDeleted>
    : ResultMoveCtorBase<T, StatusType> {
  using Base = ResultMoveCtorBase<T, StatusType>;
  using Base::Base;

  ResultMoveAssignBase() = delete;
  ResultMoveAssignBase(const ResultMoveAssignBase&) = default;
  ResultMoveAssignBase(ResultMoveAssignBase&&) = default;
  ResultMoveAssignBase& operator=(const ResultMoveAssignBase&) = default;
  ResultMoveAssignBase& operator=(ResultMoveAssignBase&&) = delete;
};

template <typename T, typename StatusType>
using ResultData = ResultMoveAssignBase<T, StatusType>;

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

// This trait is for determining if a given type is a Result.
template <typename T>
constexpr bool IsResult = false;
template <typename T, typename StatusType>
constexpr bool IsResult<Result<T, StatusType>> = true;

// This trait is for determining if a given type is an Optional.
template <typename T>
constexpr bool IsOptional = false;
template <typename T>
constexpr bool IsOptional<Result<T, HasValue>> = true;

// Detects whether `T` is constructible or convertible from `Result<U,
// StatusType>`. When `Result<T, StatusType>` is `Optional<cv bool>`, conversion
// from `Optional<U>` (e.g., via `Optional`'s `explicit operator bool()`) is
// ignored so the converting constructor takes precedence (see LWG 3836).
template <typename T, typename U, typename StatusType>
using IsConstructibleOrConvertibleFromResult = std::conjunction<
    std::negation<std::conjunction<std::is_same<StatusType, HasValue>,
                                   std::is_same<std::remove_cv_t<T>, bool>>>,
    std::disjunction<std::is_constructible<T, Result<U, StatusType>&>,
                     std::is_constructible<T, const Result<U, StatusType>&>,
                     std::is_constructible<T, Result<U, StatusType>&&>,
                     std::is_constructible<T, const Result<U, StatusType>&&>,
                     std::is_convertible<Result<U, StatusType>&, T>,
                     std::is_convertible<const Result<U, StatusType>&, T>,
                     std::is_convertible<Result<U, StatusType>&&, T>,
                     std::is_convertible<const Result<U, StatusType>&&, T>>>;

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
        std::conjunction<std::is_same<StatusType, HasValue>,
                         std::is_same<std::remove_cv_t<T>, bool>,
                         std::bool_constant<IsOptional<
                             std::remove_cv_t<std::remove_reference_t<U>>>>>,
        std::conjunction<
            std::negation<std::is_same<std::remove_cv_t<T>, StatusType>>,
            std::disjunction<
                std::is_same<StatusType,
                             std::remove_cv_t<std::remove_reference_t<U>>>,
                std::is_same<typename StatusType::Code,
                             std::remove_cv_t<std::remove_reference_t<U>>>>>,
        std::is_same<std::in_place_t,
                     std::remove_cv_t<std::remove_reference_t<U>>>,
        std::conjunction<
            std::is_same<StatusType, HasValue>,
            std::is_same<std::nullopt_t,
                         std::remove_cv_t<std::remove_reference_t<U>>>>,
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
        std::conjunction<std::is_same<StatusType, HasValue>,
                         std::is_same<std::remove_cv_t<T>, bool>,
                         std::bool_constant<IsOptional<
                             std::remove_cv_t<std::remove_reference_t<U>>>>>,
        std::conjunction<
            std::negation<std::is_same<std::remove_cv_t<T>, StatusType>>,
            std::disjunction<
                std::is_same<StatusType,
                             std::remove_cv_t<std::remove_reference_t<U>>>,
                std::is_same<typename StatusType::Code,
                             std::remove_cv_t<std::remove_reference_t<U>>>>>,
        std::is_same<std::in_place_t,
                     std::remove_cv_t<std::remove_reference_t<U>>>,
        std::conjunction<
            std::is_same<StatusType, HasValue>,
            std::is_same<std::nullopt_t,
                         std::remove_cv_t<std::remove_reference_t<U>>>>,
        IsForwardingAssignmentAmbiguous<T, U, StatusType>>>>;

// SFINAE constraints for converting, status, and value constructors/assignments
// on `Result<T, StatusType>` and `Result<void, StatusType>`.
template <typename T,
          typename U,
          typename FromRef,
          typename StatusType,
          bool kImplicit>
inline constexpr bool kCanConvertFromResult = std::conjunction<
    std::negation<std::is_same<T, U>>,
    std::is_constructible<T, FromRef>,
    std::conditional_t<kImplicit,
                       std::is_convertible<FromRef, T>,
                       std::negation<std::is_convertible<FromRef, T>>>,
    std::negation<IsConstructibleOrConvertibleFromResult<T, U, StatusType>>>::
    value;

template <typename T, typename U, typename FromRef, typename StatusType>
inline constexpr bool kCanAssignFromResult = std::conjunction<
    std::negation<std::is_same<T, U>>,
    std::is_constructible<T, FromRef>,
    std::is_assignable<T&, FromRef>,
    std::negation<
        IsConstructibleOrConvertibleOrAssignableFromResult<T, U, StatusType>>>::
    value;

template <typename T, typename URef, typename StatusType, bool kImplicit>
inline constexpr bool kCanConstructFromStatus = std::conjunction<
    std::negation<std::is_same<std::remove_cv_t<T>, StatusType>>,
    std::negation<std::is_same<std::decay_t<URef>, Result<T, StatusType>>>,
    std::negation<std::is_same<std::decay_t<URef>, std::remove_cv_t<T>>>,
    std::negation<std::is_same<std::decay_t<URef>, std::in_place_t>>,
    std::negation<
        std::conjunction<std::is_same<StatusType, HasValue>,
                         std::is_same<std::decay_t<URef>, std::nullopt_t>>>,
    std::is_constructible<StatusType, URef>,
    std::conditional_t<
        kImplicit,
        std::is_convertible<URef, StatusType>,
        std::conjunction<
            std::negation<std::is_convertible<URef, StatusType>>,
            // Prefer implicit conversion to `T` over `explicit` status
            // construction, except for `StatusType::Code` (which
            // `IsDirectInitializationValid` already excludes from the value
            // constructor even when `Code` is an unscoped enum convertible to
            // `T`, e.g. `pw_Status` -> `int`).
            std::disjunction<
                std::is_same<std::decay_t<URef>, typename StatusType::Code>,
                std::negation<std::is_convertible<URef, T>>>>>,
    std::negation<HasConversionOperatorToResult<T, URef, StatusType>>>::value;

template <typename T, typename URef, typename StatusType>
inline constexpr bool kCanAssignFromStatus =
    kCanConstructFromStatus<T, URef, StatusType, true>;

template <typename T, typename URef, typename StatusType, bool kImplicit>
inline constexpr bool kCanConstructFromValue = std::conjunction<
    IsDirectInitializationValid<T, URef, StatusType>,
    std::is_constructible<T, URef>,
    std::conditional_t<kImplicit,
                       std::is_convertible<URef, T>,
                       std::negation<std::is_convertible<URef, T>>>,
    std::disjunction<
        std::is_same<std::remove_cv_t<std::remove_reference_t<URef>>,
                     std::remove_cv_t<T>>,
        std::conjunction<
            std::disjunction<std::is_same<std::remove_cv_t<T>, StatusType>,
                             std::negation<std::conditional_t<
                                 kImplicit,
                                 std::is_convertible<URef, StatusType>,
                                 std::is_constructible<StatusType, URef>>>>,
            std::negation<
                HasConversionOperatorToResult<T, URef, StatusType>>>>>::value;

template <typename T, typename URef, typename StatusType>
inline constexpr bool kCanAssignFromValue = std::conjunction<
    IsForwardingAssignmentValid<T, URef, StatusType>,
    std::negation<std::conjunction<std::is_same<StatusType, HasValue>,
                                   std::is_scalar<T>,
                                   std::is_same<T, std::decay_t<URef>>>>,
    std::is_constructible<T, URef>,
    std::is_assignable<T&, URef>,
    std::disjunction<
        std::is_same<std::remove_cv_t<std::remove_reference_t<URef>>,
                     std::remove_cv_t<T>>,
        std::conjunction<
            std::disjunction<
                std::is_same<std::remove_cv_t<T>, StatusType>,
                std::negation<std::is_convertible<URef, StatusType>>>,
            std::negation<
                HasConversionOperatorToResult<T, URef, StatusType>>>>>::value;

template <typename URef, typename StatusType, bool kImplicit>
inline constexpr bool kCanVoidConstructFromStatus = std::conjunction<
    std::negation<std::is_same<std::decay_t<URef>, Result<void, StatusType>>>,
    std::negation<std::is_same<std::decay_t<URef>, StatusType>>,
    std::negation<std::is_same<std::decay_t<URef>, std::in_place_t>>,
    std::is_constructible<StatusType, URef>,
    std::conditional_t<kImplicit,
                       std::is_convertible<URef, StatusType>,
                       std::negation<std::is_convertible<URef, StatusType>>>,
    std::negation<HasConversionOperatorToResult<void, URef, StatusType>>>::
    value;

template <typename URef, typename StatusType>
inline constexpr bool kCanVoidAssignFromStatus = std::conjunction<
    std::negation<std::is_same<std::decay_t<URef>, Result<void, StatusType>>>,
    std::negation<std::is_same<std::decay_t<URef>, std::in_place_t>>,
    std::is_constructible<StatusType, URef>,
    std::is_convertible<URef, StatusType>,
    std::negation<HasConversionOperatorToResult<void, URef, StatusType>>>::
    value;

// Detects whether `const T& == const U&` is a valid expression convertible to
// `bool`. Guards against `void` before forming references.
template <typename T,
          typename U,
          bool = !std::is_void_v<T> && !std::is_void_v<U>,
          typename = void>
struct IsEqualityComparable : std::false_type {};

template <typename T, typename U>
struct IsEqualityComparable<
    T,
    U,
    true,
    std::enable_if_t<std::is_convertible_v<decltype(std::declval<const T&>() ==
                                                    std::declval<const U&>()),
                                           bool>>> : std::true_type {};

template <typename T,
          typename U,
          bool = !std::is_void_v<T> && !std::is_void_v<U>,
          typename = void>
struct IsInequalityComparable : std::false_type {};

template <typename T, typename U>
struct IsInequalityComparable<
    T,
    U,
    true,
    std::enable_if_t<std::is_convertible_v<decltype(std::declval<const T&>() !=
                                                    std::declval<const U&>()),
                                           bool>>> : std::true_type {};

template <typename T, typename U>
struct IsNotEqualComparable : std::disjunction<IsInequalityComparable<T, U>,
                                               IsEqualityComparable<T, U>> {};

template <typename T,
          typename U,
          bool = !std::is_void_v<T> && !std::is_void_v<U>,
          typename = void>
struct IsLessThanComparable : std::false_type {};

template <typename T, typename U>
struct IsLessThanComparable<
    T,
    U,
    true,
    std::enable_if_t<std::is_convertible_v<decltype(std::declval<const T&>() <
                                                    std::declval<const U&>()),
                                           bool>>> : std::true_type {};

template <typename T,
          typename U,
          bool = !std::is_void_v<T> && !std::is_void_v<U>,
          typename = void>
struct IsLessEqualComparable : std::false_type {};

template <typename T, typename U>
struct IsLessEqualComparable<
    T,
    U,
    true,
    std::enable_if_t<std::is_convertible_v<decltype(std::declval<const T&>() <=
                                                    std::declval<const U&>()),
                                           bool>>> : std::true_type {};

template <typename T,
          typename U,
          bool = !std::is_void_v<T> && !std::is_void_v<U>,
          typename = void>
struct IsGreaterThanComparable : std::false_type {};

template <typename T, typename U>
struct IsGreaterThanComparable<
    T,
    U,
    true,
    std::enable_if_t<std::is_convertible_v<decltype(std::declval<const T&>() >
                                                    std::declval<const U&>()),
                                           bool>>> : std::true_type {};

template <typename T,
          typename U,
          bool = !std::is_void_v<T> && !std::is_void_v<U>,
          typename = void>
struct IsGreaterEqualComparable : std::false_type {};

template <typename T, typename U>
struct IsGreaterEqualComparable<
    T,
    U,
    true,
    std::enable_if_t<std::is_convertible_v<decltype(std::declval<const T&>() >=
                                                    std::declval<const U&>()),
                                           bool>>> : std::true_type {};

// Enables `Result<T, StatusType>` status comparisons unless `T` itself is
// `StatusType` (in which case `operator==` compares the held value).
template <typename T, typename StatusType>
inline constexpr bool kEnableStatusComparison =
    !std::is_same_v<std::remove_cv_t<T>, StatusType>;

// Enables `Result<T, StatusType>` status-code comparisons unless `T` itself is
// `StatusType` or `StatusType::Code`.
template <typename T, typename StatusType>
inline constexpr bool kEnableStatusCodeComparison =
    !std::is_same_v<std::remove_cv_t<T>, StatusType> &&
    !std::is_same_v<std::remove_cv_t<T>, typename StatusType::Code>;

// Checks whether comparing `Result<T, StatusType>` and `U` (using comparison
// trait `Comp`) should compare `U` against the held value. `Result`, status
// (or types implicitly convertible to `StatusType`), and status code operands
// are handled by dedicated overloads and are excluded here, except when `T`
// itself is `StatusType`, `StatusType::Code`, or the same (cv-unqualified) type
// as `U` (in which case comparisons with `T` compare the held value).
template <typename T,
          typename U,
          typename StatusType,
          typename Comp = IsEqualityComparable<T, U>>
inline constexpr bool kIsValueComparable = std::conjunction<
    std::bool_constant<!IsResult<U>>,
    std::negation<
        std::conjunction<std::is_same<StatusType, HasValue>,
                         std::is_same<std::remove_cv_t<U>, std::nullopt_t>>>,
    std::disjunction<std::is_same<std::remove_cv_t<T>, StatusType>,
                     std::is_same<std::remove_cv_t<T>, std::remove_cv_t<U>>,
                     std::negation<std::is_convertible<const U&, StatusType>>>,
    std::disjunction<
        std::is_same<std::remove_cv_t<T>, StatusType>,
        std::is_same<std::remove_cv_t<T>, typename StatusType::Code>,
        std::negation<
            std::is_same<std::remove_cv_t<U>, typename StatusType::Code>>>,
    Comp>::value;

// This trait determines the return type of a given function without const,
// volatile or reference qualifiers.
template <typename Fn, typename... Args>
using InvokeResultType = std::remove_cv_t<
    std::remove_reference_t<std::invoke_result_t<Fn, Args...>>>;

// Shared implementation for `and_then` and `transform` across `Result<T>` and
// `Result<void>`. Befriended by `Result` to access private tag constructors.
struct Monadic {
  template <typename Ret,
            typename StatusType,
            typename StatusArg,
            typename Fn,
            typename... ValArgs>
  static constexpr Ret AndThen(bool is_ok,
                               StatusArg&& status,
                               Fn&& function,
                               ValArgs&&... args) {
    static_assert(IsResult<Ret>, "Fn must return a pw::Result");
    if constexpr (IsResult<Ret>) {
      static_assert(std::is_same_v<typename Ret::status_type, StatusType>,
                    "Fn must return a pw::Result with the same status_type");
      if (!is_ok) {
        return Ret(kErrorTag, std::forward<StatusArg>(status));
      }
      return cpp20::invoke(std::forward<Fn>(function),
                           std::forward<ValArgs>(args)...);
    } else {
      return cpp20::invoke(std::forward<Fn>(function),
                           std::forward<ValArgs>(args)...);
    }
  }

  template <typename Ret,
            typename StatusType,
            typename StatusArg,
            typename Fn,
            typename... ValArgs>
  static constexpr Result<Ret, StatusType> Transform(bool is_ok,
                                                     StatusArg&& status,
                                                     Fn&& function,
                                                     ValArgs&&... args) {
    if (!is_ok) {
      return Result<Ret, StatusType>(kErrorTag,
                                     std::forward<StatusArg>(status));
    }
    if constexpr (std::is_void_v<Ret>) {
      cpp20::invoke(std::forward<Fn>(function), std::forward<ValArgs>(args)...);
      return Result<void, StatusType>(std::in_place);
    } else {
      return Result<Ret, StatusType>(kInvokeTag,
                                     std::forward<Fn>(function),
                                     std::forward<ValArgs>(args)...);
    }
  }
};

// True if an `or_else` callback accepts `StatusArg` (or `const Status&` when
// returning `void`, since `or_else` returns `self` untouched) or no arguments.
template <typename Fn, typename StatusArg>
constexpr bool IsOrElseInvocable() {
  using ConstStatusRef =
      const std::remove_cv_t<std::remove_reference_t<StatusArg>>&;
  if constexpr (std::is_invocable_v<Fn, StatusArg>) {
    if constexpr (std::is_void_v<std::invoke_result_t<Fn, StatusArg>>) {
      if constexpr (std::is_invocable_v<Fn, ConstStatusRef>) {
        return std::is_void_v<std::invoke_result_t<Fn, ConstStatusRef>>;
      }
      return false;
    }
    return true;
  } else {
    return std::is_invocable_v<Fn>;
  }
}

template <typename Fn, typename StatusArg>
inline constexpr bool kIsOrElseInvocable = IsOrElseInvocable<Fn, StatusArg>();

// Invokes an `or_else` callback with the status, if it accepts one, or with no
// arguments.
template <typename Fn, typename StatusArg>
constexpr decltype(auto) InvokeOrElse(Fn&& function, StatusArg&& status) {
  if constexpr (std::is_invocable_v<Fn, StatusArg>) {
    return cpp20::invoke(std::forward<Fn>(function),
                         std::forward<StatusArg>(status));
  } else {
    return cpp20::invoke(std::forward<Fn>(function));
  }
}

// Implements `Result<T, StatusType>::or_else` and
// `Result<void, StatusType>::or_else`. `self` is the `Result` (`R`) as a const
// lvalue or an rvalue, and `status` is its status as passed to `function`.
template <typename R, typename Self, typename Fn, typename StatusArg>
constexpr R OrElse(Self&& self, Fn&& function, StatusArg&& status) {
  using Ret =
      decltype(InvokeOrElse(std::declval<Fn>(), std::declval<StatusArg>()));
  if constexpr (std::is_void_v<Ret>) {
    if (!self.ok()) {
      using ConstStatusRef =
          const std::remove_cv_t<std::remove_reference_t<StatusArg>>&;
      if constexpr (std::is_invocable_v<Fn, ConstStatusRef>) {
        cpp20::invoke(std::forward<Fn>(function),
                      static_cast<ConstStatusRef>(status));
      } else {
        cpp20::invoke(std::forward<Fn>(function));
      }
    }
    return std::forward<Self>(self);
  } else {
    static_assert(std::is_convertible_v<Ret, R>,
                  "Fn must be convertible to a pw::Result");
    if (self.ok()) {
      return std::forward<Self>(self);
    }
    return InvokeOrElse(std::forward<Fn>(function),
                        std::forward<StatusArg>(status));
  }
}

PW_MODIFY_DIAGNOSTICS_POP();

}  // namespace internal_result

}  // namespace pw
