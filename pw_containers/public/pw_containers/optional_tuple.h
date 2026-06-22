// Copyright 2025 The Pigweed Authors
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

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "lib/stdcompat/type_traits.h"
#include "pw_assert/assert.h"
#include "pw_containers/bitset.h"
#include "pw_polyfill/language_feature_macros.h"

namespace pw {

template <typename... Types>
class OptionalTuple;

namespace containers::internal {

// Empty tag type to indicate that an `pw::OptionalTuple` element is not set.
// Equivalent to `std::nullopt_t`. Use `pw::kTupleNull` to refer to a value of
// this type.
//
// This type is for the exclusive use of  only be used by Pigweed containers!
class OptionalTupleNullPlaceholder {
 public:
  static constexpr OptionalTupleNullPlaceholder InternalUseOnly() { return {}; }

 private:
  constexpr OptionalTupleNullPlaceholder() = default;
};

struct Empty {};

// Untagged optional type for use in OptionalTuple.
template <typename T>
union Maybe {
  // For trivially default constructible types, prefer default-constructing the
  // value over default-constructing the empty state to avoid a
  // -WWmaybe-uninitialized when building with GCC.
  template <
      typename U = T,
      typename = std::enable_if_t<std::is_trivially_default_constructible_v<U>>>
  explicit constexpr Maybe(OptionalTupleNullPlaceholder) : value{} {}

  template <typename U = T,
            typename =
                std::enable_if_t<!std::is_trivially_default_constructible_v<U>>,
            int = 0>
  explicit constexpr Maybe(OptionalTupleNullPlaceholder) : none{} {}

  template <typename... Args>
  constexpr Maybe(Args&&... args) : value(std::forward<Args>(args)...) {}

  // A destructor is required for types with non-trivial destructors.
  PW_CONSTEXPR_CPP20 ~Maybe() {}

  Empty none;
  T value;
};

template <typename T, typename First, typename... Other>
constexpr size_t FindFirstIndexOf() {
  if constexpr (std::is_same_v<T, First>) {
    return 0;
  } else if constexpr (sizeof...(Other) > 0u) {
    return 1 + FindFirstIndexOf<T, Other...>();
  } else {
    static_assert(sizeof...(Other) != 0u,
                  "The type is not present in the type parameter pack");
  }
}

template <typename T>
struct IsOptionalTuple : std::false_type {};

template <typename... Types>
struct IsOptionalTuple<OptionalTuple<Types...>> : std::true_type {};

}  // namespace containers::internal

/// @submodule{pw_containers,utilities}

/// Constant used to skip setting a `pw::OptionalTuple` element during
/// construction. Equivalent to `std::nullopt`. `pw::kTupleNull` is exclusively
/// for `pw::OptionalTuple`; do not use it for other types.
///
/// `kTupleNull` is used in place of a `std::nullopt` or another shared value to
/// prevent ambiguity. For example, with
/// `pw::OptionalTuple<std::optional<int>>`, passing `std::nullopt` could either
/// mean to leave the element uninitialized or initialize it to an empty
/// `std::optional`.
inline constexpr auto kTupleNull =
    containers::internal::OptionalTupleNullPlaceholder::InternalUseOnly();

/// Tuple class with optional elements. Equivalent to
/// `std::tuple<std::optional<Types>...>`, but much more efficient. Field
/// presence is tracked by a single `pw::BitSet`, so the tuple elements are
/// packed equivalently to `std::tuple<Types...>`.
///
/// Tuple elements are specified by their index. Elements may also be referenced
/// by type, if there is only one instance of that type in the tuple.
///
/// Compatible with `std::tuple_element` and `std::tuple_size`.
///
/// @note Like `std::optional`, moving an element out of an `OptionalTuple`
/// leaves the element in an active but moved state (`has_value<>()` returns
/// `true`). Call `reset()` to remove the element.
template <typename... Types>
class OptionalTuple {
 private:
  template <typename T>
  static constexpr size_t TypeToIndex();

