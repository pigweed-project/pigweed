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

#include <atomic>

#include "pw_assert/check.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_sync/virtual_basic_lockable.h"

#ifndef PW_SYNC_SHARED_MUTEX_ENABLE_METRICS
#if !defined(NDEBUG)
#define PW_SYNC_SHARED_MUTEX_ENABLE_METRICS 1
#else
#define PW_SYNC_SHARED_MUTEX_ENABLE_METRICS 0
#endif  // !defined(NDEBUG)
#endif  // PW_SYNC_SHARED_MUTEX_ENABLE_METRICS

#if PW_SYNC_SHARED_MUTEX_ENABLE_METRICS
#include "pw_metric/metric.h"
#endif  // PW_SYNC_SHARED_MUTEX_ENABLE_METRICS

namespace pw::sync {

/// SharedLockable provides a VirtualBasicLockable API which exercises the
/// parent SharedMutex reader API.
///
/// A single SharedLockable may be shared across threads, but a thread must not
/// recursively call `lock()` or `unlock()`. Doing so is undefined behavior.
template <typename LockT>
class PW_LOCKABLE("pw::sync::SharedLockable") SharedLockable final
    : public VirtualBasicLockable {
 public:
  explicit SharedLockable(LockT* parent) : parent_(parent) {}
  ~SharedLockable() = default;
  SharedLockable(const SharedLockable&) = delete;
  SharedLockable(SharedLockable&&) = delete;
  SharedLockable& operator=(const SharedLockable&) = delete;
  SharedLockable& operator=(SharedLockable&&) = delete;

 private:
  void DoLockOperation(Operation operation) final PW_NO_LOCK_SAFETY_ANALYSIS {
    PW_CHECK_NOTNULL(parent_, "SharedLockable's parent lock must not be null.");
    switch (operation) {
      case Operation::kLock: {
        parent_->lock_shared();
        break;
      }
      case Operation::kUnlock: {
        parent_->unlock_shared();
        break;
      }
    }
  }

  LockT* parent_;
};

/// ExclusiveLockable provides a VirtualBasicLockable API which exercises the
/// parent SharedMutex writer API.
template <typename LockT>
class PW_LOCKABLE("pw::sync::ExclusiveLockable") ExclusiveLockable final
    : public VirtualBasicLockable {
 public:
  explicit ExclusiveLockable(LockT* parent) : parent_(parent) {}
  ~ExclusiveLockable() = default;
  ExclusiveLockable(const ExclusiveLockable&) = delete;
  ExclusiveLockable(ExclusiveLockable&&) = delete;
  ExclusiveLockable& operator=(const ExclusiveLockable&) = delete;
  ExclusiveLockable& operator=(ExclusiveLockable&&) = delete;

 private:
  void DoLockOperation(Operation operation) final PW_NO_LOCK_SAFETY_ANALYSIS {
    PW_CHECK_NOTNULL(parent_,
                     "ExclusiveLockable's parent lock must not be null.");
    switch (operation) {
      case Operation::kLock: {
        parent_->lock_exclusive();
        break;
      }
      case Operation::kUnlock: {
        parent_->unlock_exclusive();
        break;
      }
    }
  }

  LockT* parent_;
};

/// The `SharedMutex` is a synchronization primitive that can be used to protect
/// shared data from being simultaneously accessed by multiple threads. It
/// offers shared (multiple readers) and exclusive (single writer),
/// non-recursive ownership semantics.
///
/// Multiple threads may concurrently acquire shared ownership by calling
/// `lock_shared()` or `try_lock_shared()`, while only a single thread may hold
/// exclusive ownership via `lock_exclusive()` (or `lock()`). Once a writer
/// begins waiting for exclusive ownership, subsequent attempts to acquire
/// shared ownership will block until the writer has acquired and released the
/// lock, preventing writer starvation.
///
/// This primitive is thread safe, but NOT IRQ safe.
class PW_LOCKABLE("pw::sync::SharedMutex") SharedMutex {
 public:
  /// Constructs an unlocked `SharedMutex`.
  SharedMutex() = default;

  /// Destroys the `SharedMutex`.
  ///
  /// @pre The mutex is not held by any reader or writer. Failures are fatal.
  ~SharedMutex();

  SharedMutex(const SharedMutex&) = delete;
  SharedMutex(SharedMutex&&) = delete;
  SharedMutex& operator=(const SharedMutex&) = delete;
  SharedMutex& operator=(SharedMutex&&) = delete;

  /// Locks the mutex for exclusive access (writer), blocking indefinitely until
  /// acquired. Failures are fatal.
  ///
  /// This is equivalent to `lock_exclusive()` and implements the C++
  /// `BasicLockable` requirements.
  ///
  /// @pre The lock isn't already held by this thread. Recursive locking is
  /// undefined behavior.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  void lock() PW_EXCLUSIVE_LOCK_FUNCTION() { lock_exclusive(); }

