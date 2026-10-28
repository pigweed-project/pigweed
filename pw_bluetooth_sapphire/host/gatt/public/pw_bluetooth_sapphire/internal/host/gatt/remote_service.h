// Copyright 2023 The Pigweed Authors
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
#include <lib/fit/function.h>
#include <lib/fit/result.h>

#include <optional>

#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/client.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/remote_characteristic.h"

namespace bt::gatt {

class RemoteService;

using ServiceList = std::vector<WeakSelf<RemoteService>::WeakPtr>;

// Callback type invoked when GATT services are removed, added, or modified.
// `added` and `modified` are not combined into `updated` for flexibility and
// debuggability. Modified service handles are not included in `removed`. NOTE:
// `removed` services should be handled first because they may share handles
// with `added` services.
using RemoteServiceWatcher = fit::function<void(
    std::vector<att::Handle> removed, ServiceList added, ServiceList modified)>;

using ServiceListCallback = fit::function<void(att::Result<>, ServiceList)>;

using DescriptorMap = std::map<DescriptorHandle, DescriptorData>;
using CharacteristicMap =
    std::map<CharacteristicHandle,
             std::pair<CharacteristicData, DescriptorMap>>;

namespace internal {
class RemoteServiceManager;
}  // namespace internal

// Represents the state of a GATT service that was discovered on a remote
// device. Clients can interact with a remote GATT service by obtaining a
// RemoteService object from the GATT system.
class RemoteService final {
 public:
  // In production, a RemoteService should only be constructed by a
  // RemoteServiceManager. The constructor and destructor are made available for
  // testing.
  RemoteService(const ServiceData& service_data, Client::WeakPtr client);
  ~RemoteService();

  // If true, a Service Changed notification for this service was received. This
  // service may no longer exist or may have been modified (and so no writes
  // should be performed upon destruction).
  void set_service_changed(bool service_changed) {
    service_changed_ = service_changed;
  }

  const ServiceData& info() const { return service_data_; }

  // Returns the service range start handle. This is used to uniquely identify
  // this service.
  att::Handle handle() const { return service_data_.range_start; }

  // Returns the service UUID.
  const UUID& uuid() const { return service_data_.type; }

  // The current ATT_MTU.
  uint16_t att_mtu() const { return client_->mtu(); }

  // Adds a handler which will be called when this service gets removed.
  // Returns false if the service was already shut down.
  bool AddRemovedHandler(fit::closure handler);

  // Returns true if all contents of this service have been discovered. This is
  // primarily intended for unit tests. Clients should not rely on this and use
  // DiscoverCharacteristics() to guarantee discovery.
  bool IsDiscovered() const;

  // Performs characteristic discovery and reports the result asynchronously in
  // |callback|. Returns the cached results if characteristics were already
  // discovered.
  //
  // If this service is destroyed while discovery is pending (e.g. because a
  // Service Changed indication modified or removed it, or the connection is
  // being torn down), |callback| is called synchronously from the destructor
  // with HostError::kFailed. By then, this service's WeakPtrs are no longer
  // alive and its removed handlers have already run.
  using CharacteristicCallback =
      fit::function<void(att::Result<>, const CharacteristicMap&)>;
  void DiscoverCharacteristics(CharacteristicCallback callback);

  // Sends a read request to the characteristic with the given identifier. Fails
  // if characteristics have not been discovered.
  // |maybe_truncated| indicates whether the full value might be longer than the
  // reported value.
  using ReadValueCallback = fit::function<void(
      att::Result<>, const ByteBuffer&, bool maybe_truncated)>;
  void ReadCharacteristic(CharacteristicHandle id, ReadValueCallback callback);

  // Performs the "Read Long Characteristic Values" procedure which allows
  // characteristic values larger than the ATT_MTU to be read over multiple
  // requests.
  //
  // The read will start at |offset| and will return at most |max_bytes| octets.
  // The resulting value will be returned via |callback|.
  // The value of |maybe_truncated| reported to the callback indicates whether
  // the full value may be larger than the reported value. This is only possible
  // if |max_bytes| are read, and |max_bytes| is less than
  // att::kMaxAttributeValueLength.
  void ReadLongCharacteristic(CharacteristicHandle id,
                              uint16_t offset,
                              size_t max_bytes,
                              ReadValueCallback callback);

