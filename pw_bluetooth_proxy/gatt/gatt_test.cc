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

#include "pw_bluetooth_proxy/gatt/gatt.h"

#include <pw_bluetooth/l2cap_frames.emb.h>

#include "pw_allocator/libc_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_containers/vector.h"
#include "pw_span/span.h"
#include "pw_sync/no_lock.h"
#include "pw_unit_test/framework.h"

#if PW_BLUETOOTH_PROXY_MULTIBUF == PW_BLUETOOTH_PROXY_MULTIBUF_V1
#include "pw_multibuf/simple_allocator.h"
#else  // PW_BLUETOOTH_PROXY_MULTIBUF
#include "pw_allocator/synchronized_allocator.h"
#include "pw_allocator/testing.h"
#endif  // PW_BLUETOOTH_PROXY_MULTIBUF

namespace pw::bluetooth::proxy::gatt {

namespace {

constexpr size_t kMultiBufAllocatorCapacity = 500;

/// Helper class that can produce an initialized MultiBufAllocator for either
/// Multibuf V1 or V2, depending on the `PW_BLUETOOTH_PROXY_MULTIBUF` config
/// option.
template <size_t kDataCapacity, typename Lock = sync::NoLock>
class MultiBufAllocatorContext {
 public:
  MultiBufAllocator& GetAllocator() { return allocator_; }

 private:
  // Use libc allocators so msan can detect use after frees.
#if PW_BLUETOOTH_PROXY_MULTIBUF == PW_BLUETOOTH_PROXY_MULTIBUF_V1

  std::array<std::byte, kDataCapacity> buffer_{};
  pw::multibuf::SimpleAllocator allocator_{
      /*data_area=*/buffer_,
      /*metadata_alloc=*/allocator::GetLibCAllocator()};

#elif PW_BLUETOOTH_PROXY_MULTIBUF == PW_BLUETOOTH_PROXY_MULTIBUF_V2
  using BlockType = allocator::test::AllocatorForTest<0>::BlockType;

  static constexpr size_t kDataSize =
      AlignUp(kDataCapacity + BlockType::kBlockOverhead, BlockType::kAlignment);

  allocator::test::AllocatorForTest<kDataSize> data_alloc_;
  allocator::SynchronizedAllocator<Lock> synced_{data_alloc_};
  MultiBufAllocator allocator_{
      /*data_alloc=*/synced_,
      /*metadata_alloc=*/allocator::GetLibCAllocator()};

#endif  // PW_BLUETOOTH_PROXY_MULTIBUF
};

constexpr ConnectionHandle kConnectionHandle1{0x0001};
constexpr ConnectionHandle kConnectionHandle2{0x0002};
constexpr AttributeHandle kAttributeHandle1{0x0001};
constexpr AttributeHandle kAttributeHandle2{0x0002};
constexpr uint16_t kAttFixedChannelId =
    static_cast<uint16_t>(emboss::L2capFixedCid::LE_U_ATTRIBUTE_PROTOCOL);

using SpanReceiveFunction = L2capChannelManagerInterface::SpanReceiveFunction;

class FakeChannel final : public ChannelProxy {
 public:
  FakeChannel(Function<StatusWithMultiBuf(FlatConstMultiBuf&&)> write_cb)
      : write_cb_(std::move(write_cb)) {}

 private:
  StatusWithMultiBuf DoWrite(FlatConstMultiBuf&& buf) override {
    return write_cb_(std::move(buf));
  }
  Status DoIsWriteAvailable() override { return Status::Unimplemented(); }
  Status DoSendAdditionalRxCredits(uint16_t) override {
    return Status::Unimplemented();
  }

  Function<StatusWithMultiBuf(FlatConstMultiBuf&&)> write_cb_;
};

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
  struct WriteWithoutResponse {
    ConnectionHandle connection_handle;
    AttributeHandle value_handle;
    FlatConstMultiBufInstance value;
  };

  FakeServerDelegate() = default;

  FakeServerDelegate(Function<void(ConnectionHandle)> write_available_fn)
      : write_available_fn_(std::move(write_available_fn)) {}

  const Vector<WriteWithoutResponse>& write_without_responses() {
    return write_without_responses_;
  }

 private:
  void DoHandleWriteWithoutResponse(ConnectionHandle connection_handle,
                                    AttributeHandle value_handle,
                                    FlatConstMultiBuf&& value) override {
    write_without_responses_.emplace_back(WriteWithoutResponse{
        connection_handle, value_handle, std::move(value)});
  }

  void DoHandleWriteAvailable(ConnectionHandle connection_handle) override {
    if (write_available_fn_) {
      write_available_fn_(connection_handle);
    }
  }

  void DoHandleError(Error /*error*/,
                     ConnectionHandle /*connection_handle*/) override {}

