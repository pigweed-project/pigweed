// Copyright 2026 The Pigweed Authors
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

#include "pw_bluetooth_proxy/rfcomm/rfcomm_manager.h"

#include "pw_allocator/libc_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/l2cap_channel_manager_interface.h"
#include "pw_bytes/span.h"
#include "pw_containers/vector.h"
#include "pw_unit_test/framework.h"

#if PW_BLUETOOTH_PROXY_MULTIBUF == PW_BLUETOOTH_PROXY_MULTIBUF_V1
#include "pw_multibuf/simple_allocator.h"
#else
#include "pw_allocator/synchronized_allocator.h"
#include "pw_multibuf/multibuf.h"
#include "pw_sync/mutex.h"
#endif  // PW_BLUETOOTH_PROXY_MULTIBUF

namespace pw::bluetooth::proxy::rfcomm {
namespace testing {

class MockChannelProxy : public ChannelProxy {
 public:
  span<const uint8_t> last_written_payload() const {
    return last_written_payload_data_;
  }

 private:
  StatusWithMultiBuf DoWrite(FlatConstMultiBuf&& payload) override {
    last_written_payload_data_.resize(payload.size());
    MultiBufAdapter::Copy(
        as_writable_bytes(span(last_written_payload_data_)), payload, 0);
    return {OkStatus()};
  }

  Status DoIsWriteAvailable() override { return OkStatus(); }

  Status DoSendAdditionalRxCredits(
      uint16_t /*additional_rx_credits*/) override {
    return OkStatus();
  }

  pw::Vector<uint8_t, 256> last_written_payload_data_;
};

class MockL2capChannelManager final : public L2capChannelManagerInterface {
 public:
  MockL2capChannelManager() = default;

  // Triggers the from_controller callback to simulate an incoming L2CAP PDU.
  bool TriggerControllerPdu(FlatMultiBuf&& pdu,
                            ConnectionHandle handle,
                            uint16_t local_cid,
                            uint16_t remote_cid) {
    if (auto* fn = std::get_if<OptionalBufferReceiveFunction>(
            &payload_from_controller_fn_)) {
      if (auto result = (*fn)(std::move(pdu), handle, local_cid, remote_cid);
          result.has_value()) {
        return true;
      }
    }
    return false;
  }

  // Triggers the event callback to simulate an L2CAP channel event.
  void TriggerL2capEvent(L2capChannelEvent event) {
    if (event_fn_) {
      event_fn_(event);
    }
  }

  uint32_t intercept_channel_count() const { return intercept_channel_count_; }

 private:
  Result<UniquePtr<ChannelProxy>> DoInterceptCreditBasedFlowControlChannel(
      ConnectionHandle,
      ConnectionOrientedChannelConfig,
      ConnectionOrientedChannelConfig,
      MultiBufReceiveFunction&&,
      ChannelEventCallback&&) override {
    return Status::Unimplemented();
  }

  Result<UniquePtr<ChannelProxy>> DoInterceptBasicModeChannel(
      ConnectionHandle /*connection_handle*/,
      uint16_t /*local_channel_id*/,
      uint16_t /*remote_channel_id*/,
      AclTransportType /*transport*/,
      BufferReceiveFunction&& payload_from_controller_fn,
      BufferReceiveFunction&& /*payload_from_host_fn*/,
      ChannelEventCallback&& event_fn) override {
    intercept_channel_count_++;
    payload_from_controller_fn_ = std::move(payload_from_controller_fn);
    event_fn_ = std::move(event_fn);
    auto proxy = allocator_.MakeUnique<MockChannelProxy>();
    last_channel_proxy_ = proxy.get();
    return proxy;
  }

  pw::allocator::test::AllocatorForTest<1024> allocator_;
  BufferReceiveFunction payload_from_controller_fn_;
  ChannelEventCallback event_fn_;
  MockChannelProxy* last_channel_proxy_ = nullptr;
  uint32_t intercept_channel_count_ = 0;
};

}  // namespace testing

class RfcommManagerTest : public ::testing::Test {
 protected:
  RfcommManagerTest()
      : l2cap_manager_(), manager_(l2cap_manager_, allocator_) {}

