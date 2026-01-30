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
#include "pw_bluetooth/gatt/constants.h"
#include "pw_bluetooth/gatt/error.h"
#include "pw_bluetooth/gatt/types.h"
#include "pw_bluetooth/internal/raii_ptr.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_multibuf/multibuf.h"
#include "pw_result/expected.h"

namespace pw::bluetooth::gatt {

/// @module{pw_bluetooth}

/// An interface for interacting with a GATT service on a peer device.
class RemoteService2 {
 public:
  enum class RemoteServiceError {
    /// The service has been modified or removed.
    kServiceRemoved = 0,

    /// The peer serving this service has disconnected.
    kPeerDisconnected = 1,
  };

  /// Wrapper around a possible truncated value received from the server.
  struct ReadValue {
    /// Characteristic or descriptor handle.
    Handle handle;

    /// The value of the characteristic or descriptor.
    multibuf::MultiBuf value;

    /// True if `value` might be truncated (the buffer was completely filled by
    /// the server and the read was a short read).  `ReadCharacteristic` or
    /// `ReadDescriptor` should be used to read the complete value.
    bool maybe_truncated;
  };

  /// A result returned by `ReadByType`.
  struct ReadByTypeResult {
    /// Characteristic or descriptor handle.
    Handle handle;

    /// The value of the characteristic or descriptor, if it was read
    /// successfully, or an error explaining why the value could not be read.
    pw::expected<ReadValue, Error> result;
  };

  /// Represents the supported options to read a long characteristic or
  /// descriptor value from a server. Long values are those that may not fit in
  /// a single message.
  struct LongReadOptions {
    /// The byte to start the read at. Must be less than the length of the
    /// value.
    uint16_t offset = 0;

    /// The maximum number of bytes to read.
    uint16_t max_bytes = kMaxValueLength;
  };

  /// Represents the supported write modes for writing characteristics &
  /// descriptors to the server.
  enum class WriteMode : uint8_t {
    /// Wait for a response from the server before returning but do not verify
    /// the echo response. Supported both for characteristics and descriptors.
    kDefault,

    /// Every value blob is verified against an echo response from the server.
    /// The procedure is aborted if a value blob has not been reliably delivered
    /// to the peer. Only supported for characteristics.
    kReliable,

    /// Delivery will not be confirmed before returning. Writing without a
    /// response is only supported for short characteristics with the
    /// `WRITE_WITHOUT_RESPONSE` property. The value must fit into a single
    /// message. It is guaranteed that at least 20 bytes will fit into a single
    /// message. If the value does not fit, a `kFailure` error will be produced.
    /// The value will be written at offset 0. Only supported for
    /// characteristics.
    kWithoutResponse,
  };

  /// Represents the supported options to write a characteristic/descriptor
  /// value to a server.
  struct WriteOptions {
    /// The mode of the write operation. For descriptors, only
    /// `WriteMode::kDefault` is supported
    WriteMode mode = WriteMode::kDefault;

    /// Request a write starting at the byte indicated.
    /// Must be 0 if `mode` is `WriteMode.kWithoutResponse`.
    uint16_t offset = 0;
  };

  virtual ~RemoteService2() = default;

  /// Poll for an Error status on this service, waking `cx` and returning
  /// Ready when there is an error condition. When an error condition is
  /// present, any previous `RemoteService2` `Waker` and `OnceReceiver`
  /// instances may or may not be woken and all other methods will be no-ops.
  /// Only 1 waker can be set at a time (additional calls will replace the
  /// existing waker).
  virtual async2::Poll<RemoteServiceError> PendError(async2::Context& cx) = 0;

  /// Asynchronously sends the characteristics in this service, up to
  /// `Vector::.max_size()`. May perform service discovery if the
  /// characteristics are not yet known.
  virtual async2::ValueFuture<Vector<Characteristic2>>
  DiscoverCharacteristics() = 0;

