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

#include <optional>

#include "coin_slot.h"
#include "pw_async2/context.h"
#include "pw_async2/future.h"
#include "pw_async2/poll.h"
#include "pw_async2/task.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"

namespace codelab {
namespace internal {

inline pw::sync::InterruptSpinLock& KeypadLock() {
  PW_CONSTINIT static pw::sync::InterruptSpinLock lock;
  return lock;
}

}  // namespace internal

class Keypad;

class KeyPressFuture {
 public:
  // The type returned by the future when it completes.
  using value_type = int;

  KeyPressFuture() = default;

  KeyPressFuture(KeyPressFuture&& other) = default;
  KeyPressFuture& operator=(KeyPressFuture&& other) = default;

  ~KeyPressFuture();

  // Pends until a key is pressed, returning the key number.
  pw::async2::Poll<value_type> Pend(pw::async2::Context& cx);

  [[nodiscard]] bool is_pendable() const { return core_.is_pendable(); }
  [[nodiscard]] bool is_complete() const { return core_.is_complete(); }

 private:
  friend class Keypad;
  friend class pw::async2::FutureCore;

  static constexpr const char kWaitReason[] = "Waiting for keypad press";

  explicit KeyPressFuture(pw::async2::FutureState::Pending)
      : core_(pw::async2::FutureState::kPending) {}

  pw::async2::Poll<value_type> DoPend(pw::async2::Context& cx);

  pw::async2::FutureCore core_;

  // When present, holds the key that was pressed.
  // If absent, the future is still pending.
  std::optional<int> key_pressed_ PW_GUARDED_BY(internal::KeypadLock());
};

// Ensure that KeyPressFuture satisfies the Future concept.
static_assert(pw::async2::Future<KeyPressFuture>);

class Keypad {
 public:
  // Returns a future that resolves when a key is pressed with the value
  // of the key.
  //
  // May only be called by one task.
  KeyPressFuture WaitForKeyPress();

  // Record a key press. Typically called from the keypad ISR.
  void Press(int key);

 private:
  friend class KeyPressFuture;

  // The list of futures waiting for a key press.
  pw::async2::FutureList<&KeyPressFuture::core_> futures_
      PW_GUARDED_BY(internal::KeypadLock());
};

// The main task that drives the vending machine.
class VendingMachineTask : public pw::async2::Task {
 public:
  VendingMachineTask(CoinSlot& coin_slot, Keypad& keypad)
      : pw::async2::Task(PW_ASYNC_TASK_NAME("VendingMachineTask")),
        coin_slot_(coin_slot),
        keypad_(keypad),
        coins_inserted_(0),
        state_(kWelcome) {}

 private:
  // This is the core of the asynchronous task. The dispatcher calls this method
  // to give the task a chance to do work.
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override;

  CoinSlot& coin_slot_;
  CoinFuture coin_future_;
  Keypad& keypad_;
  KeyPressFuture key_future_;
  unsigned coins_inserted_;

  enum State {
    kWelcome,
    kAwaitingPayment,
    kAwaitingSelection,
  };
  State state_;
};

}  // namespace codelab