  static constexpr ConnectionHandle kConnectionHandle1 =
      static_cast<ConnectionHandle>(1);
  static constexpr ConnectionHandle kConnectionHandle2 =
      static_cast<ConnectionHandle>(2);
  static constexpr uint8_t kChannelNumber1 = 2;
  static constexpr uint8_t kChannelNumber2 = 3;
  static constexpr RfcommChannelConfig kDefaultConfig = {
      .cid = 1, .max_frame_size = 100, .initial_credits = 10};

  allocator::test::AllocatorForTest<4096> allocator_;
  static constexpr size_t kDataSize = 4096;
#if PW_BLUETOOTH_PROXY_MULTIBUF == PW_BLUETOOTH_PROXY_MULTIBUF_V1
  std::array<std::byte, kDataSize> buffer_{};
  multibuf::SimpleAllocator multibuf_allocator_{
      /*data_area=*/buffer_,
      /*metadata_alloc=*/allocator::GetLibCAllocator()};
#else
  allocator::test::AllocatorForTest<kDataSize> data_alloc_;
  allocator::SynchronizedAllocator<pw::sync::Mutex> synced_{data_alloc_};
  MultiBufAllocator multibuf_allocator_{
      /*data_alloc=*/synced_,
      /*metadata_alloc=*/allocator::GetLibCAllocator()};
#endif
  testing::MockL2capChannelManager l2cap_manager_;
  RfcommManager manager_;
};

TEST_F(RfcommManagerTest, AcquireSingleChannel) {
  auto channel_result = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                      kConnectionHandle1,
                                                      kChannelNumber1,
                                                      true,
                                                      kDefaultConfig,
                                                      kDefaultConfig,
                                                      nullptr,
                                                      nullptr);
  EXPECT_TRUE(channel_result.ok());
  EXPECT_TRUE(channel_result.value());
}

TEST_F(RfcommManagerTest, AcquireMultipleChannelsSameConnection) {
  auto channel1_result = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                       kConnectionHandle1,
                                                       kChannelNumber1,
                                                       true,
                                                       kDefaultConfig,
                                                       kDefaultConfig,
                                                       nullptr,
                                                       nullptr);
  EXPECT_TRUE(channel1_result.ok());
  auto channel2_result = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                       kConnectionHandle1,
                                                       kChannelNumber2,
                                                       true,
                                                       kDefaultConfig,
                                                       kDefaultConfig,
                                                       nullptr,
                                                       nullptr);
  EXPECT_TRUE(channel2_result.ok());
  EXPECT_NE(channel1_result.value(), channel2_result.value());
}

TEST_F(RfcommManagerTest, AcquireChannelsDifferentConnections) {
  auto channel1_result = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                       kConnectionHandle1,
                                                       kChannelNumber1,
                                                       true,
                                                       kDefaultConfig,
                                                       kDefaultConfig,
                                                       nullptr,
                                                       nullptr);
  EXPECT_TRUE(channel1_result.ok());
  auto channel2_result = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                       kConnectionHandle2,
                                                       kChannelNumber1,
                                                       true,
                                                       kDefaultConfig,
                                                       kDefaultConfig,
                                                       nullptr,
                                                       nullptr);
  EXPECT_TRUE(channel2_result.ok());
  EXPECT_NE(channel1_result.value(), channel2_result.value());
}

TEST_F(RfcommManagerTest, L2capChannelClose) {
  std::optional<RfcommEvent> event;
  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    [&](RfcommEvent e) { event = e; });
  EXPECT_TRUE(channel_result.ok());

  l2cap_manager_.TriggerL2capEvent(L2capChannelEvent::kChannelClosedByOther);
  EXPECT_EQ(event, RfcommEvent::kChannelClosedByOther);
}

