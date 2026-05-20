// Copyright 2023 The Pigweed Authors
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

#include "pw_digital_io/digital_io.h"
#include "pw_spi/chip_selector.h"

namespace pw::spi {

/// @module{pw_spi}

/// An implementation of `pw::spi::ChipSelector` that sets the state of a
/// pw_digital_io output when activated.
class DigitalOutChipSelector : public ChipSelector {
 public:
  constexpr DigitalOutChipSelector(pw::digital_io::DigitalOut& cs_pin)
      : cs_pin_(cs_pin) {}

  /// Set a `pw::digital_io::DigitalOut` state as a chip select signal.
  ///
  /// @param active  `true` to set the DigitalOut to `kActive`, `false` to set
  ///                the DigitalOut to `kInactive`.
  inline Status SetActive(bool active) override {
    return cs_pin_.SetState(active ? pw::digital_io::State::kActive
                                   : pw::digital_io::State::kInactive);
  }

 private:
  pw::digital_io::DigitalOut& cs_pin_;
};

/// @}

}  // namespace pw::spi