  Function<void(ConnectionHandle)> write_available_fn_;
  Vector<WriteWithoutResponse, 15> write_without_responses_;
};

class GattTest : public ::testing::Test, public L2capChannelManagerInterface {
 public:
  void SetUp() override {
    gatt_.emplace(*this, allocator_, allocator_context_.GetAllocator());
  }

  Gatt& gatt() { return gatt_.value(); }

  Vector<SpanReceiveFunction>& receive_from_controller_functions() {
    return payload_from_controller_fns_;
  }

  Vector<SpanReceiveFunction>& receive_from_host_functions() {
    return payload_from_host_fns_;
  }

  bool ReceiveFromController(span<std::byte> payload) {
    PW_CHECK(!payload_from_controller_fns_.empty());
    return payload_from_controller_fns_[0](
        payload, kConnectionHandle1, kAttFixedChannelId, kAttFixedChannelId);
  }

  bool ReceiveFromHost(span<std::byte> payload) {
    PW_CHECK(!payload_from_host_fns_.empty());
    return payload_from_host_fns_[0](
        payload, kConnectionHandle1, kAttFixedChannelId, kAttFixedChannelId);
  }

  const Vector<ChannelEventCallback>& event_callbacks() const {
    return event_callbacks_;
  }

  void SendEvent(L2capChannelEvent event) {
    PW_CHECK(!event_callbacks_.empty());
    event_callbacks_[0](event);
  }

  void set_intercept_channel_status(Status status) {
    intercept_channel_status_ = status;
  }

  MultiBufAllocator& multibuf_allocator() {
    return allocator_context_.GetAllocator();
  }

  auto& allocator() { return allocator_; }

  const Vector<FlatConstMultiBufInstance>& write_buffers() const {
    return write_buffers_;
  }

  void set_simulate_channel_write_failures(bool enable) {
    simulate_channel_write_failures_ = enable;
  }

 private:
  Result<UniquePtr<ChannelProxy>> DoInterceptCreditBasedFlowControlChannel(
      ConnectionHandle,
      ConnectionOrientedChannelConfig,
      ConnectionOrientedChannelConfig,
      Function<void(FlatConstMultiBuf&& payload)>&&,
      ChannelEventCallback&&) override {
    return Status::Unimplemented();
  }

  Result<UniquePtr<ChannelProxy>> DoInterceptBasicModeChannel(
      ConnectionHandle /*connection_handle*/,
      uint16_t local_channel_id,
      uint16_t remote_channel_id,
      AclTransportType transport,
      BufferReceiveFunction&& payload_from_controller_fn,
      BufferReceiveFunction&& payload_from_host_fn,
      ChannelEventCallback&& event_fn) override {
    if (!intercept_channel_status_.ok()) {
      return intercept_channel_status_;
    }

    EXPECT_EQ(
        local_channel_id,
        static_cast<uint16_t>(emboss::L2capFixedCid::LE_U_ATTRIBUTE_PROTOCOL));
    EXPECT_EQ(
        remote_channel_id,
        static_cast<uint16_t>(emboss::L2capFixedCid::LE_U_ATTRIBUTE_PROTOCOL));
    EXPECT_EQ(transport, AclTransportType::kLe);

    auto write_cb = [this](FlatConstMultiBuf&& buf) -> StatusWithMultiBuf {
      if (simulate_channel_write_failures_) {
        return {Status::Unavailable(), std::move(buf)};
      }
      write_buffers_.emplace_back(std::move(buf));
      return {OkStatus()};
    };

    UniquePtr<ChannelProxy> channel =
        allocator_.MakeUnique<FakeChannel>(std::move(write_cb));
    PW_CHECK(channel != nullptr);
    payload_from_controller_fns_.emplace_back(
        std::move(std::get<SpanReceiveFunction>(payload_from_controller_fn)));
    payload_from_host_fns_.emplace_back(
        std::move(std::get<SpanReceiveFunction>(payload_from_host_fn)));
    event_callbacks_.emplace_back(std::move(event_fn));
    return channel;
  }

