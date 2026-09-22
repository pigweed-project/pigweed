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

#include <mutex>

#include "pw_sync/lock_annotations.h"
#include "pw_sync/lock_traits.h"

namespace pw {

/// @module{pw_sync}

/// RAII helper to use `BasicLockable` locks with thread safety lock annotations
/// with more capabilities than `std::lock_guard`.
///
/// @note The simpler `std::lock_guard` RAII helper is recommended by default
/// due to additional member overhead. `pw::ScopedLocker` should only be used if
/// more complex lock management is required where RAII alone is insufficient.
///
/// Unlike a `std::lock_guard` and `std::scoped_lock`, this helper can be
/// constructed with the lock deferred. In addition, this does not support
/// `std::lock` deadlock avoidance algorithm from `std::scoped_lock`.
///
/// Lastly this supports explicit `lock()` and `unlock()` like
/// `std::unique_lock`, however unlike `std::unique_lock`, `pw::ScopedLocker`
/// does not support the use of `Lockables`'s conditional lock acquisition,
/// `try_lock()`. This means that this supports thread safety lock annotations
/// unlike `std::unique_lock`.
template <typename BasicLockable>
class PW_SCOPED_LOCKABLE ScopedLocker {
 public:
  static_assert(pw::sync::is_basic_lockable_v<BasicLockable>,
                "lock type must satisfy BasicLockable");

  explicit ScopedLocker(BasicLockable& lock) noexcept
      PW_EXCLUSIVE_LOCK_FUNCTION(lock)
      : lock_(lock) {
    lock_.lock();
    locked_ = true;
  }

  ScopedLocker(BasicLockable& lock, std::adopt_lock_t) noexcept
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock)
      : lock_(lock), locked_(true) {}

  ScopedLocker(BasicLockable& lock, std::defer_lock_t) noexcept
      PW_LOCKS_EXCLUDED(lock)
      : lock_(lock), locked_(false) {}

  ~ScopedLocker() PW_UNLOCK_FUNCTION() {
    if (locked_) {
      lock_.unlock();
    }
  }

  ScopedLocker(const ScopedLocker&) = delete;
  ScopedLocker(ScopedLocker&&) = delete;
  ScopedLocker& operator=(const ScopedLocker&) = delete;
  ScopedLocker& operator=(ScopedLocker&&) = delete;

  void lock() PW_EXCLUSIVE_LOCK_FUNCTION() {
    lock_.lock();
    locked_ = true;
  }

  void unlock() PW_UNLOCK_FUNCTION() {
    locked_ = false;
    lock_.unlock();
  }

 private:
  BasicLockable& lock_;
  bool locked_ PW_GUARDED_BY(lock_);
};

// Deduction guide to allow ``ScopedLocker(lock)`` rather than
// ``ScopedLocker<T>(lock)``.
template <typename T>
ScopedLocker(T lock) -> ScopedLocker<T>;

// Deduction guide to allow ``ScopedLocker(lock, std::adopt_lock)`` rather than
// ``ScopedLocker<T>(lock, std::adopt_lock)``.
template <typename T>
ScopedLocker(T lock, std::adopt_lock_t) -> ScopedLocker<T>;

// Deduction guide to allow ``ScopedLocker(lock, std::defer_lock)`` rather than
// ``ScopedLocker<T>(lock, std::defer_lock)``.
template <typename T>
ScopedLocker(T lock, std::defer_lock_t) -> ScopedLocker<T>;

/// @}

}  // namespace pw
