// Copyright 2023 The Pigweed Authors
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

#include <cstdlib>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace pw::internal {

// Helper type trait for removing reference and cv-qualification of a type.
template <class T>
struct remove_cvref {
  typedef std::remove_cv_t<std::remove_reference_t<T>> type;
};

// Helper type trait ::type of above struct.
template <class T>
using remove_cvref_t = typename remove_cvref<T>::type;

// Helper type trait for enabling/disabling std::expected constructors.
template <class T, class W>
constexpr bool converts_from_any_cvref =
    std::disjunction_v<std::is_constructible<T, W&>,
                       std::is_convertible<W&, T>,
                       std::is_constructible<T, W>,
                       std::is_convertible<W, T>,
                       std::is_constructible<T, const W&>,
                       std::is_convertible<const W&, T>,
                       std::is_constructible<T, const W>,
                       std::is_convertible<const W, T>>;

// Helper type trait for determining if a type is any specialization of a
// template class.
template <typename T, template <typename...> class Template>
constexpr bool is_specialization = false;
template <template <typename...> class Template, typename... Ts>
constexpr bool is_specialization<Template<Ts...>, Template> = true;

// Polyfill implementaion of std::unexpected.

template <class E>
class unexpected {
 public:
  constexpr unexpected(const unexpected&) = default;
  constexpr unexpected(unexpected&&) = default;
  template <class Err = E,
            std::enable_if_t<
                !std::is_same_v<remove_cvref_t<Err>, unexpected> &&
                    !std::is_same_v<remove_cvref_t<Err>, std::in_place_t> &&
                    std::is_constructible_v<E, Err>,
                bool> = true>
  constexpr explicit unexpected(Err&& e) : unex_(std::forward<Err>(e)) {}
  template <class... Args,
            std::enable_if_t<std::is_constructible_v<E, Args...>, bool> = true>
  constexpr explicit unexpected(std::in_place_t, Args&&... args)
      : unex_(std::forward<Args>(args)...) {}
  template <
      class U,
      class... Args,
      std::enable_if_t<std::is_constructible_v<E, std::initializer_list<U>>,
                       bool> = true>
  constexpr explicit unexpected(std::in_place_t,
                                std::initializer_list<U> il,
                                Args&&... args)
      : unex_(il, std::forward<Args>(args)...) {}

  constexpr unexpected& operator=(const unexpected&) = default;
  constexpr unexpected& operator=(unexpected&&) = default;

  constexpr const E& error() const& noexcept { return unex_; }
  constexpr E& error() & noexcept { return unex_; }
  constexpr E&& error() && noexcept { return std::move(unex_); }
  constexpr const E&& error() const&& noexcept { return std::move(unex_); }

  constexpr void swap(unexpected& other) noexcept(
      std::is_nothrow_swappable<E>::value) {
    std::swap(unex_, other.unex_);
  }

  template <class E2>
  friend constexpr bool operator==(const unexpected& x,
                                   const unexpected<E2>& y) {
    return x.error() == y.error();
  }

 private:
  E unex_;
};

template <class E>
unexpected(E) -> unexpected<E>;

// Polyfill implementation of std::unexpect_t and std::unexpect.
struct unexpect_t {
  explicit unexpect_t() = default;
};

inline constexpr unexpect_t unexpect;

template <class T, class E, typename Enable = void>
class expected;

// Polyfill implementation of std::expected.
template <class T, class E>
class expected<T, E, std::enable_if_t<!std::is_void_v<T>>> {
 public:
  using value_type = T;
  using error_type = E;
  using unexpected_type = unexpected<E>;

  template <class U>
  using rebind = expected<U, error_type>;

