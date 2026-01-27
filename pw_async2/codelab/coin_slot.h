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

#include <mutex>

#include "pw_async2/future.h"
#include "pw_sync/interrupt_spin_lock.h"
#include "pw_sync/lock_annotations.h"

namespace codelab {

class CoinSlot;

class CoinFuture {
 public:
  using value_type = unsigned;

  constexpr CoinFuture() : coin_slot_(nullptr) {}

  CoinFuture(CoinFuture&& other) noexcept : CoinFuture() {
    *this = std::move(other);
  }

  CoinFuture& operator=(CoinFuture&& other) {
    core_ = std::move(other.core_);
    coin_slot_ = std::exchange(other.coin_slot_, nullptr);
    return *this;
  }

  pw::async2::Poll<unsigned> Pend(pw::async2::Context& cx) {
    return core_.DoPend(*this, cx);
  }

  [[nodiscard]] bool is_pendable() const { return core_.is_pendable(); }
  [[nodiscard]] bool is_complete() const { return core_.is_complete(); }

 private:
  friend class CoinSlot;
  friend class pw::async2::FutureCore;

  static constexpr const char kWaitReason[] = "CoinSlot::GetCoins()";

  explicit CoinFuture(CoinSlot& coin_slot);

  void Wake() { core_.Wake(); }

  pw::async2::Poll<unsigned> DoPend(pw::async2::Context& cx);

  pw::async2::FutureCore core_;
  CoinSlot* coin_slot_;
};

// Represents the coin slot hardware for a vending machine.
class CoinSlot {
 public:
  constexpr CoinSlot() : coins_deposited_(0) {}

  // Returns a future that resolves when coins are deposited.
  // May only be called by one task.
  CoinFuture GetCoins();

  // Stub for backward compatibility with later codelab steps.
  // TODO: pwbug.dev/457508399 - remove this once all steps use futures.
  pw::async2::Poll<unsigned> Pend(pw::async2::Context&) {
    return pw::async2::Ready(1);
  }

  // Report that a coin was received by the coin slot.
  // Typically called from the coin slot ISR.
  void Deposit();

 private:
  friend class CoinFuture;

  pw::sync::InterruptSpinLock lock_;

  // The number of coins deposited since the last CoinFuture was consumed.
  unsigned coins_deposited_ PW_GUARDED_BY(lock_);
  pw::async2::FutureList<&CoinFuture::core_> futures_ PW_GUARDED_BY(lock_);
};

}  // namespace codelab