  // Sends a read by type request for attribute values in this service with the
  // given |type| and returns read values via |callback|. If no matching
  // attributes are found, the callback status will indicate success and the
  // vector of values will be empty.
  //
  // If a permission error occurs for an attribute, the error and handle of the
  // attribute that caused the error will be included in the results and the
  // overall status will indicate success. If a general error occurs, the status
  // will indicate the error and no results will be returned.
  //
  // |type| must be the UUID of a characteristic or descriptor value, NOT an
  // internal GATT UUID such as a service or characteristic declaration (the
  // callback will be invoked with an error in this case).
  //
  // NOTE: The values returned may be truncated, as indicated by
  // ReadByTypeResult.maybe_truncated. ReadCharacteristic(),
  // ReadLongCharacteristic(), ReadDescriptor(), and ReadLongDescriptor() should
  // be used to read complete values.
  struct ReadByTypeResult {
    CharacteristicHandle handle;
    fit::result<att::ErrorCode, ByteBufferPtr> result;
    bool maybe_truncated;
  };
  using ReadByTypeCallback =
      fit::function<void(att::Result<>, std::vector<ReadByTypeResult>)>;
  void ReadByType(const UUID& type, ReadByTypeCallback callback);

  // Sends a write request to the characteristic with the given identifier.
  //
  // TODO(armansito): Add a ByteBuffer version.
  void WriteCharacteristic(CharacteristicHandle id,
                           std::vector<uint8_t> value,
                           att::ResultFunction<> callback);

  // Sends a write request to the characteristic with the given identifier at
  // the given offset, will write over multiple requests if needed. Fails if
  // characteristics have not been discovered.
  //
  // TODO(armansito): Add a ByteBuffer version.
  void WriteLongCharacteristic(CharacteristicHandle id,
                               uint16_t offset,
                               std::vector<uint8_t> value,
                               ReliableMode reliable_mode,
                               att::ResultFunction<> callback);

  // Sends a "Write Without Response" to the characteristic with the given
  // identifier. Fails if characteristics have not been discovered.
  void WriteCharacteristicWithoutResponse(CharacteristicHandle id,
                                          std::vector<uint8_t> value,
                                          att::ResultFunction<> cb);

  // Performs the "Read Characteristic Descriptors" procedure (v5.0, Vol 3, Part
  // G, 4.12.1).
  // The callback parameter |maybe_truncated| indicates whether the full value
  // might be longer than the reported value.
  void ReadDescriptor(DescriptorHandle id, ReadValueCallback callback);

  // Performs the "Read Long Characteristic Descriptors" procedure (v5.0, Vol 3,
  // Part G, 4.12.2).
  // The callback parameter |maybe_truncated| indicates whether the full value
  // may be larger than the reported value. This is only possible if |max_bytes|
  // are read, and |max_bytes| is less than att::kMaxAttributeValueLength.
  void ReadLongDescriptor(DescriptorHandle id,
                          uint16_t offset,
                          size_t max_bytes,
                          ReadValueCallback callback);

  // Performs the "Write Characteristic Descriptors" procedure (v5.0, Vol 3,
  // Part G, 4.12.3).
  //
  // TODO(armansito): Add a ByteBuffer version.
  void WriteDescriptor(DescriptorHandle id,
                       std::vector<uint8_t> value,
                       att::ResultFunction<> callback);

  // Performs the "Write Long Characteristic Descriptors" procedure (v5.0, Vol
  // 3, Part G, 4.12.4).
  //
  // TODO(armansito): Add a ByteBuffer version.
  void WriteLongDescriptor(DescriptorHandle id,
                           uint16_t offset,
                           std::vector<uint8_t> value,
                           att::ResultFunction<> callback);

  // Subscribe to characteristic handle/value notifications or indications
  // from the characteristic with the given identifier. Either notifications or
  // indications will be enabled depending on the characteristic properties.
  //
  // This method can be called more than once to register multiple subscribers.
  // The remote Client Characteristic Configuration descriptor will be written
  // only if this is called for the first subscriber.
  //
  // |status_callback| will be called with the status of the operation. On
  // success, a |handler_id| will be returned that can be used to unregister the
  // handler.
  //
  // On success, notifications will be delivered to |callback|.
  using ValueCallback = RemoteCharacteristic::ValueCallback;
  using NotifyStatusCallback = RemoteCharacteristic::NotifyStatusCallback;
  void EnableNotifications(CharacteristicHandle id,
                           ValueCallback callback,
                           NotifyStatusCallback status_callback);

  // Disables characteristic notifications for the given |handler_id| previously
  // obtained via EnableNotifications. The value of the Client Characteristic
  // Configuration descriptor will be cleared if no subscribers remain.
  void DisableNotifications(CharacteristicHandle characteristic_id,
                            IdType handler_id,
                            att::ResultFunction<> status_callback);

  // Simulate receiving a notification. HandleNotification is usually called by
  // RemoteServiceManager, but tests without a RemoteServiceManager may use this
  // method.
  void HandleNotificationForTesting(att::Handle value_handle,
                                    const ByteBuffer& value,
                                    bool maybe_truncated) {
    HandleNotification(value_handle, value, maybe_truncated);
  }

