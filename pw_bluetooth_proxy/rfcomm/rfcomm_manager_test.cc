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

#include <array>
#include <optional>
#include <utility>
#include <variant>

#include "pw_allocator/libc_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_bluetooth_proxy/config.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/l2cap_channel_manager_interface.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_bluetooth_proxy/rfcomm/rfcomm_snapshot.h"
#include "pw_bluetooth_proxy_private/test_utils.h"
#include "pw_bytes/span.h"
#include "pw_containers/vector.h"
#include "pw_multibuf/multibuf.h"
#include "pw_multibuf/simple_allocator.h"
#include "pw_thread/sleep.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"

namespace pw::bluetooth::proxy::rfcomm {
namespace testing {

class MockChannelProxy : public ChannelProxy {
 public:
  span<const uint8_t> last_written_payload() const {
    return last_written_payload_data_;
  }

  void set_write_status(Status status) { write_status_ = status; }

 private:
  StatusWithMultiBuf DoWrite(multibuf::MultiBuf&& payload) override {
    if (!write_status_.ok()) {
      return {write_status_, std::move(payload)};
    }
    last_written_payload_data_.resize(payload.size());
    auto bytes_copied =
        payload.CopyTo(as_writable_bytes(span(last_written_payload_data_)));
    return {bytes_copied.status()};
  }

  Status DoIsWriteAvailable() override { return write_status_; }

  Status DoSendAdditionalRxCredits(
      uint16_t /*additional_rx_credits*/) override {
    return OkStatus();
  }

  Status write_status_ = OkStatus();
  pw::Vector<uint8_t, 256> last_written_payload_data_;
};

class MockL2capChannelManager final : public L2capChannelManagerInterface {
 public:
  MockL2capChannelManager() = default;

  // Triggers the from_controller callback to simulate an incoming L2CAP PDU.
  bool TriggerControllerPdu(multibuf::MultiBuf&& pdu,
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

  MockChannelProxy* last_channel_proxy() const { return last_channel_proxy_; }

  bool allow_data_loss() const { return allow_data_loss_; }

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
      BasicModeChannelConfig config,
      BufferReceiveFunction&& payload_from_controller_fn,
      BufferReceiveFunction&& /*payload_from_host_fn*/,
      ChannelEventCallback&& event_fn) override {
    intercept_channel_count_++;
    allow_data_loss_ = config.allow_data_loss;
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
  bool allow_data_loss_ = false;
};

}  // namespace testing

class RfcommManagerTest : public ::testing::Test {
 protected:
  RfcommManagerTest()
      : l2cap_manager_(),
        manager_(l2cap_manager_,
                 allocator_,
                 [this](const RfcommStateUpdate& update) {
                   if (state_update_callback_) {
                     state_update_callback_(update);
                   }
                 }) {}

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

  std::array<std::byte, kDataSize> buffer_{};
  multibuf::SimpleAllocator multibuf_allocator_{
      /*data_area=*/buffer_,
      /*metadata_alloc=*/allocator::GetLibCAllocator()};

  testing::MockL2capChannelManager l2cap_manager_;
  RfcommStateUpdateCallback state_update_callback_;
  RfcommManager manager_;
};

TEST_F(RfcommManagerTest, AcquireSingleChannel) {
  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel_result.ok());
  EXPECT_TRUE(channel_result.value());
  EXPECT_TRUE(l2cap_manager_.allow_data_loss());
}

TEST_F(RfcommManagerTest, AcquireMultipleChannelsSameConnection) {
  auto channel1_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel1_result.ok());
  auto channel2_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber2,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel2_result.ok());
  EXPECT_NE(channel1_result.value(), channel2_result.value());
}

TEST_F(RfcommManagerTest, AcquireMultipleChannelsSameNumberDifferentDirection) {
  auto channel1_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kResponder,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel1_result.ok());
  auto channel2_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel2_result.ok());
  EXPECT_NE(channel1_result.value(), channel2_result.value());
}

TEST_F(RfcommManagerTest, AcquireChannelsDifferentConnections) {
  auto channel1_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel1_result.ok());
  auto channel2_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle2,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
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
                                    RfcommDirection::kInitiator,
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
      RfcommDirection::kResponder,
      true,
      kDefaultConfig,
      kDefaultConfig,
      [&](multibuf::MultiBuf&& pdu) {
        received_pdu1.resize(pdu.size());
        std::ignore = pdu.CopyTo(as_writable_bytes(span(received_pdu1)));
      },
      [&](RfcommEvent event) { last_event = event; });
  EXPECT_TRUE(channel_result.ok());

  // Valid UIH frame for channel_number 2.
  const pw::Vector<uint8_t, 5> kPdu1 = {0x11, 0xEF, 0x03, 0x01, 0xbf};
  auto mbuf1_result = multibuf_allocator_.AllocateContiguous(kPdu1.size());
  ASSERT_TRUE(mbuf1_result.has_value());
  ASSERT_EQ(mbuf1_result->CopyFrom(as_bytes(span(kPdu1))).status(),
            pw::OkStatus());
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
      RfcommDirection::kResponder,
      true,
      kDefaultConfig,
      kDefaultConfig,
      [&](multibuf::MultiBuf&& pdu) {
        received_pdu2.resize(pdu.size());
        std::ignore = pdu.CopyTo(as_writable_bytes(span(received_pdu2)));
      },
      nullptr);
  EXPECT_TRUE(channel2_result.ok());

  // Valid UIH frame for channel_number 3.
  const pw::Vector<uint8_t, 5> kPdu2 = {0x19, 0xEF, 0x03, 0x02, 0x55};
  auto mbuf2_result = multibuf_allocator_.AllocateContiguous(kPdu2.size());
  ASSERT_TRUE(mbuf2_result.has_value());
  ASSERT_EQ(mbuf2_result->CopyFrom(as_bytes(span(kPdu2))).status(),
            pw::OkStatus());
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
  auto mbuf3_result = multibuf_allocator_.AllocateContiguous(kPdu3.size());
  ASSERT_TRUE(mbuf3_result.has_value());
  ASSERT_EQ(mbuf3_result->CopyFrom(as_bytes(span(kPdu3))).status(),
            pw::OkStatus());
  bool handled3 = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf3_result),
                                                      kConnectionHandle1,
                                                      kDefaultConfig.cid,
                                                      kDefaultConfig.cid);
  EXPECT_TRUE(handled3);
  EXPECT_EQ(last_event, RfcommEvent::kChannelClosedByRemote);
}

TEST_F(RfcommManagerTest, UnhandledPduShouldBeForwarded) {
  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber2,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  // PDU for a channel_number that is not registered.
  const pw::Vector<uint8_t, 5> kPdu = {0x09, 0xEF, 0x03, 0x01, 0x40};
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(kPdu.size());
  ASSERT_TRUE(mbuf_result.has_value());
  ASSERT_EQ(mbuf_result->CopyFrom(as_bytes(span(kPdu))).status(),
            pw::OkStatus());
  bool handled = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf_result),
                                                     kConnectionHandle1,
                                                     kDefaultConfig.cid,
                                                     kDefaultConfig.cid);
  EXPECT_TRUE(handled);
}

TEST_F(RfcommManagerTest, InvalidFcsPduShouldBeForwarded) {
  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kResponder,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel_result.ok());

  // Valid UIH frame for channel_number 2 with invalid FCS.
  const pw::Vector<uint8_t, 5> kPdu = {0x11, 0xEF, 0x03, 0x01, 0x00};
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(kPdu.size());
  ASSERT_TRUE(mbuf_result.has_value());
  ASSERT_EQ(mbuf_result->CopyFrom(as_bytes(span(kPdu))).status(),
            pw::OkStatus());
  bool handled = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf_result),
                                                     kConnectionHandle1,
                                                     kDefaultConfig.cid,
                                                     kDefaultConfig.cid);
  EXPECT_TRUE(handled);
}

TEST_F(RfcommManagerTest, ReacquireChannelAfterRelease) {
  {
    auto channel_result =
        manager_.AcquireRfcommChannel(multibuf_allocator_,
                                      kConnectionHandle1,
                                      kChannelNumber1,
                                      RfcommDirection::kInitiator,
                                      true,
                                      kDefaultConfig,
                                      kDefaultConfig,
                                      nullptr,
                                      nullptr);
    EXPECT_TRUE(channel_result.ok());
  }  // channel_result goes out of scope and is released here.

  // Verify that acquiring the same channel again succeeds.
  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel_result.ok());
}

TEST_F(RfcommManagerTest, AcquireExistingChannelFails) {
  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel_result.ok());

  // Verify that acquiring the same channel again fails.
  auto channel_result1 =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_EQ(channel_result1.status(), Status::AlreadyExists());
}

