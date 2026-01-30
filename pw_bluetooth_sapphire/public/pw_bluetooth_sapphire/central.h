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
#include "pw_bluetooth/low_energy/central2.h"
#include "pw_bluetooth_sapphire/internal/connection.h"
#include "pw_bluetooth_sapphire/internal/host/gap/adapter.h"
#include "pw_multibuf/allocator.h"

/// Dual-mode Bluetooth host stack
namespace pw::bluetooth_sapphire {

/// @module{pw_bluetooth_sapphire}

/// Must only be constructed and destroyed on the Bluetooth thread.
class Central final : public pw::bluetooth::low_energy::Central2 {
 public:
  // The maximum number of scan results to queue in each ScanHandle
  static constexpr uint8_t kMaxScanResultsQueueSize = 10;

  // TODO: https://pwbug.dev/377301546 - Don't expose Adapter in public API.
  /// Must only be constructed on the Bluetooth thread.
  /// @param allocator The allocator to use for advertising data buffers.
  Central(bt::gap::Adapter::WeakPtr adapter,
          pw::async::Dispatcher& dispatcher,
          pw::multibuf::MultiBufAllocator& allocator);
  ~Central() override;

  async2::OptionalValueFuture<ConnectResult> Connect(
      pw::bluetooth::PeerId peer_id,
      bluetooth::low_energy::Connection2::ConnectionOptions options) override
      PW_LOCKS_EXCLUDED(lock());

  async2::OptionalValueFuture<ScanStartResult> Scan(
      const ScanOptions& options) override PW_LOCKS_EXCLUDED(lock());

  static pw::sync::Mutex& lock();

 private:
  class ScanHandleImpl final : public ScanHandle {
   public:
    explicit ScanHandleImpl(uint16_t scan_id, Central* central)
        : scan_id_(scan_id), central_(central) {}

    // Synchronously clears ScanState's pointer to this object and
    // asynchronously stops the scan procedure.
    ~ScanHandleImpl() override;

    void QueueScanResultLocked(ScanResult&& result)
        PW_EXCLUSIVE_LOCKS_REQUIRED(lock());

    void WakeLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(lock()) { waker_.Wake(); }

    void OnScanErrorLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(lock()) {
      central_ = nullptr;
      WakeLocked();
    }

   private:
    async2::PollResult<ScanResult> PendResult(async2::Context& cx) override;

    // std::unique_ptr custom Deleter
    void Release() override { delete this; }

    const uint16_t scan_id_;

    // Set to null when Central is destroyed or scanning has stopped.
    Central* central_ PW_GUARDED_BY(lock());

    // PendResult() waker. Set when Pending was returned.
    async2::Waker waker_ PW_GUARDED_BY(lock());
    std::queue<ScanResult> results_ PW_GUARDED_BY(lock());
  };

  // Created and destroyed on the Bluetooth thread.
  class ScanState {
   public:
    // Must be run on Bluetooth thread. Not thread safe.
    explicit ScanState(
        std::unique_ptr<bt::gap::LowEnergyDiscoverySession> session,
        ScanHandleImpl* scan_handle,
        uint16_t scan_id,
        Central* central);

    ~ScanState();

    void OnScanHandleDestroyedLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(lock()) {
      scan_handle_ = nullptr;
    }

   private:
    // Must be run on Bluetooth thread. Not thread safe.
    void OnScanResult(const bt::gap::Peer& peer) PW_LOCKS_EXCLUDED(lock());

    // Must be run on Bluetooth thread. Not thread safe.
    void OnError() PW_LOCKS_EXCLUDED(lock());

    const uint16_t scan_id_;

    // Set to null synchronously when ScanHandleImpl is destroyed.
    ScanHandleImpl* scan_handle_ PW_GUARDED_BY(lock());

    // Members must only be accessed on Bluetooth thread.
    Central* const central_;
    std::unique_ptr<bt::gap::LowEnergyDiscoverySession> session_;
  };

  // Asynchronously stops the scan corresponding to `scan_id` and synchronously
  // clears `ScanState.scan_handle_`.
  void StopScanLocked(uint16_t scan_id) PW_EXCLUSIVE_LOCKS_REQUIRED(lock());

  void OnConnectionResult(
      bt::PeerId peer_id,
      bt::gap::Adapter::LowEnergy::ConnectionResult result,
      async2::OptionalValueProvider<ConnectResult> result_provider)
      PW_LOCKS_EXCLUDED(lock());

  std::unordered_map<uint16_t, ScanState> scans_ PW_GUARDED_BY(lock());

  // Must only be used on the Bluetooth thread.
  bt::gap::Adapter::WeakPtr adapter_;

  // Dispatcher for Bluetooth thread. Thread safe.
  pw::async::Dispatcher& dispatcher_;
  pw::async::HeapDispatcher heap_dispatcher_;

  pw::multibuf::MultiBufAllocator& allocator_;

  // Must only be used on the Bluetooth thread.
  WeakSelf<Central> weak_factory_{this};

  // Thread safe to copy and destroy, but WeakPtr should only be used
  // or dereferenced on the Bluetooth thread (WeakRef is not thread safe).
  WeakSelf<Central>::WeakPtr self_{weak_factory_.GetWeakPtr()};
};

}  // namespace pw::bluetooth_sapphire
