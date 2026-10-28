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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/gatt2_remote_service_server.h"

#include <fuchsia/bluetooth/gatt2/cpp/fidl_test_base.h>
#include <pw_assert/check.h>

#include <algorithm>
#include <optional>

#include "fuchsia/bluetooth/gatt2/cpp/fidl.h"
#include "gtest/gtest.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/fake_gatt_fixture.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/remote_service.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bthost {
namespace {

namespace fbg = fuchsia::bluetooth::gatt2;

constexpr bt::PeerId kPeerId(1);

constexpr bt::att::Handle kServiceStartHandle = 0x0001;
constexpr bt::att::Handle kServiceEndHandle = 0xFFFE;
const bt::UUID kServiceUuid(uint16_t{0x180D});
const bt::UUID kCharacteristicUuid(uint16_t{0x180E});
const bt::UUID kDescriptorUuid(uint16_t{0x180F});

class Gatt2RemoteServiceServerTest : public bt::fidl::testing::FakeGattFixture {
 public:
  Gatt2RemoteServiceServerTest() = default;
  ~Gatt2RemoteServiceServerTest() override = default;

  void SetUp() override {
    {
      auto [svc, client] = fake_gatt()->AddPeerService(
          kPeerId,
          bt::gatt::ServiceData(bt::gatt::ServiceKind::PRIMARY,
                                kServiceStartHandle,
                                kServiceEndHandle,
                                kServiceUuid));
      service_ = std::move(svc);
      fake_client_ = std::move(client);
    }

    fidl::InterfaceHandle<fbg::RemoteService> handle;
    server_ = std::make_unique<Gatt2RemoteServiceServer>(
        service_, gatt()->GetWeakPtr(), kPeerId, handle.NewRequest());
    proxy_.Bind(std::move(handle));
  }

  void TearDown() override {
    // Clear any previous expectations that are based on the ATT Write Request,
    // so that write requests sent during RemoteService::ShutDown() are ignored.
    fake_client()->set_write_request_callback({});

    bt::fidl::testing::FakeGattFixture::TearDown();
  }

  pw::async::HeapDispatcher& heap_dispatcher() { return heap_dispatcher_; }

 protected:
  const bt::gatt::testing::FakeClient::WeakPtr& fake_client() const {
    PW_CHECK(fake_client_.is_alive());
    return fake_client_;
  }

  fbg::RemoteServicePtr& service_proxy() { return proxy_; }
  bt::gatt::RemoteService::WeakPtr service() { return service_; }

  void DestroyServer() { server_.reset(); }

 private:
  std::unique_ptr<Gatt2RemoteServiceServer> server_;

  fbg::RemoteServicePtr proxy_;
  bt::gatt::RemoteService::WeakPtr service_;
  bt::gatt::testing::FakeClient::WeakPtr fake_client_;
  pw::async_fuchsia::FuchsiaDispatcher pw_dispatcher_{dispatcher()};
  pw::async::HeapDispatcher heap_dispatcher_{pw_dispatcher_};

  BT_DISALLOW_COPY_ASSIGN_AND_MOVE(Gatt2RemoteServiceServerTest);
};

TEST_F(Gatt2RemoteServiceServerTest, DiscoverCharacteristics) {
  bt::gatt::Properties properties =
      static_cast<bt::gatt::Properties>(
          bt::gatt::Property::kAuthenticatedSignedWrites) |
      static_cast<bt::gatt::Properties>(
          bt::gatt::Property::kExtendedProperties);
  bt::gatt::ExtendedProperties ext_properties =
      static_cast<bt::gatt::ExtendedProperties>(
          bt::gatt::ExtendedProperty::kReliableWrite) |
      static_cast<bt::gatt::ExtendedProperties>(
          bt::gatt::ExtendedProperty::kWritableAuxiliaries);
  constexpr bt::att::Handle kCharacteristicHandle(kServiceStartHandle + 1);
  constexpr bt::att::Handle kCharacteristicValueHandle(kCharacteristicHandle +
                                                       1);
  const bt::UUID kCharacteristicUuid(uint16_t{0x0000});
  bt::gatt::CharacteristicData characteristic(properties,
                                              ext_properties,
                                              kCharacteristicHandle,
                                              kCharacteristicValueHandle,
                                              kCharacteristicUuid);
  fake_client()->set_characteristics({characteristic});

  constexpr bt::att::Handle kDescriptorHandle(kCharacteristicValueHandle + 1);
  const bt::UUID kDescriptorUuid(uint16_t{0x0001});
  bt::gatt::DescriptorData descriptor(kDescriptorHandle, kDescriptorUuid);
  fake_client()->set_descriptors({descriptor});

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_characteristic =
      fidl_characteristics->front();

  ASSERT_TRUE(fidl_characteristic.has_handle());
  EXPECT_EQ(fidl_characteristic.handle().value,
            static_cast<uint64_t>(kCharacteristicValueHandle));

  ASSERT_TRUE(fidl_characteristic.has_type());
  EXPECT_EQ(fidl_characteristic.type().value, kCharacteristicUuid.value());

  ASSERT_TRUE(fidl_characteristic.has_properties());
  EXPECT_EQ(fidl_characteristic.properties(),
            fbg::CharacteristicPropertyBits::AUTHENTICATED_SIGNED_WRITES |
                fbg::CharacteristicPropertyBits::RELIABLE_WRITE |
                fbg::CharacteristicPropertyBits::WRITABLE_AUXILIARIES);

  EXPECT_FALSE(fidl_characteristic.has_permissions());

  ASSERT_TRUE(fidl_characteristic.has_descriptors());
  ASSERT_EQ(fidl_characteristic.descriptors().size(), 1u);
  const fbg::Descriptor& fidl_descriptor =
      fidl_characteristic.descriptors().front();

  ASSERT_TRUE(fidl_descriptor.has_handle());
  EXPECT_EQ(fidl_descriptor.handle().value,
            static_cast<uint64_t>(kDescriptorHandle));

  ASSERT_TRUE(fidl_descriptor.has_type());
  EXPECT_EQ(fidl_descriptor.type().value, kDescriptorUuid.value());

  EXPECT_FALSE(fidl_descriptor.has_permissions());
}

TEST_F(Gatt2RemoteServiceServerTest, DiscoverCharacteristicsWithNoDescriptors) {
  bt::gatt::Properties properties = 0;
  bt::gatt::ExtendedProperties ext_properties = 0;
  constexpr bt::att::Handle kCharacteristicHandle(kServiceStartHandle + 1);
  constexpr bt::att::Handle kCharacteristicValueHandle(kCharacteristicHandle +
                                                       1);
  const bt::UUID kCharacteristicUuid(uint16_t{0x0000});
  bt::gatt::CharacteristicData characteristic(properties,
                                              ext_properties,
                                              kCharacteristicHandle,
                                              kCharacteristicValueHandle,
                                              kCharacteristicUuid);
  fake_client()->set_characteristics({characteristic});

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_characteristic =
      fidl_characteristics->front();
  EXPECT_FALSE(fidl_characteristic.has_descriptors());
}

TEST_F(Gatt2RemoteServiceServerTest, ReadByTypeSuccess) {
  constexpr bt::UUID kCharUuid(uint16_t{0xfefe});

  constexpr bt::att::Handle kHandle = kServiceStartHandle;
  const auto kValue = bt::StaticByteBuffer(0x00, 0x01, 0x02);
  const std::vector<bt::gatt::Client::ReadByTypeValue> kValues = {
      {kHandle, kValue.view(), /*maybe_truncated=*/false}};

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback([&](const bt::UUID& type,
                                                       bt::att::Handle start,
                                                       bt::att::Handle end,
                                                       auto callback) {
    switch (read_count++) {
      case 0:
        callback(fit::ok(kValues));
        break;
      case 1:
        callback(fit::error(bt::gatt::Client::ReadByTypeError{
            bt::att::Error(bt::att::ErrorCode::kAttributeNotFound), start}));
        break;
      default:
        FAIL();
    }
  });

  std::optional<fbg::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fuchsia::bluetooth::Uuid{kCharUuid.value()},
      [&](auto cb_result) { fidl_result = std::move(cb_result); });

  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_response());
  const auto& response = fidl_result->response();
  ASSERT_EQ(1u, response.results.size());
  const fbg::ReadByTypeResult& result0 = response.results[0];
  ASSERT_TRUE(result0.has_handle());
  EXPECT_EQ(result0.handle().value, static_cast<uint64_t>(kHandle));

  EXPECT_FALSE(result0.has_error());

  ASSERT_TRUE(result0.has_value());
  const fbg::ReadValue& read_value = result0.value();
  ASSERT_TRUE(read_value.has_handle());
  EXPECT_EQ(read_value.handle().value, static_cast<uint64_t>(kHandle));
  ASSERT_TRUE(read_value.has_maybe_truncated());
  EXPECT_FALSE(read_value.maybe_truncated());

  ASSERT_TRUE(read_value.has_value());
  const std::vector<uint8_t>& value = read_value.value();
  EXPECT_TRUE(
      ContainersEqual(bt::BufferView(value.data(), value.size()), kValue));
}