  template <
      class Enable = T,
      std::enable_if_t<std::is_default_constructible_v<Enable>, bool> = true>
  constexpr expected() : contents_(kInPlaceValue) {}
  constexpr expected(const expected& rhs) = default;
  constexpr expected(expected&& rhs) = default;
  template <
      class U,
      class G,
      // Constraints
      std::enable_if_t<
          std::is_constructible_v<T, const U&> &&
              std::is_constructible_v<E, const G&> &&
              !converts_from_any_cvref<T, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>>,
          bool> = true,
      // Explicit
      std::enable_if_t<!std::is_convertible_v<const U&, T> ||
                           !std::is_convertible_v<const G&, E>,
                       bool> = true>
  constexpr explicit expected(const expected<U, G>& rhs)
      : contents_(convert_variant(
            std::forward<const std::variant<U, G>&>(rhs.contents_))) {}
  template <
      class U,
      class G,
      // Constraints
      std::enable_if_t<
          std::is_constructible_v<T, const U&> &&
              std::is_constructible_v<E, const G&> &&
              !converts_from_any_cvref<T, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>>,
          bool> = true,
      // Explicit
      std::enable_if_t<std::is_convertible_v<const U&, T> &&
                           std::is_convertible_v<const G&, E>,
                       bool> = true>
  constexpr /* implicit */ expected(const expected<U, G>& rhs)
      : contents_(convert_variant(
            std::forward<const std::variant<U, G>&>(rhs.contents_))) {}
  template <
      class U,
      class G,
      // Constraints
      std::enable_if_t<
          std::is_constructible_v<T, U&&> && std::is_constructible_v<E, G&&> &&
              !converts_from_any_cvref<T, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>>,
          bool> = true,
      // Explicit
      std::enable_if_t<!std::is_convertible_v<U&&, T> ||
                           !std::is_convertible_v<G&&, E>,
                       bool> = true>
  constexpr explicit expected(expected<U, G>&& rhs)
      : contents_(
            std::forward<std::variant<U, G>>(convert_variant(rhs.contents_))) {}
  template <
      class U,
      class G,
      // Constraints
      std::enable_if_t<
          std::is_constructible_v<T, U&&> && std::is_constructible_v<E, G&&> &&
              !converts_from_any_cvref<T, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>>,
          bool> = true,
      // Explicit
      std::enable_if_t<std::is_convertible_v<U&&, T> &&
                           std::is_convertible_v<G&&, E>,
                       bool> = true>
  constexpr /* implicit */ expected(expected<U, G>&& rhs)
      : contents_(
            convert_variant(std::forward<std::variant<U, G>>(rhs.contents_))) {}
  template <
      class U = T,
      // Constraints
      std::enable_if_t<!std::is_same_v<remove_cvref_t<U>, std::in_place_t> &&
                           !std::is_same_v<remove_cvref_t<U>, expected> &&
                           !is_specialization<U, unexpected> &&
                           std::is_constructible_v<T, U>,
                       bool> = true,
      // Explicit
      std::enable_if_t<!std::is_convertible_v<U, T>, bool> = true>
  constexpr explicit expected(U&& u)
      : contents_(kInPlaceValue, std::forward<U>(u)) {}
  template <
      class U = T,
      // Constraints
      std::enable_if_t<!std::is_same_v<remove_cvref_t<U>, std::in_place_t> &&
                           !std::is_same_v<remove_cvref_t<U>, expected> &&
                           !is_specialization<U, unexpected> &&
                           std::is_constructible_v<T, U>,
                       bool> = true,
      // Explicit
      std::enable_if_t<std::is_convertible_v<U, T>, bool> = true>
  constexpr /* implicit */ expected(U&& u)
      : contents_(kInPlaceValue, std::forward<U>(u)) {}
  template <class G,
            // Constraints
            std::enable_if_t<std::is_constructible_v<E, const G&>, bool> = true,
            // Explicit
            std::enable_if_t<!std::is_convertible_v<const G&, E>, bool> = true>
  constexpr explicit expected(const unexpected<G>& e)
      : contents_(kInPlaceError, std::forward<const G&>(e.error())) {}
  template <class G,
            // Constraints
            std::enable_if_t<std::is_constructible_v<E, const G&>, bool> = true,
            // Explicit
            std::enable_if_t<std::is_convertible_v<const G&, E>, bool> = true>
  constexpr /* implicit */ expected(const unexpected<G>& e)
      : contents_(kInPlaceError, std::forward<const G&>(e.error())) {}
  template <class G,
            // Constraints
            std::enable_if_t<std::is_constructible_v<E, G>, bool> = true,
            // Explicit
            std::enable_if_t<!std::is_convertible_v<G, E>, bool> = true>
  constexpr explicit expected(unexpected<G>&& e)
      : contents_(kInPlaceError, std::forward<G>(e.error())) {}
  template <class G,
            // Constraints
            std::enable_if_t<std::is_constructible_v<E, G>, bool> = true,
            // Explicit
            std::enable_if_t<std::is_convertible_v<G, E>, bool> = true>
  constexpr /* implicit */ expected(unexpected<G>&& e)
      : contents_(kInPlaceError, std::forward<G>(e.error())) {}
  template <class... Args,
            std::enable_if_t<std::is_constructible_v<T, Args...>, bool> = true>
  constexpr explicit expected(std::in_place_t, Args&&... args)
      : contents_(kInPlaceValue, std::forward<Args>(args)...) {}
  template <class U,
            class... Args,
            std::enable_if_t<
                std::is_constructible_v<T, std::initializer_list<U>&, Args...>,
                bool> = true>
  constexpr explicit expected(std::in_place_t,
                              std::initializer_list<U> il,
                              Args&&... args)
      : contents_(kInPlaceValue, il, std::forward<Args>(args)...) {}
  template <class... Args,
            std::enable_if_t<std::is_constructible_v<E, Args...>, bool> = true>
  constexpr explicit expected(unexpect_t, Args&&... args)
      : contents_(kInPlaceError, std::forward<Args>(args)...) {}
  template <class U,
            class... Args,
            std::enable_if_t<
                std::is_constructible_v<E, std::initializer_list<U>&, Args...>,
                bool> = true>
  constexpr explicit expected(unexpect_t,
                              std::initializer_list<U> il,
                              Args&&... args)
      : contents_(kInPlaceError, il, std::forward<Args>(args)...) {}

