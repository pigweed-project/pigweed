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

#include <cstddef>
#include <string_view>

#include "pw_async2/dispatcher.h"

// Interrupt handler function invoked when the user inserts a coin into the
// vending machine.
void coin_inserted_isr();

// Interrupt handler function invoked when the user presses a key on the
// machine's keypad. Receives the value of the pressed key (1-4).
void key_press_isr(int key);

// Interrupt handler function invoked to simulate the item drop detector
// detecting confirmation that an item was successfully dispensed from the
// machine.
void item_drop_sensor_isr();

namespace codelab {

// Number of characters on the vending machine's display.
inline constexpr size_t kDisplayCharacters = 10;

// Call this to set the text on the vending machine's display.
void SetDisplay(std::string_view text);

enum MotorState { kOff, kOn };

// Call this to set the simulated dispenser motor state for a item slot (1-4).
// The motor state for each item slot is initially off.
void SetDispenserMotorState(int item, MotorState state);

// Initializes the simulated hardware, allowing for interactive input and
// output using a background thread. The given dispatcher is used to dump the
// current dispatcher state on demand for diagnostic purposes.
void HardwareInit(pw::async2::Dispatcher* dispatcher);

}  // namespace codelab