  template <size_t kIndex>
  using Element = std::tuple_element_t<kIndex, std::tuple<Types...>>;

  template <typename T>
  using ElementByType = Element<TypeToIndex<T>()>;

 public:
  /// Default constructs an `OptionalTuple` with all elements unset.
  constexpr OptionalTuple() : tuple_(kToTupleNull<Types>..., Bits{}) {}

  /// Constructs an `OptionalTuple`, forwarding each argument to its
  /// corresponding element. Pass `pw::kTupleNull` to skip initializing
  /// an element.
  template <
      typename... Args,
      typename = std::enable_if_t<
          (std::is_constructible_v<containers::internal::Maybe<Types>, Args> &&
           ...) &&
          (sizeof...(Types) != 1 || (!containers::internal::IsOptionalTuple<
                                         cpp20::remove_cvref_t<Args>>::value &&
                                     ...))>>
  constexpr OptionalTuple(Args&&... args)
      : tuple_(
            std::forward<Args>(args)...,
            Bits::LittleEndian(
                !std::is_same_v<
                    cpp20::remove_cvref_t<Args>,
                    containers::internal::OptionalTupleNullPlaceholder>...)) {}

  /// Copy constructs from another `OptionalTuple`.
  constexpr OptionalTuple(const OptionalTuple& other)
      : tuple_(kToTupleNull<Types>..., Bits{}) {
    UninitializedCopyFrom(other, kAllIdx);
  }

  /// Copy assigns this from another `OptionalTuple`.
  constexpr OptionalTuple& operator=(const OptionalTuple& other) {
    DestroyAll(kAllIdx);  // To simplify the copy, first destroy all elements.
    UninitializedCopyFrom(std::move(other), kAllIdx);
    return *this;
  }

  /// Move constructs from another `OptionalTuple`.
  constexpr OptionalTuple(OptionalTuple&& other)
      : tuple_(kToTupleNull<Types>..., Bits{}) {
    UninitializedMoveFrom(other, kAllIdx);
  }

  /// Move assigns this from another `OptionalTuple`.
  constexpr OptionalTuple& operator=(OptionalTuple&& other) {
    DestroyAll(kAllIdx);  // To simplify the move, first destroy all elements.
    UninitializedMoveFrom(other, kAllIdx);
    return *this;
  }

  // Converting copy/move constructors are not yet implemented.

  /// Destroys the `OptionalTuple` and all its active elements.
  PW_CONSTEXPR_CPP20 ~OptionalTuple() { DestroyAll(kAllIdx); }

  // swap() is not yet implemented.

  /// Checks if the `OptionalTuple` contains no active elements.
  [[nodiscard]] constexpr bool empty() const { return active().none(); }

  /// Returns the number of active elements in the `OptionalTuple`.
  constexpr size_t count() const { return active().count(); }

  /// Returns the total number elements in the `OptionalTuple`. Equivalent to
  /// `std::tuple_size<>`.
  constexpr size_t size() const { return sizeof...(Types); }

  /// Checks if the element at `kIndex` has a value.
  template <size_t kIndex>
  constexpr bool has_value() const {
    return active().template test<kIndex>();
  }

  /// Checks if the element of type `T` has a value.
  template <typename T>
  constexpr bool has_value() const {
    return has_value<TypeToIndex<T>()>();
  }

  /// Returns a reference to the element at `kIndex`.
  /// @pre `has_value<kIndex>` must be true.
  template <size_t kIndex>
  constexpr Element<kIndex>& value() & {
    PW_ASSERT(has_value<kIndex>());
    return RawValue<kIndex>();
  }

  /// Returns a const reference to the element at `kIndex`.
  /// @pre `has_value<kIndex>` must be true.
  template <size_t kIndex>
  constexpr const Element<kIndex>& value() const& {
    PW_ASSERT(has_value<kIndex>());
    return RawValue<kIndex>();
  }