  constexpr expected& operator=(const expected& rhs) = default;
  constexpr expected& operator=(expected&& rhs) = default;
  template <
      class U = T,
      std::enable_if_t<!std::is_same_v<expected, remove_cvref_t<U>> &&
                           !is_specialization<remove_cvref_t<U>, unexpected>,
                       bool> = true>
  constexpr expected& operator=(U&& u) {
    value() = std::forward<U>(u);
  }
  template <class G>
  constexpr expected& operator=(const unexpected<G>& e) {
    error() = std::forward<const G&>(e.error());
  }
  template <class G>
  constexpr expected& operator=(unexpected<G>&& e) {
    error() = std::forward<G>(e.error());
  }

  template <class... Args,
            std::enable_if_t<std::is_nothrow_constructible_v<T, Args...>,
                             bool> = true>
  constexpr T& emplace(Args&&... args) noexcept {
    return contents_.template emplace<kValue>(std::forward<Args>(args)...);
  }
  template <
      class U,
      class... Args,
      std::enable_if_t<
          std::is_nothrow_constructible_v<T, std::initializer_list<U>, Args...>,
          bool> = true>
  constexpr T& emplace(std::initializer_list<U> il, Args&&... args) noexcept {
    return contents_.template emplace<kValue>(il, std::forward<Args>(args)...);
  }

  constexpr void swap(expected& rhs) { std::swap(contents_, rhs.contents_); }

  constexpr T* operator->() noexcept { return std::addressof(value()); }
  constexpr const T* operator->() const noexcept {
    return std::addressof(value());
  }
  constexpr T& operator*() & noexcept { return value(); }
  constexpr T&& operator*() && noexcept { return std::move(value()); }
  constexpr const T& operator*() const& noexcept { return value(); }
  constexpr const T&& operator*() const&& noexcept {
    return std::move(value());
  }

  constexpr explicit operator bool() const noexcept { return has_value(); }
  constexpr bool has_value() const noexcept {
    return contents_.index() == kValue;
  }

  constexpr T& value() & { return std::get<kValue>(contents_); }
  constexpr T&& value() && { return std::move(std::get<kValue>(contents_)); }
  constexpr const T& value() const& { return std::get<kValue>(contents_); }
  constexpr const T&& value() const&& {
    return std::move(std::get<kValue>(contents_));
  }

  constexpr E& error() & { return std::get<kError>(contents_); }
  constexpr E&& error() && { return std::move(std::get<kError>(contents_)); }
  constexpr const E& error() const& { return std::get<kError>(contents_); }
  constexpr const E&& error() const&& {
    return std::move(std::get<kError>(contents_));
  }

  template <class U>
  constexpr T value_or(U&& u) && {
    return has_value() ? std::move(value())
                       : static_cast<T>(std::forward<U>(u));
  }
  template <class U>
  constexpr T value_or(U&& u) const& {
    return has_value() ? value() : static_cast<T>(std::forward<U>(u));
  }

