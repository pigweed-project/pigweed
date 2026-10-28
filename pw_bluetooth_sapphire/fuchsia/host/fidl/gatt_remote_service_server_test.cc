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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/gatt_remote_service_server.h"

#include <pw_assert/check.h>

#include "gtest/gtest.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/fake_gatt_fixture.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"
#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/remote_service.h"
#include "pw_bluetooth_sapphire/internal/host/gatt/remote_service_manager.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bthost {
namespace {

namespace fbgatt = fuchsia::bluetooth::gatt;

constexpr bt::PeerId kPeerId(1);

constexpr bt::att::Handle kServiceStartHandle = 0x0021;
constexpr bt::att::Handle kServiceEndHandle = 0x002C;
const bt::UUID kServiceUuid(uint16_t{0x180D});

class GattRemoteServiceServerTest : public bt::fidl::testing::FakeGattFixture {
 public:
  GattRemoteServiceServerTest() = default;
  ~GattRemoteServiceServerTest() override = default;

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

    fidl::InterfaceHandle<fbgatt::RemoteService> handle;
    server_ = std::make_unique<GattRemoteServiceServer>(
        service_, gatt(), kPeerId, handle.NewRequest());
    proxy_.Bind(std::move(handle));
  }

  void TearDown() override {
    // Clear any previous expectations that are based on the ATT Write Request,
    // so that write requests sent during RemoteService::ShutDown() are ignored.
    fake_client()->set_write_request_callback({});

    bt::fidl::testing::FakeGattFixture::TearDown();
  }

 protected:
  const bt::gatt::testing::FakeClient::WeakPtr& fake_client() const {
    PW_CHECK(fake_client_.is_alive());
    return fake_client_;
  }

  fbgatt::RemoteServicePtr& service_proxy() { return proxy_; }

  pw::async::HeapDispatcher& heap_dispatcher() { return heap_dispatcher_; }

 private:
  std::unique_ptr<GattRemoteServiceServer> server_;

  fbgatt::RemoteServicePtr proxy_;
  bt::gatt::RemoteService::WeakPtr service_;
  bt::gatt::testing::FakeClient::WeakPtr fake_client_;
  pw::async_fuchsia::FuchsiaDispatcher pw_dispatcher_{dispatcher()};
  pw::async::HeapDispatcher heap_dispatcher_{pw_dispatcher_};

  BT_DISALLOW_COPY_ASSIGN_AND_MOVE(GattRemoteServiceServerTest);
};

TEST_F(GattRemoteServiceServerTest, ReadByTypeSuccess) {
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

  std::optional<fbgatt::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fidl_helpers::UuidToFidl(kCharUuid),
      [&](auto cb_result) { fidl_result = std::move(cb_result); });

  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_response());
  const auto& response = fidl_result->response();
  ASSERT_EQ(1u, response.results.size());
  const fbgatt::ReadByTypeResult& result0 = response.results[0];
  ASSERT_TRUE(result0.has_id());
  EXPECT_EQ(result0.id(), static_cast<uint64_t>(kHandle));
  ASSERT_TRUE(result0.has_value());
  EXPECT_TRUE(ContainersEqual(
      bt::BufferView(result0.value().data(), result0.value().size()), kValue));
  EXPECT_FALSE(result0.has_error());
}

TEST_F(GattRemoteServiceServerTest, ReadByTypeResultWithError) {
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

  std::optional<fbgatt::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fidl_helpers::UuidToFidl(kCharUuid),
      [&](auto cb_result) { fidl_result = std::move(cb_result); });

  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_response());
  const auto& response = fidl_result->response();
  ASSERT_EQ(1u, response.results.size());
  const fbgatt::ReadByTypeResult& result0 = response.results[0];
  ASSERT_TRUE(result0.has_id());
  EXPECT_EQ(result0.id(), static_cast<uint64_t>(kServiceEndHandle));
  EXPECT_FALSE(result0.has_value());
  ASSERT_TRUE(result0.has_error());
  EXPECT_EQ(fbgatt::Error::INSUFFICIENT_AUTHORIZATION, result0.error());
}

TEST_F(GattRemoteServiceServerTest, ReadByTypeError) {
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

  std::optional<fbgatt::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fidl_helpers::UuidToFidl(kCharUuid),
      [&](auto cb_result) { fidl_result = std::move(cb_result); });

  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_err());
  const auto& err = fidl_result->err();
  EXPECT_EQ(fbgatt::Error::INVALID_RESPONSE, err);
}

