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

#include <optional>

#include "pw_async2/value_future.h"
#include "pw_bluetooth/internal/raii_ptr.h"
#include "pw_bluetooth/low_energy/connection2.h"
#include "pw_bluetooth/low_energy/phy.h"
#include "pw_bluetooth/types.h"
#include "pw_chrono/system_clock.h"
#include "pw_result/expected.h"

namespace pw::bluetooth::low_energy {

/// @module{pw_bluetooth}

/// Represents the LE central role. Used to scan and connect to peripherals.
class Central2 {
 public:
  /// Filter parameters for use during a scan. A discovered peer only matches
  /// the filter if it satisfies all of the present filter parameters.
  struct ScanFilter {
    /// Filter based on advertised service UUID.
    std::optional<Uuid> service_uuid;

    /// Filter based on service data containing the given UUID.
    std::optional<Uuid> service_data_uuid;

    /// Filter based on a manufacturer identifier present in the manufacturer
    /// data. If this filter parameter is set, then the advertising payload must
    /// contain manufacturer-specific data with the provided company identifier
    /// to satisfy this filter. Manufacturer identifiers can be found at
    /// [Assigned
    /// Numbers](https://www.bluetooth.com/specifications/assigned-numbers/company-identifiers/).
    std::optional<uint16_t> manufacturer_id;

    /// Filter based on whether or not a device is connectable. For example, a
    /// client that is only interested in peripherals that it can connect to can
    /// set this to true. Similarly a client can scan only for broadcasters by
    /// setting this to false.
    std::optional<bool> connectable;

    /// Filter results based on a portion of the advertised device name.
    /// Substring matches are allowed.
    /// The name length must be at most `pw::bluetooth::kMaxDeviceNameLength`.
    std::optional<std::string_view> name;

    /// Filter results based on the path loss of the radio wave. A device that
    /// matches this filter must satisfy the following:
    ///   1. Radio transmission power level and received signal strength must be
    ///      available for the path loss calculation.
    ///   2. The calculated path loss value must be less than, or equal to,
    ///      `max_path_loss`.
    ///
    /// @note This field is calculated using the RSSI and TX Power information
    /// obtained from advertising and scan response data during a scan
    /// procedure. It should NOT be confused with information for an active
    /// connection obtained using the "Path Loss Reporting" feature.
    std::optional<int8_t> max_path_loss;

    /// Require that a peer solicits support for a service UUID.
    std::optional<Uuid> solicitation_uuid;
  };

  enum class ScanType : uint8_t {
    kPassive,
    /// Send scanning PDUs with the public address.
    kActiveUsePublicAddress,
    /// Send scanning PDUs with the random address.
    kActiveUseRandomAddress,
    /// Send scanning PDUs with a generated Resolvable Private Address.
    kActiveUseResolvablePrivateAddress,
  };

  /// Parameters used during a scan.
  struct ScanOptions {
    /// List of filters for use during a scan. A peripheral that satisfies any
    /// of these filters will be reported. At least 1 filter must be specified.
    /// While not recommended, clients that require that all peripherals be
    /// reported can specify an empty filter.
    /// The span memory must only be valid until the call to Scan() ends.
    pw::span<const ScanFilter> filters;

    /// The time interval between scans.
    /// - Time = N * 0.625ms
    /// - Range: 0x0004 (2.5ms) - 10.24s (0x4000)
    uint16_t interval;

    /// The duration of the scan. The window must be less than or equal to the
    /// interval.
    /// - Time = N * 0.625ms
    /// - Range: 0x0004 (2.5ms) - 10.24s (0x4000)
    uint16_t window;

    /// Specifies whether to send scan requests, and if so, what type of address
    /// to use in scan requests.
    ScanType scan_type;

    /// A bitmask of the PHYs to scan with. Only the 1Megabit and LeCoded PHYs
    /// are supported.
    Phy phys = Phy::k1Megabit;
  };

  struct ScanResult {
    /// Uniquely identifies this peer on the current system.
    PeerId peer_id;

    /// Whether or not this peer is connectable. Non-connectable peers are
    /// typically in the LE broadcaster role.
    bool connectable;

    /// The last observed signal strength of this peer. This field is only
    /// present for a peer that is broadcasting. The RSSI can be stale if the
    /// peer has not been advertising.
    ///
    /// @note This field should NOT be confused with the "connection RSSI" of a
    /// peer that is currently connected to the system.
    std::optional<uint8_t> rssi;