  template <class G>
  constexpr E error_or(G&& g) && {
    return has_value() ? std::forward<G>(g) : std::move(error());
  }
  template <class G>
  constexpr E error_or(G&& g) const& {
    return has_value() ? std::forward<G>(g) : error();
  }

  template <class F>
  constexpr auto and_then(F&& f) & {
    using U = remove_cvref_t<std::invoke_result_t<F, decltype(value())>>;
    static_assert(std::is_same_v<typename U::error_type, error_type>);
    if (has_value()) {
      return std::invoke(std::forward<F>(f), value());
    } else {
      return U(unexpect, error());
    }
  }
  template <class F>
  constexpr auto and_then(F&& f) && {
    using U = remove_cvref_t<std::invoke_result_t<F, decltype(value())>>;
    static_assert(std::is_same_v<typename U::error_type, error_type>);
    if (has_value()) {
      return std::invoke(std::forward<F>(f), std::move(value()));
    } else {
      return U(unexpect, std::move(error()));
    }
  }
  template <class F>
  constexpr auto and_then(F&& f) const& {
    using U = remove_cvref_t<std::invoke_result_t<F, decltype(value())>>;
    static_assert(std::is_same_v<typename U::error_type, error_type>);
    if (has_value()) {
      return std::invoke(std::forward<F>(f), value());
    } else {
      return U(unexpect, error());
    }
  }
  template <class F>
  constexpr auto and_then(F&& f) const&& {
    using U = remove_cvref_t<std::invoke_result_t<F, decltype(value())>>;
    static_assert(std::is_same_v<typename U::error_type, error_type>);
    if (has_value()) {
      return std::invoke(std::forward<F>(f), std::move(value()));
    } else {
      return U(unexpect, std::move(error()));
    }
  }