// An allocation failure is not reported as TOO_MANY_RESULTS.
TEST_F(GattRemoteServiceServerTest, ReadByTypeOutOfMemoryReturnsFailure) {
  constexpr bt::UUID kCharUuid(uint16_t{0xfefe});

  size_t read_count = 0;
  fake_client()->set_read_by_type_request_callback(
      [&](const bt::UUID&, bt::att::Handle, bt::att::Handle, auto callback) {
        ASSERT_EQ(0u, read_count++);
        // bt::gatt::Client reports a PDU allocation failure this way.
        callback(fit::error(bt::gatt::Client::ReadByTypeError{
            bt::Error(bt::HostError::kOutOfMemory), std::nullopt}));
      });

  std::optional<fbgatt::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fidl_helpers::UuidToFidl(kCharUuid),
      [&](auto cb_result) { fidl_result = std::move(cb_result); });

  RunLoopUntilIdle();
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_err());
  EXPECT_EQ(fbgatt::Error::FAILURE, fidl_result->err());
}

TEST_F(GattRemoteServiceServerTest,
       ReadByTypeInvalidParametersErrorClosesChannel) {
  constexpr bt::UUID kCharUuid = bt::gatt::types::kCharacteristicDeclaration;

  std::optional<zx_status_t> error_status;
  service_proxy().set_error_handler(
      [&](zx_status_t status) { error_status = status; });

  std::optional<fbgatt::RemoteService_ReadByType_Result> fidl_result;
  service_proxy()->ReadByType(
      fidl_helpers::UuidToFidl(kCharUuid),
      [&](auto cb_result) { fidl_result = std::move(cb_result); });

  RunLoopUntilIdle();
  EXPECT_FALSE(fidl_result.has_value());
  EXPECT_FALSE(service_proxy().is_bound());
  EXPECT_TRUE(error_status.has_value());
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, error_status.value());
}

// Regression test for b/525207411.
TEST_F(GattRemoteServiceServerTest,
       ReadByTypeExceedingMaxReadByTypeResultsReturnsTooManyResults) {
  constexpr bt::UUID kCharUuid(uint16_t{0xfefe});
  const auto value = bt::StaticByteBuffer(0x01);
  constexpr size_t kValueCount =
      bt::gatt::RemoteService::kMaxReadByTypeResults + 1;

  // The fixture's service is too small to hold kValueCount attributes, so use
  // a separate service that spans the whole handle range.
  constexpr bt::PeerId kLargeServicePeerId(2);
  auto [large_service, large_service_client] = fake_gatt()->AddPeerService(
      kLargeServicePeerId,
      bt::gatt::ServiceData(
          bt::gatt::ServiceKind::PRIMARY, 0x0001, 0xFFFE, kServiceUuid));
  fidl::InterfaceHandle<fbgatt::RemoteService> handle;
  GattRemoteServiceServer server(
      large_service, gatt(), kLargeServicePeerId, handle.NewRequest());
  fbgatt::RemoteServicePtr proxy;
  proxy.Bind(std::move(handle));

  size_t read_count = 0;
  large_service_client->set_read_by_type_request_callback(
      [&](const bt::UUID&,
          bt::att::Handle start,
          bt::att::Handle,
          auto callback) {
        if (read_count++ == kValueCount) {
          ADD_FAILURE() << "Read continued past the result limit";
          callback(fit::error(bt::gatt::Client::ReadByTypeError{
              bt::att::Error(bt::att::ErrorCode::kAttributeNotFound), start}));
          return;
        }

        // Dispatch callback to prevent recursing too deep and breaking the
        // stack.
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

  std::optional<fbgatt::RemoteService_ReadByType_Result> fidl_result;
  proxy->ReadByType(fidl_helpers::UuidToFidl(kCharUuid), [&](auto cb_result) {
    fidl_result = std::move(cb_result);
  });

  RunLoopUntilIdle();
  EXPECT_EQ(kValueCount, read_count);
  ASSERT_TRUE(fidl_result.has_value());
  ASSERT_TRUE(fidl_result->is_err());
  EXPECT_EQ(fbgatt::Error::TOO_MANY_RESULTS, fidl_result->err());
}

}  // namespace
}  // namespace bthost