TEST_F(RfcommManagerTest, AcquireChannelWithMismatchedCidsFails) {
  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
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
  auto channel_result1 =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber2,
                                    RfcommDirection::kInitiator,
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
                                    RfcommDirection::kInitiator,
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
  auto mbuf = multibuf_allocator_.AllocateContiguous(1);
  ASSERT_TRUE(mbuf.has_value());
  multibuf::MultiBuf& flat_mbuf_instance = mbuf.value();

  struct {
    RfcommManager* manager;
    std::optional<RfcommEvent>* event;
    multibuf::MultiBuf* mbuf;
  } capture = {&manager_, &event, &flat_mbuf_instance};

  auto channel_result = manager_.AcquireRfcommChannel(
      multibuf_allocator_,
      kConnectionHandle1,
      kChannelNumber1,
      RfcommDirection::kInitiator,
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
                              RfcommDirection::kInitiator,
                              std::move(*capture.mbuf))
                      .status,
                  Status::NotFound());
        EXPECT_EQ(
            capture.manager->ReleaseRfcommChannel(kConnectionHandle1,
                                                  kChannelNumber1,
                                                  RfcommDirection::kInitiator),
            Status::NotFound());
      });
  EXPECT_TRUE(channel_result.ok());

  // Send a DISC frame.
  const pw::Vector<uint8_t, 4> kPdu = {0x17, 0x43, 0x01, 0xa0};
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(kPdu.size());
  ASSERT_TRUE(mbuf_result.has_value());
  ASSERT_EQ(mbuf_result->CopyFrom(as_bytes(span(kPdu))).status(),
            pw::OkStatus());
  bool handled = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf_result),
                                                     kConnectionHandle1,
                                                     kDefaultConfig.cid,
                                                     kDefaultConfig.cid);
  EXPECT_TRUE(handled);
  EXPECT_EQ(event, RfcommEvent::kChannelClosedByRemote);
}

TEST_F(RfcommManagerTest, ReceiveCallbackDoesNotHoldMutex) {
  pw::Vector<uint8_t, 256> received_pdu;
  bool write_success = false;
  auto mbuf_for_write = multibuf_allocator_.AllocateContiguous(1);
  ASSERT_TRUE(mbuf_for_write.has_value());
  multibuf::MultiBuf& mbuf_for_write_ref = mbuf_for_write.value();

  struct {
    RfcommManager* manager;
    multibuf::MultiBuf* mbuf;
    bool* write_success;
    pw::Vector<uint8_t, 256>* received_pdu;
  } capture = {&manager_, &mbuf_for_write_ref, &write_success, &received_pdu};

  auto receive_cb = [&capture](multibuf::MultiBuf&& pdu) {
    capture.received_pdu->resize(pdu.size());
    std::ignore = pdu.CopyTo(as_writable_bytes(span(*capture.received_pdu)));

    // Verify that calling Write() from within the receive callback is safe.
    // It should NOT deadlock on `connections_mutex_`.
    auto write_status = capture.manager->Write(kConnectionHandle1,
                                               kChannelNumber1,
                                               RfcommDirection::kResponder,
                                               std::move(*capture.mbuf));
    *capture.write_success = write_status.status.ok();
  };

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kResponder,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    std::move(receive_cb),
                                    nullptr);
  EXPECT_TRUE(channel_result.ok());

  // Send a valid UIH frame for channel_number 1 (Responder = DLCI 4) to trigger
  // the receive callback.
  const pw::Vector<uint8_t, 5> kPdu = {0x11, 0xEF, 0x03, 0x01, 0xbf};
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(kPdu.size());
  ASSERT_TRUE(mbuf_result.has_value());
  ASSERT_EQ(mbuf_result->CopyFrom(as_bytes(span(kPdu))).status(),
            pw::OkStatus());
  bool handled = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf_result),
                                                     kConnectionHandle1,
                                                     kDefaultConfig.cid,
                                                     kDefaultConfig.cid);
  EXPECT_FALSE(handled);

  // The callback should have been executed, and since the channel is still
  // open, the nested Write should succeed.
  EXPECT_TRUE(write_success);
}

TEST_F(RfcommManagerTest, ReleaseLastChannelClosesConnection) {
  auto channel1 = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                                kConnectionHandle1,
                                                kChannelNumber1,
                                                RfcommDirection::kInitiator,
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
                                                RfcommDirection::kInitiator,
                                                true,
                                                kDefaultConfig,
                                                kDefaultConfig,
                                                nullptr,
                                                nullptr);
  EXPECT_TRUE(channel2.ok());
  EXPECT_EQ(l2cap_manager_.intercept_channel_count(), 1u);

  // Release one channel, connection should remain.
  EXPECT_EQ(
      manager_.ReleaseRfcommChannel(
          kConnectionHandle1, kChannelNumber1, RfcommDirection::kInitiator),
      OkStatus());

  // Re-acquiring should not create a new L2CAP channel proxy.
  auto channel1_reacquired =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel1_reacquired.ok());
  EXPECT_EQ(l2cap_manager_.intercept_channel_count(), 1u);

  // Release one channel without `close_connection_if_empty_channel`, connection
  // should remain.
  EXPECT_EQ(
      manager_.ReleaseRfcommChannel(
          kConnectionHandle1, kChannelNumber1, RfcommDirection::kInitiator),
      OkStatus());

  // Release the last channel, connection should be closed.
  EXPECT_EQ(
      manager_.ReleaseRfcommChannel(
          kConnectionHandle1, kChannelNumber2, RfcommDirection::kInitiator),
      OkStatus());

  // Re-acquiring should create a new L2CAP channel proxy.
  auto channel_after_close =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel_after_close.ok());
  EXPECT_EQ(l2cap_manager_.intercept_channel_count(), 2u);
}

TEST_F(RfcommManagerTest, SendAdditionalRxCredits) {
  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel_result.ok());

  const uint8_t kAdditionalCredits = 5;
  EXPECT_EQ(manager_.SendAdditionalRxCredits(kConnectionHandle1,
                                             kChannelNumber1,
                                             RfcommDirection::kInitiator,
                                             kAdditionalCredits),
            OkStatus());

  ASSERT_NE(l2cap_manager_.last_channel_proxy(), nullptr);
  auto payload = l2cap_manager_.last_channel_proxy()->last_written_payload();
  ASSERT_FALSE(payload.empty());

  // A credit packet is a UIH frame with a length of 0.
  EXPECT_EQ(payload.size(),
            1 + static_cast<size_t>(
                    emboss::RfcommDataFrameOverhead::WITH_SHORT_HEADER));

  // Address field: channel_number=2, D=1 (initiated by initiator), C/R=1 (from
  // initiator), EA=1
  const uint8_t expected_address =
      (kChannelNumber1 << 3) | (1 << 2) | (1 << 1) | 1;
  EXPECT_EQ(payload[0], expected_address);

  // Control field: UIH with P/F bit.
  EXPECT_EQ(payload[1],
            static_cast<uint8_t>(
                emboss::RfcommFrameType::
                    UNNUMBERED_INFORMATION_WITH_HEADER_CHECK_AND_POLL_FINAL));

  // Length field: 0 byte of info.
  const uint8_t expected_length = (0 << 1) | 1;
  EXPECT_EQ(payload[2], expected_length);

  // Info field: number of credits.
  EXPECT_EQ(payload[3], kAdditionalCredits);
}

TEST_F(RfcommManagerTest, SendAdditionalRxCreditsNotFound) {
  const uint8_t kAdditionalCredits = 5;

  // Connection does not exist.
  EXPECT_EQ(manager_.SendAdditionalRxCredits(kConnectionHandle1,
                                             kChannelNumber1,
                                             RfcommDirection::kInitiator,
                                             kAdditionalCredits),
            Status::NotFound());

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  EXPECT_TRUE(channel_result.ok());

  // Channel does not exist.
  EXPECT_EQ(manager_.SendAdditionalRxCredits(kConnectionHandle1,
                                             kChannelNumber2,
                                             RfcommDirection::kInitiator,
                                             kAdditionalCredits),
            Status::NotFound());
}

namespace {

RfcommChannelSnapshot CreateRfcommChannelSnapshot(
    uint16_t connection_handle = 1,
    uint8_t channel_number = 2,
    RfcommDirection direction = RfcommDirection::kInitiator,
    uint16_t local_cid = 0,
    uint16_t remote_cid = 0,
    bool mux_initiator = false,
    uint8_t tx_credits = 0,
    uint8_t rx_credits = 0,
    uint8_t rx_total_credits = 0,
    uint16_t max_frame_size = 0) {
  RfcommChannelSnapshot snapshot;
  snapshot.connection_handle = connection_handle;
  snapshot.channel_number = channel_number;
  snapshot.direction = direction;
  snapshot.local_cid = local_cid;
  snapshot.remote_cid = remote_cid;
  snapshot.mux_initiator = mux_initiator;
  snapshot.tx_credits = tx_credits;
  snapshot.rx_credits = rx_credits;
  snapshot.rx_total_credits = rx_total_credits;
  snapshot.max_frame_size = max_frame_size;
  return snapshot;
}

RfcommChannelRemoved CreateRfcommChannelRemoved(
    uint16_t connection_handle = 1,
    uint8_t channel_number = 2,
    RfcommDirection direction = RfcommDirection::kInitiator) {
  RfcommChannelRemoved removed;
  removed.connection_handle = connection_handle;
  removed.channel_number = channel_number;
  removed.direction = direction;
  return removed;
}

}  // namespace

