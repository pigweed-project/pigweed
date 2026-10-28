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

#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/att/bearer.h"
#include "pw_bluetooth_sapphire/internal/host/att/write_queue.h"
#include "pw_bluetooth_sapphire/internal/host/common/uuid.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/gatt_defs.h"

namespace bt::gatt {

// Implements GATT client-role procedures. A client operates over a single ATT
// data bearer. Client objects are solely used to map GATT procedures to ATT
// protocol methods and do not maintain service state.
class Client {
 public:
  // Constructs a new Client. |bearer| must outlive this object.
  static std::unique_ptr<Client> Create(att::Bearer::WeakPtr bearer);

  virtual ~Client() = default;

  // Returns a weak pointer to this Client. The weak pointer should be checked
  // on the data bearer's thread only as Client can only be accessed on that
  // thread.
  using WeakPtr = WeakSelf<Client>::WeakPtr;
  virtual Client::WeakPtr GetWeakPtr() = 0;

  // Returns the current ATT MTU.
  virtual uint16_t mtu() const = 0;

  // Initiates an MTU exchange and adjusts the bearer's MTU as outlined in Core
  // Spec v5.3 Vol. 3 Part F 3.4.2. The request will be made using the
  // locally-preferred MTU.
  //
  // Upon successful exchange, the bearer will be updated to use the resulting
  // MTU and |callback| will be notified with the resulting MTU.
  //
  // MTU exchange support is optional per v5.3 Vol. 3 Part F Table 4.1, so if
  // the exchange fails because the peer doesn't support it, the bearer's MTU
  // will be |kLEMinMTU| and initialization should proceed. In this case,
  // |mtu_result| will be |att::Error(att::kRequestNotSupported)|.
  //
  // If the MTU exchange otherwise fails, |mtu_result| will be an error and the
  // bearer's MTU will not change.
  using MTUCallback = fit::callback<void(att::Result<uint16_t> mtu_result)>;
  virtual void ExchangeMTU(MTUCallback callback) = 0;

  // Performs a modified version of the "Discover All Primary Services"
  // procedure defined in v5.0, Vol 3, Part G, 4.4.1, genericized over primary
  // and secondary services.
  //
  // |service_callback| is run for each discovered service in order of start
  // handle. |status_callback| is run with the status of the operation.
  //
  // The |kind| parameter can be used to control whether primary or secondary
  // services get discovered.
  //
  // NOTE: |service_callback| will be called asynchronously as services are
  // discovered so a caller can start processing the results immediately while
  // the procedure is in progress. Since discovery usually occurs over multiple
  // ATT transactions, it is possible for |status_callback| to be called with an
  // error even if some services have been discovered. It is up to the client
  // to clear any cached state in this case.
  //
  // The return value of |service_callback| only controls whether the procedure
  // continues; errors are never delivered to |service_callback|. Returning
  // fit::ok() continues discovery. Returning an error stops it: no further
  // requests are sent, no further services are reported, and that error
  // becomes the result of the procedure, which is reported (unchanged) through
  // |status_callback| like any other error.
  using ServiceCallback = fit::function<att::Result<>(const ServiceData&)>;
  virtual void DiscoverServices(ServiceKind kind,
                                ServiceCallback svc_callback,
                                att::ResultFunction<> status_callback) = 0;

  // Same as DiscoverServices, but only discovers services in the range
  // [range_start, range_end]. |range_start| must be less than |range_end|.
  virtual void DiscoverServicesInRange(
      ServiceKind kind,
      att::Handle range_start,
      att::Handle range_end,
      ServiceCallback svc_callback,
      att::ResultFunction<> status_callback) = 0;