TEST_F(RfcommManagerTest, HandlePdu) {
  pw::Vector<uint8_t, 256> received_pdu1;
  RfcommEvent last_event = RfcommEvent::kInvalid;
  auto channel_result = manager_.AcquireRfcommChannel(
      multibuf_allocator_,
      kConnectionHandle1,
      kChannelNumber1,
      true,
      kDefaultConfig,
      kDefaultConfig,
      [&](FlatMultiBuf&& pdu) {
        received_pdu1.resize(pdu.size());
        MultiBufAdapter::Copy(as_writable_bytes(span(received_pdu1)), pdu, 0);
      },
      [&](RfcommEvent event) { last_event = event; });
  EXPECT_TRUE(channel_result.ok());

  // Valid UIH frame for channel_number 2.
  const pw::Vector<uint8_t, 5> kPdu1 = {0x11, 0xEF, 0x03, 0x01, 0xbf};
  auto mbuf1_result =
      MultiBufAdapter::Create(multibuf_allocator_, kPdu1.size());
  ASSERT_TRUE(mbuf1_result.has_value());
  MultiBufAdapter::Copy(*mbuf1_result, 0, as_bytes(span(kPdu1)));
  bool handled1 = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf1_result),
                                                      kConnectionHandle1,
                                                      kDefaultConfig.cid,
                                                      kDefaultConfig.cid);

  EXPECT_FALSE(handled1);
  EXPECT_EQ(
      received_pdu1.size(),
      kPdu1.size() - static_cast<size_t>(
                         emboss::RfcommDataFrameOverhead::WITH_SHORT_HEADER));
  EXPECT_EQ(received_pdu1[0], 1);
  EXPECT_EQ(last_event, RfcommEvent::kInvalid);
  received_pdu1.clear();

  // Valid UIH frame for different channel_number should be received by the
  // correct channel.
  pw::Vector<uint8_t, 256> received_pdu2;
  auto channel2_result = manager_.AcquireRfcommChannel(
      multibuf_allocator_,
      kConnectionHandle1,
      kChannelNumber2,
      true,
      kDefaultConfig,
      kDefaultConfig,
      [&](FlatMultiBuf&& pdu) {
        received_pdu2.resize(pdu.size());
        MultiBufAdapter::Copy(as_writable_bytes(span(received_pdu2)), pdu, 0);
      },
      nullptr);
  EXPECT_TRUE(channel2_result.ok());

  // Valid UIH frame for channel_number 3.
  const pw::Vector<uint8_t, 5> kPdu2 = {0x19, 0xEF, 0x03, 0x02, 0x55};
  auto mbuf2_result =
      MultiBufAdapter::Create(multibuf_allocator_, kPdu2.size());
  ASSERT_TRUE(mbuf2_result.has_value());
  MultiBufAdapter::Copy(*mbuf2_result, 0, as_bytes(span(kPdu2)));
  l2cap_manager_.TriggerControllerPdu(std::move(*mbuf2_result),
                                      kConnectionHandle1,
                                      kDefaultConfig.cid,
                                      kDefaultConfig.cid);

  EXPECT_TRUE(
      received_pdu1.empty());  // Original channel should not receive it.
  EXPECT_EQ(
      received_pdu2.size(),
      kPdu2.size() - static_cast<size_t>(
                         emboss::RfcommDataFrameOverhead::WITH_SHORT_HEADER));
  EXPECT_EQ(received_pdu2[0], 2);
  EXPECT_EQ(last_event, RfcommEvent::kInvalid);

  // DISC frame should close channel.
  const pw::Vector<uint8_t, 4> kPdu3 = {0x11, 0x43, 0x01, 0x03};
  auto mbuf3_result =
      MultiBufAdapter::Create(multibuf_allocator_, kPdu3.size());
  ASSERT_TRUE(mbuf3_result.has_value());
  MultiBufAdapter::Copy(*mbuf3_result, 0, as_bytes(span(kPdu3)));
  bool handled3 = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf3_result),
                                                      kConnectionHandle1,
                                                      kDefaultConfig.cid,
                                                      kDefaultConfig.cid);
  EXPECT_TRUE(handled3);
  EXPECT_EQ(last_event, RfcommEvent::kChannelClosedByRemote);
}

TEST_F(RfcommManagerTest, UnhandledPduShouldBeForwarded) {
  auto channel_result = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                      kConnectionHandle1,
                                                      kChannelNumber2,
                                                      true,
                                                      kDefaultConfig,
                                                      kDefaultConfig,
                                                      nullptr,
                                                      nullptr);
  // PDU for a channel_number that is not registered.
  const pw::Vector<uint8_t, 5> kPdu = {0x09, 0xEF, 0x03, 0x01, 0x40};
  auto mbuf_result = MultiBufAdapter::Create(multibuf_allocator_, kPdu.size());
  ASSERT_TRUE(mbuf_result.has_value());
  MultiBufAdapter::Copy(*mbuf_result, 0, as_bytes(span(kPdu)));
  bool handled = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf_result),
                                                     kConnectionHandle1,
                                                     kDefaultConfig.cid,
                                                     kDefaultConfig.cid);
  EXPECT_TRUE(handled);
}