  allocator::test::AllocatorForTest<1000> allocator_;
  MultiBufAllocatorContext<kMultiBufAllocatorCapacity> allocator_context_;
  std::optional<Gatt> gatt_;
  Vector<SpanReceiveFunction, 10> payload_from_controller_fns_;
  Vector<SpanReceiveFunction, 10> payload_from_host_fns_;
  Vector<ChannelEventCallback, 10> event_callbacks_;
  Vector<FlatConstMultiBufInstance, 15> write_buffers_;
  Status intercept_channel_status_ = OkStatus();
  bool simulate_channel_write_failures_ = false;
};

TEST_F(
    GattTest,
    Receive2NotificationsForMatchingHandleAndIgnore1NotificationForOtherHandle) {
  FakeClientDelegate delegate;

  Result<Client> client = gatt().CreateClient(kConnectionHandle1, delegate);
  PW_TEST_ASSERT_OK(client);
  ASSERT_EQ(receive_from_controller_functions().size(), 1u);

  PW_TEST_ASSERT_OK(client->InterceptNotification(kAttributeHandle1));

  std::array<std::byte, 1> expected_value_0 = {std::byte{0x08}};
  std::array<std::byte, 4> att_packet_0 = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x01},
      std::byte{0x00},     // handle
      expected_value_0[0]  // value
  };
  EXPECT_TRUE(ReceiveFromController(att_packet_0));
  ASSERT_EQ(delegate.notifications().size(), 1u);
  EXPECT_EQ(delegate.notifications()[0].connection_handle, kConnectionHandle1);
  EXPECT_EQ(delegate.notifications()[0].value_handle, kAttributeHandle1);

  span<const std::byte> value_0 =
      as_bytes(MultiBufAdapter::AsSpan(delegate.notifications()[0].value));
  EXPECT_TRUE(std::equal(value_0.begin(),
                         value_0.end(),
                         expected_value_0.begin(),
                         expected_value_0.end()));

  std::array<std::byte, 2> expected_value_1 = {std::byte{0x09},
                                               std::byte{0x0a}};
  std::array<std::byte, 5> att_packet_1 = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x01},
      std::byte{0x00},  // handle
      expected_value_1[0],
      expected_value_1[1]  // value
  };
  EXPECT_TRUE(ReceiveFromController(att_packet_1));
  ASSERT_EQ(delegate.notifications().size(), 2u);
  EXPECT_EQ(delegate.notifications()[1].connection_handle, kConnectionHandle1);
  EXPECT_EQ(delegate.notifications()[1].value_handle, kAttributeHandle1);

  span<const std::byte> value_1 =
      as_bytes(MultiBufAdapter::AsSpan(delegate.notifications()[1].value));
  EXPECT_TRUE(std::equal(value_1.begin(),
                         value_1.end(),
                         expected_value_1.begin(),
                         expected_value_1.end()));

  std::array<std::byte, 4> att_packet_2 = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x09},
      std::byte{0x00},  // handle
      std::byte{0x99}   // value
  };
  EXPECT_FALSE(ReceiveFromController(att_packet_2));
  ASSERT_EQ(delegate.notifications().size(), 2u);
}

TEST_F(GattTest, IgnoreReceivingUnsupportedOpcode) {
  FakeClientDelegate delegate;
  Result<Client> client = gatt().CreateClient(kConnectionHandle1, delegate);
  PW_TEST_ASSERT_OK(client);
  ASSERT_EQ(receive_from_controller_functions().size(), 1u);
  PW_TEST_ASSERT_OK(client->InterceptNotification(kAttributeHandle1));

  std::array<std::byte, 1> expected_value = {std::byte{0x08}};
  std::array<std::byte, 4> att_packet = {
      std::byte{0x12},  // opcode (write request)
      std::byte{0x01},
      std::byte{0x00},   // handle
      expected_value[0]  // value
  };
  EXPECT_FALSE(ReceiveFromController(att_packet));
  ASSERT_EQ(delegate.notifications().size(), 0u);
}

TEST_F(GattTest, TwoClientsReceiveSameNotification) {
  FakeClientDelegate delegate_0;
  Result<Client> client_0 = gatt().CreateClient(kConnectionHandle1, delegate_0);
  PW_TEST_ASSERT_OK(client_0);
  ASSERT_EQ(receive_from_controller_functions().size(), 1u);
  PW_TEST_ASSERT_OK(client_0->InterceptNotification(kAttributeHandle1));

  FakeClientDelegate delegate_1;
  Result<Client> client_1 = gatt().CreateClient(kConnectionHandle1, delegate_1);
  PW_TEST_ASSERT_OK(client_1);
  ASSERT_EQ(receive_from_controller_functions().size(), 1u);
  PW_TEST_ASSERT_OK(client_1->InterceptNotification(kAttributeHandle1));

  std::array<std::byte, 1> expected_value_0 = {std::byte{0x08}};
  std::array<std::byte, 4> att_packet_0 = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x01},
      std::byte{0x00},     // handle
      expected_value_0[0]  // value
  };
  EXPECT_TRUE(ReceiveFromController(att_packet_0));
  ASSERT_EQ(delegate_0.notifications().size(), 1u);
  EXPECT_EQ(delegate_0.notifications()[0].connection_handle,
            kConnectionHandle1);
  EXPECT_EQ(delegate_0.notifications()[0].value_handle, kAttributeHandle1);
  ASSERT_EQ(delegate_1.notifications().size(), 1u);
  EXPECT_EQ(delegate_1.notifications()[0].connection_handle,
            kConnectionHandle1);
  EXPECT_EQ(delegate_1.notifications()[0].value_handle, kAttributeHandle1);
}

