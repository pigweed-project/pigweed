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

#include "pw_async2/dispatcher.h"
#include "pw_async2/value_future.h"
#include "pw_bluetooth/gatt/error.h"
#include "pw_bluetooth/gatt/types.h"
#include "pw_bluetooth/internal/raii_ptr.h"
#include "pw_bluetooth/types.h"
#include "pw_multibuf/multibuf.h"
#include "pw_result/expected.h"
#include "pw_span/span.h"

namespace pw::bluetooth::gatt {

/// @module{pw_bluetooth}

/// Interface for serving a local GATT service. This is implemented by the API
/// client.
class LocalServiceDelegate2 {
 public:
  virtual ~LocalServiceDelegate2() = default;

  /// Called when there is a fatal error related to this service that forces the
  /// service to close. LocalServiceDelegate methods will no longer be called.
  /// This invalidates the associated LocalService. It is OK to destroy both
  /// `LocalServiceDelegate` and the associated `LocalService::Ptr` from within
  /// this method.
  virtual void OnError(Error error) = 0;

  /// This notifies the current configuration of a particular
  /// characteristic/descriptor for a particular peer. It will be called when
  /// the peer GATT client changes the configuration.
  ///
  /// The Bluetooth stack maintains the state of each peer's configuration
  /// across reconnections. As such, this method will be called with both
  /// `notify` and `indicate` set to false for each characteristic when a peer
  /// disconnects. Also, when a peer reconnects this method will be called again
  /// with the persisted state of the newly-connected peer's configuration.
  /// However, clients should not rely on this state being persisted
  /// indefinitely by the Bluetooth stack.
  ///
  /// @param peer_id The PeerId of the GATT client associated with this
  /// particular CCC.
  /// @param handle The handle of the characteristic associated with the
  /// `notify` and `indicate` parameters.
  /// @param notify True if the client has enabled notifications, false
  /// otherwise.
  /// @param indicate True if the client has enabled indications, false
  /// otherwise.
  virtual void CharacteristicConfiguration(PeerId peer_id,
                                           Handle handle,
                                           bool notify,
                                           bool indicate) = 0;

  /// Called when a peer requests to read the value of a characteristic or
  /// descriptor. It is guaranteed that the peer satisfies the permissions
  /// associated with this attribute.
  ///
  /// @param peer_id The PeerId of the GATT client making the read request.
  /// @param handle The handle of the requested descriptor/characteristic.
  /// @param offset The offset at which to start reading the requested value.
  /// @return Returns the value of the characteristic on success, or an Error on
  /// failure. The value will be truncated to fit in the MTU if necessary.
  virtual async2::ValueFuture<pw::expected<multibuf::MultiBuf, Error>>
  ReadValue(PeerId peer_id, Handle handle, uint32_t offset) = 0;

  /// Called when a peer issues a request to write the value of a characteristic
  /// or descriptor. It is guaranteed that the peer satisfies the permissions
  /// associated with this attribute.
  ///
  /// @param peer_id The PeerId of the GATT client making the write request.
  /// @param handle The handle of the requested descriptor/characteristic.
  /// @param offset The offset at which to start writing the requested value. If
  /// the offset is 0, any existing value should be overwritten by the new
  /// value. Otherwise, the existing value between `offset:(offset +
  /// len(value))` should be changed to `value`.
  /// @param value The new value for the descriptor/characteristic.
  /// @return The result of the write.
  virtual async2::ValueFuture<pw::expected<void, Error>> WriteValue(
      PeerId peer_id,
      Handle handle,
      uint32_t offset,
      multibuf::MultiBuf&& value) = 0;

  /// Called when the MTU of a peer is updated. Also called for peers that are
  /// already connected when the server is published.
  ///
  /// Notifications and indications must fit in a single packet including both
  /// the 3-byte notification/indication header and the user-provided payload.
  /// If these are not used, the MTU can be safely ignored as it is intended for
  /// use cases where the throughput needs to be optimized.
  virtual void MtuUpdate(PeerId peer_id, uint16_t mtu) = 0;
};

/// Interface provided by the backend to interact with a published service.
/// LocalService is valid for the lifetime of a published GATT service. It is
/// used to control the service and send notifications/indications.
class LocalService2 {
 public:
  /// The parameters used to signal a characteristic value change from a
  /// LocalService to a peer.
  struct ValueChangedParameters {
    /// The peers peers to signal. The LocalService should respect the
    /// Characteristic Configuration associated with a peer+handle when deciding
    /// whether to signal it. If empty, all peers which configured the handle
    /// are signalled.
    pw::span<const PeerId> peer_ids;
    /// The handle of the characteristic value being signaled.
    Handle handle;
    /// The new value for the descriptor/characteristic.
    multibuf::MultiBuf value;
  };

  /// The Result type for a ValueChanged indication or notification message. The
  /// error can be locally generated for notifications and either locally or
  /// remotely generated for indications.
  using ValueChangedResult = pw::expected<void, Error>;

  virtual ~LocalService2() = default;