TEST_F(Gatt2RemoteServiceServerTest, ReadByTypeResultPermissionError) {
  constexpr bt::UUID kCharUuid(uint16_t{0xfefe});

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback([&](const bt::UUID& type,
                                                       bt::att::Handle start,
                                                       bt::att::Handle end,
                                                       auto callback) {
    ASSERT_EQ(0u, read_count++);
    callback(fit::error(bt::gatt::Client::ReadByTypeError{
        bt::att::Error(bt::att::ErrorCode::kInsufficientAuthorization),
        kServiceEndHandle}));
  });

  std::optional<fbg::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fuchsia::bluetooth::Uuid{kCharUuid.value()},
      [&](auto cb_result) { fidl_result = std::move(cb_result); });

  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_response());
  const auto& response = fidl_result->response();
  ASSERT_EQ(1u, response.results.size());
  const fbg::ReadByTypeResult& result0 = response.results[0];
  ASSERT_TRUE(result0.has_handle());
  EXPECT_EQ(result0.handle().value, static_cast<uint64_t>(kServiceEndHandle));
  EXPECT_FALSE(result0.has_value());
  ASSERT_TRUE(result0.has_error());
  EXPECT_EQ(fbg::Error::INSUFFICIENT_AUTHORIZATION, result0.error());
}

TEST_F(Gatt2RemoteServiceServerTest, ReadByTypeReturnsError) {
  constexpr bt::UUID kCharUuid(uint16_t{0xfefe});

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback([&](const bt::UUID& type,
                                                       bt::att::Handle start,
                                                       bt::att::Handle end,
                                                       auto callback) {
    switch (read_count++) {
      case 0:
        callback(fit::error(bt::gatt::Client::ReadByTypeError{
            bt::Error(bt::HostError::kPacketMalformed), std::nullopt}));
        break;
      default:
        FAIL();
    }
  });

  std::optional<fbg::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fuchsia::bluetooth::Uuid{kCharUuid.value()},
      [&](auto cb_result) { fidl_result = std::move(cb_result); });

  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_err());
  const auto& err = fidl_result->err();
  EXPECT_EQ(fbg::Error::UNLIKELY_ERROR, err);
}

// An allocation failure is not reported as TOO_MANY_RESULTS.
TEST_F(Gatt2RemoteServiceServerTest,
       ReadByTypeOutOfMemoryReturnsUnlikelyError) {
  constexpr bt::UUID kCharUuid(uint16_t{0xfefe});

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback(
      [&](const bt::UUID&, bt::att::Handle, bt::att::Handle, auto callback) {
        ASSERT_EQ(0u, read_count++);
        // bt::gatt::Client reports a PDU allocation failure this way.
        callback(fit::error(bt::gatt::Client::ReadByTypeError{
            bt::Error(bt::HostError::kOutOfMemory), std::nullopt}));
      });

  std::optional<fbg::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fuchsia::bluetooth::Uuid{kCharUuid.value()},
      [&](auto cb_result) { fidl_result = std::move(cb_result); });

  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_err());
  EXPECT_EQ(fbg::Error::UNLIKELY_ERROR, fidl_result->err());
}

TEST_F(Gatt2RemoteServiceServerTest, ReadByTypeInvalidUuid) {
  constexpr bt::UUID kCharUuid = bt::gatt::types::kCharacteristicDeclaration;

  fake_client()->set_read_by_type_request_callback(
      [&](const bt::UUID& type,
          bt::att::Handle start,
          bt::att::Handle end,
          auto callback) { FAIL(); });

  std::optional<fbg::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fuchsia::bluetooth::Uuid{kCharUuid.value()},
      [&](auto cb_result) { fidl_result = std::move(cb_result); });

  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_err());
  const auto& err = fidl_result->err();
  EXPECT_EQ(fbg::Error::INVALID_PARAMETERS, err);
}

TEST_F(Gatt2RemoteServiceServerTest, ReadByTypeTooManyResults) {
  constexpr bt::UUID kCharUuid(uint16_t{0xfefe});
  constexpr size_t kValueSize = 150;
  static_assert(kValueSize <= bt::att::kMaxReadByTypeValueLength);
  bt::DynamicByteBuffer value(kValueSize);
  value.Fill(0xAB);

  // Return enough results that the value payloads alone exceed the channel
  // limit (each encoded result is larger than its value payload), while
  // staying within the number of results RemoteService::ReadByType() accepts,
  // so that the FIDL message size limit is what fails the read:
  //   (65536 / 150 + 1) * 150 = 437 * 150 = 65550 > 65536, and 437 <= 512.
  constexpr size_t kValueCount =
      static_cast<size_t>(ZX_CHANNEL_MAX_MSG_BYTES) / kValueSize + 1;
  static_assert(kValueCount * kValueSize >
                static_cast<size_t>(ZX_CHANNEL_MAX_MSG_BYTES));
  static_assert(kValueCount <= bt::gatt::RemoteService::kMaxReadByTypeResults);

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback([&](const bt::UUID& type,
                                                       bt::att::Handle start,
                                                       bt::att::Handle end,
                                                       auto callback) {
    if (read_count++ == kValueCount) {
      callback(fit::error(bt::gatt::Client::ReadByTypeError{
          bt::att::Error(bt::att::ErrorCode::kAttributeNotFound), start}));
      return;
    }

    // Dispatch callback to prevent recursing too deep and breaking the stack.
    (void)heap_dispatcher().Post(
        [start, cb = std::move(callback), &value = value](
            pw::async::Context /*ctx*/, pw::Status status) {
          if (status.ok()) {
            std::vector<bt::gatt::Client::ReadByTypeValue> values = {
                {start, value.view(), /*maybe_truncated=*/false}};
            cb(fit::ok(values));
          }
        });
  });

  std::optional<fbg::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fuchsia::bluetooth::Uuid{kCharUuid.value()},
      [&](auto cb_result) { fidl_result = std::move(cb_result); });
  RunLoopUntilIdle();
  // All values were read, and the read completed with kAttributeNotFound.
  EXPECT_EQ(kValueCount + 1, read_count);
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_err());
  const auto& err = fidl_result->err();
  EXPECT_EQ(fbg::Error::TOO_MANY_RESULTS, err);
}