TEST_F(GattTest, PayloadFromHostIgnored) {
  FakeClientDelegate delegate;
  Result<Client> client = gatt().CreateClient(kConnectionHandle1, delegate);
  PW_TEST_ASSERT_OK(client);
  ASSERT_EQ(receive_from_host_functions().size(), 1u);
  PW_TEST_ASSERT_OK(client->InterceptNotification(kAttributeHandle1));

  std::array<std::byte, 1> expected_value_0 = {std::byte{0x08}};
  std::array<std::byte, 4> att_packet_0 = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x01},
      std::byte{0x00},     // handle
      expected_value_0[0]  // value
  };
  EXPECT_FALSE(ReceiveFromHost(att_packet_0));
  ASSERT_EQ(delegate.notifications().size(), 0u);
}

TEST_F(GattTest, CancelInterceptNotification) {
  FakeClientDelegate delegate;
  Result<Client> client = gatt().CreateClient(kConnectionHandle1, delegate);
  PW_TEST_ASSERT_OK(client);

  PW_TEST_ASSERT_OK(client->InterceptNotification(kAttributeHandle1));

  std::array<std::byte, 1> expected_value_0 = {std::byte{0x08}};
  std::array<std::byte, 4> att_packet_0 = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x01},
      std::byte{0x00},     // handle
      expected_value_0[0]  // value
  };
  EXPECT_TRUE(ReceiveFromController(att_packet_0));
  ASSERT_EQ(delegate.notifications().size(), 1u);
  EXPECT_EQ(delegate.notifications()[0].connection_handle, kConnectionHandle1);
  EXPECT_EQ(delegate.notifications()[0].value_handle, kAttributeHandle1);

  span<const std::byte> value_0 =
      as_bytes(MultiBufAdapter::AsSpan(delegate.notifications()[0].value));
  EXPECT_TRUE(std::equal(value_0.begin(),
                         value_0.end(),
                         expected_value_0.begin(),
                         expected_value_0.end()));

  PW_TEST_ASSERT_OK(client->CancelInterceptNotification(kAttributeHandle1));

  std::array<std::byte, 2> expected_value_1 = {std::byte{0x09},
                                               std::byte{0x0a}};
  std::array<std::byte, 5> att_packet_1 = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x01},
      std::byte{0x00},  // handle
      expected_value_1[0],
      expected_value_1[1]  // value
  };
  EXPECT_FALSE(ReceiveFromController(att_packet_1));
  ASSERT_EQ(delegate.notifications().size(), 1u);
}

TEST_F(GattTest, CloseClient) {
  FakeClientDelegate delegate;
  Result<Client> client = gatt().CreateClient(kConnectionHandle1, delegate);
  PW_TEST_ASSERT_OK(client);

  PW_TEST_ASSERT_OK(client->InterceptNotification(kAttributeHandle1));
  client->Close();
  EXPECT_EQ(delegate.errors().size(), 1u);
  client->Close();  // Should be no-op
  EXPECT_EQ(delegate.errors().size(), 1u);

  std::array<std::byte, 1> expected_value_0 = {std::byte{0x08}};
  std::array<std::byte, 4> att_packet_0 = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x01},
      std::byte{0x00},     // handle
      expected_value_0[0]  // value
  };
  EXPECT_FALSE(ReceiveFromController(att_packet_0));
  ASSERT_EQ(delegate.notifications().size(), 0u);
}

TEST_F(GattTest, MovingIntoClientClosesClient) {
  FakeClientDelegate delegate;
  Result<Client> result = gatt().CreateClient(kConnectionHandle1, delegate);
  PW_TEST_ASSERT_OK(result);
  Client client = std::move(result.value());

  PW_TEST_ASSERT_OK(client.InterceptNotification(kAttributeHandle1));
  Client client2;
  client = std::move(client2);
  EXPECT_EQ(delegate.errors().size(), 1u);

  std::array<std::byte, 1> expected_value_0 = {std::byte{0x08}};
  std::array<std::byte, 4> att_packet_0 = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x01},
      std::byte{0x00},     // handle
      expected_value_0[0]  // value
  };
  EXPECT_FALSE(ReceiveFromController(att_packet_0));
  ASSERT_EQ(delegate.notifications().size(), 0u);
}

TEST_F(GattTest, NotifyClientOnChannelClose) {
  FakeClientDelegate delegate;
  Result<Client> client = gatt().CreateClient(kConnectionHandle1, delegate);
  PW_TEST_ASSERT_OK(client);
  PW_TEST_ASSERT_OK(client->InterceptNotification(kAttributeHandle1));
  ASSERT_EQ(delegate.errors().size(), 0u);

  SendEvent(L2capChannelEvent::kChannelClosedByOther);
  ASSERT_EQ(delegate.errors().size(), 1u);
  EXPECT_EQ(delegate.errors()[0].first, Error::kDisconnection);
  EXPECT_EQ(delegate.errors()[0].second, kConnectionHandle1);
}

