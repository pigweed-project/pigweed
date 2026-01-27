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

#include "vending_machine.h"

#include <mutex>

#include "pw_async2/try.h"
#include "pw_log/log.h"

namespace codelab {

KeyPressFuture::KeyPressFuture(Keypad& keypad)
    : core_(pw::async2::FutureState::kPending), keypad_(&keypad) {
  std::lock_guard lock(internal::KeypadLock());
  keypad_->futures_.Push(*this);
}

pw::async2::Poll<int> KeyPressFuture::Pend(pw::async2::Context& cx) {
  return core_.DoPend(*this, cx);
}

pw::async2::Poll<int> KeyPressFuture::DoPend(pw::async2::Context&) {
  std::lock_guard lock(internal::KeypadLock());
  if (key_pressed_.has_value()) {
    return pw::async2::Ready(*key_pressed_);
  }
  return pw::async2::Pending();
}

KeyPressFuture Keypad::WaitForKeyPress() { return KeyPressFuture(*this); }

void Keypad::Press(int key) {
  std::lock_guard lock(internal::KeypadLock());
  if (auto future = futures_.PopIfAvailable()) {
    future->key_pressed_ = key;
    future->core_.Wake();
  }
}

pw::async2::Poll<> VendingMachineTask::DoPend(pw::async2::Context& cx) {
  if (coins_inserted_ == 0) {
    if (!coin_future_.is_pendable()) {
      PW_LOG_INFO("Welcome to the Pigweed Vending Machine!");
      PW_LOG_INFO("Please insert a coin.");
      coin_future_ = coin_slot_.GetCoins();
    }

    PW_TRY_READY_ASSIGN(unsigned coins, coin_future_.Pend(cx));
    PW_LOG_INFO("Received %u coin%s.", coins, coins > 1 ? "s" : "");
    PW_LOG_INFO("Please press a keypad key.");
    coins_inserted_ += coins;
  }

  if (!key_future_.is_pendable()) {
    key_future_ = keypad_.WaitForKeyPress();
  }

  PW_TRY_READY_ASSIGN(int key, key_future_.Pend(cx));
  PW_LOG_INFO("Keypad %d was pressed. Dispensing an item.", key);

  return pw::async2::Ready();
}

}  // namespace codelab