// Regression test for b/525207411.
TEST_F(Gatt2RemoteServiceServerTest,
       ReadByTypeExceedingMaxReadByTypeResultsReturnsTooManyResults) {
  constexpr bt::UUID kCharUuid(uint16_t{0xfefe});
  const auto value = bt::StaticByteBuffer(0x01);

  // Small values, so that the results would fit in a FIDL message if
  // RemoteService::ReadByType() did not limit them.
  constexpr size_t kValueCount =
      bt::gatt::RemoteService::kMaxReadByTypeResults + 1;

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback([&](const bt::UUID& type,
                                                       bt::att::Handle start,
                                                       bt::att::Handle end,
                                                       auto callback) {
    if (read_count++ == kValueCount) {
      ADD_FAILURE() << "Read continued past the result limit";
      callback(fit::error(bt::gatt::Client::ReadByTypeError{
          bt::att::Error(bt::att::ErrorCode::kAttributeNotFound), start}));
      return;
    }

    // Dispatch callback to prevent recursing too deep and breaking the stack.
    (void)heap_dispatcher().Post(
        [start, cb = std::move(callback), &value = value](
            pw::async::Context /*ctx*/, pw::Status status) {
          if (status.ok()) {
            std::vector<bt::gatt::Client::ReadByTypeValue> values = {
                {start, value.view(), /*maybe_truncated=*/false}};
            cb(fit::ok(values));
          }
        });
  });

  std::optional<fbg::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fuchsia::bluetooth::Uuid{kCharUuid.value()},
      [&](auto cb_result) { fidl_result = std::move(cb_result); });
  RunLoopUntilIdle();
  EXPECT_EQ(kValueCount, read_count);
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_err());
  const auto& err = fidl_result->err();
  EXPECT_EQ(fbg::Error::TOO_MANY_RESULTS, err);
}

TEST_F(Gatt2RemoteServiceServerTest, DiscoverAndReadShortCharacteristic) {
  constexpr bt::att::Handle kHandle = 3;
  constexpr bt::att::Handle kValueHandle = kHandle + 1;
  const auto kValue = bt::StaticByteBuffer(0x00, 0x01, 0x02, 0x03, 0x04);

  bt::gatt::CharacteristicData char_data(bt::gatt::Property::kRead,
                                         std::nullopt,
                                         kHandle,
                                         kValueHandle,
                                         kServiceUuid);
  fake_client()->set_characteristics({char_data});

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_handle());

  size_t read_count = 0;
  fake_client()->set_read_request_callback(
      [&](bt::att::Handle handle, bt::gatt::Client::ReadCallback callback) {
        read_count++;
        EXPECT_EQ(handle, kValueHandle);
        callback(fit::ok(), kValue, /*maybe_truncated=*/false);
      });
  fake_client()->set_read_blob_request_callback(
      [](auto, auto, auto) { FAIL(); });

  fbg::ReadOptions options =
      fbg::ReadOptions::WithShortRead(fbg::ShortReadOptions());
  std::optional<fpromise::result<fbg::ReadValue, fbg::Error>> fidl_result;
  service_proxy()->ReadCharacteristic(
      fidl_char.handle(), std::move(options), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  EXPECT_EQ(read_count, 1u);
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_ok())
      << static_cast<uint32_t>(fidl_result->error());
  const fbg::ReadValue& read_value = fidl_result->value();
  EXPECT_TRUE(ContainersEqual(kValue, read_value.value()));
  EXPECT_FALSE(read_value.maybe_truncated());
}

TEST_F(Gatt2RemoteServiceServerTest,
       DiscoverAndReadLongCharacteristicWithOffsetAndMaxBytes) {
  constexpr bt::att::Handle kHandle = 3;
  constexpr bt::att::Handle kValueHandle = kHandle + 1;
  const auto kValue = bt::StaticByteBuffer(0x00, 0x01, 0x02, 0x03, 0x04, 0x05);
  constexpr uint16_t kOffset = 1;
  constexpr uint16_t kMaxBytes = 3;

  bt::gatt::CharacteristicData char_data(bt::gatt::Property::kRead,
                                         std::nullopt,
                                         kHandle,
                                         kValueHandle,
                                         kServiceUuid);
  fake_client()->set_characteristics({char_data});

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_handle());

  fbg::LongReadOptions long_options;
  long_options.set_offset(kOffset);
  long_options.set_max_bytes(kMaxBytes);
  fbg::ReadOptions read_options;
  read_options.set_long_read(std::move(long_options));

  size_t read_count = 0;
  fake_client()->set_read_request_callback([](auto, auto) { FAIL(); });
  fake_client()->set_read_blob_request_callback(
      [&](bt::att::Handle handle,
          uint16_t offset,
          bt::gatt::Client::ReadCallback cb) {
        read_count++;
        EXPECT_EQ(handle, kValueHandle);
        EXPECT_EQ(offset, kOffset);
        cb(fit::ok(), kValue.view(offset), /*maybe_truncated=*/false);
      });

  std::optional<fpromise::result<fbg::ReadValue, fbg::Error>> fidl_result;
  service_proxy()->ReadCharacteristic(
      fidl_char.handle(), std::move(read_options), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  EXPECT_EQ(read_count, 1u);
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_ok())
      << static_cast<uint32_t>(fidl_result->error());
  const fbg::ReadValue& read_value = fidl_result->value();
  EXPECT_TRUE(
      ContainersEqual(kValue.view(kOffset, kMaxBytes), read_value.value()));
  EXPECT_TRUE(read_value.maybe_truncated());
}

TEST_F(Gatt2RemoteServiceServerTest, ReadCharacteristicHandleTooLarge) {
  fbg::Handle handle;
  handle.value = std::numeric_limits<bt::att::Handle>::max() + 1ULL;

  fbg::ReadOptions options =
      fbg::ReadOptions::WithShortRead(fbg::ShortReadOptions());
  std::optional<fpromise::result<fbg::ReadValue, fbg::Error>> fidl_result;
  service_proxy()->ReadCharacteristic(
      handle, std::move(options), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_error());
  EXPECT_EQ(fidl_result->error(), fbg::Error::INVALID_HANDLE);
}

// Trying to read a characteristic that doesn't exist should return a FAILURE
// error.
TEST_F(Gatt2RemoteServiceServerTest, ReadCharacteristicFailure) {
  constexpr bt::att::Handle kHandle = 3;
  fbg::ReadOptions options =
      fbg::ReadOptions::WithShortRead(fbg::ShortReadOptions());
  std::optional<fpromise::result<fbg::ReadValue, fbg::Error>> fidl_result;
  service_proxy()->ReadCharacteristic(
      fbg::Handle{kHandle}, std::move(options), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_error());
  EXPECT_EQ(fidl_result->error(), fbg::Error::UNLIKELY_ERROR);
}