  template <class F>
  constexpr auto or_else(F&& f) & {
    using G = remove_cvref_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return G(std::in_place, value());
    } else {
      return std::invoke(std::forward<F>(f), error());
    }
  }
  template <class F>
  constexpr auto or_else(F&& f) && {
    using G = remove_cvref_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return G(std::in_place, value());
    } else {
      return std::invoke(std::forward<F>(f), std::move(error()));
    }
  }
  template <class F>
  constexpr auto or_else(F&& f) const& {
    using G = remove_cvref_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return G(std::in_place, value());
    } else {
      return std::invoke(std::forward<F>(f), error());
    }
  }
  template <class F>
  constexpr auto or_else(F&& f) const&& {
    using G = remove_cvref_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return G(std::in_place, value());
    } else {
      return std::invoke(std::forward<F>(f), std::move(error()));
    }
  }

  template <class F>
  constexpr auto transform(F&& f) & {
    using U = std::remove_cv_t<std::invoke_result_t<F, decltype(value())>>;
    if (has_value()) {
      return transform_helper<U>(std::forward<F>(f));
    } else {
      return expected<U, E>(unexpect, error());
    }
  }
  template <class F>
  constexpr auto transform(F&& f) && {
    using U = std::remove_cv_t<std::invoke_result_t<F, decltype(value())>>;
    if (has_value()) {
      return transform_helper<U>(std::forward<F>(f));
    } else {
      return expected<U, E>(unexpect, std::move(error()));
    }
  }
  template <class F>
  constexpr auto transform(F&& f) const& {
    using U = std::remove_cv_t<std::invoke_result_t<F, decltype(value())>>;
    if (has_value()) {
      return transform_helper<U>(std::forward<F>(f));
    } else {
      return expected<U, E>(unexpect, error());
    }
  }
  template <class F>
  constexpr auto transform(F&& f) const&& {
    using U = std::remove_cv_t<std::invoke_result_t<F, decltype(value())>>;
    if (has_value()) {
      return transform_helper<U>(std::forward<F>(f));
    } else {
      return expected<U, E>(unexpect, std::move(error()));
    }
  }

  template <class F>
  constexpr auto transform_error(F&& f) & {
    using G = std::remove_cv_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return expected<T, G>(std::in_place, value());
    } else {
      return expected<T, G>(unexpect, std::invoke(std::forward<F>(f), error()));
    }
  }
  template <class F>
  constexpr auto transform_error(F&& f) && {
    using G = std::remove_cv_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return expected<T, G>(std::in_place, std::move(value()));
    } else {
      return expected<T, G>(
          unexpect, std::invoke(std::forward<F>(f), std::move(error())));
    }
  }
  template <class F>
  constexpr auto transform_error(F&& f) const& {
    using G = std::remove_cv_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return expected<T, G>(std::in_place, value());
    } else {
      return expected<T, G>(unexpect, std::invoke(std::forward<F>(f), error()));
    }
  }
  template <class F>
  constexpr auto transform_error(F&& f) const&& {
    using G = std::remove_cv_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return expected<T, G>(std::in_place, std::move(value()));
    } else {
      return expected<T, G>(
          unexpect, std::invoke(std::forward<F>(f), std::move(error())));
    }
  }

 private:
  // Make all specializations of `expected` friends.
  template <class U, class G, class>
  friend class expected;

  static constexpr size_t kValue = 0;
  static constexpr size_t kError = 1;

  static constexpr auto kInPlaceValue = std::in_place_index<kValue>;
  static constexpr auto kInPlaceError = std::in_place_index<kError>;

  // Helper to convert variant<U, G> -> variant<T, E>
  template <class U, class G>
  std::variant<T, E> convert_variant(const std::variant<U, G>& v) {
    switch (v.index()) {
      case kValue:
        return std::variant<T, E>(kInPlaceValue, std::get<kValue>(v));
      case kError:
        return std::variant<T, E>(kInPlaceError, std::get<kError>(v));
      default:
        // Could only happen if valueless_by_exception, which can't be handled
        // gracefully anyways.
        std::abort();
    }
  }

  // Helper to convert variant<U, G> -> variant<T, E>
  template <class U, class G>
  std::variant<T, E> convert_variant(std::variant<U, G>&& v) {
    switch (v.index()) {
      case kValue:
        return std::variant<T, E>(kInPlaceValue,
                                  std::forward<U>(std::get<kValue>(v)));
      case kError:
        return std::variant<T, E>(kInPlaceError,
                                  std::forward<G>(std::get<kError>(v)));
      default:
        // Could only happen if valueless_by_exception, which can't be handled
        // gracefully anyways.
        std::abort();
    }
  }

  // Helper to handle transform correctly for void(Args...) functions, non-void
  // case.
  template <class U, class F, std::enable_if_t<!std::is_void_v<U>, bool> = true>
  expected<U, E> transform_helper(F&& f) & {
    return expected<U, E>(std::in_place,
                          std::invoke(std::forward<F>(f), value()));
  }
  template <class U, class F, std::enable_if_t<!std::is_void_v<U>, bool> = true>
  expected<U, E> transform_helper(F&& f) && {
    return expected<U, E>(std::in_place,
                          std::invoke(std::forward<F>(f), std::move(value())));
  }
  template <class U, class F, std::enable_if_t<!std::is_void_v<U>, bool> = true>
  expected<U, E> transform_helper(F&& f) const& {
    return expected<U, E>(std::in_place,
                          std::invoke(std::forward<F>(f), value()));
  }
  template <class U, class F, std::enable_if_t<!std::is_void_v<U>, bool> = true>
  expected<U, E> transform_helper(F&& f) const&& {
    return expected<U, E>(std::in_place,
                          std::invoke(std::forward<F>(f), std::move(value())));
  }

  // Helper to handle transform correctly for void(Args...) functions, void
  // case.
  template <class U, class F, std::enable_if_t<std::is_void_v<U>, bool> = true>
  expected<U, E> transform_helper(F&& f) & {
    std::invoke(std::forward<F>(f), value());
    return expected<U, E>();
  }
  template <class U, class F, std::enable_if_t<std::is_void_v<U>, bool> = true>
  expected<U, E> transform_helper(F&& f) && {
    std::invoke(std::forward<F>(f), std::move(value()));
    return expected<U, E>();
  }
  template <class U, class F, std::enable_if_t<std::is_void_v<U>, bool> = true>
  expected<U, E> transform_helper(F&& f) const& {
    std::invoke(std::forward<F>(f), value());
    return expected<U, E>();
  }
  template <class U, class F, std::enable_if_t<std::is_void_v<U>, bool> = true>
  expected<U, E> transform_helper(F&& f) const&& {
    std::invoke(std::forward<F>(f), std::move(value()));
    return expected<U, E>();
  }

  std::variant<T, E> contents_;
};

