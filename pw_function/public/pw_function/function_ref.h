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

#include <cstring>
#include <type_traits>
#include <utility>

#include "pw_preprocessor/compiler.h"

namespace pw {

/// `pw::FunctionRef` is a non-owning reference to a callable object.
/// It provides similar functionality to C++26 `std::function_ref`.
///
/// Unlike `pw::Function`, it does not hold ownership of the callable or its
/// context. It is intended for use as a synchronous callback parameter where
/// the callable is guaranteed to outlive the function call.
///
/// `pw::FunctionRef` is not nullable and must be initialized with a valid
/// callable object. This matches the semantics of C++26 `std::function_ref`.
///
/// Example:
/// @code{.cpp}
///   void ProcessItems(pw::FunctionRef<void(const Item&)> callback) {
///     for (const auto& item : items) {
///       callback(item);
///     }
///   }
/// @endcode
template <typename Signature>
class FunctionRef;

namespace function::internal {

template <bool kIsConst, bool IsNoExcept, typename R, typename... Args>
class BasicFunctionRef;

union Storage {
  void* ptr;
  alignas(void*) char buffer[sizeof(void*)];
};

// Specialization for non-noexcept
template <bool kIsConst, typename R, typename... Args>
class BasicFunctionRef<kIsConst, false, R, Args...> {
 public:
  // Constructor for lvalue reference
  template <typename F,
            std::enable_if_t<
                !std::is_same_v<std::decay_t<F>, BasicFunctionRef> &&
                    std::is_invocable_r_v<
                        R,
                        std::conditional_t<kIsConst,
                                           const std::remove_reference_t<F>&,
                                           std::remove_reference_t<F>&>,
                        Args...>,
                int> = 0>
  BasicFunctionRef(F&& f PW_ATTRIBUTE_LIFETIME_BOUND) noexcept
      : invoker_([](void* obj_ptr, Args... args) -> R {
          Storage* s = static_cast<Storage*>(obj_ptr);
          using FPtr = std::conditional_t<kIsConst,
                                          const std::remove_reference_t<F>*,
                                          std::remove_reference_t<F>*>;
          FPtr f_ptr;
          if constexpr (std::is_function_v<std::remove_reference_t<F>>) {
            f_ptr = reinterpret_cast<FPtr>(s->ptr);
          } else {
            f_ptr = static_cast<FPtr>(s->ptr);
          }
          return (*f_ptr)(std::forward<Args>(args)...);
        }) {
    if constexpr (std::is_function_v<std::remove_reference_t<F>>) {
      obj_.ptr = reinterpret_cast<void*>(&f);
    } else {
      obj_.ptr = const_cast<void*>(static_cast<const void*>(&f));
    }
  }

  BasicFunctionRef(const BasicFunctionRef&) noexcept = default;
  BasicFunctionRef& operator=(const BasicFunctionRef&) noexcept = default;

  R operator()(Args... args) const {
    return invoker_(const_cast<Storage*>(&obj_), std::forward<Args>(args)...);
  }

 private:
  using Invoker = R (*)(void*, Args...);

  Storage obj_;
  Invoker invoker_;
};

// Specialization for noexcept
template <bool kIsConst, typename R, typename... Args>
class BasicFunctionRef<kIsConst, true, R, Args...> {
 public:
  // Constructor for lvalue reference
  template <typename F,
            std::enable_if_t<
                !std::is_same_v<std::decay_t<F>, BasicFunctionRef> &&
                    std::is_invocable_r_v<
                        R,
                        std::conditional_t<kIsConst,
                                           const std::remove_reference_t<F>&,
                                           std::remove_reference_t<F>&>,
                        Args...> &&
                    std::is_nothrow_invocable_v<
                        std::conditional_t<kIsConst,
                                           const std::remove_reference_t<F>&,
                                           std::remove_reference_t<F>&>,
                        Args...>,
                int> = 0>
  BasicFunctionRef(F&& f PW_ATTRIBUTE_LIFETIME_BOUND) noexcept
      : invoker_([](void* obj_ptr, Args... args) noexcept -> R {
          Storage* s = static_cast<Storage*>(obj_ptr);
          using FPtr = std::conditional_t<kIsConst,
                                          const std::remove_reference_t<F>*,
                                          std::remove_reference_t<F>*>;
          FPtr f_ptr;
          if constexpr (std::is_function_v<std::remove_reference_t<F>>) {
            f_ptr = reinterpret_cast<FPtr>(s->ptr);
          } else {
            f_ptr = static_cast<FPtr>(s->ptr);
          }
          return (*f_ptr)(std::forward<Args>(args)...);
        }) {
    if constexpr (std::is_function_v<std::remove_reference_t<F>>) {
      obj_.ptr = reinterpret_cast<void*>(&f);
    } else {
      obj_.ptr = const_cast<void*>(static_cast<const void*>(&f));
    }
  }

  BasicFunctionRef(const BasicFunctionRef&) noexcept = default;
  BasicFunctionRef& operator=(const BasicFunctionRef&) noexcept = default;

  R operator()(Args... args) const noexcept {
    return invoker_(const_cast<Storage*>(&obj_), std::forward<Args>(args)...);
  }

 private:
  using Invoker = R (*)(void*, Args...) noexcept;

  Storage obj_;
  Invoker invoker_;
};

}  // namespace function::internal

// Specialization for R(Args...)
template <typename R, typename... Args>
class FunctionRef<R(Args...)> final
    : public function::internal::BasicFunctionRef<false, false, R, Args...> {
  using Base = function::internal::BasicFunctionRef<false, false, R, Args...>;

 public:
  using Base::Base;
  FunctionRef(const FunctionRef&) noexcept = default;
  FunctionRef& operator=(const FunctionRef&) noexcept = default;
};

// Specialization for R(Args...) const
template <typename R, typename... Args>
class FunctionRef<R(Args...) const> final
    : public function::internal::BasicFunctionRef<true, false, R, Args...> {
  using Base = function::internal::BasicFunctionRef<true, false, R, Args...>;

 public:
  using Base::Base;
  FunctionRef(const FunctionRef&) noexcept = default;
  FunctionRef& operator=(const FunctionRef&) noexcept = default;
};

// Specialization for R(Args...) noexcept
template <typename R, typename... Args>
class FunctionRef<R(Args...) noexcept> final
    : public function::internal::BasicFunctionRef<false, true, R, Args...> {
  using Base = function::internal::BasicFunctionRef<false, true, R, Args...>;

 public:
  using Base::Base;
  FunctionRef(const FunctionRef&) noexcept = default;
  FunctionRef& operator=(const FunctionRef&) noexcept = default;
};

// Specialization for R(Args...) const noexcept
template <typename R, typename... Args>
class FunctionRef<R(Args...) const noexcept> final
    : public function::internal::BasicFunctionRef<true, true, R, Args...> {
  using Base = function::internal::BasicFunctionRef<true, true, R, Args...>;

 public:
  using Base::Base;
  FunctionRef(const FunctionRef&) noexcept = default;
  FunctionRef& operator=(const FunctionRef&) noexcept = default;
};

}  // namespace pw