TEST_F(Gatt2RemoteServiceServerTest, DiscoverAndReadShortDescriptor) {
  constexpr bt::att::Handle kCharacteristicHandle = 2;
  constexpr bt::att::Handle kCharacteristicValueHandle = 3;
  constexpr bt::att::Handle kDescriptorHandle = 4;
  const auto kDescriptorValue =
      bt::StaticByteBuffer(0x00, 0x01, 0x02, 0x03, 0x04);

  bt::gatt::CharacteristicData char_data(bt::gatt::Property::kRead,
                                         std::nullopt,
                                         kCharacteristicHandle,
                                         kCharacteristicValueHandle,
                                         kServiceUuid);
  fake_client()->set_characteristics({char_data});
  bt::gatt::DescriptorData desc_data(kDescriptorHandle, kDescriptorUuid);
  fake_client()->set_descriptors({desc_data});

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_descriptors());
  ASSERT_EQ(fidl_char.descriptors().size(), 1u);
  const fbg::Descriptor& fidl_desc = fidl_char.descriptors().front();
  ASSERT_TRUE(fidl_desc.has_handle());

  size_t read_count = 0;
  fake_client()->set_read_request_callback(
      [&](bt::att::Handle handle, bt::gatt::Client::ReadCallback callback) {
        read_count++;
        EXPECT_EQ(handle, kDescriptorHandle);
        callback(fit::ok(), kDescriptorValue, /*maybe_truncated=*/false);
      });
  fake_client()->set_read_blob_request_callback(
      [](auto, auto, auto) { FAIL(); });

  fbg::ReadOptions options =
      fbg::ReadOptions::WithShortRead(fbg::ShortReadOptions());
  std::optional<fpromise::result<fbg::ReadValue, fbg::Error>> fidl_result;
  service_proxy()->ReadDescriptor(
      fidl_desc.handle(), std::move(options), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  EXPECT_EQ(read_count, 1u);
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_ok())
      << static_cast<uint32_t>(fidl_result->error());
  const fbg::ReadValue& read_value = fidl_result->value();
  EXPECT_TRUE(ContainersEqual(kDescriptorValue, read_value.value()));
  EXPECT_FALSE(read_value.maybe_truncated());
}

TEST_F(Gatt2RemoteServiceServerTest,
       DiscoverAndReadLongDescriptorWithOffsetAndMaxBytes) {
  constexpr bt::att::Handle kCharacteristicHandle = 2;
  constexpr bt::att::Handle kCharacteristicValueHandle = 3;
  constexpr bt::att::Handle kDescriptorHandle = 4;
  constexpr uint16_t kOffset = 1;
  constexpr uint16_t kMaxBytes = 3;
  const auto kDescriptorValue =
      bt::StaticByteBuffer(0x00, 0x01, 0x02, 0x03, 0x04);

  bt::gatt::CharacteristicData char_data(bt::gatt::Property::kRead,
                                         std::nullopt,
                                         kCharacteristicHandle,
                                         kCharacteristicValueHandle,
                                         kServiceUuid);
  fake_client()->set_characteristics({char_data});
  bt::gatt::DescriptorData desc_data(kDescriptorHandle, kDescriptorUuid);
  fake_client()->set_descriptors({desc_data});

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_descriptors());
  ASSERT_EQ(fidl_char.descriptors().size(), 1u);
  const fbg::Descriptor& fidl_desc = fidl_char.descriptors().front();
  ASSERT_TRUE(fidl_desc.has_handle());

  fbg::LongReadOptions long_options;
  long_options.set_offset(kOffset);
  long_options.set_max_bytes(kMaxBytes);
  fbg::ReadOptions read_options;
  read_options.set_long_read(std::move(long_options));

  size_t read_count = 0;
  fake_client()->set_read_request_callback([](auto, auto) { FAIL(); });
  fake_client()->set_read_blob_request_callback(
      [&](bt::att::Handle handle,
          uint16_t offset,
          bt::gatt::Client::ReadCallback cb) {
        read_count++;
        EXPECT_EQ(handle, kDescriptorHandle);
        EXPECT_EQ(offset, kOffset);
        cb(fit::ok(), kDescriptorValue.view(offset), /*maybe_truncated=*/false);
      });

  std::optional<fpromise::result<fbg::ReadValue, fbg::Error>> fidl_result;
  service_proxy()->ReadDescriptor(
      fidl_desc.handle(), std::move(read_options), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  RunLoopUntilIdle();
  EXPECT_EQ(read_count, 1u);
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_ok())
      << static_cast<uint32_t>(fidl_result->error());
  const fbg::ReadValue& read_value = fidl_result->value();
  EXPECT_TRUE(ContainersEqual(kDescriptorValue.view(kOffset, kMaxBytes),
                              read_value.value()));
  EXPECT_TRUE(read_value.maybe_truncated());
}

TEST_F(Gatt2RemoteServiceServerTest, ReadDescriptorHandleTooLarge) {
  fbg::Handle handle;
  handle.value =
      static_cast<uint64_t>(std::numeric_limits<bt::att::Handle>::max()) + 1;

  fbg::ReadOptions options =
      fbg::ReadOptions::WithShortRead(fbg::ShortReadOptions());
  std::optional<fpromise::result<fbg::ReadValue, fbg::Error>> fidl_result;
  service_proxy()->ReadDescriptor(handle, std::move(options), [&](auto result) {
    fidl_result = std::move(result);
  });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_error());
  EXPECT_EQ(fidl_result->error(), fbg::Error::INVALID_HANDLE);
}

// Trying to read a descriptor that doesn't exist should return a FAILURE error.
TEST_F(Gatt2RemoteServiceServerTest, ReadDescriptorFailure) {
  constexpr bt::att::Handle kHandle = 3;
  fbg::ReadOptions options =
      fbg::ReadOptions::WithShortRead(fbg::ShortReadOptions());
  std::optional<fpromise::result<fbg::ReadValue, fbg::Error>> fidl_result;
  service_proxy()->ReadDescriptor(
      fbg::Handle{kHandle}, std::move(options), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_error());
  EXPECT_EQ(fidl_result->error(), fbg::Error::UNLIKELY_ERROR);
}

TEST_F(Gatt2RemoteServiceServerTest, WriteCharacteristicHandleTooLarge) {
  fbg::Handle handle;
  handle.value = std::numeric_limits<bt::att::Handle>::max() + 1ULL;

  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteCharacteristic(
      handle, /*value=*/{}, fbg::WriteOptions(), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_error());
  EXPECT_EQ(fidl_result->error(), fbg::Error::INVALID_HANDLE);
}

TEST_F(Gatt2RemoteServiceServerTest,
       WriteCharacteristicWithoutResponseAndNonZeroOffsetReturnsError) {
  fbg::Handle handle;
  handle.value = 3;
  fbg::WriteOptions options;
  options.set_write_mode(fbg::WriteMode::WITHOUT_RESPONSE);
  options.set_offset(1);
  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteCharacteristic(
      handle, /*value=*/{}, std::move(options), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_error());
  EXPECT_EQ(fidl_result->error(), fbg::Error::INVALID_PARAMETERS);
}

TEST_F(Gatt2RemoteServiceServerTest, WriteCharacteristicWithoutResponse) {
  constexpr bt::att::Handle kHandle = 3;
  constexpr bt::att::Handle kValueHandle = kHandle + 1;
  const auto kValue = bt::StaticByteBuffer(0x00, 0x01, 0x02, 0x03, 0x04);

  bt::gatt::CharacteristicData char_data(
      bt::gatt::Property::kWriteWithoutResponse,
      std::nullopt,
      kHandle,
      kValueHandle,
      kServiceUuid);
  fake_client()->set_characteristics({char_data});

  int write_count = 0;
  fake_client()->set_write_without_rsp_callback(
      [&](bt::att::Handle handle,
          const bt::ByteBuffer& value,
          bt::att::ResultFunction<> cb) {
        write_count++;
        EXPECT_EQ(handle, kValueHandle);
        EXPECT_TRUE(ContainersEqual(value, kValue));
        cb(fit::ok());
      });

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_handle());

  fbg::WriteOptions options;
  options.set_write_mode(fbg::WriteMode::WITHOUT_RESPONSE);

  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteCharacteristic(
      fidl_char.handle(),
      kValue.ToVector(),
      std::move(options),
      [&](auto result) { fidl_result = std::move(result); });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  EXPECT_TRUE(fidl_result->is_ok());
  EXPECT_EQ(write_count, 1);
}