TEST(RfcommSnapshotTest, ChannelSnapshotMatchesKey) {
  RfcommChannelSnapshot snap = CreateRfcommChannelSnapshot(
      /*connection_handle=*/1,
      /*channel_number=*/2,
      /*direction=*/RfcommDirection::kInitiator);
  EXPECT_TRUE(snap.MatchesKey(1, 2, RfcommDirection::kInitiator));
  EXPECT_FALSE(snap.MatchesKey(2, 2, RfcommDirection::kInitiator));
  EXPECT_FALSE(snap.MatchesKey(1, 3, RfcommDirection::kInitiator));
  EXPECT_FALSE(snap.MatchesKey(1, 2, RfcommDirection::kResponder));

  RfcommChannelRemoved removal_matching = CreateRfcommChannelRemoved(
      /*connection_handle=*/1,
      /*channel_number=*/2,
      /*direction=*/RfcommDirection::kInitiator);
  EXPECT_TRUE(snap.MatchesKey(removal_matching));

  RfcommChannelRemoved removal_mismatch = CreateRfcommChannelRemoved(
      /*connection_handle=*/1,
      /*channel_number=*/2,
      /*direction=*/RfcommDirection::kResponder);
  EXPECT_FALSE(snap.MatchesKey(removal_mismatch));
}

TEST(RfcommSnapshotTest, ChannelSnapshotDlci) {
  RfcommChannelSnapshot snap_initiator = CreateRfcommChannelSnapshot(
      /*connection_handle=*/1,
      /*channel_number=*/2,
      /*direction=*/RfcommDirection::kInitiator);
  EXPECT_EQ(snap_initiator.dlci(), MakeDlci(2, RfcommDirection::kInitiator));

  RfcommChannelSnapshot snap_responder = CreateRfcommChannelSnapshot(
      /*connection_handle=*/1,
      /*channel_number=*/2,
      /*direction=*/RfcommDirection::kResponder);
  EXPECT_EQ(snap_responder.dlci(), MakeDlci(2, RfcommDirection::kResponder));
}

TEST(RfcommSnapshotTest, ChannelSnapshotUpdate) {
  RfcommChannelSnapshot original = CreateRfcommChannelSnapshot(
      /*connection_handle=*/1,
      /*channel_number=*/2,
      /*direction=*/RfcommDirection::kInitiator,
      /*local_cid=*/0x0040,
      /*remote_cid=*/0x0041,
      /*mux_initiator=*/true,
      /*tx_credits=*/5,
      /*rx_credits=*/10,
      /*rx_total_credits=*/10,
      /*max_frame_size=*/128);

  RfcommChannelSnapshot update = CreateRfcommChannelSnapshot(
      /*connection_handle=*/1,
      /*channel_number=*/2,
      /*direction=*/RfcommDirection::kInitiator,
      /*local_cid=*/0x0050,
      /*remote_cid=*/0x0051,
      /*mux_initiator=*/false,
      /*tx_credits=*/7,
      /*rx_credits=*/12,
      /*rx_total_credits=*/14,
      /*max_frame_size=*/256);

  PW_TEST_EXPECT_OK(original.Update(update));
  EXPECT_EQ(original.local_cid, 0x0050);
  EXPECT_EQ(original.remote_cid, 0x0051);
  EXPECT_FALSE(original.mux_initiator);
  EXPECT_EQ(original.tx_credits, 7);
  EXPECT_EQ(original.rx_credits, 12);
  EXPECT_EQ(original.rx_total_credits, 14);
  EXPECT_EQ(original.max_frame_size, 256);

  RfcommChannelSnapshot mismatch_key = update;
  mismatch_key.channel_number = 3;
  EXPECT_EQ(original.Update(mismatch_key), Status::InvalidArgument());
}

TEST(RfcommSnapshotTest, ApplyStateUpdateInsertAndModify) {
  RfcommSnapshot snapshot;
  EXPECT_TRUE(snapshot.rfcomm_channels.empty());
  EXPECT_FALSE(snapshot.snapshot_incomplete);

  RfcommChannelSnapshot ch1 = CreateRfcommChannelSnapshot(
      /*connection_handle=*/1,
      /*channel_number=*/2,
      /*direction=*/RfcommDirection::kInitiator,
      /*local_cid=*/0x40,
      /*remote_cid=*/0x41,
      /*mux_initiator=*/true,
      /*tx_credits=*/5,
      /*rx_credits=*/7,
      /*rx_total_credits=*/7,
      /*max_frame_size=*/100);

  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(ch1));
  EXPECT_EQ(snapshot.rfcomm_channels.size(), 1u);
  EXPECT_EQ(snapshot.rfcomm_channels[0].tx_credits, 5);

  ch1.tx_credits = 4;
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(ch1));
  EXPECT_EQ(snapshot.rfcomm_channels.size(), 1u);
  EXPECT_EQ(snapshot.rfcomm_channels[0].tx_credits, 4);

  RfcommChannelSnapshot ch2 = CreateRfcommChannelSnapshot(
      /*connection_handle=*/1,
      /*channel_number=*/3,
      /*direction=*/RfcommDirection::kInitiator);
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(ch2));
  EXPECT_EQ(snapshot.rfcomm_channels.size(), 2u);
}

TEST(RfcommSnapshotTest, ApplyStateUpdateRemoveChannel) {
  RfcommSnapshot snapshot;
  RfcommChannelSnapshot ch1 = CreateRfcommChannelSnapshot(
      /*connection_handle=*/1,
      /*channel_number=*/2,
      /*direction=*/RfcommDirection::kInitiator);
  RfcommChannelSnapshot ch2 = CreateRfcommChannelSnapshot(
      /*connection_handle=*/1,
      /*channel_number=*/3,
      /*direction=*/RfcommDirection::kInitiator);
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(ch1));
  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(ch2));
  EXPECT_EQ(snapshot.rfcomm_channels.size(), 2u);

  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(CreateRfcommChannelRemoved(
      /*connection_handle=*/1,
      /*channel_number=*/2,
      /*direction=*/RfcommDirection::kInitiator)));
  EXPECT_EQ(snapshot.rfcomm_channels.size(), 1u);
  EXPECT_EQ(snapshot.rfcomm_channels[0].channel_number, 3);

  PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(CreateRfcommChannelRemoved(
      /*connection_handle=*/1,
      /*channel_number=*/99,
      /*direction=*/RfcommDirection::kInitiator)));
  EXPECT_EQ(snapshot.rfcomm_channels.size(), 1u);
}

TEST(RfcommSnapshotTest, ApplyStateUpdateCapacityExhaustion) {
  RfcommSnapshot snapshot;
  for (uint8_t i = 0;
       i < PW_BLUETOOTH_PROXY_CONFIG_MAX_SNAPSHOT_RFCOMM_CHANNELS;
       ++i) {
    RfcommChannelSnapshot ch = CreateRfcommChannelSnapshot(
        /*connection_handle=*/1,
        /*channel_number=*/static_cast<uint8_t>(i + 1),
        /*direction=*/RfcommDirection::kInitiator);
    PW_TEST_EXPECT_OK(snapshot.ApplyStateUpdate(ch));
  }
  EXPECT_EQ(snapshot.rfcomm_channels.size(),
            static_cast<size_t>(
                PW_BLUETOOTH_PROXY_CONFIG_MAX_SNAPSHOT_RFCOMM_CHANNELS));
  EXPECT_FALSE(snapshot.snapshot_incomplete);

  RfcommChannelSnapshot overflow_ch = CreateRfcommChannelSnapshot(
      /*connection_handle=*/1,
      /*channel_number=*/
      static_cast<uint8_t>(
          PW_BLUETOOTH_PROXY_CONFIG_MAX_SNAPSHOT_RFCOMM_CHANNELS + 1),
      /*direction=*/RfcommDirection::kInitiator);
  EXPECT_EQ(snapshot.ApplyStateUpdate(overflow_ch),
            Status::ResourceExhausted());
  EXPECT_TRUE(snapshot.snapshot_incomplete);
}

using RfcommProxyHostTest = ProxyHostTest;

#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_RECOVERY

class RfcommManagerRecoveryTest : public RfcommManagerTest {
 protected:
  void TearDown() override {
    state_update_callback_ = nullptr;
    manager_.DeregisterAndCloseChannels(RfcommEvent::kChannelClosedByOther);
  }