template <class T,
          class E,
          class U,
          class G,
          std::enable_if_t<!std::is_void_v<U>, bool> = true>
constexpr bool operator==(const expected<T, E>& lhs,
                          const expected<U, G>& rhs) {
  if (lhs.has_value() != rhs.has_value()) {
    return false;
  }
  if (lhs.has_value()) {
    return lhs.value() == rhs.value();
  } else {
    return lhs.error() == rhs.error();
  }
}

template <class T, class E, class U>
constexpr bool operator==(const expected<T, E>& x, const U& u) {
  return x.has_value() && static_cast<bool>(*x == u);
}

template <class T, class E, class G>
constexpr bool operator==(const expected<T, E>& x, const unexpected<G> e) {
  return !x.has_value() && static_cast<bool>(x.error() == e.error());
}

// Polyfill implementation of std::expected<void, ...>
template <class T, class E>
class expected<T, E, std::enable_if_t<std::is_void_v<T>>> {
 public:
  using value_type = T;
  using error_type = E;
  using unexpected_type = unexpected<E>;

  template <class U>
  using rebind = expected<U, error_type>;

  constexpr expected() noexcept : error_contents_(std::nullopt) {}
  constexpr expected(const expected& rhs) = default;
  constexpr expected(expected&& rhs) = default;
  template <
      class U,
      class G,
      // Constraints
      std::enable_if_t<
          std::is_void_v<U> && std::is_constructible_v<E, const G&> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>>,
          bool> = true,
      // Explicit
      std::enable_if_t<std::is_convertible_v<const G&, E>, bool> = true>
  constexpr /* implicit */ expected(const expected<U, G>& rhs)
      : error_contents_(rhs.error_contents_.has_value() ? rhs.error_contents_
                                                        : std::nullopt) {}
  template <
      class U,
      class G,
      // Constraints
      std::enable_if_t<
          std::is_void_v<U> && std::is_constructible_v<E, const G&> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>>,
          bool> = true,
      // Explicit
      std::enable_if_t<!std::is_convertible_v<const G&, E>, bool> = true>
  constexpr explicit expected(const expected<U, G>& rhs)
      : error_contents_(rhs.error_contents_.has_value() ? rhs.error_contents_
                                                        : std::nullopt) {}
  template <
      class U,
      class G,
      // Constraints
      std::enable_if_t<
          std::is_void_v<U> && std::is_constructible_v<E, G> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>>,
          bool> = true,
      // Explicit
      std::enable_if_t<std::is_convertible_v<G, E>, bool> = true>
  constexpr /* implicit */ expected(expected<U, G>&& rhs)
      : error_contents_(rhs.error_contents_.has_value()
                            ? std::move(rhs.error_contents_)
                            : std::nullopt) {}
  template <
      class U,
      class G,
      // Constraints
      std::enable_if_t<
          std::is_void_v<U> && std::is_constructible_v<E, G> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, expected<U, G>> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>&> &&
              !std::is_constructible_v<unexpected<E>, const expected<U, G>>,
          bool> = true,
      // Explicit
      std::enable_if_t<!std::is_convertible_v<G, E>, bool> = true>
  constexpr explicit expected(expected<U, G>&& rhs)
      : error_contents_(rhs.error_contents_.has_value()
                            ? std::move(rhs.error_contents_)
                            : std::nullopt) {}
  template <class G,
            // Constraints
            std::enable_if_t<std::is_constructible_v<E, const G&>, bool> = true,
            // Explicit
            std::enable_if_t<!std::is_convertible_v<const G&, E>, bool> = true>
  constexpr explicit expected(const unexpected<G>& e)
      : error_contents_(std::in_place, std::forward<const G&>(e.error())) {}
  template <class G,
            // Constraints
            std::enable_if_t<std::is_constructible_v<E, const G&>, bool> = true,
            // Explicit
            std::enable_if_t<std::is_convertible_v<const G&, E>, bool> = true>
  constexpr /* implicit */ expected(const unexpected<G>& e)
      : error_contents_(std::in_place, std::forward<const G&>(e.error())) {}
  template <class... Args,
            std::enable_if_t<std::is_constructible_v<E, Args...>, bool> = true>
  constexpr explicit expected(unexpect_t, Args&&... args)
      : error_contents_(std::in_place, std::forward<Args>(args)...) {}
  template <class U,
            class... Args,
            std::enable_if_t<
                std::is_constructible_v<E, std::initializer_list<U>&, Args...>,
                bool> = true>
  constexpr explicit expected(unexpect_t,
                              std::initializer_list<U> il,
                              Args&&... args)
      : error_contents_(std::in_place, il, std::forward<Args>(args)...) {}
  template <class G,
            // Constraints
            std::enable_if_t<std::is_constructible_v<E, G>, bool> = true,
            // Explicit
            std::enable_if_t<!std::is_convertible_v<G, E>, bool> = true>
  constexpr explicit expected(unexpected<G>&& e)
      : error_contents_(std::in_place, std::forward<G>(e.error())) {}
  template <class G,
            // Constraints
            std::enable_if_t<std::is_constructible_v<E, G>, bool> = true,
            // Explicit
            std::enable_if_t<std::is_convertible_v<G, E>, bool> = true>
  constexpr /* implicit */ expected(unexpected<G>&& e)
      : error_contents_(std::in_place, std::forward<G>(e.error())) {}