TEST_F(RfcommManagerTest, InvalidFcsPduShouldBeForwarded) {
  auto channel_result = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                      kConnectionHandle1,
                                                      kChannelNumber1,
                                                      true,
                                                      kDefaultConfig,
                                                      kDefaultConfig,
                                                      nullptr,
                                                      nullptr);
  EXPECT_TRUE(channel_result.ok());

  // Valid UIH frame for channel_number 2 with invalid FCS.
  const pw::Vector<uint8_t, 5> kPdu = {0x11, 0xEF, 0x03, 0x01, 0x00};
  auto mbuf_result = MultiBufAdapter::Create(multibuf_allocator_, kPdu.size());
  ASSERT_TRUE(mbuf_result.has_value());
  MultiBufAdapter::Copy(*mbuf_result, 0, as_bytes(span(kPdu)));
  bool handled = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf_result),
                                                     kConnectionHandle1,
                                                     kDefaultConfig.cid,
                                                     kDefaultConfig.cid);
  EXPECT_TRUE(handled);
}

TEST_F(RfcommManagerTest, ReacquireChannelAfterRelease) {
  {
    auto channel_result = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                        kConnectionHandle1,
                                                        kChannelNumber1,
                                                        true,
                                                        kDefaultConfig,
                                                        kDefaultConfig,
                                                        nullptr,
                                                        nullptr);
    EXPECT_TRUE(channel_result.ok());
  }  // channel_result goes out of scope and is released here.

  // Verify that acquiring the same channel again succeeds.
  auto channel_result = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                      kConnectionHandle1,
                                                      kChannelNumber1,
                                                      true,
                                                      kDefaultConfig,
                                                      kDefaultConfig,
                                                      nullptr,
                                                      nullptr);
  EXPECT_TRUE(channel_result.ok());
}

TEST_F(RfcommManagerTest, AcquireExistingChannelFails) {
  auto channel_result = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                      kConnectionHandle1,
                                                      kChannelNumber1,
                                                      true,
                                                      kDefaultConfig,
                                                      kDefaultConfig,
                                                      nullptr,
                                                      nullptr);
  EXPECT_TRUE(channel_result.ok());

  // Verify that acquiring the same channel again fails.
  auto channel_result1 = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                       kConnectionHandle1,
                                                       kChannelNumber1,
                                                       true,
                                                       kDefaultConfig,
                                                       kDefaultConfig,
                                                       nullptr,
                                                       nullptr);
  EXPECT_EQ(channel_result1.status(), Status::AlreadyExists());
}

TEST_F(RfcommManagerTest, AcquireChannelWithMismatchedCidsFails) {
  auto channel_result = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                      kConnectionHandle1,
                                                      kChannelNumber1,
                                                      true,
                                                      kDefaultConfig,
                                                      kDefaultConfig,
                                                      nullptr,
                                                      nullptr);
  EXPECT_TRUE(channel_result.ok());

  // Verify that acquiring a channel with mismatched CIDs fails.
  const RfcommChannelConfig mismatched_config = {
      .cid = static_cast<uint16_t>(kDefaultConfig.cid + 1),
      .max_frame_size = kDefaultConfig.max_frame_size,
      .initial_credits = kDefaultConfig.initial_credits};
  auto channel_result1 = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                       kConnectionHandle1,
                                                       kChannelNumber2,
                                                       true,
                                                       mismatched_config,
                                                       mismatched_config,
                                                       nullptr,
                                                       nullptr);
  EXPECT_EQ(channel_result1.status(), Status::InvalidArgument());
}

TEST_F(RfcommManagerTest, L2capChannelReset) {
  std::optional<RfcommEvent> event;
  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    [&](RfcommEvent e) { event = e; });
  EXPECT_TRUE(channel_result.ok());

  l2cap_manager_.TriggerL2capEvent(L2capChannelEvent::kReset);
  EXPECT_EQ(event, RfcommEvent::kReset);
}

