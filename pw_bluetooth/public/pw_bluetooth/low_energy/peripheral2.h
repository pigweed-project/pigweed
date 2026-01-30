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

#include <cstdint>
#include <memory>

#include "pw_async2/dispatcher.h"
#include "pw_async2/value_future.h"
#include "pw_bluetooth/internal/raii_ptr.h"
#include "pw_bluetooth/low_energy/advertising_data.h"
#include "pw_bluetooth/low_energy/connection2.h"
#include "pw_bluetooth/low_energy/phy.h"
#include "pw_bluetooth/types.h"
#include "pw_function/function.h"
#include "pw_result/expected.h"

namespace pw::bluetooth::low_energy {

/// @module{pw_bluetooth}

/// `AdvertisedPeripheral` instances are valid for the duration of advertising.
class AdvertisedPeripheral2 {
 public:
  virtual ~AdvertisedPeripheral2() = default;

  /// For connectable advertisements, this method returns Ready when an LE
  /// central connects to the advertisement.
  ///
  /// The returned Connection2 can be used to interact with the peer. It also
  /// represents a peripheral's ownership over the connection: the client can
  /// drop the object to request a disconnection. Similarly, the Connection2
  /// error handler is called by the system to indicate that the connection to
  /// the peer has been lost. While connections are exclusive among peripherals,
  /// they may be shared with centrals, preventing disconnections.
  ///
  /// After a connection is returned, advertising will be paused until
  /// `PendConnection()` is called again. This method may return multiple
  /// connections over the lifetime of an advertisement.
  ///
  /// @param cx Awoken when a connection is established.
  virtual async2::Poll<Connection2::Ptr> PendConnection(
      async2::Context& cx) = 0;

  /// Requests that advertising be stopped. `PendStop()` can be used to wait for
  /// advertising to stop (e.g. before starting another advertisement).
  /// Destroying this object will also stop advertising, but there will be no
  /// way to determine when advertising has stopped. This method is idempotent.
  virtual void StopAdvertising() = 0;

  /// Returns Ready when advertising has stopped due to a call to
  /// `StopAdvertising()` or due to error. Awakens `cx` on stopping.
  ///
  /// @returns
  /// * @OK: Advertising was stopped successfully after a call to
  ///   `StopAdvertising()`.
  /// * @CANCELLED: An internal error occurred and the advertisement was
  ///   cancelled.
  virtual async2::Poll<pw::Status> PendStop(async2::Context& cx) = 0;

 private:
  /// Stop advertising and release memory. This method is called by the
  /// ~AdvertisedPeripheral::Ptr() when it goes out of scope, the API client
  /// should never call this method.
  virtual void Release() = 0;

 public:
  /// Movable `AdvertisedPeripheral2` smart pointer. The peripheral will
  /// continue advertising until the returned `AdvertisedPeripheral::Ptr` is
  /// destroyed.
  using Ptr =
      internal::RaiiPtr<AdvertisedPeripheral2, &AdvertisedPeripheral2::Release>;
};

/// Represents the LE Peripheral role, which advertises and is connected to.
class Peripheral2 {
 public:
  /// The range of the time interval between advertisements. Shorter intervals
  /// result in faster discovery at the cost of higher power consumption. The
  /// exact interval used is determined by the Bluetooth controller.
  /// - Time = N * 0.625ms.
  /// - Time range: 0x0020 (20ms) - 0x4000 (10.24s)
  struct AdvertisingIntervalRange {
    /// Default: 1.28s
    uint16_t min = 0x0800;
    /// Default: 1.28s
    uint16_t max = 0x0800;
  };

  /// The fields that are to be sent in a scan response packet. Clients may
  /// use this to send additional data that does not fit inside an advertising
  /// packet on platforms that do not support the advertising data length
  /// extensions.
  ///
  /// If present, advertisements will be configured to be scannable.
  using ScanResponse = AdvertisingData;

  /// If present, the controller will broadcast connectable advertisements
  /// which allow peers to initiate connections to the Peripheral. The fields
  /// of `ConnectionOptions` will configure any connections set up from
  /// advertising.
  using ConnectionOptions = Connection2::ConnectionOptions;

