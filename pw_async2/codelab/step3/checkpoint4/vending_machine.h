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
  static pw::sync::InterruptSpinLock lock;
  return lock;
}

}  // namespace internal

class KeyPressFuture;

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

  // The future that will be resolved when a key is pressed.
  KeyPressFuture* key_press_future_ PW_GUARDED_BY(internal::KeypadLock()) =
      nullptr;
};

class KeyPressFuture {
 public:
  // The type returned by the future when it completes.
  using value_type = int;

  KeyPressFuture() : state_(kDefaultConstructed) {}

  KeyPressFuture(const KeyPressFuture&) = delete;
  KeyPressFuture& operator=(const KeyPressFuture&) = delete;

  KeyPressFuture(KeyPressFuture&& other) noexcept;
  KeyPressFuture& operator=(KeyPressFuture&& other) noexcept;

  // Pends until a key is pressed, returning the key number.
  pw::async2::Poll<value_type> Pend(pw::async2::Context& cx);

  bool is_pendable() const { return state_ == kInitialized; }
  bool is_complete() const { return state_ == kCompleted; }

 private:
  friend class Keypad;

  explicit KeyPressFuture(Keypad& keypad)
      : state_(kInitialized), keypad_(&keypad) {
    std::lock_guard lock(internal::KeypadLock());
    keypad_->key_press_future_ = this;
  }

  // Possible states of the future.
  enum {
    kDefaultConstructed,
    kInitialized,
    kCompleted,
  } state_;

  Keypad* keypad_;

  // When present, holds the key that was pressed.
  // If absent, the future is still pending.
  std::optional<int> key_pressed_;

  pw::async2::Waker waker_;
};

// Ensure that KeyPressFuture satisfies the Future concept.
static_assert(pw::async2::Future<KeyPressFuture>);

// The main task that drives the vending machine.
class VendingMachineTask : public pw::async2::Task {
 public:
  VendingMachineTask(CoinSlot& coin_slot, Keypad& keypad)
      : pw::async2::Task(PW_ASYNC_TASK_NAME("VendingMachineTask")),
        coin_slot_(coin_slot),
        keypad_(keypad),
        coins_inserted_(0) {}

 private:
  // This is the core of the asynchronous task. The dispatcher calls this method
  // to give the task a chance to do work.
  pw::async2::Poll<> DoPend(pw::async2::Context& cx) override;

  CoinSlot& coin_slot_;
  CoinFuture coin_future_;
  Keypad& keypad_;
  KeyPressFuture key_future_;
  unsigned coins_inserted_;
};

}  // namespace codelab