TEST_F(GattTest, NotifyClientOnReset) {
  FakeClientDelegate delegate;
  Result<Client> client = gatt().CreateClient(kConnectionHandle1, delegate);
  PW_TEST_ASSERT_OK(client);
  PW_TEST_ASSERT_OK(client->InterceptNotification(kAttributeHandle1));
  ASSERT_EQ(delegate.errors().size(), 0u);

  SendEvent(L2capChannelEvent::kReset);
  ASSERT_EQ(delegate.errors().size(), 1u);
  EXPECT_EQ(delegate.errors()[0].first, Error::kReset);
  EXPECT_EQ(delegate.errors()[0].second, kConnectionHandle1);
}

TEST_F(GattTest, IgnoreEmptyAttPacket) {
  FakeClientDelegate delegate;
  Result<Client> client = gatt().CreateClient(kConnectionHandle1, delegate);
  PW_TEST_ASSERT_OK(client);
  PW_TEST_ASSERT_OK(client->InterceptNotification(kAttributeHandle1));

  EXPECT_FALSE(ReceiveFromController(ByteSpan()));
  ASSERT_EQ(delegate.notifications().size(), 0u);
}

TEST_F(GattTest, IgnoreReceivedTooSmallAttNotificationPacket) {
  FakeClientDelegate delegate;
  Result<Client> client = gatt().CreateClient(kConnectionHandle1, delegate);
  PW_TEST_ASSERT_OK(client);
  PW_TEST_ASSERT_OK(client->InterceptNotification(kAttributeHandle1));

  std::array<std::byte, 2> invalid_att_packet = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x01}   // truncated handle
  };
  EXPECT_FALSE(ReceiveFromController(invalid_att_packet));
  ASSERT_EQ(delegate.notifications().size(), 0u);
}

TEST_F(GattTest, InterceptNotificationOn2Connections) {
  FakeClientDelegate delegate_0;
  Result<Client> client_0 = gatt().CreateClient(kConnectionHandle1, delegate_0);
  PW_TEST_ASSERT_OK(client_0);
  PW_TEST_ASSERT_OK(client_0->InterceptNotification(kAttributeHandle1));

  FakeClientDelegate delegate_1;
  Result<Client> client_1 = gatt().CreateClient(kConnectionHandle2, delegate_1);
  PW_TEST_ASSERT_OK(client_1);
  PW_TEST_ASSERT_OK(client_1->InterceptNotification(kAttributeHandle2));

  std::array<std::byte, 1> expected_value_0 = {std::byte{0x08}};
  std::array<std::byte, 4> att_packet_0 = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x01},
      std::byte{0x00},     // handle
      expected_value_0[0]  // value
  };
  EXPECT_TRUE(ReceiveFromController(att_packet_0));
  ASSERT_EQ(delegate_0.notifications().size(), 1u);
  EXPECT_EQ(delegate_0.notifications()[0].connection_handle,
            kConnectionHandle1);
  EXPECT_EQ(delegate_0.notifications()[0].value_handle, kAttributeHandle1);

  span<const std::byte> value_0 =
      as_bytes(MultiBufAdapter::AsSpan(delegate_0.notifications()[0].value));
  EXPECT_TRUE(std::equal(value_0.begin(),
                         value_0.end(),
                         expected_value_0.begin(),
                         expected_value_0.end()));

  std::array<std::byte, 2> expected_value_1 = {std::byte{0x09},
                                               std::byte{0x0a}};
  std::array<std::byte, 5> att_packet_1 = {
      std::byte{0x1b},  // opcode (notification)
      std::byte{0x02},
      std::byte{0x00},  // handle
      expected_value_1[0],
      expected_value_1[1]  // value
  };
  EXPECT_TRUE(receive_from_controller_functions()[1](att_packet_1,
                                                     kConnectionHandle2,
                                                     kAttFixedChannelId,
                                                     kAttFixedChannelId));
  ASSERT_EQ(delegate_0.notifications().size(), 1u);
  ASSERT_EQ(delegate_1.notifications().size(), 1u);
  EXPECT_EQ(delegate_1.notifications()[0].connection_handle,
            kConnectionHandle2);
  EXPECT_EQ(delegate_1.notifications()[0].value_handle, kAttributeHandle2);
}

TEST_F(GattTest, InterceptBasicModeChannelReturnsError) {
  set_intercept_channel_status(Status::InvalidArgument());
  FakeClientDelegate delegate;
  Result<Client> client = gatt().CreateClient(kConnectionHandle1, delegate);
  EXPECT_EQ(client.status(), Status::Unavailable());
}

