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

#include "hardware.h"
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
          state_ = kAwaitingDispenseIdle;
          break;
        }

        coins_inserted_ += result.value<unsigned>();
        PW_LOG_INFO("Received a coin. Your balance is currently %u coins.",
                    coins_inserted_);
        PW_LOG_INFO("Please press a keypad key.");
        break;
      }

      case kAwaitingDispenseIdle: {
        if (!dispense_request_future_.is_pendable()) {
          dispense_request_future_ = dispense_requests_.Send(item_to_dispense_);
        }
        PW_TRY_READY_ASSIGN(bool sent, dispense_request_future_.Pend(cx));
        if (!sent) {
          PW_LOG_WARN("Dispense requests channel unexpectedly closed.");
          return pw::async2::Ready();
        }
        state_ = kAwaitingDispense;
        break;
      }

      case kAwaitingDispense: {
        if (!dispense_response_future_.is_pendable()) {
          dispense_response_future_ = dispense_responses_.Receive();
        }
        std::optional<bool> received;
        PW_TRY_READY_ASSIGN(received, dispense_response_future_.Pend(cx));
        if (!received.has_value()) {
          PW_LOG_WARN("Dispense responses channel unexpectedly closed.");
          return pw::async2::Ready();
        }

        if (received.value()) {
          PW_LOG_INFO("Item dispensed successfully.");
          state_ = kWelcome;
        } else {
          PW_LOG_ERROR("Dispense failed. Choose another selection.");
          state_ = kAwaitingSelection;
        }
      }
    }
  }
}

pw::async2::Poll<> DispenserTask::DoPend(pw::async2::Context& cx) {
  while (true) {
    switch (state_) {
      case kIdle: {
        if (!dispense_request_future_.is_pendable()) {
          dispense_request_future_ = dispense_requests_.Receive();
        }
        PW_TRY_READY_ASSIGN(item_to_dispense_,
                            dispense_request_future_.Pend(cx));
        if (!item_to_dispense_.has_value()) {
          PW_LOG_WARN("Dispense requests channel unexpectedly closed.");
          return pw::async2::Ready();
        }
        SetDispenserMotorState(*item_to_dispense_, MotorState::kOn);
        state_ = kDispensing;
        break;
      }

      case kDispensing: {
        if (!drop_future_.is_pendable()) {
          drop_future_ = drop_sensor_.Wait();
        }
        PW_TRY_READY(drop_future_.Pend(cx));
        SetDispenserMotorState(*item_to_dispense_, MotorState::kOff);
        item_to_dispense_ = std::nullopt;
        state_ = kReportDispenseSuccess;
        break;
      }

      case kReportDispenseSuccess: {
        if (!dispense_response_future_.is_pendable()) {
          dispense_response_future_ = dispense_responses_.Send(true);
        }
        PW_TRY_READY_ASSIGN(bool result, dispense_response_future_.Pend(cx));
        if (!result) {
          PW_LOG_WARN("Dispense responses channel unexpectedly closed.");
          return pw::async2::Ready();
        }
        state_ = kIdle;
        break;
      }
    }
  }

  return pw::async2::Ready();
}

}  // namespace codelab