  /// Returns an rvalue reference to the element at `kIndex`.
  /// @pre `has_value<kIndex>` must be true.
  template <size_t kIndex>
  constexpr Element<kIndex>&& value() && {
    PW_ASSERT(has_value<kIndex>());
    return std::move(RawValue<kIndex>());
  }

  /// Returns a const rvalue reference to the element at `kIndex`.
  /// @pre `has_value<kIndex>` must be true.
  template <size_t kIndex>
  constexpr const Element<kIndex>&& value() const&& {
    PW_ASSERT(has_value<kIndex>());
    return std::move(RawValue<kIndex>());
  }

  /// Returns a reference to the element of type `T`.
  /// @pre `has_value<T>` must be true.
  template <typename T>
  constexpr ElementByType<T>& value() & {
    return value<TypeToIndex<T>()>();
  }

  /// Returns a const reference to the element of type `T`.
  /// @pre `has_value<T>` must be true.
  template <typename T>
  constexpr const ElementByType<T>& value() const& {
    return value<TypeToIndex<T>()>();
  }

  /// Returns an rvalue reference to the element of type `T`.
  /// @pre `has_value<T>` must be true.
  template <typename T>
  constexpr ElementByType<T>&& value() && {
    return std::move(value<TypeToIndex<T>()>());
  }

  /// Returns a const rvalue reference to the element of type `T`.
  /// @pre `has_value<T>` must be true.
  template <typename T>
  constexpr const ElementByType<T>&& value() const&& {
    return std::move(value<TypeToIndex<T>()>());
  }

  /// Returns the element at the specified index, if present. Otherwise, returns
  /// `default_value`.
  template <size_t kIndex, int&... kExplicitGuard, typename U>
  constexpr Element<kIndex> value_or(U&& default_value) const& {
    return has_value<kIndex>()
               ? RawValue<kIndex>()
               : static_cast<Element<kIndex>>(std::forward<U>(default_value));
  }

  /// Returns the element with the specified type, if present. Otherwise,
  /// returns `default_value`.
  template <typename T, int&... kExplicitGuard, typename U>
  constexpr ElementByType<T> value_or(U&& default_value) const& {
    return value_or<TypeToIndex<T>()>(std::forward<U>(default_value));
  }

  /// Moves and returns the element at the specified index, if present.
  /// Otherwise, returns `default_value`.
  template <size_t kIndex, int&... kExplicitGuard, typename U>
  constexpr Element<kIndex> value_or(U&& default_value) && {
    return has_value<kIndex>()
               ? std::move(RawValue<kIndex>())
               : static_cast<Element<kIndex>>(std::forward<U>(default_value));
  }

  /// Moves and returns the element with the specified type, if present.
  /// Otherwise, returns `default_value`.
  template <typename T, int&... kExplicitGuard, typename U>
  constexpr ElementByType<T> value_or(U&& default_value) && {
    return std::move(*this).template value_or<TypeToIndex<T>()>(
        std::forward<U>(default_value));
  }

  /// Constructs an element in place and marks it active. Destroys the previous
  /// value, if any.
  ///
  /// @returns A reference to the newly initialized item.
  template <size_t kIndex, int&... kExplicitGuard, typename... Args>
  constexpr Element<kIndex>& emplace(Args&&... args) {
    DestroyIfActive<kIndex>();
    active().template set<kIndex>();
    return *new (&RawValue<kIndex>())
        Element<kIndex>(std::forward<Args>(args)...);
  }

  /// Constructs an element of type `T` in place and marks it active. Destroys
  /// the previous value, if any.
  ///
  /// @returns A reference to the newly initialized item.
  template <typename T, int&... kExplicitGuard, typename... Args>
  constexpr ElementByType<T>& emplace(Args&&... args) {
    return emplace<TypeToIndex<T>()>(std::forward<Args>(args)...);
  }

  /// Resets (clears) the value at the specified index, if any.
  template <size_t kIndex>
  constexpr void reset() {
    DestroyIfActive<kIndex>();
    active().template reset<kIndex>();
  }