TEST_F(GattTest, ServerSendNotification) {
  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);

  std::array<std::byte, 2> payload = {std::byte{0x09}, std::byte{0x0a}};
  std::optional<FlatMultiBufInstance> value_buf =
      MultiBufAdapter::Create(multibuf_allocator(), 2);
  ASSERT_TRUE(value_buf.has_value());
  MultiBufAdapter::Copy(*value_buf, /*dst_offset=*/0, payload);

  StatusWithMultiBuf result = server->SendNotification(
      kAttributeHandle1, std::move(MultiBufAdapter::Unwrap(*value_buf)));
  PW_TEST_ASSERT_OK(result.status);
  ASSERT_EQ(write_buffers().size(), 1u);
  std::array<uint8_t, 5> kExpectedPacket = {
      0x1b,  // opcode (ATT_HANDLE_VALUE_NTF)
      0x01,
      0x00,  // attribute handle
      0x09,
      0x0a,  // payload
  };
  span<const uint8_t> actual_packet =
      MultiBufAdapter::AsSpan(write_buffers()[0]);
  EXPECT_TRUE(std::equal(actual_packet.begin(),
                         actual_packet.end(),
                         kExpectedPacket.begin(),
                         kExpectedPacket.end()));
  server->Close();
}

TEST_F(GattTest, ServerSendNotificationForUnownedCharacteristicFails) {
  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);

  std::array<std::byte, 2> payload = {std::byte{0x09}, std::byte{0x0a}};
  std::optional<FlatMultiBufInstance> value_buf =
      MultiBufAdapter::Create(multibuf_allocator(), 2);
  ASSERT_TRUE(value_buf.has_value());
  MultiBufAdapter::Copy(*value_buf, /*dst_offset=*/0, payload);

  StatusWithMultiBuf send_result = server->SendNotification(
      kAttributeHandle2, std::move(MultiBufAdapter::Unwrap(*value_buf)));
  EXPECT_FALSE(send_result.status.ok());
  EXPECT_TRUE(send_result.buf.has_value());
  server->Close();
}

TEST_F(GattTest, ServerSendNotificationForCharacteristicOwnedByOtherServer) {
  FakeServerDelegate delegate;

  std::array<CharacteristicInfo, 1> characteristic_info_0 = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server_0 =
      gatt().CreateServer(kConnectionHandle1, characteristic_info_0, delegate);
  PW_TEST_ASSERT_OK(server_0);

  std::array<CharacteristicInfo, 1> characteristic_info_1 = {
      CharacteristicInfo{kAttributeHandle2}};
  Result<Server> server_1 =
      gatt().CreateServer(kConnectionHandle1, characteristic_info_1, delegate);
  PW_TEST_ASSERT_OK(server_1);

  std::array<std::byte, 2> payload = {std::byte{0x09}, std::byte{0x0a}};
  std::optional<FlatMultiBufInstance> value_buf =
      MultiBufAdapter::Create(multibuf_allocator(), 2);
  ASSERT_TRUE(value_buf.has_value());
  MultiBufAdapter::Copy(*value_buf, /*dst_offset=*/0, payload);

  StatusWithMultiBuf send_result = server_0->SendNotification(
      kAttributeHandle2, std::move(MultiBufAdapter::Unwrap(*value_buf)));
  EXPECT_FALSE(send_result.status.ok());
  EXPECT_TRUE(send_result.buf.has_value());
  server_0->Close();
}

TEST_F(GattTest, CreateClientAndServerForSameConnection) {
  FakeClientDelegate client_delegate;
  Result<Client> client =
      gatt().CreateClient(kConnectionHandle1, client_delegate);
  PW_TEST_ASSERT_OK(client);

  FakeServerDelegate server_delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server = gatt().CreateServer(
      kConnectionHandle1, characteristic_info, server_delegate);
  PW_TEST_ASSERT_OK(server);

  client->Close();
  server->Close();
}

TEST_F(GattTest, ServerSendNotificationInsideHandleWriteAvailable) {
  struct {
    GattTest* test;
    Result<Server> server;
    int write_available_count = 0;
  } capture;
  capture.test = this;

  auto write_available_cb = [&capture](ConnectionHandle handle) {
    ++capture.write_available_count;
    EXPECT_EQ(handle, kConnectionHandle1);

    std::array<std::byte, 2> payload = {std::byte{0x09}, std::byte{0x0a}};
    std::optional<FlatMultiBufInstance> value_buf =
        MultiBufAdapter::Create(capture.test->multibuf_allocator(), 2);
    ASSERT_TRUE(value_buf.has_value());
    MultiBufAdapter::Copy(*value_buf, /*dst_offset=*/0, payload);

    StatusWithMultiBuf result = capture.server.value().SendNotification(
        kAttributeHandle1, std::move(MultiBufAdapter::Unwrap(*value_buf)));
    PW_TEST_ASSERT_OK(result.status);
  };

  FakeServerDelegate delegate(std::move(write_available_cb));
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  capture.server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(capture.server);

  SendEvent(L2capChannelEvent::kWriteAvailable);
  EXPECT_EQ(capture.write_available_count, 1);
  ASSERT_EQ(write_buffers().size(), 1u);
  capture.server->Close();
}

