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

#include "pw_async2/dispatcher.h"
#include "pw_async2/value_future.h"
#include "pw_bluetooth/vendor.h"
#include "pw_channel/channel.h"
#include "pw_function/function.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::bluetooth {

/// @module{pw_bluetooth}

/// The Controller class is a shim for communication between the Host and the
/// Controller. Controller is a `pw::Channel` used to send and receive HCI
/// packets. The first byte of each datagram is a UART packet indicator
/// (`H4PacketType` Emboss enum).
class Controller2
    : public pw::channel::Implement<pw::channel::ReliableDatagramReaderWriter> {
 public:
  /// Bitmask of features the controller supports.
  enum class FeaturesBits : uint32_t {
    /// Indicates support for transferring Synchronous Connection-Oriented (SCO)
    /// link data over the HCI. Offloaded SCO links may still be supported even
    /// if HCI SCO isn't.
    kHciSco = (1 << 0),
    /// Indicates support for the Set Acl Priority command.
    kSetAclPriorityCommand = (1 << 1),
    /// Indicates support for the `LE_Get_Vendor_Capabilities` command
    /// documented at
    /// [HCI
    /// requirements](https://source.android.com/docs/core/connect/bluetooth/hci_requirements).
    kAndroidVendorExtensions = (1 << 2),
    /// Bits 3-31 reserved.
  };

  enum class ScoCodingFormat : uint8_t {
    kCvsd,
    kMsbc,
  };

  enum class ScoEncoding : uint8_t {
    k8Bits,
    k16Bits,
  };

  enum class ScoSampleRate : uint8_t {
    k8Khz,
    k16Khz,
  };

  /// Returns Ready when fatal errors occur after initialization. After a fatal
  /// error, this object is invalid.
  virtual async2::Poll<Status> PendError(async2::Context& cx) = 0;

  /// Initializes the controller interface and starts processing packets.
  /// Asynchronously returns the result of initialization.
  ///
  /// On success, HCI packets may now be sent and received with this object.
  virtual async2::ValueFuture<Status> Initialize() = 0;

  /// Configure the HCI for a SCO connection with the indicated parameters.
  /// @returns
  /// * OK - success, packets can be sent/received.
  /// * UNIMPLEMENTED - the implementation/controller does not support SCO over
  ///     HCI
  /// * ALREADY_EXISTS - a SCO connection is already configured
  /// * INTERNAL - an internal error occurred
  virtual async2::ValueFuture<Status> ConfigureSco(
      ScoCodingFormat coding_format,
      ScoEncoding encoding,
      ScoSampleRate sample_rate) = 0;

  /// Releases the resources held by an active SCO connection. This should be
  /// called when a SCO connection is closed. No-op if no SCO connection is
  /// configured.
  /// @returns
  /// * OK - success, the SCO configuration was reset.
  /// * UNIMPLEMENTED - the implementation/controller does not support SCO over
  /// * HCI INTERNAL - an internal error occurred
  virtual async2::ValueFuture<Status> ResetSco() = 0;

  /// @returns A bitmask of features supported by the controller.
  virtual async2::ValueFuture<pw::Result<FeaturesBits>> GetFeatures() = 0;

  /// Encode the vendor-specific HCI command for a generic type of vendor
  /// command, and return the encoded command in a buffer.
  /// @param parameters Vendor command to encode.
  /// @returns Returns the result of the encoding request. On success, contains
  /// the command buffer.
  virtual async2::ValueFuture<Result<multibuf::MultiBuf>> EncodeVendorCommand(
      VendorCommandParameters parameters) = 0;
};

inline constexpr bool operator&(Controller2::FeaturesBits left,
                                Controller2::FeaturesBits right) {
  return static_cast<bool>(
      static_cast<std::underlying_type_t<Controller2::FeaturesBits>>(left) &
      static_cast<std::underlying_type_t<Controller2::FeaturesBits>>(right));
}

inline constexpr Controller2::FeaturesBits operator|(
    Controller2::FeaturesBits left, Controller2::FeaturesBits right) {
  return static_cast<Controller2::FeaturesBits>(
      static_cast<std::underlying_type_t<Controller2::FeaturesBits>>(left) |
      static_cast<std::underlying_type_t<Controller2::FeaturesBits>>(right));
}

inline constexpr Controller2::FeaturesBits& operator|=(
    Controller2::FeaturesBits& left, Controller2::FeaturesBits right) {
  return left = left | right;
}

}  // namespace pw::bluetooth
