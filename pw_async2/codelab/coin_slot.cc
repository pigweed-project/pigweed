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

#include <mutex>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_assert/check.h"

namespace codelab {

CoinFuture CoinSlot::GetCoins() {
  std::lock_guard lock(lock_);
  PW_CHECK(futures_.empty(),
           "Called GetCoins() while a CoinFuture is already active");
  return CoinFuture(*this);
}

void CoinSlot::Deposit() {
  std::lock_guard lock(lock_);
  coins_deposited_ += 1;
  if (CoinFuture* future = futures_.PopIfAvailable(); future != nullptr) {
    future->Wake();
  }
}

CoinFuture::CoinFuture(CoinSlot& coin_slot)
    : core_(pw::async2::FutureState::kPending), coin_slot_(&coin_slot) {
  coin_slot_->futures_.Push(core_);
}

pw::async2::Poll<unsigned> CoinFuture::DoPend(pw::async2::Context&) {
  PW_ASSERT(!is_complete());

  std::lock_guard lock(coin_slot_->lock_);
  unsigned coins = std::exchange(coin_slot_->coins_deposited_, 0);
  if (coins > 0) {
    return pw::async2::Ready(coins);
  }
  return pw::async2::Pending();
}

}  // namespace codelab