TEST_F(GattTest, ServerWriteAvailableFor2Servers) {
  struct {
    int write_available_count = 0;
  } capture_0;

  auto write_available_cb_0 = [&capture_0](ConnectionHandle handle) {
    ++capture_0.write_available_count;
    EXPECT_EQ(handle, kConnectionHandle1);
  };

  FakeServerDelegate delegate_0(std::move(write_available_cb_0));
  std::array<CharacteristicInfo, 1> characteristic_info_0 = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server_0 = gatt().CreateServer(
      kConnectionHandle1, characteristic_info_0, delegate_0);
  PW_TEST_ASSERT_OK(server_0);

  struct {
    int write_available_count = 0;
  } capture_1;

  auto write_available_cb_1 = [&capture_1](ConnectionHandle handle) {
    ++capture_1.write_available_count;
    EXPECT_EQ(handle, kConnectionHandle1);
  };

  FakeServerDelegate delegate_1(std::move(write_available_cb_1));
  std::array<CharacteristicInfo, 1> characteristic_info_1 = {
      CharacteristicInfo{kAttributeHandle2}};
  Result<Server> server_1 = gatt().CreateServer(
      kConnectionHandle1, characteristic_info_1, delegate_1);
  PW_TEST_ASSERT_OK(server_1);

  SendEvent(L2capChannelEvent::kWriteAvailable);
  EXPECT_EQ(capture_0.write_available_count, 1);
  EXPECT_EQ(capture_1.write_available_count, 1);
  server_1->Close();
}

TEST_F(GattTest, ServerSendNotificationWriteFails) {
  set_simulate_channel_write_failures(true);

  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);

  std::array<std::byte, 2> payload = {std::byte{0x09}, std::byte{0x0a}};
  std::optional<FlatMultiBufInstance> value_buf =
      MultiBufAdapter::Create(multibuf_allocator(), 2);
  ASSERT_TRUE(value_buf.has_value());
  MultiBufAdapter::Copy(*value_buf, /*dst_offset=*/0, payload);

  StatusWithMultiBuf result = server->SendNotification(
      kAttributeHandle1, std::move(MultiBufAdapter::Unwrap(*value_buf)));
  EXPECT_EQ(result.status, Status::Unavailable());
  EXPECT_TRUE(result.buf.has_value());
  server->Close();
}

TEST_F(GattTest, ServerReceivesWriteWithoutResponse) {
  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);

  const std::array<uint8_t, 2> kExpectedValue = {0x09, 0x0a};
  std::array<std::byte, 5> att_packet = {
      std::byte{0x52},  // opcode (ATT_WRITE_CMD)
      std::byte{0x01},
      std::byte{0x00},  // handle
      std::byte{0x09},
      std::byte{0x0a}  // value
  };
  ReceiveFromController(att_packet);
  ASSERT_EQ(delegate.write_without_responses().size(), 1u);
  EXPECT_EQ(delegate.write_without_responses()[0].connection_handle,
            kConnectionHandle1);
  EXPECT_EQ(delegate.write_without_responses()[0].value_handle,
            kAttributeHandle1);
  span<const uint8_t> value_span =
      MultiBufAdapter::AsSpan(delegate.write_without_responses()[0].value);
  EXPECT_TRUE(std::equal(value_span.begin(),
                         value_span.end(),
                         kExpectedValue.begin(),
                         kExpectedValue.end()));
  server->Close();
}

TEST_F(GattTest, ServerReceivesInvalidWriteWithoutResponse) {
  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);
  std::array<std::byte, 2> att_packet = {
      std::byte{0x52},  // opcode (ATT_WRITE_CMD)
      std::byte{0x01},  // first half of handle
  };
  EXPECT_FALSE(ReceiveFromController(att_packet));
  ASSERT_EQ(delegate.write_without_responses().size(), 0u);
  server->Close();
}

TEST_F(GattTest, ServerReceivesWriteWithoutResponseForUnknownHandle) {
  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);
  std::array<std::byte, 5> att_packet = {
      std::byte{0x52},  // opcode (ATT_WRITE_CMD)
      std::byte{0x09},
      std::byte{0x00},  // handle (9 is not offloaded)
      std::byte{0x09},
      std::byte{0x0a}  // value
  };
  EXPECT_FALSE(ReceiveFromController(att_packet));
  ASSERT_EQ(delegate.write_without_responses().size(), 0u);
  server->Close();
}

