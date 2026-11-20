// Copyright 2026 The Pigweed Authors
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

#include <cstdint>
#include <type_traits>
#include <utility>

#include "pw_containers/internal/tag.h"
#include "pw_polyfill/language_feature_macros.h"

namespace pw::containers::internal {

template <typename... Ts>
inline constexpr bool kAllTriviallyDestructible =
    (std::is_trivially_destructible_v<Ts> && ...);

template <typename T, typename Source>
inline constexpr bool kIsConstructibleAndAssignable =
    std::is_constructible_v<T, Source> && std::is_assignable_v<T&, Source>;

template <typename T, typename Source>
inline constexpr bool kIsTriviallyReassignable =
    std::is_trivially_constructible_v<T, Source> &&
    std::is_trivially_assignable_v<T&, Source> &&
    std::is_trivially_destructible_v<T>;

// 3-state classification for special member functions:
// - `kTrivial`: defaulted (`= default`), preserving triviality.
// - `kNonTrivial`: user-provided (`ConstructFromTag` ctor / `AssignFrom`).
// - `kDeleted`: deleted (`= delete`), SFINAE-disabling the operation.
enum class SpecialMemberTrait : uint8_t {
  kTrivial,
  kNonTrivial,
  kDeleted,
};

constexpr SpecialMemberTrait SelectSpecialMemberTrait(
    bool all_available, bool all_trivial) noexcept {
  if (!all_available) {
    return SpecialMemberTrait::kDeleted;
  }
  return all_trivial ? SpecialMemberTrait::kTrivial
                     : SpecialMemberTrait::kNonTrivial;
}

template <typename... Ts>
inline constexpr SpecialMemberTrait kMoveCtorTrait = SelectSpecialMemberTrait(
    (std::is_move_constructible_v<Ts> && ...),
    (std::is_trivially_move_constructible_v<Ts> && ...));

template <typename... Ts>
inline constexpr SpecialMemberTrait kCopyCtorTrait = SelectSpecialMemberTrait(
    (std::is_copy_constructible_v<Ts> && ...),
    (std::is_trivially_copy_constructible_v<Ts> && ...));

template <typename... Ts>
inline constexpr SpecialMemberTrait kMoveAssignTrait =
    SelectSpecialMemberTrait((kIsConstructibleAndAssignable<Ts, Ts&&> && ...),
                             (kIsTriviallyReassignable<Ts, Ts&&> && ...));

template <typename... Ts>
inline constexpr SpecialMemberTrait kCopyAssignTrait = SelectSpecialMemberTrait(
    (kIsConstructibleAndAssignable<Ts, const Ts&> && ...),
    (kIsTriviallyReassignable<Ts, const Ts&> && ...));

namespace detail {

// Conditional destructor layer.
template <typename Base, bool kTrivialDtor>
class DestructLayer : public Base {
 public:
  using Base::Base;
};

template <typename Base>
class DestructLayer<Base, false> : public Base {
 public:
  using Base::Base;
  constexpr DestructLayer() = default;
  DestructLayer(const DestructLayer&) = delete;
  DestructLayer(DestructLayer&&) = delete;
  DestructLayer& operator=(const DestructLayer&) = delete;
  DestructLayer& operator=(DestructLayer&&) = delete;
  PW_CONSTEXPR_CPP20 ~DestructLayer() noexcept { this->Destroy(); }
};

// Generates a 3-state (`kTrivial` / `kNonTrivial` / `kDeleted`) special member
// base layer wrapping `Base`. The `kTrivial` primary template declares only
// `using Base::Base;` so triviality and non-const deleted signatures propagate
// unchanged to later layers.
//
// `CopyCtorLayer` omits `operator=(const&)` until `CopyAssignLayer` normalizes
// it, so types with deleted non-const copy operations (e.g. `T(T&) = delete`
// or `T& operator=(T&) = delete`) remain valid in C++17.
//
// When `kConstexpr` is `true`, non-trivial operations are marked `constexpr`
// in C++17 as well as C++20 (for `StorageBase` implementations whose
// `ConstructFromTag` constructor and `AssignFrom` are `constexpr`-compatible in
// C++17).
template <typename Base, SpecialMemberTrait kTrait, bool kConstexpr = false>
class CopyCtorLayer : public Base {
 public:
  using Base::Base;
};

#define _PW_CONTAINERS_COPY_CTOR_NONTRIVIAL(kConstexprVal, Specifier)       \
  template <typename Base>                                                  \
  class CopyCtorLayer<Base, SpecialMemberTrait::kNonTrivial, kConstexprVal> \
      : public Base {                                                       \
   public:                                                                  \
    using Base::Base;                                                       \
    constexpr CopyCtorLayer() = default;                                    \
    Specifier CopyCtorLayer(const CopyCtorLayer& other) noexcept            \
        : Base(ConstructFromTag{}, other) {}                                \
    constexpr CopyCtorLayer(CopyCtorLayer&&) = default;                     \
    constexpr CopyCtorLayer& operator=(CopyCtorLayer&&) = default;          \
  }

_PW_CONTAINERS_COPY_CTOR_NONTRIVIAL(false, PW_CONSTEXPR_CPP20);
_PW_CONTAINERS_COPY_CTOR_NONTRIVIAL(true, constexpr);
#undef _PW_CONTAINERS_COPY_CTOR_NONTRIVIAL

template <typename Base, bool kConstexpr>
class CopyCtorLayer<Base, SpecialMemberTrait::kDeleted, kConstexpr>
    : public Base {
 public:
  using Base::Base;
  constexpr CopyCtorLayer() = default;
  constexpr CopyCtorLayer(const CopyCtorLayer&) = delete;
  constexpr CopyCtorLayer(CopyCtorLayer&&) = default;
  constexpr CopyCtorLayer& operator=(CopyCtorLayer&&) = default;
};

#define _PW_CONTAINERS_SPECIAL_MEMBER_LAYER(                                   \
    Name, MoveCtor, CopyAssign, MoveAssign, BodyMacro)                         \
  template <typename Base, SpecialMemberTrait kTrait, bool kConstexpr = false> \
  class Name : public Base {                                                   \
   public:                                                                     \
    using Base::Base;                                                          \
  };                                                                           \
  template <typename Base>                                                     \
  class Name<Base, SpecialMemberTrait::kNonTrivial, false> : public Base {     \
    BodyMacro(PW_CONSTEXPR_CPP20)                                              \
  };                                                                           \
  template <typename Base>                                                     \
  class Name<Base, SpecialMemberTrait::kNonTrivial, true> : public Base {      \
    BodyMacro(constexpr)                                                       \
  };                                                                           \
  template <typename Base, bool kConstexpr>                                    \
  class Name<Base, SpecialMemberTrait::kDeleted, kConstexpr> : public Base {   \
   public:                                                                     \
    using Base::Base;                                                          \
    constexpr Name() = default;                                                \
    constexpr Name(const Name&) = default;                                     \
    constexpr Name(Name&&) = MoveCtor;                                         \
    constexpr Name& operator=(const Name&) = CopyAssign;                       \
    constexpr Name& operator=(Name&&) = MoveAssign;                            \
  }

#define _PW_CONTAINERS_COPY_ASSIGN_BODY(Specifier)             \
 public:                                                       \
  using Base::Base;                                            \
  constexpr CopyAssignLayer() = default;                       \
  constexpr CopyAssignLayer(const CopyAssignLayer&) = default; \
  constexpr CopyAssignLayer(CopyAssignLayer&&) = default;      \
  Specifier CopyAssignLayer& operator=(                        \
      const CopyAssignLayer& other) noexcept {                 \
    if (this != &other) {                                      \
      this->AssignFrom(other);                                 \
    }                                                          \
    return *this;                                              \
  }                                                            \
  constexpr CopyAssignLayer& operator=(CopyAssignLayer&&) = default;

#define _PW_CONTAINERS_MOVE_CTOR_BODY(Specifier)                      \
 public:                                                              \
  using Base::Base;                                                   \
  constexpr MoveCtorLayer() = default;                                \
  constexpr MoveCtorLayer(const MoveCtorLayer&) = default;            \
  Specifier MoveCtorLayer(MoveCtorLayer&& other) noexcept             \
      : Base(ConstructFromTag{}, std::move(other)) {}                 \
  constexpr MoveCtorLayer& operator=(const MoveCtorLayer&) = default; \
  constexpr MoveCtorLayer& operator=(MoveCtorLayer&&) = default;

#define _PW_CONTAINERS_MOVE_ASSIGN_BODY(Specifier)                         \
 public:                                                                   \
  using Base::Base;                                                        \
  constexpr MoveAssignLayer() = default;                                   \
  constexpr MoveAssignLayer(const MoveAssignLayer&) = default;             \
  constexpr MoveAssignLayer(MoveAssignLayer&&) = default;                  \
  constexpr MoveAssignLayer& operator=(const MoveAssignLayer&) = default;  \
  Specifier MoveAssignLayer& operator=(MoveAssignLayer&& other) noexcept { \
    if (this != &other) {                                                  \
      this->AssignFrom(std::move(other));                                  \
    }                                                                      \
    return *this;                                                          \
  }

_PW_CONTAINERS_SPECIAL_MEMBER_LAYER(
    CopyAssignLayer, default, delete, default, _PW_CONTAINERS_COPY_ASSIGN_BODY);

_PW_CONTAINERS_SPECIAL_MEMBER_LAYER(
    MoveCtorLayer, delete, default, default, _PW_CONTAINERS_MOVE_CTOR_BODY);

_PW_CONTAINERS_SPECIAL_MEMBER_LAYER(
    MoveAssignLayer, default, default, delete, _PW_CONTAINERS_MOVE_ASSIGN_BODY);

#undef _PW_CONTAINERS_MOVE_ASSIGN_BODY
#undef _PW_CONTAINERS_MOVE_CTOR_BODY
#undef _PW_CONTAINERS_COPY_ASSIGN_BODY
#undef _PW_CONTAINERS_SPECIAL_MEMBER_LAYER

}  // namespace detail

