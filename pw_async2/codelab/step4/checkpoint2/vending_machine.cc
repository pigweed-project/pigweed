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

#include "vending_machine.h"

#include <mutex>

#include "pw_async2/try.h"
#include "pw_log/log.h"

namespace codelab {

pw::async2::Poll<int> KeyPressFuture::Pend(pw::async2::Context& cx) {
  return core_.DoPend(*this, cx);
}

KeyPressFuture::~KeyPressFuture() {
  std::lock_guard lock(internal::KeypadLock());
  core_.Unlist();
}

pw::async2::Poll<int> KeyPressFuture::DoPend(pw::async2::Context&) {
  std::lock_guard lock(internal::KeypadLock());
  if (key_pressed_.has_value()) {
    return pw::async2::Ready(*key_pressed_);
  }
  return pw::async2::Pending();
}

KeyPressFuture Keypad::WaitForKeyPress() {
  std::lock_guard lock(internal::KeypadLock());
  KeyPressFuture future(pw::async2::FutureState::kPending);
  futures_.Push(future.core_);
  return future;
}

void Keypad::Press(int key) {
  std::lock_guard lock(internal::KeypadLock());
  if (auto future = futures_.PopIfAvailable()) {
    future->key_pressed_ = key;
    future->core_.Wake();
  }
}

pw::async2::Poll<> VendingMachineTask::DoPend(pw::async2::Context& cx) {
  while (true) {
    switch (state_) {
      case kWelcome: {
        PW_LOG_INFO("Welcome to the Pigweed Vending Machine!");
        PW_LOG_INFO("Please insert a coin.");
        state_ = kAwaitingPayment;
        break;
      }

      case kAwaitingPayment: {
        if (!select_future_.is_pendable()) {
          select_future_ = pw::async2::Select(coin_slot_.GetCoins(),
                                              keypad_.WaitForKeyPress());
        }
        PW_TRY_READY_ASSIGN(auto result, select_future_.Pend(cx));
        if (result.has_value<0>()) {
          unsigned coins = result.value<0>();
          PW_LOG_INFO("Received %u coin%s.", coins, coins > 1 ? "s" : "");
          PW_LOG_INFO("Please press a keypad key.");
          coins_inserted_ += coins;
          state_ = kAwaitingSelection;
        } else {
          PW_LOG_ERROR("Please insert a coin before making a selection.");
        }
        break;
      }

      case kAwaitingSelection: {
        if (!select_future_.is_pendable()) {
          select_future_ = pw::async2::Select(coin_slot_.GetCoins(),
                                              keypad_.WaitForKeyPress());
        }
        PW_TRY_READY_ASSIGN(auto result, select_future_.Pend(cx));
        if (result.has_value<1>()) {
          int key = result.value<1>();
          PW_LOG_INFO("Keypad %d was pressed. Dispensing an item.", key);
          return pw::async2::Ready();
        }

        // If we didn't get a key press, we must have gotten a coin.
        coins_inserted_ += result.value<0>();
        PW_LOG_INFO("Received a coin. Your balance is currently %u coins.",
                    coins_inserted_);
        PW_LOG_INFO("Please press a keypad key.");
        break;
      }
    }
  }
}

}  // namespace codelab
