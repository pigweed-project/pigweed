// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_LAZY_INIT_INCLUDE_LIB_LAZY_INIT_LAZY_INIT_H_
#define SRC_LIB_LAZY_INIT_INCLUDE_LIB_LAZY_INIT_LAZY_INIT_H_

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "internal/storage.h"
#include "options.h"

namespace lazy_init {

// Wrapper type for global variables that removes automatic constructor and
// destructor generation and provides explicit control over initialization.
// This avoids initialization order hazards between globals, as well as code
// that runs before and after global constructors are invoked.
//
// This is the base type that specifies the default parameters that control
// consistency checks and global destructor generation options.
//
// See lib/lazy_init/options.h for a description of CheckType and Destructor
// option values.
//
// Note, T must be construtible by LazyInit, however, its constructor need not
// be public if Access is declared as a friend. For example:
//
//   class Foo {
//     friend lazy_init::Access;
//
//     Foo(int arg);
//   };
//
template <typename T, CheckType = CheckType::Default, Destructor = Destructor::Default>
class LazyInit;

// Utility type to provide access to non-public constructors. Types with private
// or protected constructors that need lazy initialization must friend Access.
class Access {
  template <typename, CheckType, Destructor>
  friend class LazyInit;

  // Constructs the given storage using the constructor matching T(Args...).
  template <typename T, typename... Args>
  static void Initialize(T* value, Args&&... args) {
    new (value) T(std::forward<Args>(args)...);
  }
};

// Specialization that does not provide consistency checks.
template <typename T>
class LazyInit<T, CheckType::None, Destructor::Disabled> {
 public:
  constexpr LazyInit() = default;
  ~LazyInit() = default;

  LazyInit(const LazyInit&) = delete;
  LazyInit& operator=(const LazyInit&) = delete;
  LazyInit(LazyInit&&) = delete;
  LazyInit& operator=(LazyInit&&) = delete;

  // Explicitly constructs the wrapped global. It is up to the caller to
  // ensure that initialization is not already performed.
  // Returns a reference to the newly constructed global.
  template <typename... Args>
  T& Initialize(Args&&... args) {
    static_assert(alignof(LazyInit) >= alignof(T));
    Access::Initialize(&storage_.value, std::forward<Args>(args)...);
    return *storage_;
  }

  // Returns a reference to the wrapped global. It is up to the caller to
  // ensure that initialization is already performed and that the effects of
  // initialization are visible.
  T& Get() { return *storage_; }
  const T& Get() const { return *storage_; }

  // Accesses the wrapped global by pointer. It is up to the caller to ensure
  // that initialization is already performed and that the effects of
  // initialization are visible.
  T* operator->() { return &Get(); }
  const T* operator->() const { return &Get(); }

  // Returns a pointer to the wrapped global. All specializations return a
  // pointer without performing consistency checks. This should be used
  // cautiously, preferably only in constant expressions that take the address
  // of the wrapped global.
  constexpr T* GetAddressUnchecked() { return storage_.GetStorageAddress(); }
  constexpr const T* GetAddressUnchecked() const { return storage_.GetStorageAddress(); }

 private:
  template <typename, CheckType, Destructor>
  friend class LazyInit;

  // Explicitly destroys the wrapped global. Called by the specialization of
  // LazyInit with the destructor enabled.
  void Destruct() { storage_->T::~T(); }

  // Lazy-initialized storage for a value of type T.
  internal::LazyInitStorage<T> storage_;
};

// Specialization that provides basic consistency checks. It is up to the caller
// to ensure proper synchronization and barriers.
template <typename T>
class LazyInit<T, CheckType::Basic, Destructor::Disabled> {
 public:
  constexpr LazyInit() = default;
  ~LazyInit() = default;

  LazyInit(const LazyInit&) = delete;
  LazyInit& operator=(const LazyInit&) = delete;
  LazyInit(LazyInit&&) = delete;
  LazyInit& operator=(LazyInit&&) = delete;

  // Explicitly constructs the wrapped global. Asserts that initialization is
  // not already performed.
  // Returns a reference to the newly constructed global.
  template <typename... Args>
  T& Initialize(Args&&... args) {
    static_assert(alignof(LazyInit) >= alignof(T));

    internal::Assert(!initialized_);
    initialized_ = true;
    Access::Initialize(&storage_.value, std::forward<Args>(args)...);
    return *storage_;
  }

  // Returns a reference to the wrapped global. Asserts that initialization
  // is already performed, however, it is up to the caller to ensure that the
  // effects of initialization are visible.
  T& Get() {
    internal::Assert(initialized_);
    return *storage_;
  }

  const T& Get() const {
    internal::Assert(initialized_);
    return *storage_;
  }