TEST_F(Gatt2RemoteServiceServerTest,
       WriteCharacteristicWithoutResponseValueTooLong) {
  constexpr bt::att::Handle kHandle = 3;
  constexpr bt::att::Handle kValueHandle = kHandle + 1;
  ASSERT_EQ(fake_client()->mtu(), bt::att::kLEMinMTU);
  bt::StaticByteBuffer<bt::att::kLEMinMTU> kValue;
  kValue.Fill(0x03);

  bt::gatt::CharacteristicData char_data(
      bt::gatt::Property::kWriteWithoutResponse,
      std::nullopt,
      kHandle,
      kValueHandle,
      kServiceUuid);
  fake_client()->set_characteristics({char_data});

  int write_count = 0;
  fake_client()->set_write_without_rsp_callback(
      [&](bt::att::Handle handle,
          const bt::ByteBuffer& value,
          bt::att::ResultFunction<> callback) {
        write_count++;
        EXPECT_EQ(handle, kValueHandle);
        EXPECT_TRUE(ContainersEqual(value, kValue));
        callback(bt::ToResult(bt::HostError::kFailed));
      });

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_handle());

  fbg::WriteOptions options;
  options.set_write_mode(fbg::WriteMode::WITHOUT_RESPONSE);

  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteCharacteristic(
      fidl_char.handle(),
      kValue.ToVector(),
      std::move(options),
      [&](auto result) { fidl_result = std::move(result); });
  RunLoopUntilIdle();
  EXPECT_EQ(write_count, 1);
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_error());
  EXPECT_EQ(fidl_result->error(), fbg::Error::UNLIKELY_ERROR);
}

TEST_F(Gatt2RemoteServiceServerTest, WriteShortCharacteristic) {
  constexpr bt::att::Handle kHandle = 3;
  constexpr bt::att::Handle kValueHandle = kHandle + 1;
  const auto kValue = bt::StaticByteBuffer(0x00, 0x01, 0x02, 0x03, 0x04);

  bt::gatt::CharacteristicData char_data(bt::gatt::Property::kWrite,
                                         std::nullopt,
                                         kHandle,
                                         kValueHandle,
                                         kServiceUuid);
  fake_client()->set_characteristics({char_data});

  int write_count = 0;
  fake_client()->set_write_request_callback(
      [&](bt::att::Handle handle,
          const bt::ByteBuffer& value,
          bt::att::ResultFunction<> callback) {
        write_count++;
        EXPECT_EQ(handle, kValueHandle);
        EXPECT_TRUE(ContainersEqual(value, kValue));
        callback(fit::ok());
      });

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_handle());

  fbg::WriteOptions options;
  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteCharacteristic(
      fidl_char.handle(),
      kValue.ToVector(),
      std::move(options),
      [&](auto result) { fidl_result = std::move(result); });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  EXPECT_TRUE(fidl_result->is_ok());
  EXPECT_EQ(write_count, 1);
}

TEST_F(Gatt2RemoteServiceServerTest,
       WriteShortCharacteristicWithNonZeroOffset) {
  constexpr bt::att::Handle kHandle = 3;
  constexpr bt::att::Handle kValueHandle = kHandle + 1;
  const auto kValue = bt::StaticByteBuffer(0x00, 0x01, 0x02, 0x03, 0x04);
  const uint16_t kOffset = 1;

  bt::gatt::CharacteristicData char_data(bt::gatt::Property::kWrite,
                                         std::nullopt,
                                         kHandle,
                                         kValueHandle,
                                         kServiceUuid);
  fake_client()->set_characteristics({char_data});

  int write_count = 0;
  fake_client()->set_execute_prepare_writes_callback(
      [&](bt::att::PrepareWriteQueue prep_write_queue,
          bt::gatt::ReliableMode reliable,
          bt::att::ResultFunction<> callback) {
        write_count++;
        ASSERT_EQ(prep_write_queue.size(), 1u);
        EXPECT_EQ(prep_write_queue.front().handle(), kValueHandle);
        EXPECT_EQ(prep_write_queue.front().offset(), kOffset);
        EXPECT_EQ(reliable, bt::gatt::ReliableMode::kDisabled);
        EXPECT_TRUE(ContainersEqual(prep_write_queue.front().value(), kValue));
        callback(fit::ok());
      });

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_handle());

  fbg::WriteOptions options;
  options.set_offset(kOffset);
  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteCharacteristic(
      fidl_char.handle(),
      kValue.ToVector(),
      std::move(options),
      [&](auto result) { fidl_result = std::move(result); });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  EXPECT_TRUE(fidl_result->is_ok());
  EXPECT_EQ(write_count, 1);
}

TEST_F(Gatt2RemoteServiceServerTest, WriteShortCharacteristicWithReliableMode) {
  constexpr bt::att::Handle kHandle = 3;
  constexpr bt::att::Handle kValueHandle = kHandle + 1;
  const auto kValue = bt::StaticByteBuffer(0x00, 0x01, 0x02, 0x03, 0x04);

  bt::gatt::CharacteristicData char_data(bt::gatt::Property::kWrite,
                                         std::nullopt,
                                         kHandle,
                                         kValueHandle,
                                         kServiceUuid);
  fake_client()->set_characteristics({char_data});

  int write_count = 0;
  fake_client()->set_execute_prepare_writes_callback(
      [&](bt::att::PrepareWriteQueue prep_write_queue,
          bt::gatt::ReliableMode reliable,
          bt::att::ResultFunction<> callback) {
        write_count++;
        ASSERT_EQ(prep_write_queue.size(), 1u);
        EXPECT_EQ(reliable, bt::gatt::ReliableMode::kEnabled);
        EXPECT_EQ(prep_write_queue.front().handle(), kValueHandle);
        EXPECT_EQ(prep_write_queue.front().offset(), 0u);
        EXPECT_TRUE(ContainersEqual(prep_write_queue.front().value(), kValue));
        callback(fit::ok());
      });

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_handle());

  fbg::WriteOptions options;
  options.set_write_mode(fbg::WriteMode::RELIABLE);
  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteCharacteristic(
      fidl_char.handle(),
      kValue.ToVector(),
      std::move(options),
      [&](auto result) { fidl_result = std::move(result); });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  EXPECT_TRUE(fidl_result->is_ok());
  EXPECT_EQ(write_count, 1);
}

TEST_F(Gatt2RemoteServiceServerTest, WriteLongCharacteristicDefaultOptions) {
  constexpr bt::att::Handle kHandle = 3;
  constexpr bt::att::Handle kValueHandle = kHandle + 1;
  constexpr size_t kHeaderSize =
      sizeof(bt::att::OpCode) + sizeof(bt::att::PrepareWriteRequestParams);
  const uint16_t kMtu = fake_client()->mtu();
  const size_t kFirstPacketValueSize = kMtu - kHeaderSize;
  bt::DynamicByteBuffer kValue(kMtu);
  kValue.Fill(0x03);

  bt::gatt::CharacteristicData char_data(bt::gatt::Property::kWrite,
                                         std::nullopt,
                                         kHandle,
                                         kValueHandle,
                                         kServiceUuid);
  fake_client()->set_characteristics({char_data});

  int write_count = 0;
  fake_client()->set_execute_prepare_writes_callback(
      [&](bt::att::PrepareWriteQueue prep_write_queue,
          bt::gatt::ReliableMode reliable,
          bt::att::ResultFunction<> callback) {
        write_count++;
        EXPECT_EQ(reliable, bt::gatt::ReliableMode::kDisabled);
        ASSERT_EQ(prep_write_queue.size(), 2u);
        EXPECT_EQ(prep_write_queue.front().handle(), kValueHandle);
        EXPECT_EQ(prep_write_queue.front().offset(), 0u);
        EXPECT_TRUE(ContainersEqual(kValue.view(0, kFirstPacketValueSize),
                                    prep_write_queue.front().value()));
        prep_write_queue.pop();
        EXPECT_EQ(prep_write_queue.front().handle(), kValueHandle);
        EXPECT_EQ(prep_write_queue.front().offset(), kFirstPacketValueSize);
        EXPECT_TRUE(ContainersEqual(kValue.view(kFirstPacketValueSize),
                                    prep_write_queue.front().value()));
        callback(fit::ok());
      });

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_handle());

  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteCharacteristic(
      fidl_char.handle(),
      kValue.ToVector(),
      fbg::WriteOptions(),
      [&](auto result) { fidl_result = std::move(result); });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  EXPECT_TRUE(fidl_result->is_ok());
  EXPECT_EQ(write_count, 1);
}