  constexpr expected& operator=(const expected& rhs) {
    error_contents_ = rhs.error_contents_;
    return *this;
  }
  constexpr expected& operator=(expected&& rhs) noexcept(
      std::is_nothrow_move_constructible_v<E> &&
      std::is_nothrow_move_assignable_v<E>) {
    error_contents_ = std::move(rhs.error_contents_);
    return *this;
  }
  template <class G>
  constexpr expected& operator=(const unexpected<G>& rhs) {
    error_contents_ = rhs.error();
    return *this;
  }
  template <class G>
  constexpr expected& operator=(unexpected<G>&& rhs) {
    error_contents_ = std::move(rhs.error());
    return *this;
  }

  constexpr void swap(expected& rhs) {
    error_contents_.swap(rhs.error_contents_);
  }

  constexpr explicit operator bool() const noexcept { return has_value(); }
  constexpr bool has_value() const noexcept {
    return !error_contents_.has_value();
  }

  constexpr void operator*() const noexcept {}
  constexpr void value() const& {}
  constexpr void value() && {}

  constexpr E& error() & { return *error_contents_; }
  constexpr E&& error() && { return std::move(*error_contents_); }
  constexpr const E& error() const& { return *error_contents_; }
  constexpr const E&& error() const&& { return std::move(*error_contents_); }

  template <class G = E>
  constexpr E error_or(G&& g) const& {
    if (has_value()) {
      return std::forward<G>(g);
    } else {
      return error();
    }
  }
  template <class G = E>
  constexpr E error_or(G&& g) const&& {
    if (has_value()) {
      return std::forward<G>(g);
    } else {
      return std::move(error());
    }
  }

  template <class F>
  constexpr auto and_then(F&& f) & {
    using U = remove_cvref_t<std::invoke_result_t<F>>;
    static_assert(std::is_same_v<typename U::error_type, error_type>);
    if (has_value()) {
      return std::invoke(std::forward<F>(f));
    } else {
      return U(unexpect, error());
    }
  }
  template <class F>
  constexpr auto and_then(F&& f) && {
    using U = remove_cvref_t<std::invoke_result_t<F>>;
    static_assert(std::is_same_v<typename U::error_type, error_type>);
    if (has_value()) {
      return std::invoke(std::forward<F>(f));
    } else {
      return U(unexpect, std::move(error()));
    }
  }
  template <class F>
  constexpr auto and_then(F&& f) const& {
    using U = remove_cvref_t<std::invoke_result_t<F>>;
    static_assert(std::is_same_v<typename U::error_type, error_type>);
    if (has_value()) {
      return std::invoke(std::forward<F>(f));
    } else {
      return U(unexpect, error());
    }
  }
  template <class F>
  constexpr auto and_then(F&& f) const&& {
    using U = remove_cvref_t<std::invoke_result_t<F>>;
    static_assert(std::is_same_v<typename U::error_type, error_type>);
    if (has_value()) {
      return std::invoke(std::forward<F>(f));
    } else {
      return U(unexpect, std::move(error()));
    }
  }

