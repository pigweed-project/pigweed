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

#include <new>
#include <type_traits>
#include <utility>

namespace pw {

/// @module{pw_toolchain}

/// Declares a global variable that is initialized at runtime and whose
/// destructor is never run.
///
/// This class is the same as `pw::NoDestructor`, except that `pw::NoDestructor`
/// may be `constinit` if `T` is `constexpr` constructible.
/// `pw::RuntimeInitGlobal` instances can never be `constinit`.
/// `pw::RuntimeInitGlobal` prevents constant initialization inserting empty
/// volatile inline assembly.
///
/// @note `RuntimeInitGlobal` should only be used when `T` should not be
/// constant initialized; otherwise, use `pw::NoDestructor`. Constant
/// initialization moves objects from `.bss` to `.data`. This can increase
/// binary size if the object is larger than the code that initializes it.
///
/// @warning Misuse of `RuntimeInitGlobal` can cause memory leaks and other
/// problems. `RuntimeInitGlobal` should only be used for global variables.
template <typename T>
class RuntimeInitGlobal {
 public:
  using value_type = T;

  // Initializes a T in place.
  //
  // This overload is disabled when it might collide with copy/move.
  template <
      typename... Args,
      typename std::enable_if<!std::is_same<void(std::decay_t<Args>&...),
                                            void(RuntimeInitGlobal&)>::value,
                              int>::type = 0>
  explicit RuntimeInitGlobal(Args&&... args) {
    new (storage_) T(std::forward<Args>(args)...);
    asm volatile("");  // Prevent constant initialization
  }

  // Move or copy from the contained type. This allows for construction from an
  // initializer list, e.g. for std::vector.
  explicit RuntimeInitGlobal(const T& x) {
    new (storage_) T(x);
    asm volatile("");  // Prevent constant initialization
  }

  explicit RuntimeInitGlobal(T&& x) {
    new (storage_) T(std::move(x));
    asm volatile("");  // Prevent constant initialization
  }

  RuntimeInitGlobal(const RuntimeInitGlobal&) = delete;
  RuntimeInitGlobal& operator=(const RuntimeInitGlobal&) = delete;

  ~RuntimeInitGlobal() = default;

  const T& operator*() const { return *operator->(); }
  T& operator*() { return *operator->(); }

  const T* operator->() const {
    return std::launder(reinterpret_cast<const T*>(&storage_));
  }
  T* operator->() { return std::launder(reinterpret_cast<T*>(&storage_)); }

 private:
  // Use aligned storage instead of a union to remove the destructor, since GCC
  // incorrectly prevents RuntimeInitGlobal from wrapping types with private
  // destructors if the type is stored in a union.
  alignas(T) unsigned char storage_[sizeof(T)];
};

}  // namespace pw