  using WeakPtr = WeakSelf<RemoteService>::WeakPtr;
  RemoteService::WeakPtr GetWeakPtr() { return weak_self_.GetWeakPtr(); }

 private:
  friend class internal::RemoteServiceManager;

  // Returns a pointer to the characteristic with |id|. Returns nullptr if not
  // found.
  fit::result<Error<>> GetCharacteristic(CharacteristicHandle id,
                                         RemoteCharacteristic** out_char);

  // Returns a pointer to the characteristic descriptor with |id|. Returns
  // nullptr if not found.
  fit::result<Error<>> GetDescriptor(DescriptorHandle id,
                                     const DescriptorData** out_desc);

  // Called immediately after characteristic discovery to initiate descriptor
  // discovery.
  void StartDescriptorDiscovery();

  // Discovers the descriptors of the characteristic at |iter| and then of each
  // following characteristic, one at a time, until all are discovered or an
  // error occurs.
  void DiscoverDescriptorsFrom(
      std::map<CharacteristicHandle, RemoteCharacteristic>::iterator iter);

  // Called when descriptor discovery for the characteristic with |chrc_handle|
  // completes. |discovery_id| identifies the descriptor discovery procedure
  // that the request belongs to.
  void OnDescriptorDiscoveryComplete(uint32_t discovery_id,
                                     CharacteristicHandle chrc_handle,
                                     att::Result<> status);

  // Completes all pending characteristic discovery requests.
  void CompleteCharacteristicDiscovery(att::Result<> status);

  // Breaks Long Write requests down into a PrepareWriteQueue, then enqueues
  // for the client to process. Drives the "Write Long Characteristic/
  // Descriptor Values" procedure. Called by WriteCharacteristic() and
  // WriteDescriptor().
  void SendLongWriteRequest(att::Handle value_handle,
                            uint16_t offset,
                            BufferView value,
                            ReliableMode reliable_mode,
                            att::ResultFunction<> callback);

  // Helper function that drives the recursive "Read Long Characteristic Values"
  // procedure. Called by ReadLongCharacteristic().
  void ReadLongHelper(att::Handle value_handle,
                      uint16_t offset,
                      MutableByteBufferPtr buffer,
                      size_t bytes_read,
                      ReadValueCallback callback);

  // Helper function that drives the recursive "Read by Type" procedure.
  // Accumulates attribute values in |results| until either |start| > |end| or
  // an error occurs. On completion, accumulated |results| and the status are
  // passed to |callback|. Called by ReadByType().
  void ReadByTypeHelper(const UUID& type,
                        att::Handle start,
                        att::Handle end,
                        std::vector<ReadByTypeResult> results,
                        ReadByTypeCallback callback);

  // Returns true if characteristic discovery has completed.
  inline bool HasCharacteristics() const { return characteristics_discovered_; }

  // Called by RemoteServiceManager when a notification is received for one of
  // this service's characteristics.
  void HandleNotification(att::Handle value_handle,
                          const ByteBuffer& value,
                          bool maybe_truncated);

  ServiceData service_data_;

  // The GATT Client bearer for performing remote procedures.
  Client::WeakPtr client_;

  // Queued discovery requests.
  using PendingDiscoveryList = std::vector<CharacteristicCallback>;
  PendingDiscoveryList pending_discov_reqs_;

  // The known characteristics of this service, keyed (and therefore ordered) by
  // characteristic value handle. If not |characteristics_discovered_|, this may
  // contain a partial list of characteristics stored during the discovery
  // process.
  std::map<CharacteristicHandle, RemoteCharacteristic> characteristics_;

  // True once characteristic discovery (including descriptor discovery) has
  // completed successfully.
  bool characteristics_discovered_ = false;

  // Identifies the current descriptor discovery procedure. Incremented each
  // time descriptor discovery starts so that results from an earlier procedure
  // are ignored.
  uint32_t descriptor_discovery_id_ = 0;

  // True while DiscoverDescriptorsFrom() is waiting for a characteristic's
  // descriptor discovery call to return. Used to continue discovery in a loop,
  // rather than recursively, when a request completes synchronously.
  bool in_descriptor_discovery_loop_ = false;

  // The characteristic whose descriptor discovery completed successfully while
  // inside DiscoverDescriptorsFrom(). Discovery continues with the
  // characteristic after it.
  std::optional<CharacteristicHandle> last_sync_descriptor_discovery_;

  // Indicates whether the service was changed, as indicated by a service
  // changed notification.
  bool service_changed_ = false;

  // Called by ShutDown().
  std::vector<fit::callback<void()>> rm_handlers_;

  WeakSelf<RemoteService> weak_self_{this};

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(RemoteService);
};

}  // namespace bt::gatt
