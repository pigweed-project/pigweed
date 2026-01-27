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

#include "pw_async2/try.h"
#include "pw_log/log.h"

namespace codelab {

pw::async2::Poll<> VendingMachineTask::DoPend(pw::async2::Context& cx) {
  if (!coin_future_.is_pendable()) {
    PW_LOG_INFO("Welcome to the Pigweed Vending Machine!");
    PW_LOG_INFO("Please insert a coin.");
    coin_future_ = coin_slot_.GetCoins();
  }

  pw::async2::Poll<unsigned> poll_result = coin_future_.Pend(cx);
  if (poll_result.IsPending()) {
    return pw::async2::Pending();
  }
  unsigned coins = poll_result.value();
  PW_LOG_INFO(
      "Received %u coin%s. Dispensing item.", coins, coins > 1 ? "s" : "");
  return pw::async2::Ready();
}

}  // namespace codelab