  /// Sends a notification to peers. Notifications should be used instead of
  /// indications when the service does *not* require peer confirmation of the
  /// update.
  ///
  /// Notifications should not be sent to peers which have not enabled
  /// notifications on a particular characteristic or that have disconnected.
  /// If notifications are sent, they will not be propagated and the
  /// `result_sender` will be set to an error condition. The Bluetooth stack
  /// will track this configuration for the lifetime of the service.
  ///
  /// The maximum size of the `parameters.value` field depends on the Maximum
  /// Transmission Unit (MTU) negotiated with the peer. A 3-byte header plus the
  /// value contents must fit in a packet of MTU bytes.
  ///
  /// @param parameters The parameters associated with the changed
  /// characteristic.
  /// @return The result is returned when the notification has been sent to
  /// all peers or an error is produced when trying to send the notification to
  /// any of the peers. This value is only set only once when all associated
  /// work is done, if the implementation wishes to receive a call on a
  /// per-peer basis, they should send this event with a single PeerId in
  /// `parameters.peer_ids`. Additional values should not be notified until
  /// this notification completes.
  virtual async2::ValueFuture<ValueChangedResult> NotifyValue(
      ValueChangedParameters&& parameters) = 0;

  /// Sends an indication to peers. Indications should be used instead of
  /// notifications when the service *does* require peer confirmation of the
  /// update.
  ///
  /// Indications should not be sent to peers which have not enabled indications
  /// on a particular characteristic. If they are sent, they will not be
  /// propagated and the `result_sender` will be set to an error condition. The
  /// Bluetooth stack will track this configuration for the lifetime of the
  /// service.
  ///
  /// If any of the peers in `parameters.peer_ids` fails to confirm the
  /// indication within the ATT transaction timeout (30 seconds per
  /// Bluetooth 6.0 Vol. 3 Part F 3.3.3), the link between the peer and the
  /// local adapter will be closed.
  ///
  /// The maximum size of the `parameters.value` field depends on the MTU
  /// negotiated with the peer. A 3-byte header plus the value contents must fit
  /// in a packet of MTU bytes.
  ///
  /// @param parameters The parameters associated with the changed
  /// characteristic.
  /// @return When all the peers listed in `parameters.peer_ids`
  /// have confirmed the indication, the result is returned. If the
  /// implementation wishes to receive indication confirmations on a per-peer
  /// basis, they should send this event with a single PeerId in
  /// `parameters.peer_ids`. Additional values should not be indicated until
  /// this procedure completes.
  virtual async2::ValueFuture<ValueChangedResult> IndicateValue(
      ValueChangedParameters&& parameters) = 0;

 private:
  /// Unpublish the local service. This method is called by the
  /// ~LocalService::Ptr() when it goes out of scope, the API client should
  /// never call this method.
  virtual void UnpublishService() = 0;

 public:
  /// Movable LocalService smart pointer. When the LocalService::Ptr object is
  /// destroyed the service will be unpublished.
  using Ptr =
      internal::RaiiPtr<LocalService2, &LocalService2::UnpublishService>;
};

/// Interface for a GATT server that serves many GATT services.
class Server2 {
 public:
  enum class PublishServiceError {
    kInternalError = 0,

    /// The service handle provided was not unique.
    kInvalidHandle = 1,

    /// Invalid service UUID provided.
    kInvalidUuid = 2,

    /// Invalid service characteristics provided.
    kInvalidCharacteristics = 3,

    /// Invalid service includes provided.
    kInvalidIncludes = 4,
  };

  /// Parameters for registering a local GATT service.
  struct LocalServiceInfo {
    /// A unique (within a Server) handle identifying this service.
    ServiceHandle handle;

    /// Indicates whether this is a primary or secondary service.
    bool primary;

    /// The UUID that identifies the type of this service.
    /// There may be multiple services with the same UUID.
    Uuid type;

    /// The characteristics of this service.
    span<const Characteristic2> characteristics;

    /// Handles of other services that are included by this service. The
    /// included services need to already be published.
    span<const ServiceHandle> includes;
  };

  /// The Result passed by PublishService.
  using PublishServiceResult =
      pw::expected<LocalService2::Ptr, PublishServiceError>;

  virtual ~Server2() = default;

  /// Publishes the service defined by `info` and implemented by `delegate` so
  /// that it is available to all remote peers.
  ///
  /// The caller must assign distinct handles to the characteristics and
  /// descriptors listed in `info` per call to `PublishService` (handles can be
  /// reused across calls). These identifiers will be used in requests sent to
  /// `delegate`.
  ///
  /// @return On success, a `LocalService::Ptr` is returned via
  /// `result_sender`. When the `LocalService::Ptr` is destroyed or an error
  /// occurs (LocalServiceDelegate.OnError), the service will be unpublished.
  virtual async2::ValueFuture<PublishServiceResult> PublishService(
      const LocalServiceInfo& info, LocalServiceDelegate2& delegate) = 0;
};

}  // namespace pw::bluetooth::gatt