  multibuf::MultiBuf MakeCreditPdu(uint8_t channel_number,
                                   RfcommDirection direction,
                                   bool mux_initiator,
                                   uint8_t credits) {
    std::array<uint8_t, 5> raw_pdu = {};
    auto frame_writer =
        emboss::MakeRfcommFrameView(raw_pdu.data(), raw_pdu.size());

    frame_writer.extended_address().Write(true);
    frame_writer.command_response().Write(mux_initiator);
    frame_writer.direction().Write(direction == RfcommDirection::kInitiator);
    frame_writer.channel().Write(channel_number);
    frame_writer.control().Write(
        pw::bluetooth::emboss::RfcommFrameType::
            UNNUMBERED_INFORMATION_WITH_HEADER_CHECK_AND_POLL_FINAL);

    frame_writer.length_extended_flag().Write(
        pw::bluetooth::emboss::RfcommLengthExtended::NORMAL);
    frame_writer.length().Write(0);
    frame_writer.credits().Write(credits);

    static constexpr pw::checksum::Crc8 kRfcommCrc =
        pw::checksum::Crc8(0x07, 0xFF, true, true, 0xff);
    frame_writer.fcs().Write(kRfcommCrc.Calculate(as_bytes(span(
        raw_pdu.data(),
        static_cast<size_t>(emboss::RfcommHeaderLength::WITHOUT_LENGTH)))));

    auto result = multibuf_allocator_.AllocateContiguous(raw_pdu.size());
    EXPECT_TRUE(result.has_value());
    multibuf::MultiBuf new_buffer = std::move(result.value());
    EXPECT_EQ(new_buffer.CopyFrom(as_bytes(span(raw_pdu))).status(),
              OkStatus());
    return new_buffer;
  }
};

TEST_F(RfcommManagerRecoveryTest, RecoverFromNullSnapshotFails) {
  EXPECT_EQ(manager_.RecoverFromSnapshot(nullptr), Status::InvalidArgument());
}

TEST_F(RfcommManagerRecoveryTest, RecoverFromIncompleteSnapshotFails) {
  RfcommSnapshot snapshot;
  snapshot.snapshot_incomplete = true;
  EXPECT_EQ(manager_.RecoverFromSnapshot(&snapshot), Status::DataLoss());
}

TEST_F(RfcommManagerRecoveryTest,
       AcquireChannelAfterIncompleteSnapshotRecoveryFailureSucceeds) {
  struct CallbackState {
    size_t updates_received = 0;
    std::optional<RfcommChannelSnapshot> emitted_snapshot;
  } callback_state;
  state_update_callback_ = [&callback_state](const RfcommStateUpdate& update) {
    if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
      callback_state.emitted_snapshot = *snap;
      ++callback_state.updates_received;
    }
  };

  RfcommSnapshot snapshot;
  snapshot.snapshot_incomplete = true;
  // Snapshot has custom credit counts that must NOT be applied due to
  // DataLoss.
  snapshot.rfcomm_channels.push_back(CreateRfcommChannelSnapshot(
      /*connection_handle=*/static_cast<uint16_t>(kConnectionHandle1),
      /*channel_number=*/kChannelNumber1,
      /*direction=*/RfcommDirection::kInitiator,
      /*local_cid=*/kDefaultConfig.cid,
      /*remote_cid=*/kDefaultConfig.cid,
      /*mux_initiator=*/true,
      /*tx_credits=*/99,
      /*rx_credits=*/88,
      /*rx_total_credits=*/77,
      /*max_frame_size=*/100));

  EXPECT_EQ(manager_.RecoverFromSnapshot(&snapshot), Status::DataLoss());

  // Subsequent channel acquisition must succeed and initialize cleanly with
  // the requested config rather than corrupted/incomplete snapshot state.
  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_TRUE(channel_result.value());

  // Since recovery failed with DataLoss, restored_snapshot_ remained nullptr,
  // so acquiring the channel emits an immediate state update with default
  // config.
  EXPECT_EQ(callback_state.updates_received, 1u);
  ASSERT_TRUE(callback_state.emitted_snapshot.has_value());
  EXPECT_EQ(callback_state.emitted_snapshot->tx_credits,
            kDefaultConfig.initial_credits);
  EXPECT_EQ(callback_state.emitted_snapshot->rx_credits,
            kDefaultConfig.initial_credits);
  EXPECT_EQ(callback_state.emitted_snapshot->rx_total_credits,
            kDefaultConfig.initial_credits);

  // Channel can write data normally.
  auto mbuf = multibuf_allocator_.AllocateContiguous(10);
  ASSERT_TRUE(mbuf.has_value());
  PW_TEST_EXPECT_OK(channel_result.value().Write(std::move(*mbuf)).status);
}

TEST_F(RfcommManagerRecoveryTest, RecoverFromValidSnapshot) {
  RfcommSnapshot snapshot;
  snapshot.snapshot_incomplete = false;
  PW_TEST_EXPECT_OK(manager_.RecoverFromSnapshot(&snapshot));
}

TEST_F(RfcommManagerRecoveryTest, AcquireChannelRestoresSnapshotState) {
  RfcommSnapshot snapshot;
  RfcommChannelSnapshot ch_snap = CreateRfcommChannelSnapshot(
      /*connection_handle=*/static_cast<uint16_t>(kConnectionHandle1),
      /*channel_number=*/kChannelNumber1,
      /*direction=*/RfcommDirection::kInitiator,
      /*local_cid=*/kDefaultConfig.cid,
      /*remote_cid=*/kDefaultConfig.cid,
      /*mux_initiator=*/true,
      /*tx_credits=*/15,
      /*rx_credits=*/12,
      /*rx_total_credits=*/20,
      /*max_frame_size=*/100);
  snapshot.rfcomm_channels.push_back(ch_snap);

  PW_TEST_ASSERT_OK(manager_.RecoverFromSnapshot(&snapshot));

#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
  std::optional<RfcommChannelSnapshot> update_snap;
  state_update_callback_ = [&update_snap](const RfcommStateUpdate& update) {
    if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
      update_snap = *snap;
    }
  };
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_TRUE(channel_result.value());

#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
  manager_.CompleteRecovery();

  PW_TEST_ASSERT_OK(manager_.SendAdditionalRxCredits(
      kConnectionHandle1, kChannelNumber1, RfcommDirection::kInitiator, 1));
  ASSERT_TRUE(update_snap.has_value());
  // Verify that restored snapshot values (15 tx, 12 rx, 20 rx_total) were used
  // rather than default config values (10 tx, 10 rx, 10 rx_total).
  EXPECT_EQ(update_snap->tx_credits, 15);
  EXPECT_EQ(update_snap->rx_credits, 13);
  EXPECT_EQ(update_snap->rx_total_credits, 21);
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
}

TEST_F(RfcommManagerRecoveryTest, RecoverySuppressesInitialStateUpdate) {
  size_t updates_received = 0;
  state_update_callback_ = [&updates_received](const RfcommStateUpdate&) {
    ++updates_received;
  };

  RfcommSnapshot snapshot;
  snapshot.rfcomm_channels.push_back(CreateRfcommChannelSnapshot(
      /*connection_handle=*/static_cast<uint16_t>(kConnectionHandle1),
      /*channel_number=*/kChannelNumber1,
      /*direction=*/RfcommDirection::kInitiator));
  PW_TEST_ASSERT_OK(manager_.RecoverFromSnapshot(&snapshot));

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_EQ(updates_received, 0u);

  manager_.CompleteRecovery();
  EXPECT_EQ(updates_received, 0u);

  auto channel2_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber2,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel2_result.status());
  EXPECT_EQ(updates_received, 1u);
}

TEST_F(RfcommManagerRecoveryTest, UnmatchedChannelAcquisitionDuringRecovery) {
  struct CallbackState {
    size_t updates_received = 0;
    std::optional<RfcommChannelSnapshot> last_snapshot;
  } callback_state;
  state_update_callback_ = [&callback_state](const RfcommStateUpdate& update) {
    if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
      callback_state.last_snapshot = *snap;
      ++callback_state.updates_received;
    }
  };

  RfcommSnapshot snapshot;
  snapshot.rfcomm_channels.push_back(CreateRfcommChannelSnapshot(
      /*connection_handle=*/static_cast<uint16_t>(kConnectionHandle1),
      /*channel_number=*/kChannelNumber1,
      /*direction=*/RfcommDirection::kInitiator));
  PW_TEST_ASSERT_OK(manager_.RecoverFromSnapshot(&snapshot));

  auto channel2_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber2,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel2_result.status());
  EXPECT_TRUE(channel2_result.value());

  // Acquiring an unmatched channel during the recovery window must emit an
  // initial state update to notify the container of the newly created channel.
  EXPECT_EQ(callback_state.updates_received, 1u);
  ASSERT_TRUE(callback_state.last_snapshot.has_value());
  EXPECT_EQ(callback_state.last_snapshot->channel_number, kChannelNumber2);
}