TEST_F(Gatt2RemoteServiceServerTest, WriteDescriptorHandleTooLarge) {
  fbg::Handle handle;
  handle.value = std::numeric_limits<bt::att::Handle>::max() + 1ULL;

  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteDescriptor(
      handle, /*value=*/{}, fbg::WriteOptions(), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_error());
  EXPECT_EQ(fidl_result->error(), fbg::Error::INVALID_HANDLE);
}

TEST_F(Gatt2RemoteServiceServerTest,
       WriteDescriptorWithoutResponseNotSupported) {
  constexpr bt::att::Handle kHandle = 3;
  fbg::WriteOptions options;
  options.set_write_mode(fbg::WriteMode::WITHOUT_RESPONSE);

  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteDescriptor(
      fbg::Handle{kHandle}, /*value=*/{}, std::move(options), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_error());
  EXPECT_EQ(fidl_result->error(), fbg::Error::INVALID_PARAMETERS);
}

TEST_F(Gatt2RemoteServiceServerTest, WriteDescriptorReliableNotSupported) {
  constexpr bt::att::Handle kHandle = 3;
  fbg::WriteOptions options;
  options.set_write_mode(fbg::WriteMode::RELIABLE);

  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteDescriptor(
      fbg::Handle{kHandle}, /*value=*/{}, std::move(options), [&](auto result) {
        fidl_result = std::move(result);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_error());
  EXPECT_EQ(fidl_result->error(), fbg::Error::INVALID_PARAMETERS);
}

TEST_F(Gatt2RemoteServiceServerTest, WriteShortDescriptor) {
  constexpr bt::att::Handle kHandle = 3;
  constexpr bt::att::Handle kCharacteristicValueHandle = kHandle + 1;
  bt::gatt::CharacteristicData char_data(bt::gatt::Property::kWrite,
                                         std::nullopt,
                                         kHandle,
                                         kCharacteristicValueHandle,
                                         kServiceUuid);
  fake_client()->set_characteristics({char_data});

  constexpr bt::att::Handle kDescriptorHandle(kCharacteristicValueHandle + 1);
  const bt::UUID kDescriptorUuid(uint16_t{0x0001});
  bt::gatt::DescriptorData descriptor(kDescriptorHandle, kDescriptorUuid);
  const auto kValue = bt::StaticByteBuffer(0x00, 0x01, 0x02, 0x03, 0x04);
  fake_client()->set_descriptors({descriptor});

  int write_count = 0;
  fake_client()->set_write_request_callback(
      [&](bt::att::Handle handle,
          const bt::ByteBuffer& value,
          bt::att::ResultFunction<> callback) {
        write_count++;
        EXPECT_EQ(handle, kDescriptorHandle);
        EXPECT_TRUE(ContainersEqual(value, kValue));
        callback(fit::ok());
      });

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_descriptors());
  ASSERT_EQ(fidl_char.descriptors().size(), 1u);

  fbg::WriteOptions options;
  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteDescriptor(
      fidl_char.descriptors().front().handle(),
      kValue.ToVector(),
      std::move(options),
      [&](auto result) { fidl_result = std::move(result); });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  EXPECT_TRUE(fidl_result->is_ok());
  EXPECT_EQ(write_count, 1);
}

TEST_F(Gatt2RemoteServiceServerTest, WriteShortDescriptorWithNonZeroOffset) {
  constexpr bt::att::Handle kHandle = 3;
  constexpr bt::att::Handle kCharacteristicValueHandle = kHandle + 1;
  bt::gatt::CharacteristicData char_data(bt::gatt::Property::kWrite,
                                         std::nullopt,
                                         kHandle,
                                         kCharacteristicValueHandle,
                                         kServiceUuid);
  fake_client()->set_characteristics({char_data});

  constexpr bt::att::Handle kDescriptorHandle(kCharacteristicValueHandle + 1);
  const bt::UUID kDescriptorUuid(uint16_t{0x0001});
  bt::gatt::DescriptorData descriptor(kDescriptorHandle, kDescriptorUuid);
  fake_client()->set_descriptors({descriptor});

  const auto kValue = bt::StaticByteBuffer(0x00, 0x01, 0x02, 0x03, 0x04);
  const uint16_t kOffset = 1;

  int write_count = 0;
  fake_client()->set_execute_prepare_writes_callback(
      [&](bt::att::PrepareWriteQueue prep_write_queue,
          bt::gatt::ReliableMode reliable,
          bt::att::ResultFunction<> callback) {
        write_count++;
        ASSERT_EQ(prep_write_queue.size(), 1u);
        EXPECT_EQ(prep_write_queue.front().handle(), kDescriptorHandle);
        EXPECT_EQ(prep_write_queue.front().offset(), kOffset);
        EXPECT_EQ(reliable, bt::gatt::ReliableMode::kDisabled);
        EXPECT_TRUE(ContainersEqual(prep_write_queue.front().value(), kValue));
        callback(fit::ok());
      });

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_descriptors());
  ASSERT_EQ(fidl_char.descriptors().size(), 1u);

  fbg::WriteOptions options;
  options.set_offset(kOffset);
  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteDescriptor(
      fidl_char.descriptors().front().handle(),
      kValue.ToVector(),
      std::move(options),
      [&](auto result) { fidl_result = std::move(result); });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  EXPECT_TRUE(fidl_result->is_ok());
  EXPECT_EQ(write_count, 1);
}