  /// Reads characteristics and descriptors with the specified type. This method
  /// is useful for reading values before discovery has completed, thereby
  /// reducing latency.
  /// @param uuid The UUID of the characteristics/descriptors to read.
  /// @return The result of the read. Results may be empty if no matching values
  /// are read. If reading a value results in a permission error, the handle and
  /// error will be included.
  ///
  /// This may fail with the following errors:
  /// - `kInvalidParameters`: if `uuid` refers to an internally reserved
  /// descriptor type (e.g. the Client Characteristic Configuration descriptor).
  /// - `kTooManyResults`: More results were read than can fit in the Vector.
  /// Consider reading characteristics/descriptors individually after performing
  /// discovery.
  /// - `kFailure`: The server returned an error not specific to a single
  /// result.
  virtual async2::ValueFuture<
      std::optional<pw::expected<Vector<ReadByTypeResult, 5>, Error>>>
  ReadByType(Uuid uuid) = 0;

  /// Reads the value of a characteristic.
  /// @param handle The handle of the characteristic to be read.
  /// @param options If null, a short read will be performed, which may be
  /// truncated to what fits in a single message (at least 22 bytes). If long
  /// read options are present, performs a long read with the indicated options.
  /// @return The result of the read and the value of the characteristic if
  /// successful.
  /// Returns the following errors:
  /// - kInvalidHandle `handle` is invalid.
  /// - kInvalidParameters `options` is invalid.
  /// - kReadNotPermitted The server rejected the request.
  /// - kInsufficient* The server rejected the request.
  /// - kApplicationError* An application error was returned by the GATT
  ///     profile.
  /// - kFailure The server returned an error not covered by the above.
  virtual async2::OptionalValueFuture<pw::expected<ReadValue, Error>>
  ReadCharacteristic(Handle handle, std::optional<LongReadOptions> options) = 0;

  /// Writes `value` to the characteristic with `handle` using the provided
  /// `options`.
  ///
  /// @param handle Handle of the characteristic to be written to
  /// @param value The value to be written.
  /// @param options Options that apply to the write.
  /// @return A result is returned when a response to the write is
  ///     received. For WriteWithoutResponse, this is set as soon as the write
  ///     is sent.
  /// Returns the following errors:
  /// - kInvalidHandle `handle` is invalid.
  /// - kInvalidParameters`options is invalid.
  /// - kWriteNotPermitted The server rejected the request.
  /// - kInsufficient* The server rejected the request.
  /// - kApplicationError* An application error was returned by the GATT
  ///     profile.
  /// - kFailure The server returned an error not covered by the above
  /// errors.
  virtual async2::OptionalValueFuture<pw::expected<void, Error>>
  WriteCharacteristic(Handle handle,
                      pw::multibuf::MultiBuf&& value,
                      WriteOptions options) = 0;

  /// Reads the value of the characteristic descriptor with `handle` and
  /// returns it in the reply.
  /// @param handle The descriptor handle to read.
  /// @param options Options that apply to the read.
  /// @param result_sender Set to a result containing the value of the
  /// descriptor on success.
  /// @retval kInvalidHandle `handle` is invalid.
  /// @retval kInvalidParameters `options` is invalid.
  /// @retval kReadNotPermitted
  /// @retval kInsufficient* The server rejected the request.
  /// @retval kApplicationError* An application error was returned by the GATT
  ///     profile.
  /// @retval kFailure The server returned an error not covered by the above
  /// errors.
  virtual async2::OptionalValueFuture<pw::expected<ReadValue, Error>>
  ReadDescriptor(Handle handle, std::optional<LongReadOptions> options) = 0;

  /// Writes `value` to the descriptor with `handle` using the provided. It is
  /// not recommended to send additional writes while a write is already in
  /// progress.
  ///
  /// @param handle Handle of the descriptor to be written to.
  /// @param value The value to be written.
  /// @return The result upon completion of the write.
  /// Possible errors:
  /// - kInvalidHandle `handle` is invalid.
  /// - kInvalidParameters `options is invalid.
  /// - kWriteNotPermitted The server rejected the request.
  /// - kInsufficient* The server rejected the request.
  /// - kApplicationError* An application error was returned by the GATT
  ///     profile.
  /// - kFailure The server returned an error not covered by the above
  /// errors.
  virtual async2::OptionalValueFuture<pw::expected<void, Error>>
  WriteDescriptor(Handle handle, pw::multibuf::MultiBuf&& value) = 0;

