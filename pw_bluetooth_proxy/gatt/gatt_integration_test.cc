// Copyright 2025 The Pigweed Authors
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

#include "../pw_bluetooth_proxy_private/test_utils.h"
#include "pw_allocator/testing.h"
#include "pw_bluetooth/l2cap_frames.emb.h"
#include "pw_bluetooth_proxy/gatt/gatt.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_containers/vector.h"
#include "pw_span/span.h"
#include "pw_unit_test/framework.h"

namespace pw::bluetooth::proxy::gatt {

namespace {

constexpr ConnectionHandle kConnectionHandle1{0x0001};
constexpr AttributeHandle kAttributeHandle1{0x0001};

constexpr uint16_t kAttFixedChannelId =
    static_cast<uint16_t>(emboss::L2capFixedCid::LE_U_ATTRIBUTE_PROTOCOL);

struct Notification {
  ConnectionHandle connection_handle;
  AttributeHandle value_handle;
  FlatConstMultiBufInstance value;
};

class FakeClientDelegate final : public Client::Delegate {
 public:
  const Vector<Notification>& notifications() const { return notifications_; }
  const Vector<std::pair<Error, ConnectionHandle>>& errors() const {
    return errors_;
  }

 private:
  void DoHandleNotification(ConnectionHandle connection_handle,
                            AttributeHandle value_handle,
                            FlatConstMultiBuf&& value) override {
    notifications_.push_back(
        {connection_handle, value_handle, std::move(value)});
  }

  void DoHandleError(Error error, ConnectionHandle connection_handle) override {
    errors_.emplace_back(error, connection_handle);
  }

  Vector<Notification, 15> notifications_;
  Vector<std::pair<Error, ConnectionHandle>, 10> errors_;
};

class FakeServerDelegate final : public Server::Delegate {
 public:
  FakeServerDelegate() = default;

  FakeServerDelegate(Function<void(ConnectionHandle)> write_available_fn)
      : write_available_fn_(std::move(write_available_fn)) {}

 private:
  void DoHandleWriteWithoutResponse(ConnectionHandle,
                                    AttributeHandle,
                                    FlatConstMultiBuf&&) override {}

  void DoHandleWriteAvailable(ConnectionHandle connection_handle) override {
    if (write_available_fn_) {
      write_available_fn_(connection_handle);
    }
  }

  void DoHandleError(Error /*error*/,
                     ConnectionHandle /*connection_handle*/) override {}

  Function<void(ConnectionHandle)> write_available_fn_;
};

class GattIntegrationTest : public ProxyHostTest {};

TEST_F(GattIntegrationTest, ReceiveNotification) {
  allocator::test::AllocatorForTest<4000> allocator;
  pw::bluetooth::proxy::MultiBufAllocator multibuf_allocator{allocator,
                                                             allocator};

  Function<void(H4PacketWithHci&&)> send_to_host_fn([](H4PacketWithHci&&) {});
  Function<void(H4PacketWithH4&&)> send_to_controller_fn(
      [](H4PacketWithH4&&) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/10,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              &allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 10));
  PW_TEST_ASSERT_OK(
      SendLeConnectionCompleteEvent(proxy,
                                    cpp23::to_underlying(kConnectionHandle1),
                                    emboss::StatusCode::SUCCESS));

  Gatt gatt(proxy, allocator, multibuf_allocator);

  FakeClientDelegate delegate;
  Result<Client> client = gatt.CreateClient(kConnectionHandle1, delegate);
  RunDispatcher();
  PW_TEST_ASSERT_OK(client);

  PW_TEST_ASSERT_OK(client->InterceptNotification(kAttributeHandle1));

  uint8_t expected_value = 0x08;
  std::array<uint8_t, 4> att_packet = {
      // ATT header
      0x1b,  // opcode (notification)
      0x01,
      0x00,           // kAttributeHandle1
      expected_value  // value
  };
  SendL2capBFrame(proxy,
                  cpp23::to_underlying(kConnectionHandle1),
                  att_packet,
                  att_packet.size(),
                  kAttFixedChannelId);

  ASSERT_EQ(delegate.notifications().size(), 1u);
  EXPECT_EQ(delegate.notifications()[0].connection_handle, kConnectionHandle1);
  EXPECT_EQ(delegate.notifications()[0].value_handle, kAttributeHandle1);
  span<const uint8_t> received_value =
      MultiBufAdapter::AsSpan(delegate.notifications()[0].value);
  EXPECT_EQ(received_value[0], expected_value);
}

TEST_F(GattIntegrationTest, SendNotifications) {
  allocator::test::AllocatorForTest<4000> allocator;
  pw::bluetooth::proxy::MultiBufAllocator multibuf_allocator{allocator,
                                                             allocator};

  struct {
    Vector<H4PacketWithH4, 20> packets_sent_to_controller;
  } capture;
  Function<void(H4PacketWithH4&&)> send_to_controller_fn(
      [&capture](H4PacketWithH4&& h4) {
        capture.packets_sent_to_controller.emplace_back(std::move(h4));
      });
  Function<void(H4PacketWithHci&&)> send_to_host_fn([](H4PacketWithHci&&) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              &allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 10));
  PW_TEST_ASSERT_OK(
      SendLeConnectionCompleteEvent(proxy,
                                    cpp23::to_underlying(kConnectionHandle1),
                                    emboss::StatusCode::SUCCESS));

  Gatt gatt(proxy, allocator, multibuf_allocator);
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};

  int write_available_count = 0;
  auto write_available_fn = [&write_available_count](ConnectionHandle) {
    ++write_available_count;
  };
  FakeServerDelegate delegate(std::move(write_available_fn));
  Result<Server> server =
      gatt.CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);

  FlatConstMultiBuf::Instance buffer(allocator);
  std::array<std::byte, 2> data = {std::byte{0x09}, std::byte{0x0a}};
  buffer->PushBack(data);

  StatusWithMultiBuf send_result =
      server->SendNotification(kAttributeHandle1, std::move(buffer));
  PW_TEST_EXPECT_OK(send_result.status);
  RunDispatcher();
  ASSERT_EQ(capture.packets_sent_to_controller.size(), 1u);

  std::array<uint8_t, 14> kExpected = {
      0x02,  // H4 indicator (ACL)
             // ACL header
      0x01,
      0x00,  // connection handle
      0x09,
      0x00,  // data total length
             // L2CAP header
      0x05,
      0x00,  // length
      0x04,
      0x00,  // channel ID (ATT)
             // ATT header
      0x1b,  //  opcode (ATT_HANDLE_VALUE_NTF)
      0x01,
      0x00,  // attribute handle
      0x09,
      0x0a,  // payload
  };
  auto sent = capture.packets_sent_to_controller.front().GetH4Span();
  EXPECT_TRUE(
      std::equal(sent.begin(), sent.end(), kExpected.begin(), kExpected.end()));

  do {
    FlatConstMultiBuf::Instance temp_buffer(allocator);
    temp_buffer->PushBack(data);
    send_result =
        server->SendNotification(kAttributeHandle1, std::move(temp_buffer));
  } while (send_result.status.ok());

  EXPECT_EQ(write_available_count, 0);
  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(
      proxy, {{cpp23::to_underlying(kConnectionHandle1), 1}}));
  EXPECT_EQ(write_available_count, 1);
  capture.packets_sent_to_controller.clear();
}

}  // namespace
}  // namespace pw::bluetooth::proxy::gatt
