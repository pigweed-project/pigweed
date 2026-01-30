// Copyright 2024 The Pigweed Authors
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

#include "pw_async2/value_future.h"
#include "pw_bluetooth/low_energy/peripheral2.h"
#include "pw_bluetooth_sapphire/internal/host/gap/adapter.h"

namespace pw::bluetooth_sapphire {

/// @module{pw_bluetooth_sapphire}

/// Must only be constructed and destroyed on the Bluetooth thread.
class Peripheral final : public pw::bluetooth::low_energy::Peripheral2 {
 public:
  // TODO: https://pwbug.dev/377301546 - Don't expose Adapter in public API.
  /// Must only be constructed on the Bluetooth thread.
  Peripheral(bt::gap::Adapter::WeakPtr adapter,
             pw::async::Dispatcher& dispatcher);

  /// Must only be destroyed on the Bluetooth thread.
  ~Peripheral() override;

  /// Thread safe.
  /// The AdvertisedPeripheral2 returned on success is thread safe.
  async2::OptionalValueFuture<AdvertiseResult> Advertise(
      const AdvertisingParameters& parameters) override
      PW_LOCKS_EXCLUDED(lock());

  static pw::sync::Mutex& lock();

 private:
  class Advertisement;

  // Thin client handle for an active advertisement.
  // Thread safe.
  class AdvertisedPeripheralImpl final
      : public pw::bluetooth::low_energy::AdvertisedPeripheral2 {
   public:
    AdvertisedPeripheralImpl(bt::gap::AdvertisementId id,
                             Peripheral* peripheral)
        : id_(id), peripheral_(peripheral) {}
    ~AdvertisedPeripheralImpl() override {
      std::lock_guard lock(Peripheral::lock());
      if (peripheral_) {
        peripheral_->OnAdvertisedPeripheralDestroyedLocked(id_);
      }
    }

    // AdvertisedPeripheral2 overrides:
    async2::Poll<pw::bluetooth::low_energy::Connection2::Ptr> PendConnection(
        [[maybe_unused]] async2::Context& cx) override {
      // TODO: https://pwbug.dev/377301546 - Implement connection handling.
      return async2::Pending();
    }
    void StopAdvertising() override;
    async2::Poll<pw::Status> PendStop(async2::Context& cx) override;
    void Release() override { delete this; }

   private:
    friend class Advertisement;

    const bt::gap::AdvertisementId id_;

    // Set to null if the advertisement is stopped or Peripheral is destroyed.
    Peripheral* peripheral_ PW_GUARDED_BY(lock());

    std::optional<pw::Status> stop_status_ PW_GUARDED_BY(lock());

    // Waker shared by PendConnection() and PendStop().
    async2::Waker waker_ PW_GUARDED_BY(lock());
  };

  // Active advertisement state. Must only be destroyed on the Bluetooth thread.
  class Advertisement final {
   public:
    Advertisement(bt::gap::AdvertisementInstance&& instance,
                  AdvertisedPeripheralImpl* advertised_peripheral)
        : advertised_peripheral_(advertised_peripheral),
          instance_(std::move(instance)) {}

    // Must only be destroyed on Bluetooth thread.
    ~Advertisement();

    void OnAdvertisedPeripheralDestroyedLocked()
        PW_EXCLUSIVE_LOCKS_REQUIRED(lock()) {
      advertised_peripheral_ = nullptr;
    }

    void OnStopLocked(pw::Status status) PW_EXCLUSIVE_LOCKS_REQUIRED(lock());

   private:
    friend class AdvertisedPeripheralImpl;

    AdvertisedPeripheralImpl* advertised_peripheral_;

    // Must only be modified/destroyed on the Bluetooth thread.
    bt::gap::AdvertisementInstance instance_;
  };

  // Thread safe.
  void OnAdvertisedPeripheralDestroyedLocked(
      bt::gap::AdvertisementId advertisement_id)
      PW_EXCLUSIVE_LOCKS_REQUIRED(lock());

  // Thread safe. The lock may be held when calling this function as the stop
  // message is immediately posted to the Bluetooth thread.
  void StopAdvertising(bt::gap::AdvertisementId advertisement_id);

  // Always called on Bluetooth thread.
  void OnAdvertiseResult(
      bt::gap::AdvertisementInstance instance,
      bt::hci::Result<> result,
      async2::OptionalValueProvider<AdvertiseResult> result_provider);

  // Always called on Bluetooth thread.
  void OnConnection(bt::gap::AdvertisementId advertisement_id,
                    bt::gap::Adapter::LowEnergy::ConnectionResult result);

  // Dispatcher for Bluetooth thread. Thread safe.
  pw::async::HeapDispatcher dispatcher_;

  // Must only be used on the Bluetooth thread.
  bt::gap::Adapter::WeakPtr adapter_;

  std::unordered_map<bt::gap::AdvertisementId, Advertisement> advertisements_
      PW_GUARDED_BY(lock());

  // Must only be used on the Bluetooth thread.
  WeakSelf<Peripheral> weak_factory_{this};

  // Thread safe to copy and destroy, but WeakPtr should only be used
  // or dereferenced on the Bluetooth thread (WeakRef is not thread safe).
  WeakSelf<Peripheral>::WeakPtr self_{weak_factory_.GetWeakPtr()};
};

}  // namespace pw::bluetooth_sapphire