  /// Resets (clears) the value of type `T`, if any.
  template <typename T>
  constexpr void reset() {
    return reset<TypeToIndex<T>()>();
  }

 private:
  static constexpr auto kAllIdx = std::make_index_sequence<sizeof...(Types)>{};

  // Used to expand N types to N uses of kTupleNull.
  template <typename>
  static constexpr auto kToTupleNull = kTupleNull;

  template <size_t... kIndices>
  constexpr void DestroyAll(std::index_sequence<kIndices...>) {
    (DestroyIfActive<kIndices>(), ...);
  }

  template <size_t kIndex>
  constexpr void DestroyIfActive() {
    if (has_value<kIndex>()) {
      std::destroy_at(&RawValue<kIndex>());
    }
  }

  template <size_t... kIndices>
  constexpr void UninitializedCopyFrom(const OptionalTuple& other,
                                       std::index_sequence<kIndices...>) {
    (CopyElement<kIndices>(other), ...);
    active() = other.active();
  }

  template <size_t kIndex>
  constexpr void CopyElement(const OptionalTuple& other) {
    if (other.has_value<kIndex>()) {
      new (&RawValue<kIndex>()) Element<kIndex>(other.RawValue<kIndex>());
    }
  }

  template <size_t... kIndices>
  constexpr void UninitializedMoveFrom(OptionalTuple& other,
                                       std::index_sequence<kIndices...>) {
    (MoveElement<kIndices>(other), ...);
    active() = other.active();
    other.active().reset();
  }

  template <size_t kIndex>
  constexpr void MoveElement(OptionalTuple& other) {
    if (other.has_value<kIndex>()) {
      new (&RawValue<kIndex>())
          Element<kIndex>(std::move(other.RawValue<kIndex>()));
      std::destroy_at(&other.RawValue<kIndex>());
    }
  }

  template <size_t kIndex>
  constexpr Element<kIndex>& RawValue() {
    static_assert(kIndex < sizeof...(Types));
    return std::get<kIndex>(tuple_).value;
  }

  template <size_t kIndex>
  constexpr const Element<kIndex>& RawValue() const {
    static_assert(kIndex < sizeof...(Types));
    return std::get<kIndex>(tuple_).value;
  }

 private:
  using Bits = BitSet<sizeof...(Types)>;

  constexpr Bits& active() { return std::get<sizeof...(Types)>(tuple_); }
  constexpr const Bits& active() const {
    return std::get<sizeof...(Types)>(tuple_);
  }

  // Include the bitset as the final tuple element for more efficient packing.
  std::tuple<containers::internal::Maybe<Types>..., Bits> tuple_;
};

/// @}

template <typename... Types>
template <typename T>
constexpr size_t OptionalTuple<Types...>::TypeToIndex() {
  if constexpr ((std::is_same_v<T, Types> + ...) == 1) {
    return containers::internal::FindFirstIndexOf<T, Types...>();
  } else {  // Use if constexpr and return 0 to clean up error messages.
    static_assert(
        (std::is_same_v<T, Types> + ...) == 1,
        "To access an item by type, the type must appear exactly once in the "
        "OptionalTuple. Specify the item's index instead.");
    return 0;
  }
}

}  // namespace pw

// Specialize std::tuple_element for pw::OptionalTuple.
template <typename First, typename... Other>
struct std::tuple_element<0, ::pw::OptionalTuple<First, Other...>> {
  using type = First;
};

template <std::size_t kIndex, typename First, typename... Other>
struct std::tuple_element<kIndex, ::pw::OptionalTuple<First, Other...>>
    : public std::tuple_element<kIndex - 1, ::pw::OptionalTuple<Other...>> {};

// Specialize std::tuple_size for pw::OptionalTuple.
template <typename... Types>
struct std::tuple_size<::pw::OptionalTuple<Types...>>
    : std::integral_constant<std::size_t, sizeof...(Types)> {};