  // Performs the "Discover All Primary Services by UUID" procedure defined in
  // v5.0, Vol 3, Part G, 4.4.2. |service_callback| is run for each discovered
  // service in order of start handle. |status_callback| is run with the result
  // of the operation.
  //
  // The |kind| parameter can be used to control whether primary or secondary
  // services get discovered.
  //
  // NOTE: |service_callback| will be called asynchronously as services are
  // discovered so a caller can start processing the results immediately while
  // the procedure is in progress. Since discovery usually occurs over multiple
  // ATT transactions, it is possible for |status_callback| to be called with an
  // error even if some services have been discovered. It is up to the client
  // to clear any cached state in this case.
  virtual void DiscoverServicesWithUuids(ServiceKind kind,
                                         ServiceCallback svc_callback,
                                         att::ResultFunction<> status_callback,
                                         std::vector<UUID> uuids) = 0;

  // Same as DiscoverServicesWithUuids, but only discovers services in the range
  // [range_start, range_end]. |range_start| must be <= |range_end|.
  virtual void DiscoverServicesWithUuidsInRange(
      ServiceKind kind,
      att::Handle range_start,
      att::Handle range_end,
      ServiceCallback svc_callback,
      att::ResultFunction<> status_callback,
      std::vector<UUID> uuids) = 0;

  // Performs the "Discover All Characteristics of a Service" procedure defined
  // in v5.0, Vol 3, Part G, 4.6.1.
  using CharacteristicCallback = fit::function<void(const CharacteristicData&)>;
  virtual void DiscoverCharacteristics(
      att::Handle range_start,
      att::Handle range_end,
      CharacteristicCallback chrc_callback,
      att::ResultFunction<> status_callback) = 0;

  // Performs the "Discover All Characteristic Descriptors" procedure defined in
  // Vol 3, Part G, 4.7.1.
  using DescriptorCallback = fit::function<void(const DescriptorData&)>;
  virtual void DiscoverDescriptors(att::Handle range_start,
                                   att::Handle range_end,
                                   DescriptorCallback desc_callback,
                                   att::ResultFunction<> status_callback) = 0;

  // Sends an ATT Read Request with the requested attribute |handle| and returns
  // the resulting value in |callback|. This can be used to send a (short) read
  // request to any attribute. (Vol 3, Part F, 3.4.4.3).
  //
  // Reports the status of the procedure and the resulting value in |callback|.
  // Returns an empty buffer if the status is an error.
  // If the attribute value might be longer than the reported value,
  // |maybe_truncated| will be true. This can happen if the MTU is too small to
  // read the complete value. ReadBlobRequest() should be used to read the
  // complete value.
  using ReadCallback = fit::function<void(
      att::Result<>, const ByteBuffer&, bool maybe_truncated)>;
  virtual void ReadRequest(att::Handle handle, ReadCallback callback) = 0;

  // Sends an ATT Read by Type Request with the requested attribute handle range
  // |start_handle| to |end_handle| (inclusive), and returns a ReadByTypeResult
  // containing either the handle-value pairs successfully read, or an error
  // status and related handle (if any).
  //
  // Attribute values may be truncated, as indicated by
  // ReadByTypeValue.maybe_truncated. ReadRequest() or ReadBlobRequest() should
  // be used to read the complete values. (Core Spec v5.2, Vol 3, Part
  // F, 3.4.4.2)
  //
  // The attributes returned will be the attributes with the lowest handles in
  // the handle range in ascending order, and may not include all matching
  // attributes. To read all attributes, make additional requests with an
  // updated |start_handle| until an error response with error code "Attribute
  // Not Found" is received, or the entire handle range has been read. (Core
  // Spec v5.2, Vol 3, Part F, 3.4.4.1).
  struct ReadByTypeValue {
    att::Handle handle;
    // The underlying value buffer is only valid for the duration of |callback|.
    // Callers must make a copy if they need to retain the buffer.
    BufferView value;
    // True if |value| might be truncated.
    bool maybe_truncated;
  };
  struct ReadByTypeError {
    att::Error error;
    // Only some att protocol errors include a handle. This handle can either be
    // |start_handle| or the handle of the attribute causing the error,
    // depending on the error (Core Spec v5.2, Vol 3, Part F, 3.4.4.1).
    std::optional<att::Handle> handle;
  };
  using ReadByTypeResult =
      fit::result<ReadByTypeError, std::vector<ReadByTypeValue>>;
  using ReadByTypeCallback = fit::function<void(ReadByTypeResult)>;
  virtual void ReadByTypeRequest(const UUID& type,
                                 att::Handle start_handle,
                                 att::Handle end_handle,
                                 ReadByTypeCallback callback) = 0;