TEST_F(GattTest, ServerReceivesWriteWithoutResponseWhileAllocatorExhausted) {
  std::optional<FlatMultiBufInstance> buffer =
      MultiBufAdapter::Create(multibuf_allocator(), kMultiBufAllocatorCapacity);
  ASSERT_TRUE(buffer.has_value());

  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);
  std::array<std::byte, 5> att_packet = {
      std::byte{0x52},  // opcode (ATT_WRITE_CMD)
      std::byte{0x01},
      std::byte{0x00},  // handle
      std::byte{0x09},
      std::byte{0x0a}  // value
  };
  EXPECT_TRUE(ReceiveFromController(att_packet));
  ASSERT_EQ(delegate.write_without_responses().size(), 0u);
  server->Close();
}

TEST_F(GattTest, ServerAddCharacteristic) {
  FakeServerDelegate delegate;
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, /*characteristics=*/{}, delegate);
  PW_TEST_ASSERT_OK(server);
  PW_TEST_ASSERT_OK(
      server->AddCharacteristic(CharacteristicInfo{kAttributeHandle1}));
  std::array<std::byte, 5> att_packet = {
      std::byte{0x52},  // opcode (ATT_WRITE_CMD)
      std::byte{0x01},
      std::byte{0x00},  // handle
      std::byte{0x09},
      std::byte{0x0a}  // value
  };
  ReceiveFromController(att_packet);
  ASSERT_EQ(delegate.write_without_responses().size(), 1u);
  server->Close();
}

TEST_F(GattTest, ServerAddCharacteristicAlreadyExists) {
  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);
  EXPECT_EQ(server->AddCharacteristic(characteristic_info[0]),
            Status::AlreadyExists());
  server->Close();
}

TEST_F(GattTest, ServerAddCharacteristicConnectionNotFound) {
  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);
  SendEvent(L2capChannelEvent::kChannelClosedByOther);
  EXPECT_EQ(server->AddCharacteristic(characteristic_info[0]),
            Status::FailedPrecondition());
  server->Close();
}

TEST_F(GattTest, ServerAddCharacteristicAllocationFailure) {
  FakeServerDelegate delegate;
  Result<Server> server = gatt().CreateServer(kConnectionHandle1, {}, delegate);
  PW_TEST_ASSERT_OK(server);
  allocator().Exhaust();
  EXPECT_EQ(server->AddCharacteristic(CharacteristicInfo{kAttributeHandle1}),
            Status::ResourceExhausted());
  server->Close();
}

TEST_F(GattTest, ServerRemoveCharacteristic) {
  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);
  PW_TEST_ASSERT_OK(server->RemoveCharacteristic(characteristic_info[0]));
  // Write commands for the removed characteristic should be ignored and
  // forwarded.
  std::array<std::byte, 5> att_packet = {
      std::byte{0x52},  // opcode (ATT_WRITE_CMD)
      std::byte{0x01},
      std::byte{0x00},  // handle
      std::byte{0x09},
      std::byte{0x0a}  // value
  };
  EXPECT_FALSE(ReceiveFromController(att_packet));
  ASSERT_EQ(delegate.write_without_responses().size(), 0u);
  server->Close();
}

TEST_F(GattTest, ServerRemoveCharacteristicConnectionClosed) {
  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);
  SendEvent(L2capChannelEvent::kChannelClosedByOther);
  EXPECT_EQ(server->RemoveCharacteristic(characteristic_info[0]),
            Status::FailedPrecondition());
  server->Close();
}

TEST_F(GattTest, ServerRemoveCharacteristicThatIsNotOffloaded) {
  FakeServerDelegate delegate;
  std::array<CharacteristicInfo, 1> characteristic_info = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server =
      gatt().CreateServer(kConnectionHandle1, characteristic_info, delegate);
  PW_TEST_ASSERT_OK(server);
  EXPECT_EQ(server->RemoveCharacteristic(CharacteristicInfo{kAttributeHandle2}),
            Status::NotFound());
  server->Close();
}

TEST_F(GattTest, ServerRemoveCharacteristicOffloadedByDifferentServer) {
  FakeServerDelegate delegate;

  std::array<CharacteristicInfo, 1> characteristic_info_0 = {
      CharacteristicInfo{kAttributeHandle1}};
  Result<Server> server_0 =
      gatt().CreateServer(kConnectionHandle1, characteristic_info_0, delegate);
  PW_TEST_ASSERT_OK(server_0);

  std::array<CharacteristicInfo, 1> characteristic_info_1 = {
      CharacteristicInfo{kAttributeHandle2}};
  Result<Server> server_1 =
      gatt().CreateServer(kConnectionHandle1, characteristic_info_1, delegate);
  PW_TEST_ASSERT_OK(server_1);

  EXPECT_EQ(
      server_1->RemoveCharacteristic(CharacteristicInfo{kAttributeHandle1}),
      Status::NotFound());
  server_1->Close();
}

}  // namespace
}  // namespace pw::bluetooth::proxy::gatt