  // Accesses the wrapped global by pointer. Asserts that initialization is
  // already performed, however, it is up the caller to ensure that the
  // effects of initialization are visible.
  T* operator->() { return &Get(); }
  const T* operator->() const { return &Get(); }

  // Returns a pointer to the wrapped global. All specializations return a
  // pointer without performing consistency checks. This should be used
  // cautiously, preferably only in constant expressions that take the address
  // of the wrapped global.
  constexpr T* GetAddressUnchecked() { return storage_.GetStorageAddress(); }
  constexpr const T* GetAddressUnchecked() const { return storage_.GetStorageAddress(); }

 private:
  template <typename, CheckType, Destructor>
  friend class LazyInit;

  // Explicitly destroys the wrapped global. Called by the specialization of
  // LazyInit with the destructor enabled.
  void Destruct() {
    internal::Assert(initialized_);
    storage_->T::~T();

    // Prevent the compiler from omitting this write during destruction.
    static_cast<volatile bool&>(initialized_) = false;
  }

  // Lazy-initialized storage for a value of type T.
  internal::LazyInitStorage<T> storage_;

  // Guard variable to check for multiple initializations and access before
  // initialization.
  bool initialized_{};
};

// Specialization that provides atomic consistency checks. Checks are guaranteed
// to be consistent under races over initialization.
template <typename T>
class LazyInit<T, CheckType::Atomic, Destructor::Disabled> {
 public:
  constexpr LazyInit() = default;
  ~LazyInit() = default;

  LazyInit(const LazyInit&) = delete;
  LazyInit& operator=(const LazyInit&) = delete;
  LazyInit(LazyInit&&) = delete;
  LazyInit& operator=(LazyInit&&) = delete;

  // Explicitly constructs the wrapped global. Asserts that initialization is
  // not already performed.
  // Returns a reference to the newly constructed global.
  template <typename... Args>
  T& Initialize(Args&&... args) {
    static_assert(alignof(LazyInit) >= alignof(T));

    TransitionState(State::Uninitialized, State::Constructing);
    Access::Initialize(&storage_.value, std::forward<Args>(args)...);
    state_.store(State::Initialized, std::memory_order_release);

    return *storage_;
  }

  // Returns a reference to the wrapped global. Asserts that initialization
  // is already performed. The effects of initialization are guaranteed to be
  // visible if the assertion passes.
  T& Get() {
    internal::Assert(State::Initialized == state_.load(std::memory_order_relaxed));
    return *storage_;
  }

  const T& Get() const {
    internal::Assert(State::Initialized == state_.load(std::memory_order_relaxed));
    return *storage_;
  }

  // Accesses the wrapped global by pointer. Asserts that initialization is
  // already performed. The effects of initialization are guaranteed to be
  // visible if the assertion passes.
  T* operator->() { return &Get(); }
  const T* operator->() const { return &Get(); }

  // Returns a pointer to the wrapped global. All specializations return a
  // pointer without performing consistency checks. This should be used
  // cautiously, preferably only in constant expressions that take the address
  // of the wrapped global.
  constexpr T* GetAddressUnchecked() { return storage_.GetStorageAddress(); }
  constexpr const T* GetAddressUnchecked() const { return storage_.GetStorageAddress(); }

 private:
  template <typename, CheckType, Destructor>
  friend class LazyInit;

  // Enum representing the states of initialization.
  enum class State : int {
    Uninitialized = 0,
    Constructing,
    Initialized,
    Destructing,
    Destroyed,
  };

  // Transitions the guard from the |expected| state to the |target| state.
  // Asserts that the expected state matches the actual state.
  void TransitionState(State expected, State target) {
    State actual = state_.load(std::memory_order_relaxed);
    internal::Assert(expected == actual);
    while (!state_.compare_exchange_weak(actual, target, std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
      internal::Assert(expected == actual);
    }
  }

  // Explicitly destroys the wrapped global. Called by the specialization of
  // LazyInit with the destructor enabled.
  void Destruct() {
    TransitionState(State::Initialized, State::Destructing);
    storage_->T::~T();
    state_.store(State::Destroyed, std::memory_order_release);
  }

  // Lazy-initialized storage for a value of type T.
  internal::LazyInitStorage<T> storage_;

  // Guard variable to check for multiple initializations and access before
  // initialization.
  std::atomic<State> state_{};
};

// Specialization that includes a global destructor. This type is based on the
// equivalent specialization without a global destructor and simply calls the
// base type Destruct() method.
template <typename T, CheckType Check>
class LazyInit<T, Check, Destructor::Enabled> : public LazyInit<T, Check, Destructor::Disabled> {
 public:
  using LazyInit<T, Check, Destructor::Disabled>::LazyInit;
  ~LazyInit() { this->Destruct(); }
};

}  // namespace lazy_init

#endif  // SRC_LIB_LAZY_INIT_INCLUDE_LIB_LAZY_INIT_LAZY_INIT_H_