TEST_F(RfcommManagerTest, CallbacksAreSafe) {
  std::optional<RfcommEvent> event;
  auto mbuf = MultiBufAdapter::Create(multibuf_allocator_, 1);
  ASSERT_TRUE(mbuf.has_value());
  FlatMultiBufInstance& flat_mbuf_instance = mbuf.value();

  struct {
    RfcommManager* manager;
    std::optional<RfcommEvent>* event;
    FlatMultiBufInstance* mbuf;
  } capture = {&manager_, &event, &flat_mbuf_instance};

  auto channel_result = manager_.AcquireRfcommChannel(
      multibuf_allocator_,
      kConnectionHandle1,
      kChannelNumber1,
      true,
      kDefaultConfig,
      kDefaultConfig,
      nullptr,
      [&capture](RfcommEvent e) {
        *capture.event = e;
        // Verify that calling Write() and ReleaseRfcommChannel() is safe.
        EXPECT_EQ(capture.manager
                      ->Write(kConnectionHandle1,
                              kChannelNumber1,
                              std::move(MultiBufAdapter::Unwrap(*capture.mbuf)))
                      .status,
                  Status::NotFound());
        EXPECT_EQ(capture.manager->ReleaseRfcommChannel(kConnectionHandle1,
                                                        kChannelNumber1),
                  Status::NotFound());
      });
  EXPECT_TRUE(channel_result.ok());

  // Send a DISC frame.
  const pw::Vector<uint8_t, 4> kPdu = {0x17, 0x43, 0x01, 0xa0};
  auto mbuf_result = MultiBufAdapter::Create(multibuf_allocator_, kPdu.size());
  ASSERT_TRUE(mbuf_result.has_value());
  MultiBufAdapter::Copy(*mbuf_result, 0, as_bytes(span(kPdu)));
  bool handled = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf_result),
                                                     kConnectionHandle1,
                                                     kDefaultConfig.cid,
                                                     kDefaultConfig.cid);
  EXPECT_TRUE(handled);
  EXPECT_EQ(event, RfcommEvent::kChannelClosedByRemote);
}

TEST_F(RfcommManagerTest, ReleaseLastChannelClosesConnection) {
  auto channel1 = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                kConnectionHandle1,
                                                kChannelNumber1,
                                                true,
                                                kDefaultConfig,
                                                kDefaultConfig,
                                                nullptr,
                                                nullptr);
  EXPECT_TRUE(channel1.ok());
  EXPECT_EQ(l2cap_manager_.intercept_channel_count(), 1u);

  auto channel2 = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                kConnectionHandle1,
                                                kChannelNumber2,
                                                true,
                                                kDefaultConfig,
                                                kDefaultConfig,
                                                nullptr,
                                                nullptr);
  EXPECT_TRUE(channel2.ok());
  EXPECT_EQ(l2cap_manager_.intercept_channel_count(), 1u);

  // Release one channel, connection should remain.
  EXPECT_EQ(manager_.ReleaseRfcommChannel(kConnectionHandle1, kChannelNumber1),
            OkStatus());

  // Re-acquiring should not create a new L2CAP channel proxy.
  auto channel1_reacquired = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                           kConnectionHandle1,
                                                           kChannelNumber1,
                                                           true,
                                                           kDefaultConfig,
                                                           kDefaultConfig,
                                                           nullptr,
                                                           nullptr);
  EXPECT_TRUE(channel1_reacquired.ok());
  EXPECT_EQ(l2cap_manager_.intercept_channel_count(), 1u);

  // Release one channel without `close_connection_if_empty_channel`, connection
  // should remain.
  EXPECT_EQ(manager_.ReleaseRfcommChannel(kConnectionHandle1, kChannelNumber1),
            OkStatus());

  // Release the last channel, connection should be closed.
  EXPECT_EQ(manager_.ReleaseRfcommChannel(kConnectionHandle1, kChannelNumber2),
            OkStatus());

  // Re-acquiring should create a new L2CAP channel proxy.
  auto channel_after_close = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                           kConnectionHandle1,
                                                           kChannelNumber1,
                                                           true,
                                                           kDefaultConfig,
                                                           kDefaultConfig,
                                                           nullptr,
                                                           nullptr);
  EXPECT_TRUE(channel_after_close.ok());
  EXPECT_EQ(l2cap_manager_.intercept_channel_count(), 2u);
}

}  // namespace pw::bluetooth::proxy::rfcomm