  /// Use legacy advertising PDUs. Use this if you need compatibility with old
  /// devices.
  struct LegacyAdvertising {
    /// See `ScanResponse` documentation.
    std::optional<ScanResponse> scan_response;

    /// See `ConnectionOptions` documentation.
    std::optional<ConnectionOptions> connection_options;
  };

  /// Advertise using the newer extended advertising Protocol Data Unit (PDU),
  /// which aren't supported by older devices.
  struct ExtendedAdvertising {
    /// Anonymous advertisements do not include the address.
    struct Anonymous {};

    /// Extended advertisements can have a scan response, be connectable, be
    /// anonymous, or none of the above. See `ScanResponse`,
    /// `ConnectionOptions`, and `Anonymous` documentation.
    std::variant<std::monostate, ScanResponse, ConnectionOptions, Anonymous>
        configuration;

    /// The maximum power level to transmit with. Null indicates no preference.
    /// Range: -127 to +20
    /// Units: dBm
    std::optional<int8_t> tx_power;

    /// The primary physical layer configuration to advertise with. Can only be
    /// 1Megabit or LeCoded PHY. If the PHY is not supported, a `kNotSupported`
    /// error will be returned.
    Phy primary_phy = Phy::k1Megabit;

    /// The secondary physical layer configuration to advertise with. Can be any
    /// PHY. If the PHY is not supported, a `kNotSupported` error will be
    /// returned.
    Phy secondary_phy = Phy::k1Megabit;
  };

  using AdvertisingProcedure =
      std::variant<LegacyAdvertising, ExtendedAdvertising>;

  /// Represents the parameters for configuring advertisements.
  struct AdvertisingParameters {
    /// The fields that will be encoded in the data section of advertising
    /// packets.
    AdvertisingData data;

    /// See `AdvertisingIntervalRange` documentation.
    AdvertisingIntervalRange interval_range;

    /// The type of address to include in advertising packets. If null, the host
    /// stack will select an address type. If the address type could not be used
    /// (either because of controller error or host configuration), a `kFailed`
    /// error wil be returned.
    std::optional<Address::Type> address_type;

    /// Specifies the advertising procedure to use and the parameters specific
    /// to that procedure.
    AdvertisingProcedure procedure;
  };

  /// Errors returned by `Advertise`.
  enum class AdvertiseError {
    /// The operation or parameters requested are not supported on the current
    /// hardware.
    kNotSupported = 1,

    /// The provided advertising data exceeds the maximum allowed length when
    /// encoded.
    kAdvertisingDataTooLong = 2,

    /// The provided scan response data exceeds the maximum allowed length when
    /// encoded.
    kScanResponseDataTooLong = 3,

    /// The requested parameters are invalid.
    kInvalidParameters = 4,

    /// The maximum number of simultaneous advertisements has already been
    /// reached.
    kNotEnoughAdvertisingSlots = 5,

    /// Advertising could not be initiated due to a hardware or system error.
    kFailed = 6,
  };

  using AdvertiseResult =
      pw::expected<AdvertisedPeripheral2::Ptr, AdvertiseError>;

  virtual ~Peripheral2() = default;

  /// Start advertising continuously as a LE peripheral. If advertising cannot
  /// be initiated then `result_callback` will be called with an error. Once
  /// started, advertising can be stopped by destroying the returned
  /// `AdvertisedPeripheral::Ptr`.
  ///
  /// If the system supports multiple advertising, this may be called as many
  /// times as there are advertising slots. To reconfigure an advertisement,
  /// first close the original advertisement and then initiate a new
  /// advertisement.
  ///
  /// @param parameters Parameters used while configuring the advertising
  /// instance.
  /// @return Asynchronously returns a result once advertising has started or
  /// failed. On success, returns an `AdvertisedPeripheral2` that models the
  /// lifetime of the advertisement. Destroying it will stop advertising.
  virtual async2::OptionalValueFuture<AdvertiseResult> Advertise(
      const AdvertisingParameters& parameters) = 0;
};

}  // namespace pw::bluetooth::low_energy
