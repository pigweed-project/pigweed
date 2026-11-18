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
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "lib/stdcompat/type_traits.h"
#include "pw_assert/assert.h"
#include "pw_containers/bitset.h"
#include "pw_containers/internal/maybe.h"
#include "pw_containers/internal/pack_indexing.h"
#include "pw_containers/internal/special_members.h"
#include "pw_containers/internal/tag.h"
#include "pw_polyfill/language_feature_macros.h"

namespace pw {

template <typename... Types>
class OptionalTuple;

namespace containers::internal {

template <typename T>
struct IsOptionalTuple : std::false_type {};

template <typename... Types>
struct IsOptionalTuple<OptionalTuple<Types...>> : std::true_type {};

template <size_t kIndex, typename T>
struct OptionalTupleLeaf {
  constexpr OptionalTupleLeaf() noexcept
      : element(NullPlaceholder::InternalUseOnly()) {}

  template <typename Arg>
  constexpr explicit OptionalTupleLeaf(std::in_place_t, Arg&& arg) noexcept
      : element(std::forward<Arg>(arg)) {}

  Maybe<T> element;
};

template <typename Indices, typename... Types>
class OptionalTupleStorageImpl;

template <size_t... kIndices, typename... Types>
class OptionalTupleStorageImpl<std::index_sequence<kIndices...>, Types...>
    : public OptionalTupleLeaf<kIndices, Types>... {
 public:
  using Bits = BitSet<sizeof...(Types)>;

  template <size_t kIndex>
  using ElementType = SafeNthType<kIndex, Types...>;

  constexpr OptionalTupleStorageImpl() noexcept = default;

  PW_CONSTEXPR_CPP20 explicit OptionalTupleStorageImpl(
      UninitializedTag) noexcept
      : OptionalTupleLeaf<kIndices, Types>()..., active_{} {}

  template <typename... Args>
  constexpr explicit OptionalTupleStorageImpl(std::in_place_t,
                                              Args&&... args) noexcept
      : OptionalTupleLeaf<kIndices, Types>(std::in_place,
                                           std::forward<Args>(args))...,
        active_(Bits::LittleEndian(
            !std::is_same_v<cpp20::remove_cvref_t<Args>, NullPlaceholder>...)) {
  }

  constexpr OptionalTupleStorageImpl(const OptionalTupleStorageImpl&) = default;
  constexpr OptionalTupleStorageImpl(OptionalTupleStorageImpl&&) = default;
  constexpr OptionalTupleStorageImpl& operator=(
      const OptionalTupleStorageImpl&) = default;
  constexpr OptionalTupleStorageImpl& operator=(OptionalTupleStorageImpl&&) =
      default;
  ~OptionalTupleStorageImpl() = default;

  template <size_t kIndex>
  constexpr ElementType<kIndex>& RawValue() noexcept {
    static_assert(kIndex < sizeof...(Types));
    using Leaf = OptionalTupleLeaf<kIndex, ElementType<kIndex>>;
    return static_cast<Leaf&>(*this).element.value;
  }

  template <size_t kIndex>
  constexpr const ElementType<kIndex>& RawValue() const noexcept {
    static_assert(kIndex < sizeof...(Types));
    using Leaf = OptionalTupleLeaf<kIndex, ElementType<kIndex>>;
    return static_cast<const Leaf&>(*this).element.value;
  }

  template <size_t kIndex, typename... Args>
  PW_CONSTEXPR_CPP20 ElementType<kIndex>& EmplaceElement(
      Args&&... args) noexcept {
    ResetElement<kIndex>();
    using Leaf = OptionalTupleLeaf<kIndex, ElementType<kIndex>>;
    auto& ref = static_cast<Leaf&>(*this).element.Construct(
        std::forward<Args>(args)...);
    active_.template set<kIndex>();
    return ref;
  }

  template <size_t kIndex>
  PW_CONSTEXPR_CPP20 void ResetElement() noexcept {
    if (active_.template test<kIndex>()) {
      active_.template reset<kIndex>();
      if constexpr (!std::is_trivially_destructible_v<ElementType<kIndex>>) {
        std::destroy_at(std::addressof(RawValue<kIndex>()));
      }
    }
  }

  PW_CONSTEXPR_CPP20 void Destroy() noexcept {
    (ResetElement<kIndices>(), ...);
  }

  PW_CONSTEXPR_CPP20 void ConstructFrom(
      const OptionalTupleStorageImpl& other) noexcept {
    ((other.active_.template test<kIndices>()
          ? static_cast<void>(
                EmplaceElement<kIndices>(other.RawValue<kIndices>()))
          : static_cast<void>(0)),
     ...);
  }

  PW_CONSTEXPR_CPP20 void ConstructFrom(
      OptionalTupleStorageImpl&& other) noexcept {
    ((other.active_.template test<kIndices>()
          ? (static_cast<void>(EmplaceElement<kIndices>(
                 std::move(other.RawValue<kIndices>()))),
             other.ResetElement<kIndices>())
          : static_cast<void>(0)),
     ...);
  }

  template <typename Other>
  PW_CONSTEXPR_CPP20 void AssignFrom(Other&& other) noexcept {
    Destroy();
    ConstructFrom(std::forward<Other>(other));
  }

  Bits active_{};
};

template <typename... Types>
using OptionalTupleStorageBase =
    OptionalTupleStorageImpl<std::make_index_sequence<sizeof...(Types)>,
                             Types...>;

template <typename... Types>
using OptionalTupleData = SpecialMembersAdapter<
    OptionalTupleStorageBase<Types...>,
    kAllTriviallyDestructible<Types...>,
    SelectSpecialMemberTrait((std::is_move_constructible_v<Types> && ...),
                             ((std::is_trivially_copyable_v<Types> &&
                               std::is_trivially_move_constructible_v<Types>) &&
                              ...)),
    kCopyCtorTrait<Types...>,
    SelectSpecialMemberTrait((std::is_move_constructible_v<Types> && ...),
                             ((std::is_trivially_copyable_v<Types> &&
                               kIsTriviallyReassignable<Types, Types&&>) &&
                              ...)),
    SelectSpecialMemberTrait(
        (std::is_copy_constructible_v<Types> && ...),
        (kIsTriviallyReassignable<Types, const Types&> && ...))>;

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
    containers::internal::NullPlaceholder::InternalUseOnly();

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
class OptionalTuple
    : private containers::internal::OptionalTupleData<Types...> {
  static_assert((std::is_same_v<Types, std::decay_t<Types>> && ...),
                "Types in OptionalTuple must not be references, arrays, or "
                "const/volatile qualified");

  using Base = containers::internal::OptionalTupleData<Types...>;

  template <typename T>
  static constexpr size_t GetTypeIndex() {
    constexpr size_t kIndex =
        containers::internal::kUniqueTypeIndex<T, Types...>;
    if constexpr (kIndex != containers::internal::kInvalidTypeIndex) {
      return kIndex;
    } else {
      static_assert(kIndex != containers::internal::kInvalidTypeIndex,
                    "The type must appear exactly once in the OptionalTuple "
                    "type parameter pack to look up elements by type");
      return 0;
    }
  }

  template <size_t kIndex>
  using ElementType = containers::internal::SafeNthType<kIndex, Types...>;

  template <typename T>
  using ElementByType = ElementType<GetTypeIndex<T>()>;

 public:
  /// Default constructs an `OptionalTuple` with all elements unset.
  constexpr OptionalTuple() = default;

  /// Constructs an `OptionalTuple`, forwarding each argument to its
  /// corresponding element. Pass `pw::kTupleNull` to skip initializing
  /// an element.
  template <
      typename... Args,
      typename = std::enable_if_t<
          (sizeof...(Args) == sizeof...(Types)) && (sizeof...(Types) != 0) &&
          (std::is_constructible_v<containers::internal::Maybe<Types>, Args> &&
           ...) &&
          (sizeof...(Types) != 1 || (!containers::internal::IsOptionalTuple<
                                         cpp20::remove_cvref_t<Args>>::value &&
                                     ...))>>
  constexpr OptionalTuple(Args&&... args)
      : Base(std::in_place, std::forward<Args>(args)...) {}

  constexpr OptionalTuple(const OptionalTuple&) = default;
  constexpr OptionalTuple(OptionalTuple&&) = default;
  constexpr OptionalTuple& operator=(const OptionalTuple&) = default;
  constexpr OptionalTuple& operator=(OptionalTuple&&) = default;
  ~OptionalTuple() = default;

  /// Returns the total number elements in the `OptionalTuple`. Equivalent to
  /// `std::tuple_size<>`.
  [[nodiscard]] constexpr size_t size() const noexcept {
    return sizeof...(Types);
  }

  /// Returns the number of active elements in the `OptionalTuple`.
  [[nodiscard]] constexpr size_t count() const noexcept {
    return this->active_.count();
  }

  /// Checks if the `OptionalTuple` contains no active elements.
  [[nodiscard]] constexpr bool empty() const noexcept {
    return this->active_.none();
  }

  /// Checks if the element at `kIndex` has a value.
  template <size_t kIndex>
  [[nodiscard]] constexpr bool has_value() const noexcept {
    return this->active_.template test<kIndex>();
  }

  /// Checks if the element of type `T` has a value.
  template <typename T>
  [[nodiscard]] constexpr bool has_value() const noexcept {
    return has_value<GetTypeIndex<T>()>();
  }

  /// Returns a reference to the element at `kIndex`.
  /// @pre `has_value<kIndex>` must be true.
  template <size_t kIndex>
  constexpr ElementType<kIndex>& value() & {
    PW_ASSERT(has_value<kIndex>());
    return this->template RawValue<kIndex>();
  }

  /// Returns a const reference to the element at `kIndex`.
  /// @pre `has_value<kIndex>` must be true.
  template <size_t kIndex>
  constexpr const ElementType<kIndex>& value() const& {
    PW_ASSERT(has_value<kIndex>());
    return this->template RawValue<kIndex>();
  }

  /// Returns an rvalue reference to the element at `kIndex`.
  /// @pre `has_value<kIndex>` must be true.
  template <size_t kIndex>
  constexpr ElementType<kIndex>&& value() && {
    PW_ASSERT(has_value<kIndex>());
    return std::move(this->template RawValue<kIndex>());
  }

  /// Returns a const rvalue reference to the element at `kIndex`.
  /// @pre `has_value<kIndex>` must be true.
  template <size_t kIndex>
  constexpr const ElementType<kIndex>&& value() const&& {
    PW_ASSERT(has_value<kIndex>());
    return std::move(this->template RawValue<kIndex>());
  }

  /// Returns a reference to the element of type `T`.
  /// @pre `has_value<T>` must be true.
  template <typename T>
  constexpr ElementByType<T>& value() & {
    return value<GetTypeIndex<T>()>();
  }

  /// Returns a const reference to the element of type `T`.
  /// @pre `has_value<T>` must be true.
  template <typename T>
  constexpr const ElementByType<T>& value() const& {
    return value<GetTypeIndex<T>()>();
  }

  /// Returns an rvalue reference to the element of type `T`.
  /// @pre `has_value<T>` must be true.
  template <typename T>
  constexpr ElementByType<T>&& value() && {
    return std::move(*this).template value<GetTypeIndex<T>()>();
  }

  /// Returns a const rvalue reference to the element of type `T`.
  /// @pre `has_value<T>` must be true.
  template <typename T>
  constexpr const ElementByType<T>&& value() const&& {
    return std::move(*this).template value<GetTypeIndex<T>()>();
  }

  /// Returns the element at the specified index, if present. Otherwise, returns
  /// `default_value`.
  template <size_t kIndex, int&... kExplicitGuard, typename U>
  constexpr ElementType<kIndex> value_or(U&& default_value) const& {
    return has_value<kIndex>() ? this->template RawValue<kIndex>()
                               : static_cast<ElementType<kIndex>>(
                                     std::forward<U>(default_value));
  }

  /// Moves and returns the element at the specified index, if present.
  /// Otherwise, returns `default_value`.
  template <size_t kIndex, int&... kExplicitGuard, typename U>
  constexpr ElementType<kIndex> value_or(U&& default_value) && {
    return has_value<kIndex>() ? std::move(this->template RawValue<kIndex>())
                               : static_cast<ElementType<kIndex>>(
                                     std::forward<U>(default_value));
  }

  /// Returns the element with the specified type, if present. Otherwise,
  /// returns `default_value`.
  template <typename T, int&... kExplicitGuard, typename U>
  constexpr ElementByType<T> value_or(U&& default_value) const& {
    return value_or<GetTypeIndex<T>()>(std::forward<U>(default_value));
  }

  /// Moves and returns the element with the specified type, if present.
  /// Otherwise, returns `default_value`.
  template <typename T, int&... kExplicitGuard, typename U>
  constexpr ElementByType<T> value_or(U&& default_value) && {
    return std::move(*this).template value_or<GetTypeIndex<T>()>(
        std::forward<U>(default_value));
  }

  /// Constructs an element in place and marks it active. Destroys the previous
  /// value, if any.
  ///
  /// @returns A reference to the newly initialized item.
  template <size_t kIndex, int&... kExplicitGuard, typename... Args>
  PW_CONSTEXPR_CPP20 ElementType<kIndex>& emplace(Args&&... args) {
    return this->template EmplaceElement<kIndex>(std::forward<Args>(args)...);
  }

  /// Constructs an element of type `T` in place and marks it active. Destroys
  /// the previous value, if any.
  ///
  /// @returns A reference to the newly initialized item.
  template <typename T, int&... kExplicitGuard, typename... Args>
  PW_CONSTEXPR_CPP20 ElementByType<T>& emplace(Args&&... args) {
    return emplace<GetTypeIndex<T>()>(std::forward<Args>(args)...);
  }

  /// Resets (clears) the value at the specified index, if any.
  template <size_t kIndex>
  PW_CONSTEXPR_CPP20 void reset() {
    this->template ResetElement<kIndex>();
  }

  /// Resets (clears) the value of type `T`, if any.
  template <typename T>
  PW_CONSTEXPR_CPP20 void reset() {
    reset<GetTypeIndex<T>()>();
  }
};

/// @}

}  // namespace pw

namespace std {

template <typename... Types>
struct tuple_size<pw::OptionalTuple<Types...>>
    : std::integral_constant<size_t, sizeof...(Types)> {};

template <size_t kIndex, typename... Types>
struct tuple_element<kIndex, pw::OptionalTuple<Types...>> {
  static_assert(kIndex < sizeof...(Types), "OptionalTuple index out of bounds");
  using type = pw::containers::internal::SafeNthType<kIndex, Types...>;
};

}  // namespace std
