// Copyright 2020 The Pigweed Authors
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

#include <stdbool.h>

#include "pw_chrono/system_clock.h"
#include "pw_preprocessor/util.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"

#ifdef __cplusplus

#include "pw_sync/virtual_basic_lockable.h"

namespace pw::sync {

/// @module{pw_sync}

/// The `TimedMutex` is a synchronization primitive that can be used to protect
/// shared data from being simultaneously accessed by multiple threads with
/// timeouts and deadlines, extending the `Mutex`. It offers exclusive,
/// non-recursive ownership semantics where priority inheritance is used to
/// solve the classic priority-inversion problem. This is thread safe, but NOT
/// IRQ safe.
///
/// @warning In order to support global statically constructed TimedMutexes, the
/// user and/or backend MUST ensure that any initialization required in your
/// environment is done prior to the creation and/or initialization of the
/// native synchronization primitives (e.g. kernel initialization).
class TimedMutex : public Mutex {
 public:
  TimedMutex() = default;
  ~TimedMutex() = default;
  TimedMutex(const TimedMutex&) = delete;
  TimedMutex(TimedMutex&&) = delete;
  TimedMutex& operator=(const TimedMutex&) = delete;
  TimedMutex& operator=(TimedMutex&&) = delete;

  /// Tries to lock the mutex. Blocks until specified the timeout has elapsed or
  /// the lock is acquired, whichever comes first.
  /// Returns true if the mutex was successfully acquired.
  ///
  /// @pre The lock isn't already held by this thread. Recursive locking is
  /// undefined behavior.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  [[nodiscard]] bool try_lock_for(chrono::SystemClock::duration timeout)
      PW_EXCLUSIVE_TRYLOCK_FUNCTION(true);

  /// Tries to lock the mutex. Blocks until specified deadline has been reached
  /// or the lock is acquired, whichever comes first.
  /// Returns true if the mutex was successfully acquired.
  ///
  /// @pre The lock isn't already held by this thread. Recursive locking is
  /// undefined behavior.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  [[nodiscard]] bool try_lock_until(chrono::SystemClock::time_point deadline)
      PW_EXCLUSIVE_TRYLOCK_FUNCTION(true);
};

class PW_LOCKABLE("pw::sync::VirtualTimedMutex") VirtualTimedMutex final
    : public GenericLockable<TimedMutex> {
 public:
  TimedMutex& timed_mutex() { return impl(); }

  [[nodiscard]] bool try_lock_for(chrono::SystemClock::duration timeout)
      PW_EXCLUSIVE_TRYLOCK_FUNCTION(true) {
    return impl().try_lock_for(timeout);
  }

  [[nodiscard]] bool try_lock_until(chrono::SystemClock::time_point deadline)
      PW_EXCLUSIVE_TRYLOCK_FUNCTION(true) {
    return impl().try_lock_until(deadline);
  }
};

/// @}

}  // namespace pw::sync

#include "pw_sync_backend/timed_mutex_inline.h"

using pw_sync_TimedMutex = pw::sync::TimedMutex;

#else  // !defined(__cplusplus)

typedef struct pw_sync_TimedMutex pw_sync_TimedMutex;

#endif  // __cplusplus

PW_EXTERN_C_START

/// @module{pw_sync}

/// Invokes the `TimedMutex::lock` member function on the given `mutex`.
///
/// @threadsafe{yes}
/// @isrsafe{no}
/// @nmisafe{no}
void pw_sync_TimedMutex_Lock(pw_sync_TimedMutex* mutex)
    PW_NO_LOCK_SAFETY_ANALYSIS;

/// Invokes the `TimedMutex::try_lock` member function on the given `mutex`.
///
/// @threadsafe{yes}
/// @isrsafe{no}
/// @nmisafe{no}
bool pw_sync_TimedMutex_TryLock(pw_sync_TimedMutex* mutex)
    PW_NO_LOCK_SAFETY_ANALYSIS;

/// Invokes the `TimedMutex::try_lock_for` member function on the given `mutex`.
///
/// @threadsafe{yes}
/// @isrsafe{no}
/// @nmisafe{no}
bool pw_sync_TimedMutex_TryLockFor(pw_sync_TimedMutex* mutex,
                                   pw_chrono_SystemClock_Duration timeout)
    PW_NO_LOCK_SAFETY_ANALYSIS;

/// Invokes the `TimedMutex::try_lock_until` member function on the given
/// `mutex`.
///
/// @threadsafe{yes}
/// @isrsafe{no}
/// @nmisafe{no}
bool pw_sync_TimedMutex_TryLockUntil(pw_sync_TimedMutex* mutex,
                                     pw_chrono_SystemClock_TimePoint deadline)
    PW_NO_LOCK_SAFETY_ANALYSIS;

/// Invokes the `TimedMutex::unlock` member function on the given `mutex`.
///
/// @threadsafe{yes}
/// @isrsafe{no}
/// @nmisafe{no}
void pw_sync_TimedMutex_Unlock(pw_sync_TimedMutex* mutex)
    PW_NO_LOCK_SAFETY_ANALYSIS;

/// @}

PW_EXTERN_C_END