  template <class F>
  constexpr auto or_else(F&& f) & {
    using G = remove_cvref_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return G();
    } else {
      return std::invoke(std::forward<F>(f), error());
    }
  }
  template <class F>
  constexpr auto or_else(F&& f) && {
    using G = remove_cvref_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return G();
    } else {
      return std::invoke(std::forward<F>(f), std::move(error()));
    }
  }
  template <class F>
  constexpr auto or_else(F&& f) const& {
    using G = remove_cvref_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return G();
    } else {
      return std::invoke(std::forward<F>(f), error());
    }
  }
  template <class F>
  constexpr auto or_else(F&& f) const&& {
    using G = remove_cvref_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return G();
    } else {
      return std::invoke(std::forward<F>(f), std::move(error()));
    }
  }

  template <class F>
  constexpr auto transform(F&& f) & {
    using U = std::remove_cv_t<std::invoke_result_t<F>>;
    if (has_value()) {
      return transform_helper<U>(std::forward<F>(f));
    } else {
      return expected<U, E>(unexpect, error());
    }
  }
  template <class F>
  constexpr auto transform(F&& f) && {
    using U = std::remove_cv_t<std::invoke_result_t<F>>;
    if (has_value()) {
      return transform_helper<U>(std::forward<F>(f));
    } else {
      return expected<U, E>(unexpect, std::move(error()));
    }
  }
  template <class F>
  constexpr auto transform(F&& f) const& {
    using U = std::remove_cv_t<std::invoke_result_t<F>>;
    if (has_value()) {
      return transform_helper<U>(std::forward<F>(f));
    } else {
      return expected<U, E>(unexpect, error());
    }
  }
  template <class F>
  constexpr auto transform(F&& f) const&& {
    using U = std::remove_cv_t<std::invoke_result_t<F>>;
    if (has_value()) {
      return transform_helper<U>(std::forward<F>(f));
    } else {
      return expected<U, E>(unexpect, std::move(error()));
    }
  }

  template <class F>
  constexpr auto transform_error(F&& f) & {
    using G = std::remove_cv_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return expected<T, G>();
    } else {
      return expected<T, G>(unexpect, std::invoke(std::forward<F>(f), error()));
    }
  }
  template <class F>
  constexpr auto transform_error(F&& f) && {
    using G = std::remove_cv_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return expected<T, G>();
    } else {
      return expected<T, G>(
          unexpect, std::invoke(std::forward<F>(f), std::move(error())));
    }
  }
  template <class F>
  constexpr auto transform_error(F&& f) const& {
    using G = std::remove_cv_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return expected<T, G>();
    } else {
      return expected<T, G>(unexpect, std::invoke(std::forward<F>(f), error()));
    }
  }
  template <class F>
  constexpr auto transform_error(F&& f) const&& {
    using G = std::remove_cv_t<std::invoke_result_t<F, decltype(error())>>;
    if (has_value()) {
      return expected<T, G>();
    } else {
      return expected<T, G>(
          unexpect, std::invoke(std::forward<F>(f), std::move(error())));
    }
  }

 private:
  // Make all specializations of `expected` friends.
  template <class U, class G, class>
  friend class expected;

  // Helper to handle transform correctly for void(Args...) functions, non-void
  // case.
  template <class U, class F, std::enable_if_t<!std::is_void_v<U>, bool> = true>
  expected<U, E> transform_helper(F&& f) const {
    return expected<U, E>(std::in_place, std::invoke(std::forward<F>(f)));
  }

  // Helper to handle transform correctly for void(Args...) functions, void
  // case.
  template <class U, class F, std::enable_if_t<std::is_void_v<U>, bool> = true>
  expected<U, E> transform_helper(F&& f) const {
    std::invoke(std::forward<F>(f));
    return expected<U, E>();
  }

  std::optional<E> error_contents_;
};

template <class T,
          class E,
          class U,
          class G,
          std::enable_if_t<std::is_void_v<U>, bool> = true>
constexpr bool operator==(const expected<T, E>& lhs,
                          const expected<U, G>& rhs) {
  if (lhs.has_value() != rhs.has_value())
    return false;
  return lhs.has_value() || static_cast<bool>(lhs.error() == rhs.error());
}

template <class T, class E, class G>
constexpr bool operator==(const expected<T, E>& lhs, const unexpected<G>& rhs) {
  return !lhs.has_value() && static_cast<bool>(lhs.error() == rhs.error());
}

}  // namespace pw::internal
