// Copyright 2021 The Pigweed Authors
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

#include "pw_polyfill/language_feature_macros.h"
#include "pw_sync/lock_annotations.h"

namespace pw::sync {

/// @module{pw_sync}

/// The `VirtualBasicLockable` is a virtual lock abstraction for locks which
/// meet the C++ named BasicLockable requirements of lock() and unlock().
///
/// This virtual indirection is useful in case you need configurable lock
/// selection in a portable module where the final type is not defined upstream
/// and ergo module configuration cannot be used or in case the lock type is not
/// fixed at compile time, for example to support run time and crash time use of
/// an object without incurring the code size hit for templating the object.
class PW_LOCKABLE("pw::sync::VirtualBasicLockable") VirtualBasicLockable {
 public:
  /// Lock the underlying BasicLockable object.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  void lock() PW_EXCLUSIVE_LOCK_FUNCTION() {
    DoLockOperation(Operation::kLock);
  }

  /// Unlock the underlying BasicLockable object.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  void unlock() PW_UNLOCK_FUNCTION() { DoLockOperation(Operation::kUnlock); }

 protected:
  ~VirtualBasicLockable() = default;

  enum class Operation {
    kLock,
    kUnlock,
  };

 private:
  /// Uses a single virtual method with an enum to minimize the vtable cost per
  /// implementation of `VirtualBasicLockable`.
  virtual void DoLockOperation(Operation operation) = 0;
};

/// The `NoOpLock` is a type of `VirtualBasicLockable` that does nothing, i.e.
/// lock operations are no-ops.
class PW_LOCKABLE("pw::sync::NoOpLock") NoOpLock final
    : public VirtualBasicLockable {
 public:
  constexpr NoOpLock() {}
  NoOpLock(const NoOpLock&) = delete;
  NoOpLock(NoOpLock&&) = delete;
  NoOpLock& operator=(const NoOpLock&) = delete;
  NoOpLock& operator=(NoOpLock&&) = delete;

  /// Gives access to a global NoOpLock instance. It is not necessary to have
  /// multiple NoOpLock instances since they have no state and do nothing.
  static NoOpLock& Instance() {
    PW_CONSTINIT static NoOpLock lock;
    return lock;
  }

 private:
  void DoLockOperation(Operation) override {}
};

/// Templated base class to facilitate making `Virtual{LockType}` from a
/// `LockType` class that provides `lock()` and `unlock()` methods.
/// The resulting classes will derive from `VirtualBasicLockable`.
template <typename LockType>
class GenericBasicLockable : public VirtualBasicLockable {
 public:
  virtual ~GenericBasicLockable() = default;

 protected:
  LockType& impl() { return impl_; }

 private:
  void DoLockOperation(Operation operation) override
      PW_NO_LOCK_SAFETY_ANALYSIS {
    switch (operation) {
      case Operation::kLock:
        return impl_.lock();

      case Operation::kUnlock:
      default:
        return impl_.unlock();
    }
  }

  LockType impl_;
};

/// Templated base class to facilitate making `Virtual{LockType}` from a
/// `LockType` class that derives from `VirtualBasicLockable`, and also meets
/// the C++ named Lockable requirements by providing try_lock().
///
/// Example: `class VirtualMutex : public GenericLockable<Mutex> {};`
template <typename LockType>
class PW_LOCKABLE("pw::sync::GenericLockable") GenericLockable
    : public GenericBasicLockable<LockType> {
 public:
  [[nodiscard]] bool try_lock() PW_EXCLUSIVE_TRYLOCK_FUNCTION(true) {
    return GenericBasicLockable<LockType>::impl().try_lock();
  }
};

/// @}

}  // namespace pw::sync