TEST_F(RfcommManagerRecoveryTest,
       RestoredSnapshotWithZeroCreditsPreservesZeroCredits) {
  constexpr RfcommChannelConfig kNonZeroConfig = {
      .cid = 1, .max_frame_size = 100, .initial_credits = 10};
  RfcommSnapshot snapshot;
  snapshot.rfcomm_channels.push_back(RfcommChannelSnapshot{
      .connection_handle = static_cast<uint16_t>(kConnectionHandle1),
      .channel_number = kChannelNumber1,
      .direction = RfcommDirection::kInitiator,
      .local_cid = 1,
      .remote_cid = 1,
      .mux_initiator = true,
      .tx_credits = 0,
      .rx_credits = 0,
      .rx_total_credits = 0,
  });
  PW_TEST_ASSERT_OK(manager_.RecoverFromSnapshot(&snapshot));

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kNonZeroConfig,
                                    kNonZeroConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());

  // Capture snapshot to verify rx_total_credits was preserved as 0,
  // rather than being overridden by kNonZeroConfig.initial_credits (10).
  std::optional<RfcommChannelSnapshot> captured_snapshot;
  state_update_callback_ =
      [&captured_snapshot](const RfcommStateUpdate& update) {
        if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
          captured_snapshot = *snap;
        }
      };

  manager_.CompleteRecovery();

  // Trigger a credit mutation to read snapshot.
  PW_TEST_ASSERT_OK(manager_.SendAdditionalRxCredits(
      kConnectionHandle1, kChannelNumber1, RfcommDirection::kInitiator, 1));
  ASSERT_TRUE(captured_snapshot.has_value());
  EXPECT_EQ(captured_snapshot->rx_total_credits, 1);
  EXPECT_EQ(captured_snapshot->rx_credits, 1);
}

TEST_F(RfcommManagerRecoveryTest, CompleteRecoverySweepsAbandonedChannels) {
  Vector<RfcommChannelRemoved, 4> removed_channels;
  state_update_callback_ =
      [&removed_channels](const RfcommStateUpdate& update) {
        if (const auto* removal = std::get_if<RfcommChannelRemoved>(&update)) {
          removed_channels.push_back(*removal);
        }
      };

  RfcommSnapshot snapshot;
  snapshot.rfcomm_channels.push_back(CreateRfcommChannelSnapshot(
      /*connection_handle=*/static_cast<uint16_t>(kConnectionHandle1),
      /*channel_number=*/kChannelNumber1,
      /*direction=*/RfcommDirection::kInitiator));
  snapshot.rfcomm_channels.push_back(CreateRfcommChannelSnapshot(
      /*connection_handle=*/static_cast<uint16_t>(kConnectionHandle1),
      /*channel_number=*/kChannelNumber2,
      /*direction=*/RfcommDirection::kInitiator));
  snapshot.rfcomm_channels.push_back(CreateRfcommChannelSnapshot(
      /*connection_handle=*/static_cast<uint16_t>(kConnectionHandle2),
      /*channel_number=*/kChannelNumber1,
      /*direction=*/RfcommDirection::kResponder));
  PW_TEST_ASSERT_OK(manager_.RecoverFromSnapshot(&snapshot));

  auto ch1 = manager_.AcquireRfcommChannel(multibuf_allocator_,
                                           kConnectionHandle1,
                                           kChannelNumber1,
                                           RfcommDirection::kInitiator,
                                           true,
                                           kDefaultConfig,
                                           kDefaultConfig,
                                           nullptr,
                                           nullptr);
  PW_TEST_ASSERT_OK(ch1.status());

  manager_.CompleteRecovery();

  ASSERT_EQ(removed_channels.size(), 2u);

  EXPECT_EQ(removed_channels[0].connection_handle,
            static_cast<uint16_t>(kConnectionHandle1));
  EXPECT_EQ(removed_channels[0].channel_number, kChannelNumber2);
  EXPECT_EQ(removed_channels[0].direction, RfcommDirection::kInitiator);

  EXPECT_EQ(removed_channels[1].connection_handle,
            static_cast<uint16_t>(kConnectionHandle2));
  EXPECT_EQ(removed_channels[1].channel_number, kChannelNumber1);
  EXPECT_EQ(removed_channels[1].direction, RfcommDirection::kResponder);
}

TEST_F(RfcommManagerRecoveryTest, CompleteRecoveryIdempotent) {
  size_t removal_count = 0;
  state_update_callback_ = [&removal_count](const RfcommStateUpdate& update) {
    if (std::holds_alternative<RfcommChannelRemoved>(update)) {
      ++removal_count;
    }
  };

  RfcommSnapshot snapshot;
  snapshot.rfcomm_channels.push_back(CreateRfcommChannelSnapshot(
      /*connection_handle=*/static_cast<uint16_t>(kConnectionHandle1),
      /*channel_number=*/kChannelNumber1,
      /*direction=*/RfcommDirection::kInitiator));
  PW_TEST_ASSERT_OK(manager_.RecoverFromSnapshot(&snapshot));

  manager_.CompleteRecovery();
  EXPECT_EQ(removal_count, 1u);

  manager_.CompleteRecovery();
  EXPECT_EQ(removal_count, 1u);
}

TEST_F(RfcommManagerRecoveryTest,
       CompleteRecoveryWithNoRestoredSnapshotIsNoOp) {
  size_t callback_count = 0;
  state_update_callback_ = [&callback_count](const RfcommStateUpdate&) {
    ++callback_count;
  };

  manager_.CompleteRecovery();
  EXPECT_EQ(callback_count, 0u);
}

TEST_F(RfcommManagerRecoveryTest, StateUpdateEmittedOnChannelRelease) {
  struct CallbackState {
    std::optional<RfcommChannelSnapshot> last_channel_snapshot;
    std::optional<RfcommChannelRemoved> last_channel_removed;
  } callback_state;

  state_update_callback_ = [&callback_state](const RfcommStateUpdate& update) {
    if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
      callback_state.last_channel_snapshot = *snap;
    } else if (const auto* rem = std::get_if<RfcommChannelRemoved>(&update)) {
      callback_state.last_channel_removed = *rem;
    }
  };

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  ASSERT_TRUE(callback_state.last_channel_snapshot.has_value());
  EXPECT_EQ(callback_state.last_channel_snapshot->connection_handle,
            static_cast<uint16_t>(kConnectionHandle1));
  EXPECT_EQ(callback_state.last_channel_snapshot->channel_number,
            kChannelNumber1);
  EXPECT_EQ(callback_state.last_channel_snapshot->direction,
            RfcommDirection::kInitiator);

  PW_TEST_EXPECT_OK(manager_.ReleaseRfcommChannel(
      kConnectionHandle1, kChannelNumber1, RfcommDirection::kInitiator));
  ASSERT_TRUE(callback_state.last_channel_removed.has_value());
  EXPECT_EQ(callback_state.last_channel_removed->connection_handle,
            static_cast<uint16_t>(kConnectionHandle1));
  EXPECT_EQ(callback_state.last_channel_removed->channel_number,
            kChannelNumber1);
  EXPECT_EQ(callback_state.last_channel_removed->direction,
            RfcommDirection::kInitiator);
}

TEST_F(RfcommManagerRecoveryTest, StateUpdateEmittedOnRemoteDisconnection) {
  std::optional<RfcommChannelRemoved> last_channel_removed;
  state_update_callback_ =
      [&last_channel_removed](const RfcommStateUpdate& update) {
        if (const auto* rem = std::get_if<RfcommChannelRemoved>(&update)) {
          last_channel_removed = *rem;
        }
      };

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());

  const pw::Vector<uint8_t, 4> kDiscPdu = {0x17, 0x43, 0x01, 0xa0};
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(kDiscPdu.size());
  ASSERT_TRUE(mbuf_result.has_value());
  PW_TEST_ASSERT_OK(mbuf_result->CopyFrom(as_bytes(span(kDiscPdu))).status());

  bool handled = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf_result),
                                                     kConnectionHandle1,
                                                     kDefaultConfig.cid,
                                                     kDefaultConfig.cid);
  EXPECT_TRUE(handled);
  ASSERT_TRUE(last_channel_removed.has_value());
  EXPECT_EQ(last_channel_removed->connection_handle,
            static_cast<uint16_t>(kConnectionHandle1));
  EXPECT_EQ(last_channel_removed->channel_number, kChannelNumber1);
}