TEST_F(Gatt2RemoteServiceServerTest, WriteLongDescriptorDefaultOptions) {
  constexpr bt::att::Handle kHandle = 3;
  constexpr bt::att::Handle kCharacteristicValueHandle = kHandle + 1;
  bt::gatt::CharacteristicData char_data(bt::gatt::Property::kWrite,
                                         std::nullopt,
                                         kHandle,
                                         kCharacteristicValueHandle,
                                         kServiceUuid);
  fake_client()->set_characteristics({char_data});

  constexpr bt::att::Handle kDescriptorHandle(kCharacteristicValueHandle + 1);
  const bt::UUID kDescriptorUuid(uint16_t{0x0001});
  bt::gatt::DescriptorData descriptor(kDescriptorHandle, kDescriptorUuid);
  fake_client()->set_descriptors({descriptor});

  constexpr size_t kHeaderSize =
      sizeof(bt::att::OpCode) + sizeof(bt::att::PrepareWriteRequestParams);
  const uint16_t kMtu = fake_client()->mtu();
  const size_t kFirstPacketValueSize = kMtu - kHeaderSize;
  bt::DynamicByteBuffer kValue(kMtu);
  kValue.Fill(0x03);

  int write_count = 0;
  fake_client()->set_execute_prepare_writes_callback(
      [&](bt::att::PrepareWriteQueue prep_write_queue,
          bt::gatt::ReliableMode reliable,
          bt::att::ResultFunction<> callback) {
        write_count++;
        EXPECT_EQ(reliable, bt::gatt::ReliableMode::kDisabled);
        ASSERT_EQ(prep_write_queue.size(), 2u);
        EXPECT_EQ(prep_write_queue.front().handle(), kDescriptorHandle);
        EXPECT_EQ(prep_write_queue.front().offset(), 0u);
        EXPECT_TRUE(ContainersEqual(kValue.view(0, kFirstPacketValueSize),
                                    prep_write_queue.front().value()));
        prep_write_queue.pop();
        EXPECT_EQ(prep_write_queue.front().handle(), kDescriptorHandle);
        EXPECT_EQ(prep_write_queue.front().offset(), kFirstPacketValueSize);
        EXPECT_TRUE(ContainersEqual(kValue.view(kFirstPacketValueSize),
                                    prep_write_queue.front().value()));
        callback(fit::ok());
      });

  std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
  service_proxy()->DiscoverCharacteristics(
      [&](std::vector<fbg::Characteristic> chars) {
        fidl_characteristics = std::move(chars);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_characteristics.has_value());
  ASSERT_EQ(fidl_characteristics->size(), 1u);
  const fbg::Characteristic& fidl_char = fidl_characteristics->front();
  ASSERT_TRUE(fidl_char.has_descriptors());
  ASSERT_EQ(fidl_char.descriptors().size(), 1u);

  std::optional<fpromise::result<void, fbg::Error>> fidl_result;
  service_proxy()->WriteDescriptor(
      fidl_char.descriptors().front().handle(),
      kValue.ToVector(),
      fbg::WriteOptions(),
      [&](auto result) { fidl_result = std::move(result); });
  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  EXPECT_TRUE(fidl_result->is_ok());
  EXPECT_EQ(write_count, 1);
}

class FakeCharacteristicNotifier
    : public fbg::testing::CharacteristicNotifier_TestBase {
 public:
  struct Notification {
    fbg::ReadValue value;
    OnNotificationCallback notification_cb;
  };
  explicit FakeCharacteristicNotifier(
      fidl::InterfaceRequest<fbg::CharacteristicNotifier> request)
      : binding_(this, std::move(request)) {
    binding_.set_error_handler([this](zx_status_t status) { error_ = status; });
  }

  void Unbind() { binding_.Unbind(); }

  void OnNotification(fbg::ReadValue value,
                      OnNotificationCallback callback) override {
    notifications_.push_back(
        Notification{std::move(value), std::move(callback)});
  }

  std::vector<Notification>& notifications() { return notifications_; }

  std::optional<zx_status_t> error() const { return error_; }

 private:
  void NotImplemented_(const std::string& name) override {
    FAIL() << name << " is not implemented";
  }

  fidl::Binding<fbg::CharacteristicNotifier> binding_;
  std::vector<Notification> notifications_;
  std::optional<zx_status_t> error_;
};

class Gatt2RemoteServiceServerCharacteristicNotifierTest
    : public Gatt2RemoteServiceServerTest {
 public:
  void SetUp() override {
    Gatt2RemoteServiceServerTest::SetUp();
    bt::gatt::CharacteristicData characteristic(bt::gatt::Property::kNotify,
                                                /*ext_props=*/std::nullopt,
                                                char_handle_,
                                                char_value_handle_,
                                                kCharacteristicUuid);
    fake_client()->set_characteristics({characteristic});
    bt::gatt::DescriptorData ccc_descriptor(
        ccc_descriptor_handle_, bt::gatt::types::kClientCharacteristicConfig);
    fake_client()->set_descriptors({ccc_descriptor});

    std::optional<std::vector<fbg::Characteristic>> fidl_characteristics;
    service_proxy()->DiscoverCharacteristics(
        [&](std::vector<fbg::Characteristic> chars) {
          fidl_characteristics = std::move(chars);
        });
    RunLoopUntilIdle();
    ASSERT_TRUE(fidl_characteristics.has_value());
    ASSERT_EQ(fidl_characteristics->size(), 1u);
    characteristic_ = std::move(fidl_characteristics->front());
  }

 protected:
  const fbg::Characteristic& characteristic() const { return characteristic_; }
  bt::att::Handle characteristic_handle() const { return char_handle_; }
  bt::att::Handle characteristic_value_handle() const {
    return char_value_handle_;
  }
  bt::att::Handle ccc_descriptor_handle() const {
    return ccc_descriptor_handle_;
  }

 private:
  fbg::Characteristic characteristic_;
  const bt::att::Handle char_handle_ = 2;
  const bt::att::Handle char_value_handle_ = 3;
  const bt::att::Handle ccc_descriptor_handle_ = 4;
};

TEST_F(Gatt2RemoteServiceServerCharacteristicNotifierTest,
       RegisterCharacteristicNotifierReceiveNotificationsAndUnregister) {
  const auto kValue0 = bt::StaticByteBuffer(0x01, 0x02, 0x03);
  const auto kValue1 = bt::StaticByteBuffer(0x04, 0x05, 0x06);

  // Respond to CCC write with success status so that notifications are enabled.
  fake_client()->set_write_request_callback([&](bt::att::Handle handle,
                                                const bt::ByteBuffer& /*value*/,
                                                auto status_callback) {
    EXPECT_EQ(handle, ccc_descriptor_handle());
    status_callback(fit::ok());
  });

  fidl::InterfaceHandle<fbg::CharacteristicNotifier> notifier_handle;
  FakeCharacteristicNotifier notifier_server(notifier_handle.NewRequest());
  auto& notifications = notifier_server.notifications();

  std::optional<fpromise::result<void, fbg::Error>> register_result;
  auto register_cb = [&](fpromise::result<void, fbg::Error> result) {
    register_result = result;
  };
  service_proxy()->RegisterCharacteristicNotifier(characteristic().handle(),
                                                  std::move(notifier_handle),
                                                  std::move(register_cb));
  RunLoopUntilIdle();
  ASSERT_TRUE(register_result.has_value());
  EXPECT_TRUE(register_result->is_ok());

  // Send 2 notifications to test flow control.
  service()->HandleNotificationForTesting(characteristic_value_handle(),
                                          kValue0,
                                          /*maybe_truncated=*/false);
  service()->HandleNotificationForTesting(characteristic_value_handle(),
                                          kValue1,
                                          /*maybe_truncated=*/true);
  RunLoopUntilIdle();
  ASSERT_EQ(notifications.size(), 1u);
  fbg::ReadValue& notification_0 = notifications[0].value;
  ASSERT_TRUE(notification_0.has_value());
  EXPECT_TRUE(bt::ContainersEqual(notification_0.value(), kValue0));
  ASSERT_TRUE(notification_0.has_handle());
  EXPECT_EQ(notification_0.handle().value,
            static_cast<uint64_t>(characteristic_value_handle()));
  ASSERT_TRUE(notification_0.has_maybe_truncated());
  ASSERT_FALSE(notification_0.maybe_truncated());

  notifications[0].notification_cb();
  RunLoopUntilIdle();
  ASSERT_EQ(notifications.size(), 2u);
  fbg::ReadValue& notification_1 = notifications[1].value;
  ASSERT_TRUE(notification_1.has_value());
  EXPECT_TRUE(bt::ContainersEqual(notification_1.value(), kValue1));
  ASSERT_TRUE(notification_1.has_handle());
  EXPECT_EQ(notification_1.handle().value,
            static_cast<uint64_t>(characteristic_value_handle()));
  ASSERT_TRUE(notification_1.has_maybe_truncated());
  ASSERT_TRUE(notification_1.maybe_truncated());

  notifications[1].notification_cb();
  RunLoopUntilIdle();
  EXPECT_EQ(notifications.size(), 2u);

  notifier_server.Unbind();
  RunLoopUntilIdle();

  // Notifications should be ignored after notifier is unregistered.
  service()->HandleNotificationForTesting(characteristic_value_handle(),
                                          kValue0,
                                          /*maybe_truncated=*/false);
  RunLoopUntilIdle();
}

TEST_F(Gatt2RemoteServiceServerCharacteristicNotifierTest,
       QueueTooManyNotificationsAndCloseNotifier) {
  const auto kValue = bt::StaticByteBuffer(0x01, 0x02, 0x03);

  // Respond to CCC write with success status so that notifications are enabled.
  fake_client()->set_write_request_callback([&](bt::att::Handle handle,
                                                const bt::ByteBuffer& /*value*/,
                                                auto status_callback) {
    EXPECT_EQ(handle, ccc_descriptor_handle());
    status_callback(fit::ok());
  });

  fidl::InterfaceHandle<fbg::CharacteristicNotifier> notifier_handle;
  FakeCharacteristicNotifier notifier_server(notifier_handle.NewRequest());
  auto& notifications = notifier_server.notifications();

  std::optional<fpromise::result<void, fbg::Error>> register_result;
  auto register_cb = [&](fpromise::result<void, fbg::Error> result) {
    register_result = result;
  };
  service_proxy()->RegisterCharacteristicNotifier(characteristic().handle(),
                                                  std::move(notifier_handle),
                                                  std::move(register_cb));
  RunLoopUntilIdle();
  ASSERT_TRUE(register_result.has_value());
  EXPECT_TRUE(register_result->is_ok());

  // Fill the pending notifier values queue.
  for (size_t i = 0; i < Gatt2RemoteServiceServer::kMaxPendingNotifierValues;
       i++) {
    service()->HandleNotificationForTesting(characteristic_value_handle(),
                                            kValue,
                                            /*maybe_truncated=*/false);
  }
  RunLoopUntilIdle();
  ASSERT_EQ(notifications.size(), 1u);
  EXPECT_FALSE(notifier_server.error().has_value());

  // This notification should exceed the max queue size.
  service()->HandleNotificationForTesting(characteristic_value_handle(),
                                          kValue,
                                          /*maybe_truncated=*/false);
  RunLoopUntilIdle();
  EXPECT_TRUE(notifier_server.error().has_value());

  // Notifications should be ignored after notifier is unregistered due to an
  // error.
  service()->HandleNotificationForTesting(characteristic_value_handle(),
                                          kValue,
                                          /*maybe_truncated=*/false);
  RunLoopUntilIdle();
  notifications[0].notification_cb();
  EXPECT_EQ(notifications.size(), 1u);
}

TEST_F(Gatt2RemoteServiceServerCharacteristicNotifierTest,
       RegisterCharacteristicNotifierWriteError) {
  // Respond to CCC write with error status so that registration fails.
  fake_client()->set_write_request_callback([&](bt::att::Handle handle,
                                                const bt::ByteBuffer& /*value*/,
                                                auto status_callback) {
    EXPECT_EQ(handle, ccc_descriptor_handle());
    status_callback(
        bt::ToResult(bt::att::ErrorCode::kInsufficientAuthentication));
  });

  fidl::InterfaceHandle<fbg::CharacteristicNotifier> notifier_handle;
  FakeCharacteristicNotifier notifier_server(notifier_handle.NewRequest());
  std::optional<fpromise::result<void, fbg::Error>> register_result;
  auto register_cb = [&](fpromise::result<void, fbg::Error> result) {
    register_result = result;
  };
  service_proxy()->RegisterCharacteristicNotifier(characteristic().handle(),
                                                  std::move(notifier_handle),
                                                  std::move(register_cb));
  RunLoopUntilIdle();
  ASSERT_TRUE(register_result.has_value());
  ASSERT_TRUE(register_result->is_error());
  EXPECT_EQ(register_result->error(), fbg::Error::INSUFFICIENT_AUTHENTICATION);
  EXPECT_TRUE(notifier_server.error().has_value());
}

TEST_F(
    Gatt2RemoteServiceServerCharacteristicNotifierTest,
    RegisterCharacteristicNotifierAndDestroyServerBeforeStatusCallbackCausesNotificationsToBeDisabled) {
  // Respond to CCC write with success status so that notifications are enabled.
  int ccc_write_count = 0;
  bt::att::ResultFunction<> enable_notifications_status_cb = nullptr;
  fake_client()->set_write_request_callback(
      [&](bt::att::Handle handle,
          const bt::ByteBuffer& value,
          bt::att::ResultFunction<> status_callback) {
        ccc_write_count++;
        EXPECT_EQ(handle, ccc_descriptor_handle());
        if (ccc_write_count == 1) {
          EXPECT_NE(value[0], 0u);  // Enable value
          enable_notifications_status_cb = std::move(status_callback);
        } else {
          EXPECT_EQ(value[0], 0u);  // Disable value
          status_callback(fit::ok());
        }
      });

  fidl::InterfaceHandle<fbg::CharacteristicNotifier> notifier_handle;
  FakeCharacteristicNotifier notifier_server(notifier_handle.NewRequest());

  std::optional<fpromise::result<void, fbg::Error>> register_result;
  auto register_cb = [&](fpromise::result<void, fbg::Error> result) {
    register_result = result;
  };
  service_proxy()->RegisterCharacteristicNotifier(characteristic().handle(),
                                                  std::move(notifier_handle),
                                                  std::move(register_cb));
  RunLoopUntilIdle();
  EXPECT_FALSE(register_result.has_value());
  EXPECT_EQ(ccc_write_count, 1);

  DestroyServer();
  ASSERT_TRUE(enable_notifications_status_cb);
  enable_notifications_status_cb(fit::ok());
  RunLoopUntilIdle();
  // Notifications should have been disabled in enable notifications status
  // callback.
  EXPECT_EQ(ccc_write_count, 2);
}

TEST_F(
    Gatt2RemoteServiceServerCharacteristicNotifierTest,
    RegisterCharacteristicNotifierAndDestroyServerAfterStatusCallbackCausesNotificationsToBeDisabled) {
  // Respond to CCC write with success status so that notifications are enabled.
  int ccc_write_count = 0;
  bt::att::ResultFunction<> enable_notifications_status_cb = nullptr;
  fake_client()->set_write_request_callback(
      [&](bt::att::Handle handle,
          const bt::ByteBuffer& value,
          bt::att::ResultFunction<> status_callback) {
        ccc_write_count++;
        EXPECT_EQ(handle, ccc_descriptor_handle());
        if (ccc_write_count == 1) {
          EXPECT_NE(value[0], 0u);  // Enable value
        } else {
          EXPECT_EQ(value[0], 0u);  // Disable value
        }
        status_callback(fit::ok());
      });

  fidl::InterfaceHandle<fbg::CharacteristicNotifier> notifier_handle;
  FakeCharacteristicNotifier notifier_server(notifier_handle.NewRequest());

  std::optional<fpromise::result<void, fbg::Error>> register_result;
  auto register_cb = [&](fpromise::result<void, fbg::Error> result) {
    register_result = result;
  };
  service_proxy()->RegisterCharacteristicNotifier(characteristic().handle(),
                                                  std::move(notifier_handle),
                                                  std::move(register_cb));
  RunLoopUntilIdle();
  EXPECT_TRUE(register_result.has_value());
  EXPECT_TRUE(register_result->is_ok());
  EXPECT_EQ(ccc_write_count, 1);

  DestroyServer();
  RunLoopUntilIdle();
  // Notifications should have been disabled in the server destructor.
  EXPECT_EQ(ccc_write_count, 2);
}

}  // namespace
}  // namespace bthost
