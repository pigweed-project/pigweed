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

#include "coin_slot.h"
#include "hardware.h"
#include "item_drop_sensor.h"
#include "pw_async2/basic_dispatcher.h"
#include "vending_machine.h"

namespace {

codelab::CoinSlot coin_slot;
codelab::Keypad keypad;
codelab::ItemDropSensor item_drop_sensor;

}  // namespace

// Interrupt handler function invoked when the user inserts a coin into the
// vending machine.
void coin_inserted_isr() { coin_slot.Deposit(); }

// Interrupt handler function invoked when the user presses a key on the
// machine's keypad. Receives the value of the pressed key (0-9).
void key_press_isr(int key) { keypad.Press(key); }

// Interrupt handler function invoked to simulate the item drop detector
// detecting confirmation that an item was successfully dispensed from the
// machine.
void item_drop_sensor_isr() { item_drop_sensor.Drop(); }

int main() {
  pw::async2::BasicDispatcher dispatcher;
  codelab::HardwareInit(&dispatcher);

  pw::async2::ChannelStorage<int, 1> dispense_requests;
  pw::async2::ChannelStorage<bool, 1> dispense_responses;

  auto [req_handle, dispense_requests_sender, dispense_requests_receiver] =
      pw::async2::CreateSpscChannel(dispense_requests);
  auto [res_handle, dispense_responses_sender, dispense_responses_receiver] =
      pw::async2::CreateSpscChannel(dispense_responses);
  req_handle.Release();
  res_handle.Release();

  codelab::VendingMachineTask task(coin_slot,
                                   keypad,
                                   std::move(dispense_requests_sender),
                                   std::move(dispense_responses_receiver));
  dispatcher.Post(task);

  codelab::DispenserTask dispenser_task(item_drop_sensor,
                                        std::move(dispense_requests_receiver),
                                        std::move(dispense_responses_sender));
  dispatcher.Post(dispenser_task);

  dispatcher.RunToCompletion();

  return 0;
}
