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
#include <stddef.h>

#include "pw_chrono/system_clock.h"
#include "pw_preprocessor/util.h"

#ifdef __cplusplus

#include "pw_sync_backend/binary_semaphore_native.h"

namespace pw::sync {

/// @module{pw_sync}

/// `BinarySemaphore` is a specialization of `CountingSemaphore` with an
/// arbitrary token limit of 1. Note that that max() is >= 1, meaning it may be
/// released up to `max()` times but only acquired once for those `N` releases.
/// Implementations of `BinarySemaphore` are typically more efficient than the
/// default implementation of `CountingSemaphore`. The entire API is thread safe
/// but only a subset is IRQ safe.
///
/// @warning In order to support global statically constructed BinarySemaphores,
/// the user and/or backend MUST ensure that any initialization required in your
/// environment is done prior to the creation and/or initialization of the
/// native synchronization primitives (e.g. kernel initialization).
///
/// The `BinarySemaphore` is initialized to being empty or having no tokens.
class BinarySemaphore {
 public:
  using native_handle_type = backend::NativeBinarySemaphoreHandle;

  BinarySemaphore();
  ~BinarySemaphore();
  BinarySemaphore(const BinarySemaphore&) = delete;
  BinarySemaphore(BinarySemaphore&&) = delete;
  BinarySemaphore& operator=(const BinarySemaphore&) = delete;
  BinarySemaphore& operator=(BinarySemaphore&&) = delete;

  /// Atomically increments the internal counter by 1.
  /// Any thread(s) waiting for the counter to be greater than 0, i.e.
  /// blocked in acquire, will subsequently be unblocked.
  ///
  /// There exists an overflow risk if one releases more than max() times
  /// between acquires because many RTOS implementations internally
  /// increment the counter past one where it is only cleared when acquired.
  ///
  /// @pre `1 <= max() - counter`
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{no}
  void release();

  /// Decrements the internal counter to 0 or blocks indefinitely until it can.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  void acquire();

  /// Tries to decrement by the internal counter to 0 without blocking.
  ///
  /// @retval true if the internal counter was reset successfully.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{no}
  [[nodiscard]] bool try_acquire() noexcept;

  /// Tries to decrement the internal counter to 0. Blocks until the specified
  /// timeout has elapsed or the counter was decremented to 0, whichever comes
  /// first.
  ///
  /// @retval true if the internal counter was decremented successfully.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  [[nodiscard]] bool try_acquire_for(chrono::SystemClock::duration timeout);

  /// Tries to decrement the internal counter to 0. Blocks until the specified
  /// deadline has been reached or the counter was decremented to 0, whichever
  /// comes first.
  ///
  /// @retval true if the internal counter was decremented successfully.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{no}
  /// @nmisafe{no}
  [[nodiscard]] bool try_acquire_until(
      chrono::SystemClock::time_point deadline);

  /// Returns the internal counter's maximum possible value.
  ///
  /// @retval backend::kBinarySemaphoreMaxValue the internal counter's maximum
  /// possible value.
  ///
  /// @threadsafe{yes}
  /// @isrsafe{yes}
  /// @nmisafe{yes}
  [[nodiscard]] static constexpr ptrdiff_t max() noexcept {
    return backend::kBinarySemaphoreMaxValue;
  }

  [[nodiscard]] native_handle_type native_handle();

 private:
  /// This may be a wrapper around a native type with additional members.
  backend::NativeBinarySemaphore native_type_;
};

/// @}

}  // namespace pw::sync

#include "pw_sync_backend/binary_semaphore_inline.h"

using pw_sync_BinarySemaphore = pw::sync::BinarySemaphore;

#else  // !defined(__cplusplus)

typedef struct pw_sync_BinarySemaphore pw_sync_BinarySemaphore;

#endif  // __cplusplus

PW_EXTERN_C_START

/// @module{pw_sync}

/// Invokes the `BinarySemaphore::release` member function on the given
/// `semaphore`.
///
/// @threadsafe{yes}
/// @isrsafe{yes}
/// @nmisafe{no}
void pw_sync_BinarySemaphore_Release(pw_sync_BinarySemaphore* semaphore);

/// Invokes the `BinarySemaphore::acquire` member function on the given
/// `semaphore`.
///
/// @threadsafe{yes}
/// @isrsafe{no}
/// @nmisafe{no}
void pw_sync_BinarySemaphore_Acquire(pw_sync_BinarySemaphore* semaphore);

/// Invokes the `BinarySemaphore::try_acquire` member function on the given
/// `semaphore`.
///
/// @threadsafe{yes}
/// @isrsafe{yes}
/// @nmisafe{no}
bool pw_sync_BinarySemaphore_TryAcquire(pw_sync_BinarySemaphore* semaphore);

/// Invokes the `BinarySemaphore::try_acquire_for` member function on the given
/// `semaphore`.
///
/// @threadsafe{yes}
/// @isrsafe{no}
/// @nmisafe{no}
bool pw_sync_BinarySemaphore_TryAcquireFor(
    pw_sync_BinarySemaphore* semaphore, pw_chrono_SystemClock_Duration timeout);

/// Invokes the `BinarySemaphore::try_acquire_until` member function on the
/// given `semaphore`.
///
/// @threadsafe{yes}
/// @isrsafe{no}
/// @nmisafe{no}
bool pw_sync_BinarySemaphore_TryAcquireUntil(
    pw_sync_BinarySemaphore* semaphore,
    pw_chrono_SystemClock_TimePoint deadline);

/// Invokes the `BinarySemaphore::max` member function.
///
/// @threadsafe{yes}
/// @isrsafe{yes}
/// @nmisafe{yes}
ptrdiff_t pw_sync_BinarySemaphore_Max(void);

/// @}

PW_EXTERN_C_END