    /// This contains the advertising data last received from the peer.
    pw::multibuf::MultiBuf data;

    /// The name of this peer. The name is often obtained during a scan
    /// procedure and can get updated during the name discovery procedure
    /// following a connection.
    ///
    /// This field is present if the name is known.
    std::optional<InlineString<22>> name;

    /// Timestamp of when the information in this `ScanResult` was last updated.
    chrono::SystemClock::time_point last_updated;
  };

  /// Represents an ongoing LE scan.
  class ScanHandle {
   public:
    /// Stops the scan.
    virtual ~ScanHandle() = default;

    /// Returns the next `ScanResult` if available. Otherwise, invokes
    /// `cx.waker()` when a `ScanResult` is available. Only one waker is
    /// supported at a time.
    ///
    /// @returns
    /// * @OK: `ScanResult` was returned.
    /// * @CANCELLED: An internal error occurred and the scan was cancelled.
    virtual async2::PollResult<ScanResult> PendResult(async2::Context& cx) = 0;

   private:
    /// Stop the current scan. This method is called by the ~ScanHandle::Ptr()
    /// when it goes out of scope, the API client should never call this method.
    virtual void Release() = 0;

   public:
    /// Movable ScanHandle smart pointer. The controller will continue scanning
    /// until the ScanHandle::Ptr is destroyed.
    using Ptr = internal::RaiiPtr<ScanHandle, &ScanHandle::Release>;
  };

  /// Possible errors returned by `Connect`.
  enum class ConnectError : uint8_t {
    /// The peer ID is unknown.
    kUnknownPeer,

    /// The `ConnectionOptions` were invalid.
    kInvalidOptions,

    /// A connection to the peer already exists.
    kAlreadyExists,

    /// The connection procedure failed at the link layer or timed out
    /// immediately after being established. A "could not be established" error
    /// was reported by the controller. This may be due to interference.
    kCouldNotBeEstablished,
  };

  enum class StartScanError : uint8_t {
    /// A scan is already in progress. Only 1 scan may be active at a time.
    kScanInProgress,
    /// Some of the scan options are invalid.
    kInvalidParameters,
    /// An internal error occurred and a scan could not be started.
    kInternal,
  };

  /// The result type returned by Connect().
  using ConnectResult = pw::expected<Connection2::Ptr, ConnectError>;

  /// The result type returned by Scan().
  using ScanStartResult = pw::expected<ScanHandle::Ptr, StartScanError>;

  virtual ~Central2() = default;

  /// Connect to the peer with the given identifier.
  ///
  /// The returned `Connection2` represents the client's interest in the LE
  /// connection to the peer. Destroying all `Connection2` instances for a peer
  /// will disconnect from the peer.
  ///
  /// The `Connection` will be closed by the system if the connection to the
  /// peer is lost or an error occurs, as indicated by `Connection.OnError`.
  ///
  /// @param peer_id Identifier of the peer to initiate a connection to.
  /// @param options Options used to configure the connection.
  /// @return Returns a result when a connection is successfully established, or
  /// an error occurs.
  ///
  /// Possible errors are documented in `ConnectError`.
  virtual async2::OptionalValueFuture<ConnectResult> Connect(
      PeerId peer_id, Connection2::ConnectionOptions options) = 0;

  /// Scans for nearby LE peripherals and broadcasters. The lifetime of the scan
  /// session is tied to the returned `ScanHandle` object in `ScanStartResult`.
  /// Once a scan is started, `ScanHandle::PendResult` can be called to get scan
  /// results. Only 1 scan may be active at a time.
  ///
  /// @param options Options used to configure the scan session. These options
  ///     are *suggestions* only, and the implementation may use different
  ///     parameters to meet power or radio requirements.
  /// @return Returns a `ScanHandle` object if the scan successfully
  /// starts, or a `ScanError` otherwise.
  /// `ScanHandle::PendResult` can be called to get `ScanResult`s for LE
  /// peers that satisfy the filters indicated in `options`. The initial results
  /// may report recently discovered peers. Subsequent results will be reported
  /// only when peers have been scanned or updated since the last call.
  virtual async2::OptionalValueFuture<ScanStartResult> Scan(
      const ScanOptions& options) = 0;
};

}  // namespace pw::bluetooth::low_energy