  /// Subscribe to notifications & indications from the characteristic with
  /// the given `handle`.
  ///
  /// Either notifications or indications will be enabled depending on
  /// characteristic properties. Indications will be preferred if they are
  /// supported. This operation fails if the characteristic does not have the
  /// "notify" or "indicate" property.
  ///
  /// A write request will be issued to configure the characteristic for
  /// notifications/indications if it contains a Client Characteristic
  /// Configuration (CCC) descriptor. This method fails if an error occurs while
  /// writing to the descriptor.
  ///
  /// On success, `PendNotification` will return Ready when the peer sends a
  /// notification or indication. Indications are automatically confirmed.
  ///
  /// Subscriptions can be canceled with `StopNotifications`.
  ///
  /// @param handle the handle of the characteristic to subscribe to.
  /// @return The result of enabling notifications/indications.
  /// - kFailure The characteristic does not support notifications or
  ///     indications.
  /// - kInvalidHandle `handle` is invalid.
  /// - kWriteNotPermitted CCC descriptor write error.
  /// - kInsufficient*  Insufficient security properties to write to CCC
  ///     descriptor.
  virtual async2::OptionalValueFuture<pw::expected<void, Error>>
  EnableNotifications(Handle handle) = 0;

  /// After notifications have been enabled with `EnableNotifications`, this
  /// method can be used to check for notifications. This method will safely
  /// return Pending when notifications are disabled.
  /// @param handle The handle of the characteristic to await for notifications.
  /// @param cx The Context to awaken when a notification is available. Only
  ///     one Waker per handle is supported at a time (subsequent calls will
  ///     overwrite the old Waker).
  virtual async2::Poll<ReadValue> PendNotification(Handle handle,
                                                   async2::Context& cx) = 0;

  /// Stops notifications for the characteristic with the given `handle`.
  /// @return The result of disabling notifications/indications.
  /// Possible errors:
  /// - kFailure The characteristic does not support notifications or
  /// indications.
  /// - kInvalidHandle `handle` is invalid.
  /// - kWriteNotPermitted CCC descriptor write error.
  /// - Insufficient* CCC descriptor write error.
  virtual async2::OptionalValueFuture<pw::expected<void, Error>>
  StopNotifications(Handle handle) = 0;

 private:
  /// Disconnect from the remote service. This method is called by the
  /// ~RemoteService::Ptr() when it goes out of scope, the API client should
  /// never call this method.
  virtual void Disconnect() = 0;

 public:
  /// Movable `RemoteService2` smart pointer. The remote server will remain
  /// connected until the returned `RemoteService2::Ptr` is destroyed.
  using Ptr = internal::RaiiPtr<RemoteService2, &RemoteService2::Disconnect>;
};

/// Represents a GATT client that interacts with services on a GATT server.
class Client2 {
 public:
  /// Represents a remote GATT service.
  struct RemoteServiceInfo {
    /// Uniquely identifies this GATT service.
    ServiceHandle handle;

    /// Indicates whether this is a primary or secondary service.
    bool primary;

    /// The UUID that identifies the type of this service.
    /// There may be multiple services with the same UUID.
    Uuid type;
  };

  virtual ~Client2() = default;

  /// Enumerates existing services found on the peer that this object
  /// represents and notifies of modifications to services or new services
  /// thereafter. If service discovery hasn't happened yet, it may be started.
  /// To further interact with services, clients must obtain a `RemoteService2`
  /// protocol by calling `ConnectToService`.
  /// @return  Will return `Ready` with `RemoteServiceInfo` when there are
  /// services that are updated/modified. This can can be called repeatedly
  /// until `Pending` is returned to get all previously discovered services.
  virtual async2::Poll<RemoteServiceInfo> PendServiceUpdate(
      async2::Context& cx);

  /// Returns the handles of services that have been removed. Note that handles
  /// may be reused, so it is recommended to check for removed services before
  /// calling `PendServiceUpdate`. This should be called repeatedly until
  /// `Pending` is returned.
  ///
  /// @param cx Awoken when a service is removed after Pending is returned.
  virtual async2::Poll<ServiceHandle> PendServiceRemoved(async2::Context& cx);

  /// Connects to a `RemoteService2`. Only 1 connection per service is allowed.
  ///
  /// @param handle The handle of the service to connect to.
  ///
  /// @return kInvalidParameters `handle` does not correspond to a known
  /// service.
  virtual pw::expected<RemoteService2::Ptr, Error> ConnectToService(
      ServiceHandle handle) = 0;
};

}  // namespace pw::bluetooth::gatt