// Composes `detail::DestructLayer`, `detail::CopyCtorLayer`,
// `detail::CopyAssignLayer`, `detail::MoveCtorLayer`, and
// `detail::MoveAssignLayer` with explicit trait controls. Copy layers precede
// move layers so deleted non-const copy signatures normalize to `const` before
// later layers default them in C++17.
template <typename StorageBase,
          bool kTrivialDtor,
          SpecialMemberTrait kMoveCtor,
          SpecialMemberTrait kCopyCtor,
          SpecialMemberTrait kMoveAssign,
          SpecialMemberTrait kCopyAssign,
          bool kConstexpr = false>
using SpecialMembersAdapter = detail::MoveAssignLayer<
    detail::MoveCtorLayer<
        detail::CopyAssignLayer<
            detail::CopyCtorLayer<
                detail::DestructLayer<StorageBase, kTrivialDtor>,
                kCopyCtor,
                kConstexpr>,
            kCopyAssign,
            kConstexpr>,
        kMoveCtor,
        kConstexpr>,
    kMoveAssign,
    kConstexpr>;

// Standard special member selector deriving all 5 traits from `Ts...`.
template <typename StorageBase, typename... Ts>
using SelectSpecialMembers =
    SpecialMembersAdapter<StorageBase,
                          kAllTriviallyDestructible<Ts...>,
                          kMoveCtorTrait<Ts...>,
                          kCopyCtorTrait<Ts...>,
                          kMoveAssignTrait<Ts...>,
                          kCopyAssignTrait<Ts...>>;

// Standard special member selector enabling `constexpr` non-trivial copy and
// move operations in C++17 when supported by `StorageBase`.
template <typename StorageBase, typename... Ts>
using SelectConstexprSpecialMembers =
    SpecialMembersAdapter<StorageBase,
                          kAllTriviallyDestructible<Ts...>,
                          kMoveCtorTrait<Ts...>,
                          kCopyCtorTrait<Ts...>,
                          kMoveAssignTrait<Ts...>,
                          kCopyAssignTrait<Ts...>,
                          /*kConstexpr=*/true>;

}  // namespace pw::containers::internal
