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
#include "pw_async/heap_dispatcher.h"
#include "pw_bluetooth_sapphire/internal/host/att/error.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/client.h"

namespace bt::gatt::testing {

class FakeClient final : public Client {
 public:
  explicit FakeClient(pw::async::Dispatcher& pw_dispatcher);
  ~FakeClient() override = default;

  void set_server_mtu(uint16_t mtu) { server_mtu_ = mtu; }
  void set_exchange_mtu_status(att::Result<> status) {
    exchange_mtu_status_ = status;
  }

  void set_services(std::vector<ServiceData> services) {
    services_ = std::move(services);
  }

  void set_characteristics(std::vector<CharacteristicData> chrcs) {
    chrcs_ = std::move(chrcs);
  }

  void set_descriptors(std::vector<DescriptorData> descs) {
    descs_ = std::move(descs);
  }

  void set_characteristic_discovery_status(att::Result<> status) {
    chrc_discovery_status_ = status;
  }

  // If |count| is set to a non-zero value, |status| only applies to |count|th
  // request and all other requests will succeed. Otherwise, |status| applies to
  // all requests.
  void set_descriptor_discovery_status(att::Result<> status, size_t count = 0) {
    desc_discovery_status_target_ = count;
    desc_discovery_status_ = status;
  }

  att::Handle last_chrc_discovery_start_handle() const {
    return last_chrc_discovery_start_handle_;
  }

  att::Handle last_chrc_discovery_end_handle() const {
    return last_chrc_discovery_end_handle_;
  }

  att::Handle last_desc_discovery_start_handle() const {
    return last_desc_discovery_start_handle_;
  }

  att::Handle last_desc_discovery_end_handle() const {
    return last_desc_discovery_end_handle_;
  }

  size_t chrc_discovery_count() const { return chrc_discovery_count_; }
  size_t desc_discovery_count() const { return desc_discovery_count_; }

  // Sets a callback which will run when DiscoverServices gets called.
  using DiscoverServicesCallback = fit::function<att::Result<>(ServiceKind)>;
  void set_discover_services_callback(DiscoverServicesCallback callback) {
    discover_services_callback_ = std::move(callback);
  }

  // Sets a callback which will run when DiscoverDescriptors gets called.
  using DiscoverDescriptorsCallback =
      fit::function<void(att::Handle range_start,
                         att::Handle range_end,
                         DescriptorCallback desc_callback,
                         att::ResultFunction<> status_callback)>;
  void set_discover_descriptors_callback(DiscoverDescriptorsCallback callback) {
    discover_descriptors_callback_ = std::move(callback);
  }

  // Sets a callback which will run when ReadRequest gets called.
  using ReadRequestCallback = fit::function<void(att::Handle, ReadCallback)>;
  void set_read_request_callback(ReadRequestCallback callback) {
    read_request_callback_ = std::move(callback);
  }

  // Sets a callback which will run when ReadByTypeRequest gets called.
  using ReadByTypeRequestCallback = fit::function<void(const UUID& type,
                                                       att::Handle start_handle,
                                                       att::Handle end_handle,
                                                       ReadByTypeCallback)>;
  void set_read_by_type_request_callback(ReadByTypeRequestCallback callback) {
    read_by_type_request_callback_ = std::move(callback);
  }

  // Sets a callback which will run when ReadBlobRequest gets called.
  using ReadBlobRequestCallback =
      fit::function<void(att::Handle, uint16_t offset, ReadCallback)>;
  void set_read_blob_request_callback(ReadBlobRequestCallback callback) {
    read_blob_request_callback_ = std::move(callback);
  }

  // Sets a callback which will run when WriteRequest gets called.
  using WriteRequestCallback = fit::function<void(
      att::Handle, const ByteBuffer&, att::ResultFunction<>)>;
  void set_write_request_callback(WriteRequestCallback callback) {
    write_request_callback_ = std::move(callback);
  }

  using ExecutePrepareWritesCallback =
      fit::function<void(att::PrepareWriteQueue prep_write_queue,
                         ReliableMode,
                         att::ResultFunction<>)>;
  void set_execute_prepare_writes_callback(
      ExecutePrepareWritesCallback callback) {
    execute_prepare_writes_callback_ = std::move(callback);
  }

  // Sets a callback which will run when PrepareWriteRequest gets called.
  using PrepareWriteRequestCallback = fit::function<void(
      att::Handle, uint16_t offset, const ByteBuffer&, PrepareCallback)>;
  void set_prepare_write_request_callback(
      PrepareWriteRequestCallback callback) {
    prepare_write_request_callback_ = std::move(callback);
  }

  // Sets a callback which will run when ExecuteWriteRequest gets called.
  using ExecuteWriteRequestCallback =
      fit::function<void(att::ExecuteWriteFlag flag, att::ResultFunction<>)>;
  void set_execute_write_request_callback(
      ExecuteWriteRequestCallback callback) {
    execute_write_request_callback_ = std::move(callback);
  }

  // Sets a callback which will run when WriteWithoutResponse gets called.
  using WriteWithoutResponseCallback = fit::function<void(
      att::Handle, const ByteBuffer&, att::ResultFunction<>)>;
  void set_write_without_rsp_callback(WriteWithoutResponseCallback callback) {
    write_without_rsp_callback_ = std::move(callback);
  }