TEST_F(RfcommManagerRecoveryTest, StateUpdateEmittedOnDeregisterAndClose) {
  Vector<RfcommChannelRemoved, 4> removed_channels;
  state_update_callback_ =
      [&removed_channels](const RfcommStateUpdate& update) {
        if (const auto* rem = std::get_if<RfcommChannelRemoved>(&update)) {
          removed_channels.push_back(*rem);
        }
      };

  auto channel1_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel1_result.status());

  auto channel2_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber2,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel2_result.status());

  manager_.DeregisterAndCloseChannels(RfcommEvent::kChannelClosedByOther);
  EXPECT_EQ(removed_channels.size(), 2u);
}

TEST_F(RfcommManagerRecoveryTest, StateUpdateEmittedOnL2capEvent) {
  std::optional<RfcommChannelRemoved> last_channel_removed;
  state_update_callback_ =
      [&last_channel_removed](const RfcommStateUpdate& update) {
        if (const auto* rem = std::get_if<RfcommChannelRemoved>(&update)) {
          last_channel_removed = *rem;
        }
      };

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());

  l2cap_manager_.TriggerL2capEvent(L2capChannelEvent::kReset);
  ASSERT_TRUE(last_channel_removed.has_value());
  EXPECT_EQ(last_channel_removed->connection_handle,
            static_cast<uint16_t>(kConnectionHandle1));
  EXPECT_EQ(last_channel_removed->channel_number, kChannelNumber1);
}

#if PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES
TEST_F(RfcommManagerRecoveryTest,
       StateUpdateOnSendAdditionalRxCreditsAndSuppressionOnZero) {
  struct CallbackState {
    size_t update_count = 0;
    std::optional<RfcommChannelSnapshot> last_channel_snapshot;
  } callback_state;
  state_update_callback_ = [&callback_state](const RfcommStateUpdate& update) {
    if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
      callback_state.last_channel_snapshot = *snap;
      ++callback_state.update_count;
    }
  };

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_EQ(callback_state.update_count, 1u);
  ASSERT_TRUE(callback_state.last_channel_snapshot.has_value());
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_credits,
            kDefaultConfig.initial_credits);

  // Sending 0 additional credits is a no-op and must NOT trigger a
  // notification.
  PW_TEST_ASSERT_OK(manager_.SendAdditionalRxCredits(
      kConnectionHandle1, kChannelNumber1, RfcommDirection::kInitiator, 0));
  EXPECT_EQ(callback_state.update_count, 1u);

  // Sending 5 additional credits mutates state and triggers a notification.
  // SendCredits transmits the frame immediately, so exactly one state update
  // is emitted without redundant duplicates.
  PW_TEST_ASSERT_OK(manager_.SendAdditionalRxCredits(
      kConnectionHandle1, kChannelNumber1, RfcommDirection::kInitiator, 5));
  EXPECT_EQ(callback_state.update_count, 2u);
  ASSERT_TRUE(callback_state.last_channel_snapshot.has_value());
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_credits,
            static_cast<uint8_t>(kDefaultConfig.initial_credits + 5));
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_total_credits,
            static_cast<uint8_t>(kDefaultConfig.initial_credits + 5));
}

TEST_F(RfcommManagerRecoveryTest,
       StateUpdateOnSendAdditionalRxCreditsWhenL2capWriteDeferred) {
  struct CallbackState {
    size_t update_count = 0;
    std::optional<RfcommChannelSnapshot> last_channel_snapshot;
  } callback_state;
  state_update_callback_ = [&callback_state](const RfcommStateUpdate& update) {
    if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
      callback_state.last_channel_snapshot = *snap;
      ++callback_state.update_count;
    }
  };

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_EQ(callback_state.update_count, 1u);

  // Simulate L2CAP congestion/unavailable so SendCredits cannot transmit
  // immediately and queues the credit frame.
  l2cap_manager_.last_channel_proxy()->set_write_status(Status::Unavailable());

  // Sending 5 additional credits increases rx_total_credits_. Because the
  // credit frame cannot be transmitted immediately, SendAdditionalRxCredits
  // notifies so the persistent container reflects the mutated
  // rx_total_credits_.
  PW_TEST_ASSERT_OK(manager_.SendAdditionalRxCredits(
      kConnectionHandle1, kChannelNumber1, RfcommDirection::kInitiator, 5));
  EXPECT_EQ(callback_state.update_count, 2u);
  ASSERT_TRUE(callback_state.last_channel_snapshot.has_value());
  // rx_credits has not increased yet because the frame was not transmitted.
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_credits,
            kDefaultConfig.initial_credits);
  // rx_total_credits has increased.
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_total_credits,
            static_cast<uint8_t>(kDefaultConfig.initial_credits + 5));
}

TEST_F(RfcommManagerRecoveryTest,
       StateUpdateOnWriteAndSuppressionWhenNoCredits) {
  constexpr RfcommChannelConfig kOneCreditConfig = {
      .cid = 1, .max_frame_size = 100, .initial_credits = 1};
  struct CallbackState {
    size_t update_count = 0;
    std::optional<RfcommChannelSnapshot> last_channel_snapshot;
  } callback_state;
  state_update_callback_ = [&callback_state](const RfcommStateUpdate& update) {
    if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
      callback_state.last_channel_snapshot = *snap;
      ++callback_state.update_count;
    }
  };

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kOneCreditConfig,
                                    kOneCreditConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_EQ(callback_state.update_count, 1u);
  ASSERT_TRUE(callback_state.last_channel_snapshot.has_value());
  EXPECT_EQ(callback_state.last_channel_snapshot->tx_credits, 1u);

  // Write 1: Has 1 credit, so packet is transmitted over L2CAP and tx_credits
  // decrements to 0.
  auto mbuf1 = multibuf_allocator_.AllocateContiguous(10);
  ASSERT_TRUE(mbuf1.has_value());
  auto write_status1 = manager_.Write(kConnectionHandle1,
                                      kChannelNumber1,
                                      RfcommDirection::kInitiator,
                                      std::move(*mbuf1));
  PW_TEST_ASSERT_OK(write_status1.status);
  EXPECT_EQ(callback_state.update_count, 2u);
  EXPECT_EQ(callback_state.last_channel_snapshot->tx_credits, 0u);

  // Write 2: tx_credits is 0. Packet is queued without sending; credit state
  // does not change.
  auto mbuf2 = multibuf_allocator_.AllocateContiguous(10);
  ASSERT_TRUE(mbuf2.has_value());
  auto write_status2 = manager_.Write(kConnectionHandle1,
                                      kChannelNumber1,
                                      RfcommDirection::kInitiator,
                                      std::move(*mbuf2));
  PW_TEST_ASSERT_OK(write_status2.status);
  // Notification must be suppressed.
  EXPECT_EQ(callback_state.update_count, 2u);
}

TEST_F(RfcommManagerRecoveryTest, StateUpdateOnAddCreditsAndSuppressionOnZero) {
  constexpr RfcommChannelConfig kZeroCreditsConfig = {
      .cid = 1, .max_frame_size = 100, .initial_credits = 0};
  struct CallbackState {
    size_t update_count = 0;
    std::optional<RfcommChannelSnapshot> last_channel_snapshot;
  } callback_state;
  state_update_callback_ = [&callback_state](const RfcommStateUpdate& update) {
    if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
      callback_state.last_channel_snapshot = *snap;
      ++callback_state.update_count;
    }
  };

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kZeroCreditsConfig,
                                    kZeroCreditsConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_EQ(callback_state.update_count, 1u);
  ASSERT_TRUE(callback_state.last_channel_snapshot.has_value());
  EXPECT_EQ(callback_state.last_channel_snapshot->tx_credits, 0u);

  // Receiving 0 credits is a no-op and must NOT emit a notification.
  bool handled = l2cap_manager_.TriggerControllerPdu(
      MakeCreditPdu(kChannelNumber1, RfcommDirection::kInitiator, true, 0),
      kConnectionHandle1,
      kZeroCreditsConfig.cid,
      kZeroCreditsConfig.cid);
  EXPECT_FALSE(handled);
  EXPECT_EQ(callback_state.update_count, 1u);

  // Receiving 3 credits on an empty queue mutates tx_credits from 0 to 3 ->
  // emits notification.
  handled = l2cap_manager_.TriggerControllerPdu(
      MakeCreditPdu(kChannelNumber1, RfcommDirection::kInitiator, true, 3),
      kConnectionHandle1,
      kZeroCreditsConfig.cid,
      kZeroCreditsConfig.cid);
  EXPECT_FALSE(handled);
  EXPECT_EQ(callback_state.update_count, 2u);
  EXPECT_EQ(callback_state.last_channel_snapshot->tx_credits, 3u);
}