  // Sends an ATT Read Blob request with the requested attribute |handle| and
  // returns the result value in |callback|. This can be called multiple times
  // to read the value of a characteristic that is larger than (ATT_MTU - 1).
  // If the attribute value might be longer than the reported value, the
  // |maybe_truncated| callback parameter will be true. (Vol 3, Part G, 4.8.3)
  virtual void ReadBlobRequest(att::Handle handle,
                               uint16_t offset,
                               ReadCallback callback) = 0;

  // Sends an ATT Write Request with the requested attribute |handle| and
  // |value|. This can be used to send a write request to any attribute.
  // (Vol 3, Part F, 3.4.5.1).
  //
  // Reports the status of the procedure in |callback|.
  // HostError::kPacketMalformed is returned if |value| is too large to write in
  // a single ATT request.
  virtual void WriteRequest(att::Handle handle,
                            const ByteBuffer& value,
                            att::ResultFunction<> callback) = 0;

  // Adds a new PrepareWriteQueue to be sent. This sends multiple
  // PrepareWriteRequests followed by an ExecuteWriteRequest. The request will
  // be enqueued if there are any pending.
  //
  // Reports the status of the procedure in |callback|.
  // HostError::kPacketMalformed is returned if any writes in the queue are too
  // large to write in a single ATT request.
  virtual void ExecutePrepareWrites(att::PrepareWriteQueue prep_write_queue,
                                    ReliableMode reliable_mode,
                                    att::ResultFunction<> callback) = 0;

  // Sends an ATT Prepare Write Request with the requested attribute |handle|,
  // |offset|, and |part_value|. This can be used to send a long write request
  // to any attribute by following with 0-N prepare write requests and finally
  // an Execute Write Request.
  // (Vol 3, Part G, 4.9.4)
  //
  // Reports the status of the procedure in |callback|, along with mirroring the
  // data written to the buffer.
  // HostError::kPacketMalformed is returned if |part_value| is too large to
  // write in a single ATT request.
  using PrepareCallback = fit::function<void(att::Result<>, const ByteBuffer&)>;
  virtual void PrepareWriteRequest(att::Handle handle,
                                   uint16_t offset,
                                   const ByteBuffer& part_value,
                                   PrepareCallback callback) = 0;

  // Following a series of Prepare Write Requests, this will write the series if
  // the input is kWritePending, or cancel all pending if it is kCancelAll.
  // (Vol 3, Part G, 4.9.4)
  //
  // Reports the status of the procedure in |callback|.
  virtual void ExecuteWriteRequest(att::ExecuteWriteFlag flag,
                                   att::ResultFunction<> callback) = 0;

  // Sends an ATT Write Command with the requested |handle| and |value|. This
  // should only be used with characteristics that support the "Write Without
  // Response" property.
  //
  // Reports the status of the procedure in |callback|.
  virtual void WriteWithoutResponse(att::Handle handle,
                                    const ByteBuffer& value,
                                    att::ResultFunction<> callback) = 0;

  // Assigns a callback that will be called when a notification or indication
  // PDU is received.
  using NotificationCallback = fit::function<void(bool indication,
                                                  att::Handle handle,
                                                  const ByteBuffer& value,
                                                  bool maybe_truncated)>;
  virtual void SetNotificationHandler(NotificationCallback handler) = 0;
};

}  // namespace bt::gatt