  // Emulates the receipt of a notification or indication PDU.
  void SendNotification(bool indicate,
                        att::Handle handle,
                        const ByteBuffer& value,
                        bool maybe_truncated);

  // Methods to obtain a weak pointer to both FakeClient and the base class
  // types.
  Client::WeakPtr GetWeakPtr() override { return weak_self_.GetWeakPtr(); }
  using WeakPtr = WeakSelf<FakeClient>::WeakPtr;
  FakeClient::WeakPtr AsFakeWeakPtr() { return weak_fake_.GetWeakPtr(); }

  // Client overrides:
  uint16_t mtu() const override;

 private:
  // Client overrides:
  void ExchangeMTU(MTUCallback callback) override;
  void DiscoverServices(ServiceKind kind,
                        ServiceCallback svc_callback,
                        att::ResultFunction<> status_callback) override;
  void DiscoverServicesInRange(ServiceKind kind,
                               att::Handle start,
                               att::Handle end,
                               ServiceCallback svc_callback,
                               att::ResultFunction<> status_callback) override;
  void DiscoverCharacteristics(att::Handle range_start,
                               att::Handle range_end,
                               CharacteristicCallback chrc_callback,
                               att::ResultFunction<> status_callback) override;
  void DiscoverDescriptors(att::Handle range_start,
                           att::Handle range_end,
                           DescriptorCallback desc_callback,
                           att::ResultFunction<> status_callback) override;
  void DiscoverServicesWithUuids(ServiceKind kind,
                                 ServiceCallback svc_callback,
                                 att::ResultFunction<> status_callback,
                                 std::vector<UUID> uuids) override;
  void DiscoverServicesWithUuidsInRange(ServiceKind kind,
                                        att::Handle start,
                                        att::Handle end,
                                        ServiceCallback svc_callback,
                                        att::ResultFunction<> status_callback,
                                        std::vector<UUID> uuids) override;
  void ReadRequest(att::Handle handle, ReadCallback callback) override;
  void ReadByTypeRequest(const UUID& type,
                         att::Handle start_handle,
                         att::Handle end_handle,
                         ReadByTypeCallback callback) override;
  void ReadBlobRequest(att::Handle handle,
                       uint16_t offset,
                       ReadCallback callback) override;
  void WriteRequest(att::Handle handle,
                    const ByteBuffer& value,
                    att::ResultFunction<> callback) override;
  void ExecutePrepareWrites(att::PrepareWriteQueue prep_write_queue,
                            ReliableMode reliable_mode,
                            att::ResultFunction<> callback) override;
  void PrepareWriteRequest(att::Handle handle,
                           uint16_t offset,
                           const ByteBuffer& part_value,
                           PrepareCallback callback) override;
  void ExecuteWriteRequest(att::ExecuteWriteFlag flag,
                           att::ResultFunction<> callback) override;
  void WriteWithoutResponse(att::Handle handle,
                            const ByteBuffer& value,
                            att::ResultFunction<> callback) override;
  void SetNotificationHandler(NotificationCallback callback) override;

  // All callbacks will be posted on this dispatcher to emulate asynchronous
  // behavior.
  pw::async::HeapDispatcher heap_dispatcher_;

  // Value to return for MTU exchange.
  uint16_t server_mtu_ = att::kLEMinMTU;

  // Data used for DiscoveryPrimaryServices().
  std::vector<ServiceData> services_;

  // Fake status values to return for GATT procedures.
  att::Result<> exchange_mtu_status_ = fit::ok();
  att::Result<> primary_service_discovery_status_ = fit::ok();
  att::Result<> secondary_service_discovery_status_ = fit::ok();
  att::Result<> chrc_discovery_status_ = fit::ok();

  size_t desc_discovery_status_target_ = 0;
  att::Result<> desc_discovery_status_ = fit::ok();

  // Data used for DiscoverCharacteristics().
  std::vector<CharacteristicData> chrcs_;
  att::Handle last_chrc_discovery_start_handle_ = 0;
  att::Handle last_chrc_discovery_end_handle_ = 0;
  size_t chrc_discovery_count_ = 0;

  // Data used for DiscoverDescriptors().
  std::vector<DescriptorData> descs_;
  att::Handle last_desc_discovery_start_handle_ = 0;
  att::Handle last_desc_discovery_end_handle_ = 0;
  size_t desc_discovery_count_ = 0;

  DiscoverServicesCallback discover_services_callback_;
  DiscoverDescriptorsCallback discover_descriptors_callback_;
  ReadRequestCallback read_request_callback_;
  ReadByTypeRequestCallback read_by_type_request_callback_;
  ReadBlobRequestCallback read_blob_request_callback_;
  WriteRequestCallback write_request_callback_;
  ExecutePrepareWritesCallback execute_prepare_writes_callback_;
  PrepareWriteRequestCallback prepare_write_request_callback_;
  ExecuteWriteRequestCallback execute_write_request_callback_;

  WriteWithoutResponseCallback write_without_rsp_callback_;
  NotificationCallback notification_callback_;

  WeakSelf<Client> weak_self_;
  WeakSelf<FakeClient> weak_fake_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(FakeClient);
};

}  // namespace bt::gatt::testing