TEST_F(RfcommManagerRecoveryTest,
       StateUpdateSuppressedOnSaturatedStreamAddCredits) {
  constexpr RfcommChannelConfig kOneCreditConfig = {
      .cid = 1, .max_frame_size = 100, .initial_credits = 1};
  struct CallbackState {
    size_t update_count = 0;
    std::optional<RfcommChannelSnapshot> last_channel_snapshot;
  } callback_state;
  state_update_callback_ = [&callback_state](const RfcommStateUpdate& update) {
    if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
      callback_state.last_channel_snapshot = *snap;
      ++callback_state.update_count;
    }
  };

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kOneCreditConfig,
                                    kOneCreditConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  ASSERT_TRUE(callback_state.last_channel_snapshot.has_value());
  EXPECT_EQ(callback_state.last_channel_snapshot->tx_credits, 1u);

  // Drain the 1 credit.
  auto mbuf1 = multibuf_allocator_.AllocateContiguous(10);
  ASSERT_TRUE(mbuf1.has_value());
  PW_TEST_ASSERT_OK(manager_
                        .Write(kConnectionHandle1,
                               kChannelNumber1,
                               RfcommDirection::kInitiator,
                               std::move(*mbuf1))
                        .status);
  EXPECT_EQ(callback_state.last_channel_snapshot->tx_credits, 0u);

  // Queue a packet while tx_credits == 0.
  auto mbuf2 = multibuf_allocator_.AllocateContiguous(10);
  ASSERT_TRUE(mbuf2.has_value());
  PW_TEST_ASSERT_OK(manager_
                        .Write(kConnectionHandle1,
                               kChannelNumber1,
                               RfcommDirection::kInitiator,
                               std::move(*mbuf2))
                        .status);

  // Reset update counter before testing AddCredits optimization.
  callback_state.update_count = 0;

  // Add 1 credit from peer: AddCredits(1) adds 1 credit, immediately calls
  // TryToSendPacket(), which sends the queued packet, consuming the 1 credit.
  // tx_credits ends at 0 (same as initial).
  bool handled = l2cap_manager_.TriggerControllerPdu(
      MakeCreditPdu(kChannelNumber1, RfcommDirection::kInitiator, true, 1),
      kConnectionHandle1,
      kOneCreditConfig.cid,
      kOneCreditConfig.cid);
  EXPECT_FALSE(handled);

  // Verification: notification is suppressed during saturated ACK/credit
  // return.
  EXPECT_EQ(callback_state.update_count, 0u);
}

TEST_F(RfcommManagerRecoveryTest, StateUpdateSuppressedWhenL2capWriteFails) {
  constexpr RfcommChannelConfig kOneCreditConfig = {
      .cid = 1, .max_frame_size = 100, .initial_credits = 1};
  size_t update_count = 0;
  state_update_callback_ = [&update_count](const RfcommStateUpdate& update) {
    if (std::holds_alternative<RfcommChannelSnapshot>(update)) {
      ++update_count;
    }
  };

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kOneCreditConfig,
                                    kOneCreditConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_EQ(update_count, 1u);

  // Simulate L2CAP congestion/failure.
  l2cap_manager_.last_channel_proxy()->set_write_status(Status::Unavailable());

  auto mbuf = multibuf_allocator_.AllocateContiguous(10);
  ASSERT_TRUE(mbuf.has_value());
  PW_TEST_ASSERT_OK(manager_
                        .Write(kConnectionHandle1,
                               kChannelNumber1,
                               RfcommDirection::kInitiator,
                               std::move(*mbuf))
                        .status);

  // Because L2CAP write failed, packet remains in queue, tx_credits is not
  // decremented, and no notification is emitted.
  EXPECT_EQ(update_count, 1u);
}

TEST_F(RfcommManagerRecoveryTest,
       StateUpdateOnIncomingDataPduDecrementsRxCredits) {
  struct CallbackState {
    size_t update_count = 0;
    std::optional<RfcommChannelSnapshot> last_channel_snapshot;
  } callback_state;
  state_update_callback_ = [&callback_state](const RfcommStateUpdate& update) {
    if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
      callback_state.last_channel_snapshot = *snap;
      ++callback_state.update_count;
    }
  };

  auto channel_result = manager_.AcquireRfcommChannel(
      multibuf_allocator_,
      kConnectionHandle1,
      kChannelNumber1,
      RfcommDirection::kResponder,
      true,
      kDefaultConfig,
      kDefaultConfig,
      [](multibuf::MultiBuf&& /*pdu*/) {},
      nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_EQ(callback_state.update_count, 1u);
  ASSERT_TRUE(callback_state.last_channel_snapshot.has_value());
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_credits,
            kDefaultConfig.initial_credits);

  // Send a valid UIH data frame (channel 1 responder = DLCI 4) with 1 byte
  // payload.
  const pw::Vector<uint8_t, 5> kPdu = {0x11, 0xEF, 0x03, 0x01, 0xbf};
  auto mbuf_result = multibuf_allocator_.AllocateContiguous(kPdu.size());
  ASSERT_TRUE(mbuf_result.has_value());
  PW_TEST_ASSERT_OK(mbuf_result->CopyFrom(as_bytes(span(kPdu))).status());

  bool handled = l2cap_manager_.TriggerControllerPdu(std::move(*mbuf_result),
                                                     kConnectionHandle1,
                                                     kDefaultConfig.cid,
                                                     kDefaultConfig.cid);
  EXPECT_FALSE(handled);

  // Expect rx_credits decremented from 10 to 9, triggering a notification.
  EXPECT_EQ(callback_state.update_count, 2u);
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_credits,
            static_cast<uint8_t>(kDefaultConfig.initial_credits - 1));
}

TEST_F(RfcommManagerRecoveryTest,
       StateUpdateEmittedWhenDeferredRxCreditGrantFlushes) {
  struct CallbackState {
    size_t update_count = 0;
    std::optional<RfcommChannelSnapshot> last_channel_snapshot;
  } callback_state;
  state_update_callback_ = [&callback_state](const RfcommStateUpdate& update) {
    if (const auto* snap = std::get_if<RfcommChannelSnapshot>(&update)) {
      callback_state.last_channel_snapshot = *snap;
      ++callback_state.update_count;
    }
  };

  auto channel_result =
      manager_.AcquireRfcommChannel(multibuf_allocator_,
                                    kConnectionHandle1,
                                    kChannelNumber1,
                                    RfcommDirection::kInitiator,
                                    true,
                                    kDefaultConfig,
                                    kDefaultConfig,
                                    nullptr,
                                    nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_EQ(callback_state.update_count, 1u);
  ASSERT_TRUE(callback_state.last_channel_snapshot.has_value());
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_credits,
            kDefaultConfig.initial_credits);
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_total_credits,
            kDefaultConfig.initial_credits);

  // 1. Simulate L2CAP congestion so SendAdditionalRxCredits cannot transmit
  // immediately.
  l2cap_manager_.last_channel_proxy()->set_write_status(Status::Unavailable());

  PW_TEST_ASSERT_OK(manager_.SendAdditionalRxCredits(
      kConnectionHandle1, kChannelNumber1, RfcommDirection::kInitiator, 5));
  // Notification is emitted immediately for the mutation to rx_total_credits,
  // but rx_credits remains at initial_credits (10) because the frame is
  // pending.
  EXPECT_EQ(callback_state.update_count, 2u);
  ASSERT_TRUE(callback_state.last_channel_snapshot.has_value());
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_credits,
            kDefaultConfig.initial_credits);
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_total_credits,
            static_cast<uint8_t>(kDefaultConfig.initial_credits + 5));

  // 2. Unblock L2CAP channel proxy.
  l2cap_manager_.last_channel_proxy()->set_write_status(OkStatus());

  // 3. Trigger a send attempt via Write.
  auto mbuf = multibuf_allocator_.AllocateContiguous(10);
  ASSERT_TRUE(mbuf.has_value());
  PW_TEST_ASSERT_OK(manager_
                        .Write(kConnectionHandle1,
                               kChannelNumber1,
                               RfcommDirection::kInitiator,
                               std::move(*mbuf))
                        .status);

  // 4. Verify that the deferred credit grant was flushed and a new state update
  // was emitted showing rx_credits increased to 15.
  EXPECT_EQ(callback_state.update_count, 3u);
  ASSERT_TRUE(callback_state.last_channel_snapshot.has_value());
  EXPECT_EQ(callback_state.last_channel_snapshot->rx_credits,
            static_cast<uint8_t>(kDefaultConfig.initial_credits + 5));
}
#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_CREDIT_SNAPSHOT_UPDATES