  /// Attempts to lock the mutex for exclusive access (writer) in a non-blocking
  /// manner.
  ///
  /// This is equivalent to `try_lock_exclusive()` and implements the C++
  /// `Lockable` requirements.
  ///
  /// @returns `true` if exclusive ownership was successfully acquired, `false`
  /// otherwise.
  ///
  /// @pre The lock isn't already held by this thread. Recursive locking is
  /// undefined behavior.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  [[nodiscard]] bool try_lock() PW_EXCLUSIVE_TRYLOCK_FUNCTION(true) {
    return try_lock_exclusive();
  }

  /// Unlocks the mutex from exclusive access (writer). Failures are fatal.
  ///
  /// This is equivalent to `unlock_exclusive()` and implements the C++
  /// `BasicLockable` requirements.
  ///
  /// @pre Exclusive ownership is held by this thread.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  void unlock() PW_UNLOCK_FUNCTION() { unlock_exclusive(); }

  /// Locks the mutex for exclusive access (writer), blocking indefinitely until
  /// acquired. Failures are fatal.
  ///
  /// Blocks until all active readers and any prior writer have released the
  /// lock. Once a thread begins waiting for exclusive access, subsequent calls
  /// to `lock_shared()` will block until the writer has finished.
  ///
  /// @pre The lock isn't already held by this thread. Recursive locking is
  /// undefined behavior.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  void lock_exclusive() PW_EXCLUSIVE_LOCK_FUNCTION();

  /// Attempts to lock the mutex for exclusive access (writer) in a non-blocking
  /// manner.
  ///
  /// @returns `true` if exclusive ownership was successfully acquired, `false`
  /// otherwise.
  ///
  /// @pre The lock isn't already held by this thread. Recursive locking is
  /// undefined behavior.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  [[nodiscard]] bool try_lock_exclusive() PW_EXCLUSIVE_TRYLOCK_FUNCTION(true);

  /// Unlocks the mutex from exclusive access (writer). Failures are fatal.
  ///
  /// @pre Exclusive ownership is held by this thread.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  void unlock_exclusive() PW_UNLOCK_FUNCTION();

  /// Locks the mutex for shared access (reader), blocking indefinitely until
  /// acquired. Failures are fatal.
  ///
  /// Multiple threads may hold shared ownership concurrently. If another thread
  /// holds or is waiting for exclusive access, this call will block.
  ///
  /// @pre Recursive locking is undefined behavior.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  void lock_shared() PW_SHARED_LOCK_FUNCTION();

  /// Attempts to lock the mutex for shared access (reader) in a non-blocking
  /// manner.
  ///
  /// @returns `true` if shared access was successfully acquired, `false`
  /// otherwise.
  ///
  /// @pre Recursive locking is undefined behavior.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  [[nodiscard]] bool try_lock_shared() PW_SHARED_TRYLOCK_FUNCTION(true);

  /// Releases shared access (reader). Failures are fatal.
  ///
  /// @pre Shared ownership is held by this thread.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  void unlock_shared() PW_UNLOCK_FUNCTION();

  /// Returns the number of readers currently holding shared access.
  ///
  /// @note This value is read using relaxed memory ordering and should only be
  /// relied on for diagnostics, metrics, or testing.
  ///
  /// @returns The current active reader count.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{no}
  size_t reader_count() const {
    return reader_count_.load(std::memory_order_relaxed);
  }
#if PW_SYNC_SHARED_MUTEX_ENABLE_METRICS
  /// Returns the maximum number of concurrent readers observed.
  ///
  /// Only available when `PW_SYNC_SHARED_MUTEX_ENABLE_METRICS` is enabled.
  ///
  /// @returns Peak concurrent reader count.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{no}
  size_t max_concurrent_readers() const {
    return max_concurrent_readers_.value();
  }

  /// Returns the metric tracking the maximum number of concurrent readers.
  ///
  /// Only available when `PW_SYNC_SHARED_MUTEX_ENABLE_METRICS` is enabled.
  ///
  /// @returns A reference to the max concurrent readers metric.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{yes}
  const pw::metric::TypedMetric<uint32_t>& max_concurrent_readers_metric()
      const {
    return max_concurrent_readers_;
  }
#endif  // PW_SYNC_SHARED_MUTEX_ENABLE_METRICS

 private:
  pw::sync::Mutex exclusive_lock_;
  pw::sync::ThreadNotification writer_notification_;
  std::atomic<bool> writer_waiting_{false};
  std::atomic<size_t> reader_count_{0};
#if PW_SYNC_SHARED_MUTEX_ENABLE_METRICS
  PW_METRIC(max_concurrent_readers_, "max_concurrent_readers", 0u);
#endif  // PW_SYNC_SHARED_MUTEX_ENABLE_METRICS
};

}  // namespace pw::sync