TEST_F(RfcommProxyHostTest,
       AclDisconnectionCascadesToRfcommChannelRemovedNotification) {
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  constexpr ConnectionHandle kConnectionHandle1 =
      static_cast<ConnectionHandle>(1);
  constexpr uint8_t kChannelNumber1 = 2;
  constexpr RfcommChannelConfig kDefaultConfig = {
      .cid = 0x0040, .max_frame_size = 100, .initial_credits = 10};

  std::array<std::byte, 2048> buffer{};
  multibuf::SimpleAllocator multibuf_allocator(
      /*data_area=*/buffer,
      /*metadata_alloc=*/allocator::GetLibCAllocator());

  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/1,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_ASSERT_OK(SendReadBufferResponseFromController(proxy, 1, 251));
  PW_TEST_ASSERT_OK(
      SendConnectionCompleteEvent(proxy, 1, emboss::StatusCode::SUCCESS));

  pw::allocator::test::AllocatorForTest<4096> test_allocator;
  std::optional<RfcommChannelRemoved> removed_notification;
  RfcommManager rfcomm_mgr(
      proxy,
      test_allocator,
      [&removed_notification](const RfcommStateUpdate& update) {
        if (const auto* rem = std::get_if<RfcommChannelRemoved>(&update)) {
          removed_notification = *rem;
        }
      });

  std::optional<RfcommEvent> received_event;
  auto channel_result = rfcomm_mgr.AcquireRfcommChannel(
      multibuf_allocator,
      kConnectionHandle1,
      kChannelNumber1,
      RfcommDirection::kInitiator,
      true,
      kDefaultConfig,
      kDefaultConfig,
      nullptr,
      [&received_event](RfcommEvent event) { received_event = event; });
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_TRUE(channel_result.value());

  // Simulate an ACL disconnection complete event for connection handle 1.
  PW_TEST_ASSERT_OK(SendDisconnectionCompleteEvent(proxy, 1));
  RunDispatcher();

  // Verify the disconnection cascaded upwards to the RFCOMM layer:
  // 1. Client event callback was invoked with
  // RfcommEvent::kChannelClosedByOther.
  EXPECT_EQ(received_event, RfcommEvent::kChannelClosedByOther);

  // 2. State update callback received an RfcommChannelRemoved notification.
  ASSERT_TRUE(removed_notification.has_value());
  EXPECT_EQ(removed_notification->connection_handle,
            static_cast<uint16_t>(kConnectionHandle1));
  EXPECT_EQ(removed_notification->channel_number, kChannelNumber1);

  // 3. Internal channel is removed: attempting to write to the channel fails.
  auto mbuf = multibuf_allocator.AllocateContiguous(10);
  ASSERT_TRUE(mbuf.has_value());
  EXPECT_EQ(channel_result.value().Write(std::move(*mbuf)).status,
            Status::NotFound());
}

TEST_F(RfcommProxyHostTest, AcquireRfcommChannelSucceedsUnderSnapshotDataLoss) {
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  constexpr ConnectionHandle kConnectionHandle1 =
      static_cast<ConnectionHandle>(1);
  constexpr uint8_t kChannelNumber1 = 2;
  constexpr RfcommChannelConfig kDefaultConfig = {
      .cid = 0x0040, .max_frame_size = 100, .initial_credits = 10};

  std::array<std::byte, 2048> buffer{};
  multibuf::SimpleAllocator multibuf_allocator(
      /*data_area=*/buffer,
      /*metadata_alloc=*/allocator::GetLibCAllocator());

  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/1,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);

  // Populate snapshot with an ACL connection that had queued host packets,
  // indicating data loss occurred during crash/downtime.
  ProxyHostSnapshot proxy_snapshot;
  proxy_snapshot.acl.acl_connections.push_back(
      AclConnectionSnapshot{.connection_handle = 1,
                            .transport = AclTransportType::kBrEdr,
                            .num_queued_host_packets = 1});

  L2capChannelSnapshot l2cap_channel_snapshot;
  l2cap_channel_snapshot.connection_handle = 1;
  l2cap_channel_snapshot.transport = AclTransportType::kBrEdr;
  l2cap_channel_snapshot.local_cid = kDefaultConfig.cid;
  l2cap_channel_snapshot.remote_cid = kDefaultConfig.cid;
  l2cap_channel_snapshot.mode = L2capChannelMode::kBasic;
  l2cap_channel_snapshot.allow_data_loss = true;
  proxy_snapshot.l2cap.l2cap_channels.push_back(l2cap_channel_snapshot);
  proxy_snapshot.l2cap.l2cap_signaling_states.push_back(
      L2capSignalingStateSnapshot{
          .connection_handle = 1,
          .transport = AclTransportType::kBrEdr,
      });

  PW_TEST_ASSERT_OK(proxy.RecoverFromSnapshot(proxy_snapshot));
  PW_TEST_ASSERT_OK(SendReadBufferResponseFromController(proxy, 1, 251));

  pw::allocator::test::AllocatorForTest<4096> test_allocator;
  RfcommManager rfcomm_mgr(proxy, test_allocator);

  RfcommSnapshot rfcomm_snapshot;
  rfcomm_snapshot.rfcomm_channels.push_back(CreateRfcommChannelSnapshot(
      /*connection_handle=*/static_cast<uint16_t>(kConnectionHandle1),
      /*channel_number=*/kChannelNumber1,
      /*direction=*/RfcommDirection::kInitiator,
      /*local_cid=*/kDefaultConfig.cid,
      /*remote_cid=*/kDefaultConfig.cid,
      /*mux_initiator=*/true,
      /*tx_credits=*/15,
      /*rx_credits=*/12,
      /*rx_total_credits=*/20,
      /*max_frame_size=*/100));

  PW_TEST_ASSERT_OK(rfcomm_mgr.RecoverFromSnapshot(&rfcomm_snapshot));

  // Re-acquiring the RFCOMM channel during recovery succeeds even though
  // data loss occurred, because RFCOMM basic-mode channels set allow_data_loss
  // = true.
  auto channel_result =
      rfcomm_mgr.AcquireRfcommChannel(multibuf_allocator,
                                      kConnectionHandle1,
                                      kChannelNumber1,
                                      RfcommDirection::kInitiator,
                                      true,
                                      kDefaultConfig,
                                      kDefaultConfig,
                                      nullptr,
                                      nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());
  EXPECT_TRUE(channel_result.value());

  auto mbuf = multibuf_allocator.AllocateContiguous(10);
  ASSERT_TRUE(mbuf.has_value());
  PW_TEST_EXPECT_OK(channel_result.value().Write(std::move(*mbuf)).status);
}

#endif  // PW_BLUETOOTH_PROXY_CONFIG_ENABLE_RECOVERY

#if PW_BLUETOOTH_PROXY_ASYNC != 0

TEST_F(RfcommProxyHostTest, RfcommDeadlockAtPduWhileTx) {
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  constexpr ConnectionHandle kConnectionHandle1 =
      static_cast<ConnectionHandle>(1);
  constexpr uint8_t kChannelNumber1 = 2;
  constexpr RfcommChannelConfig kDefaultConfig = {
      .cid = 1, .max_frame_size = 100, .initial_credits = 10};

  std::array<std::byte, 2048> buffer{};
  multibuf::SimpleAllocator multibuf_allocator(
      /*data_area=*/buffer,
      /*metadata_alloc=*/allocator::GetLibCAllocator());

  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_ASSERT_OK(SendReadBufferResponseFromController(proxy, 1, 251));
  PW_TEST_ASSERT_OK(
      SendLeConnectionCompleteEvent(proxy, 1, emboss::StatusCode::SUCCESS));

  pw::allocator::test::AllocatorForTest<4096> test_allocator;
  RfcommManager rfcomm_mgr(proxy, test_allocator);

  auto channel_result =
      rfcomm_mgr.AcquireRfcommChannel(multibuf_allocator,
                                      kConnectionHandle1,
                                      kChannelNumber1,
                                      RfcommDirection::kResponder,
                                      true,
                                      kDefaultConfig,
                                      kDefaultConfig,
                                      nullptr,
                                      nullptr);
  PW_TEST_ASSERT_OK(channel_result.status());

  struct ClientCapture {
    multibuf::SimpleAllocator& multibuf_allocator;
    RfcommManager& rfcomm_mgr;
  } capture{multibuf_allocator, rfcomm_mgr};

  pw::thread::test::TestThreadContext context;
  pw::Thread client_thread(context.options(), [&capture]() {
    for (int i = 0; i < 10; ++i) {
      auto mbuf = capture.multibuf_allocator.AllocateContiguous(20);
      if (mbuf.has_value()) {
        std::ignore = capture.rfcomm_mgr.Write(kConnectionHandle1,
                                               kChannelNumber1,
                                               RfcommDirection::kResponder,
                                               std::move(*mbuf));
      }
    }
  });

  pw::this_thread::sleep_for(std::chrono::milliseconds(50));

  const pw::Vector<uint8_t, 5> kPdu = {0x11, 0xEF, 0x03, 0x01, 0xbf};
  SendL2capBFrame(proxy, 1, span(kPdu), kPdu.size(), 1);

  client_thread.join();
  RunDispatcher();
}

#endif  // PW_BLUETOOTH_PROXY_ASYNC != 0

}  // namespace pw::bluetooth::proxy::rfcomm
