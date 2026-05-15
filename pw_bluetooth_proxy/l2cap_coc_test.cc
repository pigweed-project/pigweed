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

#include <cstdint>
#include <mutex>

#include "pw_allocator/libc_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy_private/test_utils.h"
#include "pw_containers/flat_map.h"
#include "pw_multibuf/from_span.h"
#include "pw_multibuf/multibuf.h"
#include "pw_sync/mutex.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"

namespace pw::bluetooth::proxy {
namespace {

constexpr uint16_t kConnectionHandle = 123;

// ########## L2capCocTest

class L2capCocTest : public ProxyHostTest {};

TEST_F(L2capCocTest, CannotCreateChannelWithInvalidArgs) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);

  // Connection handle too large by 1.
  EXPECT_EQ(BuildCocWithResult(proxy, CocParameters{.handle = 0x0FFF}).status(),
            PW_STATUS_INVALID_ARGUMENT);

  // Local CID invalid (0).
  EXPECT_EQ(BuildCocWithResult(proxy, CocParameters{.local_cid = 0}).status(),
            PW_STATUS_INVALID_ARGUMENT);

  // Remote CID invalid (0).
  EXPECT_EQ(BuildCocWithResult(proxy, CocParameters{.remote_cid = 0}).status(),
            PW_STATUS_INVALID_ARGUMENT);
}

TEST_F(L2capCocTest, CanDeleteChannelInEventCallback) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  // Allow proxy to reserve 1 credit.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  std::optional<L2capCoc> channel;
  channel = BuildCoc(
      proxy,
      CocParameters{
          .handle = kConnectionHandle,
          .tx_credits = 1,
          .event_fn = [&channel]([[maybe_unused]] L2capChannelEvent event) {
            channel.reset();
          }});
  channel->Close();
  EXPECT_FALSE(channel.has_value());
}

// ########## L2capCocWriteTest

class L2capCocWriteTest : public ProxyHostTest {};

// Size of sdu_length field in first K-frames.
constexpr uint8_t kSduLengthFieldSize = 2;
// Size of a K-Frame over Acl packet with no payload.
constexpr uint8_t kFirstKFrameOverAclMinSize =
    emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
    emboss::FirstKFrame::MinSizeInBytes();

TEST_F(L2capCocWriteTest, BasicWrite) {
  struct {
    int sends_called = 0;
    // First four bits 0x0 encode PB & BC flags
    uint16_t handle = 0x0ACB;
    // Length of L2CAP PDU
    uint16_t acl_data_total_length = 0x0009;
    // L2CAP header PDU length field
    uint16_t pdu_length = 0x0005;
    // Random CID
    uint16_t channel_id = 0x1234;
    // Length of L2CAP SDU
    uint16_t sdu_length = 0x0003;
    // L2CAP information payload
    std::array<uint8_t, 3> payload = {0xAB, 0xCD, 0xEF};

    // Built from the preceding values in little endian order (except payload in
    // big endian).
    std::array<uint8_t, 13> expected_hci_packet = {0xCB,
                                                   0x0A,
                                                   0x09,
                                                   0x00,
                                                   0x05,
                                                   0x00,
                                                   0x34,
                                                   0x12,
                                                   0x03,
                                                   0x00,
                                                   0xAB,
                                                   0xCD,
                                                   0xEF};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        ++capture.sends_called;
        EXPECT_EQ(packet.GetH4Type(), emboss::H4PacketType::ACL_DATA);
        EXPECT_EQ(packet.GetHciSpan().size(),
                  capture.expected_hci_packet.size());
        EXPECT_TRUE(std::equal(packet.GetHciSpan().begin(),
                               packet.GetHciSpan().end(),
                               capture.expected_hci_packet.begin(),
                               capture.expected_hci_packet.end()));
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto acl,
            MakeEmbossView<emboss::AclDataFrameView>(packet.GetHciSpan()));
        EXPECT_EQ(acl.header().handle().Read(), capture.handle);
        EXPECT_EQ(acl.header().packet_boundary_flag().Read(),
                  emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
        EXPECT_EQ(acl.header().broadcast_flag().Read(),
                  emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
        EXPECT_EQ(acl.data_total_length().Read(),
                  capture.acl_data_total_length);
        emboss::FirstKFrameView kframe = emboss::MakeFirstKFrameView(
            acl.payload().BackingStorage().data(), acl.SizeInBytes());
        EXPECT_EQ(kframe.pdu_length().Read(), capture.pdu_length);
        EXPECT_EQ(kframe.channel_id().Read(), capture.channel_id);
        EXPECT_EQ(kframe.sdu_length().Read(), capture.sdu_length);
        for (size_t i = 0; i < 3; ++i) {
          EXPECT_EQ(kframe.payload()[i].Read(), capture.payload[i]);
        }
      });

  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  // Allow proxy to reserve 1 credit.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, capture.handle, emboss::StatusCode::SUCCESS));

  L2capCoc channel = BuildCoc(proxy,
                              CocParameters{.handle = capture.handle,
                                            .remote_cid = capture.channel_id});
  auto mbuf_result = multibuf::FromSpan(
      *allocator, as_writable_bytes(span(capture.payload)), [](ByteSpan) {});
  ASSERT_TRUE(mbuf_result.has_value());
  multibuf::MultiBuf mbuf = std::move(*mbuf_result);
  PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
  RunDispatcher();
  EXPECT_EQ(capture.sends_called, 1);
}

TEST_F(L2capCocWriteTest, ErrorOnWriteToStoppedChannel) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  // Allow proxy to reserve 1 credit.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .tx_credits = 1,
                    .event_fn = []([[maybe_unused]] L2capChannelEvent event) {
                      FAIL();
                    }});

  channel.Stop();
  EXPECT_EQ(channel.IsWriteAvailable(), PW_STATUS_FAILED_PRECONDITION);
  multibuf::MultiBuf empty = MakeEmptyMultiBuf();
  EXPECT_EQ(channel.Write(std::move(empty)).status,
            Status::FailedPrecondition());
  RunDispatcher();
}

TEST_F(L2capCocWriteTest, WriteExceedingMtuFails) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) { FAIL(); });
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  // Allow proxy to reserve 1 credit.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  // Payload size exceeds MTU.
  L2capCoc small_mtu_channel = BuildCoc(proxy, CocParameters{.tx_mtu = 1});
  std::array<uint8_t, 24> payload;
  auto mbuf_result = multibuf::FromSpan(
      *allocator, as_writable_bytes(span(payload)), [](ByteSpan) {});
  ASSERT_TRUE(mbuf_result.has_value());
  multibuf::MultiBuf mbuf = std::move(*mbuf_result);
  EXPECT_EQ(small_mtu_channel.Write(std::move(mbuf)).status,
            Status::InvalidArgument());
  RunDispatcher();
}

TEST_F(L2capCocWriteTest, MultipleWritesSameChannel) {
  struct {
    int sends_called = 0;
    std::array<uint8_t, 3> payload = {0xAB, 0xCD, 0xEF};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        ++capture.sends_called;
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto acl,
            MakeEmbossView<emboss::AclDataFrameView>(packet.GetHciSpan()));
        emboss::FirstKFrameView kframe = emboss::MakeFirstKFrameView(
            acl.payload().BackingStorage().data(), acl.SizeInBytes());
        for (size_t i = 0; i < 3; ++i) {
          EXPECT_EQ(kframe.payload()[i].Read(), capture.payload[i]);
        }
      });
  auto* allocator = GetProxyHostAllocator();

  uint16_t num_writes = 5;
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/num_writes,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(
      proxy,
      /*num_credits_to_reserve=*/static_cast<uint8_t>(num_writes)));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  L2capCoc channel = BuildCoc(proxy, CocParameters{.tx_credits = num_writes});
  for (int i = 0; i < num_writes; ++i) {
    auto mbuf_result = multibuf::FromSpan(
        *allocator, as_writable_bytes(span(capture.payload)), [](ByteSpan) {});
    ASSERT_TRUE(mbuf_result.has_value());
    multibuf::MultiBuf mbuf = std::move(*mbuf_result);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    std::for_each(capture.payload.begin(),
                  capture.payload.end(),
                  [](uint8_t& byte) { ++byte; });
  }

  EXPECT_EQ(capture.sends_called, num_writes);
}

// Verify we get unavailable when send queue is full due to running out
// of  ACL credits. And that it reports available again once send queue is no
// longer full.
// TODO: https://pwbug.dev/380299794 - Add equivalent test for other channel
// types.
TEST_F(L2capCocWriteTest, FlowControlDueToAclCredits) {
  uint16_t kHandle = 123;
  // We will send enough packets to use up ACL LE credits and to fill the
  // queue. And then send one more to verify we get an unavailable.
  const uint16_t kAclLeCredits = 2;
  const uint16_t kExpectedSuccessfulWrites =
      kAclLeCredits + L2capChannel::QueueCapacity() - NumBufferedPayloads();
  // Set plenty of kL2capTxCredits to ensure that isn't the bottleneck.
  const uint16_t kL2capTxCredits = kExpectedSuccessfulWrites + 1;

  struct {
    int write_available_events = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/kAclLeCredits,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(
      proxy,
      /*num_credits_to_reserve=*/kAclLeCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  ChannelEventCallback&& kEventFn{[&capture](L2capChannelEvent event) {
    if (event == L2capChannelEvent::kWriteAvailable) {
      capture.write_available_events++;
    }
  }};
  L2capCoc channel = BuildCoc(proxy,
                              CocParameters{.handle = kHandle,
                                            .tx_credits = kL2capTxCredits,
                                            .event_fn = std::move(kEventFn)});

  // Use up the ACL credits and fill up the send queue.
  for (int i = 0; i < kExpectedSuccessfulWrites; ++i) {
    EXPECT_EQ(channel.IsWriteAvailable(), PW_STATUS_OK);
    multibuf::MultiBuf empty = MakeEmptyMultiBuf();
    EXPECT_EQ(channel.Write(std::move(empty)).status, PW_STATUS_OK);
    RunDispatcher();
  }
  EXPECT_EQ(0, capture.write_available_events);

  // Send queue is full, so Write should get unavailable.
  multibuf::MultiBuf empty1 = MakeEmptyMultiBuf();
  EXPECT_EQ(channel.Write(std::move(empty1)).status, PW_STATUS_UNAVAILABLE);
  RunDispatcher();

  // Release a ACL credit, so even should trigger and write should be available
  // again.
  EXPECT_EQ(0, capture.write_available_events);
  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(proxy, {{kHandle, 1}}));
  RunDispatcher();

  EXPECT_EQ(1, capture.write_available_events);
  EXPECT_EQ(channel.IsWriteAvailable(), PW_STATUS_OK);
  multibuf::MultiBuf empty2 = MakeEmptyMultiBuf();
  EXPECT_EQ(channel.Write(std::move(empty2)).status, PW_STATUS_OK);
  RunDispatcher();

  // Verify event on just IsWriteAvailable
  EXPECT_EQ(channel.IsWriteAvailable(), PW_STATUS_UNAVAILABLE);
  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(proxy, {{kHandle, 1}}));
  RunDispatcher();

  EXPECT_EQ(2, capture.write_available_events);
}

// Verify we get unavailable when send queue is full due to running out of L2cap
// CoC credit.
// TODO: https://pwbug.dev/380299794 - Add equivalent test for other channel
// types (where appropriate).
TEST_F(L2capCocWriteTest, UnavailableWhenSendQueueIsFullDueToL2capCocCredits) {
  // We will send enough packets to use up L2CAP CoC credits and to fill the
  // queue. And then send one more to verify we get an unavailable.
  const uint16_t kL2capTxCredits = 2;
  const uint16_t kExpectedSuccessfulWrites =
      kL2capTxCredits + L2capChannel::QueueCapacity();
  // Set plenty of kAclLeCredits to ensure that isn't the bottleneck.
  const uint16_t kAclLeCredits = kExpectedSuccessfulWrites + 1;

  struct {
    int write_available_events = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/kAclLeCredits,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(
      proxy,
      /*num_credits_to_reserve=*/kAclLeCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  ChannelEventCallback&& kEventFn{[&capture](L2capChannelEvent event) {
    if (event == L2capChannelEvent::kWriteAvailable) {
      capture.write_available_events++;
    }
  }};
  L2capCoc channel = BuildCoc(proxy,
                              CocParameters{.tx_credits = kL2capTxCredits,
                                            .event_fn = std::move(kEventFn)});

  // Use up the CoC credits and fill up the send queue.
  for (int i = 0; i < kExpectedSuccessfulWrites; ++i) {
    EXPECT_EQ(channel.IsWriteAvailable(), PW_STATUS_OK);
    multibuf::MultiBuf empty = MakeEmptyMultiBuf();
    EXPECT_EQ(channel.Write(std::move(empty)).status, PW_STATUS_OK);
    RunDispatcher();
  }
  EXPECT_EQ(0, capture.write_available_events);

  // Send queue is full, so client should now get unavailable.
  EXPECT_EQ(channel.IsWriteAvailable(), PW_STATUS_UNAVAILABLE);
  multibuf::MultiBuf empty = MakeEmptyMultiBuf();
  EXPECT_EQ(channel.Write(std::move(empty)).status, PW_STATUS_UNAVAILABLE);
  RunDispatcher();
  EXPECT_EQ(0, capture.write_available_events);

  // TODO: https://pwbug.dev/380299794 - Verify we properly show available once
  // Write is available again.
}

TEST_F(L2capCocWriteTest, MultipleWritesMultipleChannels) {
  struct {
    int sends_called = 0;
    std::array<uint8_t, 3> payload = {0xAB, 0xCD, 0xEF};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        ++capture.sends_called;
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto acl,
            MakeEmbossView<emboss::AclDataFrameView>(packet.GetHciSpan()));
        emboss::FirstKFrameView kframe = emboss::MakeFirstKFrameView(
            acl.payload().BackingStorage().data(), acl.SizeInBytes());
        for (size_t i = 0; i < 3; ++i) {
          EXPECT_EQ(kframe.payload()[i].Read(), capture.payload[i]);
        }
      });
  constexpr uint16_t kNumChannels = 5;
  auto* allocator = GetProxyHostAllocator();

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/kNumChannels,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(
      proxy,
      /*num_credits_to_reserve=*/kNumChannels));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 123;
  uint16_t remote_cid = 456;
  Vector<L2capCoc, kNumChannels> channels;
  for (uint16_t i = 0; i < kNumChannels; ++i) {
    channels.emplace_back(
        BuildCoc(proxy,
                 CocParameters{
                     .local_cid = static_cast<uint16_t>(local_cid + i),
                     .remote_cid = static_cast<uint16_t>(remote_cid + i),
                 }));
  }
  for (size_t i = 0; i < kNumChannels; ++i) {
    auto mbuf_result = multibuf::FromSpan(
        *allocator, as_writable_bytes(span(capture.payload)), [](ByteSpan) {});
    ASSERT_TRUE(mbuf_result.has_value());
    multibuf::MultiBuf mbuf = std::move(*mbuf_result);
    PW_TEST_EXPECT_OK(channels[i].Write(std::move(mbuf)).status);
    RunDispatcher();

    std::for_each(capture.payload.begin(),
                  capture.payload.end(),
                  [](uint8_t& byte) { ++byte; });
  }

  EXPECT_EQ(capture.sends_called, kNumChannels);
}

// TODO: https://pwbug.dev/365161669 - Disable test at build-level once
// joinability is a build-system constraint.
#if PW_THREAD_JOINING_ENABLED
// Have multiple threads write to a L2capCoc channel. Verify all resulting ACL
// packets are sent towards controller in the correct order per channel.
// This test be run repetitively with googletest by using:
// clang-format off
// bazelisk --config=googletest //pw_bluetooth_proxy:pw_bluetooth_proxy_test --
//   --gtest_filter=L2capCocWriteTest.MultithreadedWrite --gtest_repeat=1000
// clang-format on
// TODO: https://pwbug.dev/441841355 - Update test to send larger packets and
// verify segment order is correct.
TEST_F(L2capCocWriteTest, MultithreadedWrite) {
  constexpr unsigned int kNumThreads = 40;
  constexpr unsigned int kPacketsPerThread = L2capChannel::QueueCapacity() - 1;
  constexpr uint16_t kBaseLocalCid = 0xb000;   // 176
  constexpr uint16_t kBaseRemoteCid = 0xc000;  // 192
  constexpr uint16_t kPayloadSize = 10;

  struct {
    const uint16_t kExpectedSduLength = kPayloadSize;
    const uint16_t kExpectedPduLength =
        kSduLengthFieldSize + kExpectedSduLength;
    const uint16_t kExpectedAclDataTotalLength =
        emboss::BasicL2capHeader::IntrinsicSizeInBytes() + kExpectedPduLength;
    sync::Mutex sends_by_channel_mutex;
    std::array<unsigned int, kNumThreads> sends_by_channel
        PW_GUARDED_BY(sends_by_channel_mutex){};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        EXPECT_EQ(packet.GetH4Type(), emboss::H4PacketType::ACL_DATA);
        EXPECT_EQ(packet.GetHciSpan().size(),
                  static_cast<unsigned long>(
                      emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                      capture.kExpectedAclDataTotalLength));

        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto acl,
            MakeEmbossView<emboss::AclDataFrameView>(packet.GetHciSpan()));
        EXPECT_EQ(acl.header().handle().Read(), kConnectionHandle);
        EXPECT_EQ(acl.header().packet_boundary_flag().Read(),
                  emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
        EXPECT_EQ(acl.header().broadcast_flag().Read(),
                  emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
        EXPECT_EQ(acl.data_total_length().Read(),
                  capture.kExpectedAclDataTotalLength);

        PW_TEST_ASSERT_OK_AND_ASSIGN(
            emboss::FirstKFrameView kframe,
            MakeEmbossView<emboss::FirstKFrameView>(
                acl.payload().BackingStorage().data(), acl.SizeInBytes()));

        EXPECT_EQ(kframe.pdu_length().Read(), capture.kExpectedPduLength);
        EXPECT_EQ(kframe.sdu_length().Read(), capture.kExpectedSduLength);

        // Each channel's remote cid has the thread index as its LSB.
        uint16_t current_remote_cid = kframe.channel_id().Read();
        unsigned int current_thread_id = current_remote_cid & ~kBaseRemoteCid;
        {
          std::lock_guard lock(capture.sends_by_channel_mutex);

          // Each payload byte should match the send count (to verify ordering).
          for (size_t i = 0; i < kPayloadSize; ++i) {
            EXPECT_EQ(kframe.payload()[i].Read(),
                      capture.sends_by_channel[current_thread_id]);
          }
          capture.sends_by_channel[current_thread_id]++;
        }
      });

  allocator::test::AllocatorForTest<65536> allocator;
  // Use libc allocators so msan can detect use after frees.
  std::array<std::byte, 200 * 1024> packet_buffer{};
  pw::multibuf::SimpleAllocator multibuf_allocator{
      /*data_area=*/packet_buffer,
      /*metadata_alloc=*/allocator::GetLibCAllocator()};

  ProxyHost proxy =
      ProxyHost(std::move(send_to_host_fn),
                std::move(send_to_controller_fn),
                /*le_acl_credits_to_reserve=*/kNumThreads * kPacketsPerThread,
                /*br_edr_acl_credits_to_reserve=*/0,
                &allocator);

  // If using async, start a dispatcher thread.
  StartDispatcherOnNewThread(proxy);

  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(
      proxy, kNumThreads * kPacketsPerThread));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  RunDispatcher();

  struct ThreadCapture {
    L2capCoc channel;
    multibuf::MultiBufAllocator& packet_allocator;
  };

  pw::Vector<ThreadCapture, kNumThreads> captures;
  pw::thread::test::TestThreadContext context;
  pw::Vector<pw::Thread, kNumThreads> threads;

  for (unsigned int i = 0; i < kNumThreads; ++i) {
    uint16_t local_cid = static_cast<uint16_t>(kBaseLocalCid + i);
    // Each channel's remote cid has the thread index its LSB. That is
    // used to track packet ordering per channel.
    uint16_t remote_cid = static_cast<uint16_t>(kBaseRemoteCid + i);
    // TODO: https://pwbug.dev/422222575 -  Move channel creation, close, and
    // destruction inside each thread once we have proper channel lifecycle
    // locking.
    ThreadCapture thread_capture{
        BuildCoc(proxy,
                 CocParameters{.handle = kConnectionHandle,
                               .local_cid = local_cid,
                               .remote_cid = remote_cid,
                               .tx_credits = kPacketsPerThread}),
        multibuf_allocator,
    };
    captures.emplace_back(std::move(thread_capture));
  }

  for (unsigned int thread_numb = 0; thread_numb < kNumThreads; ++thread_numb) {
    ThreadCapture& thread_capture = captures[thread_numb];
    threads.emplace_back(context.options(), [&thread_capture]() {
      for (unsigned int packet_numb = 0; packet_numb < kPacketsPerThread;
           ++packet_numb) {
        auto mbuf_result =
            thread_capture.packet_allocator.AllocateContiguous(kPayloadSize);
        ASSERT_TRUE(mbuf_result.has_value());
        multibuf::MultiBuf mbuf = std::move(*mbuf_result);
        std::fill(
            mbuf.begin(), mbuf.end(), static_cast<std::byte>(packet_numb));
        PW_TEST_EXPECT_OK(thread_capture.channel.Write(std::move(mbuf)).status);
      }
    });
  }

  // Ensure the writer threads complete, drop the channels, and ensure the
  // dispatcher thread completes.
  for (auto& t : threads) {
    t.join();
  }
  captures.clear();
  JoinDispatcherThread();

  {
    std::lock_guard lock(capture.sends_by_channel_mutex);
    for (unsigned int i = 0; i < kNumThreads; ++i) {
      EXPECT_EQ(capture.sends_by_channel[i], kPacketsPerThread);
    }
  }
}
#endif  // PW_THREAD_JOINING_ENABLED

// ########## L2capCocReadTest

class L2capCocReadTest : public ProxyHostTest {};

TEST_F(L2capCocReadTest, BasicRead) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  struct {
    int receives_called = 0;
    std::array<uint8_t, 3> expected_payload = {0xAB, 0xCD, 0xEF};
  } capture;

  uint16_t local_cid = 234;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .local_cid = local_cid,
                    .receive_fn = [&capture](multibuf::MultiBuf&& payload) {
                      ++capture.receives_called;
                      ASSERT_FALSE(payload.empty());
                      ConstByteSpan rx_sdu = *payload.ConstChunks().begin();
                      ConstByteSpan expected_sdu =
                          as_bytes(span(capture.expected_payload.data(),
                                        capture.expected_payload.size()));
                      EXPECT_TRUE(std::equal(rx_sdu.begin(),
                                             rx_sdu.end(),
                                             expected_sdu.begin(),
                                             expected_sdu.end()));
                    }});

  std::array<uint8_t,
             kFirstKFrameOverAclMinSize + capture.expected_payload.size()>
      hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes() +
                                 capture.expected_payload.size());

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  kframe.pdu_length().Write(kSduLengthFieldSize +
                            capture.expected_payload.size());
  kframe.channel_id().Write(local_cid);
  kframe.sdu_length().Write(capture.expected_payload.size());
  std::copy(capture.expected_payload.begin(),
            capture.expected_payload.end(),
            hci_arr.begin() + kFirstKFrameOverAclMinSize);

  // Send ACL data packet destined for the CoC we registered.
  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(capture.receives_called, 1);
}

TEST_F(L2capCocReadTest, RxCreditsAreReplenished) {
  const uint16_t kRxCredits = 10;
  // Corresponds to kRxCreditReplenishThreshold in l2cap_coc.cc times
  // kRxCredits.
  // TODO: b/353734827 - Update test once client can to determine this constant.
  const uint16_t kRxThreshold = 3;
  struct {
    uint16_t local_cid = 234;
    uint16_t tx_packets_sent = 0;
    // We expect when we reach threshold to replenish exactly that amount.
    uint16_t expected_additional_credits = kRxThreshold;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture]([[maybe_unused]] H4PacketWithH4&& packet) {
        capture.tx_packets_sent++;

        // Verify packet is properly formed FLOW_CONTROL_CREDIT_IND with the
        // expected credits.
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto acl,
            MakeEmbossView<emboss::AclDataFrameView>(packet.GetHciSpan()));
        EXPECT_EQ(acl.header().handle().Read(), kConnectionHandle);
        EXPECT_EQ(
            acl.data_total_length().Read(),
            emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
                emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes());
        emboss::CFrameView cframe = emboss::MakeCFrameView(
            acl.payload().BackingStorage().data(), acl.payload().SizeInBytes());
        EXPECT_EQ(cframe.pdu_length().Read(),
                  emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes());
        // 0x0005 = LE-U fixed signaling channel ID.
        EXPECT_EQ(cframe.channel_id().Read(), 0x0005);
        emboss::L2capFlowControlCreditIndView ind =
            emboss::MakeL2capFlowControlCreditIndView(
                cframe.payload().BackingStorage().data(),
                cframe.payload().SizeInBytes());
        EXPECT_EQ(ind.command_header().code().Read(),
                  emboss::L2capSignalingPacketCode::FLOW_CONTROL_CREDIT_IND);
        EXPECT_EQ(
            ind.command_header().data_length().Read(),
            emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes() -
                emboss::L2capSignalingCommandHeader::IntrinsicSizeInBytes());
        EXPECT_EQ(ind.cid().Read(), capture.local_cid);
        EXPECT_EQ(ind.credits().Read(), capture.expected_additional_credits);
      });
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/10,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 12));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  L2capCoc channel = BuildCoc(proxy,
                              CocParameters{.handle = kConnectionHandle,
                                            .local_cid = capture.local_cid,
                                            .rx_credits = kRxCredits});

  auto SendRxH4Packet = [this, &capture, &proxy]() {
    std::array<uint8_t, 3> expected_payload = {0xAB, 0xCD, 0xEF};
    std::array<uint8_t, kFirstKFrameOverAclMinSize + expected_payload.size()>
        hci_arr;
    hci_arr.fill(0);
    H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

    Result<emboss::AclDataFrameWriter> acl =
        MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
    acl->header().handle().Write(kConnectionHandle);
    acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes() +
                                   expected_payload.size());

    emboss::FirstKFrameWriter kframe =
        emboss::MakeFirstKFrameView(acl->payload().BackingStorage().data(),
                                    acl->data_total_length().Read());
    kframe.pdu_length().Write(kSduLengthFieldSize + expected_payload.size());
    kframe.channel_id().Write(capture.local_cid);
    kframe.sdu_length().Write(expected_payload.size());
    std::copy(expected_payload.begin(),
              expected_payload.end(),
              hci_arr.begin() + kFirstKFrameOverAclMinSize);

    proxy.HandleH4HciFromController(std::move(h4_packet));
    RunDispatcher();
  };

  // Rx packets before threshold should not trigger a credit packet.
  EXPECT_EQ(0, capture.tx_packets_sent);
  for (int i = 0; i < kRxThreshold - 1; i++) {
    // Send ACL data packet destined for the CoC we registered.
    SendRxH4Packet();
    EXPECT_EQ(0, capture.tx_packets_sent);
  }

  // RX packet at threshold should trigger exactly one credit packet with
  // threshold credits.
  SendRxH4Packet();
  EXPECT_EQ(1, capture.tx_packets_sent);

  // Send just up to threshold again.
  for (int i = 0; i < kRxThreshold - 1; i++) {
    SendRxH4Packet();
    EXPECT_EQ(1, capture.tx_packets_sent);
  }

  // RX packet at threshold should once again trigger exactly one credit packet
  // with threshold credits.
  SendRxH4Packet();
  EXPECT_EQ(2, capture.tx_packets_sent);
}

TEST_F(L2capCocReadTest, ChannelHandlesReadWithNullReceiveFn) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {
        EXPECT_EQ(packet.GetH4Type(), emboss::H4PacketType::EVENT);
      });
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 234;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .local_cid = local_cid,
                    .rx_credits = 1,
                    .receive_fn = nullptr,
                    .event_fn = []([[maybe_unused]] L2capChannelEvent event) {
                      FAIL();
                    }});

  std::array<uint8_t, kFirstKFrameOverAclMinSize> hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes());

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->payload().SizeInBytes());
  kframe.pdu_length().Write(kSduLengthFieldSize);
  kframe.channel_id().Write(local_cid);
  kframe.sdu_length().Write(0);

  proxy.HandleH4HciFromController(std::move(h4_packet));
}

TEST_F(L2capCocReadTest, ErrorOnRxToStoppedChannel) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve*/ 0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  int events_received = 0;
  uint16_t num_invalid_rx = 3;
  uint16_t local_cid = 234;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .local_cid = local_cid,
                    .rx_credits = num_invalid_rx,
                    .receive_fn = [](multibuf::MultiBuf&&) { FAIL(); },
                    .event_fn =
                        [&events_received](L2capChannelEvent event) {
                          ++events_received;
                          EXPECT_EQ(event, L2capChannelEvent::kRxWhileStopped);
                        }});

  std::array<uint8_t, kFirstKFrameOverAclMinSize> hci_arr;
  hci_arr.fill(0);

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes());

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->payload().SizeInBytes());
  kframe.pdu_length().Write(kSduLengthFieldSize);
  kframe.channel_id().Write(local_cid);
  kframe.sdu_length().Write(0);

  channel.Stop();
  for (int i = 0; i < num_invalid_rx; ++i) {
    H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};
    proxy.HandleH4HciFromController(std::move(h4_packet));
  }
  EXPECT_EQ(events_received, num_invalid_rx);
}

TEST_F(L2capCocReadTest, TooShortAclPassedToHost) {
  int sends_called = 0;
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&sends_called]([[maybe_unused]] H4PacketWithHci&& packet) {
        if (packet.GetH4Type() == emboss::H4PacketType::ACL_DATA) {
          ++sends_called;
        }
      });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 234;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .local_cid = local_cid,
                    .receive_fn = [](multibuf::MultiBuf&&) { FAIL(); }});

  std::array<uint8_t, kFirstKFrameOverAclMinSize> hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  // Write size larger than buffer size.
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes() + 5);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(sends_called, 1);
}

TEST_F(L2capCocReadTest, ChannelClosedWithErrorIfMtuExceeded) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 234;
  constexpr uint16_t kRxMtu = 5;
  int events_received = 0;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .local_cid = local_cid,
                    .rx_mtu = kRxMtu,
                    .receive_fn = [](multibuf::MultiBuf&&) { FAIL(); },
                    .event_fn =
                        [&events_received](L2capChannelEvent event) {
                          ++events_received;
                          EXPECT_EQ(event, L2capChannelEvent::kRxInvalid);
                        }});

  constexpr uint16_t kPayloadSize = kRxMtu + 1;
  std::array<uint8_t, kFirstKFrameOverAclMinSize + kPayloadSize> hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes() +
                                 kPayloadSize);

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  kframe.pdu_length().Write(kSduLengthFieldSize + kPayloadSize);
  kframe.channel_id().Write(local_cid);
  kframe.sdu_length().Write(kPayloadSize);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(events_received, 1);
}

TEST_F(L2capCocReadTest, ChannelClosedWithErrorIfMpsExceeded) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 234;
  constexpr uint16_t kRxMps = 5;
  int events_received = 0;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .local_cid = local_cid,
                    .rx_mps = kRxMps,
                    .receive_fn = [](multibuf::MultiBuf&&) { FAIL(); },
                    .event_fn =
                        [&events_received](L2capChannelEvent event) {
                          ++events_received;
                          EXPECT_EQ(event, L2capChannelEvent::kRxInvalid);
                        }});

  constexpr uint16_t kPayloadSize = kRxMps + 1;
  std::array<uint8_t, kFirstKFrameOverAclMinSize + kPayloadSize> hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes() +
                                 kPayloadSize);

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  kframe.pdu_length().Write(kSduLengthFieldSize + kPayloadSize);
  kframe.channel_id().Write(local_cid);
  kframe.sdu_length().Write(kPayloadSize);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(events_received, 1);
}

TEST_F(L2capCocReadTest, ChannelClosedWithErrorIfPayloadsExceedSduLength) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 234;
  int events_received = 0;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .local_cid = local_cid,
                    .receive_fn = [](multibuf::MultiBuf&&) { FAIL(); },
                    .event_fn =
                        [&events_received](L2capChannelEvent event) {
                          ++events_received;
                          EXPECT_EQ(event, L2capChannelEvent::kRxInvalid);
                        }});

  constexpr uint16_t k1stPayloadSize = 1;
  constexpr uint16_t k2ndPayloadSize = 3;
  ASSERT_GT(k2ndPayloadSize, k1stPayloadSize + 1);
  // Indicate SDU length that does not account for the 2nd payload size.
  constexpr uint16_t kSduLength = k1stPayloadSize + 1;

  std::array<uint8_t,
             kFirstKFrameOverAclMinSize +
                 std::max(k1stPayloadSize, k2ndPayloadSize)>
      hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_1st_segment{
      emboss::H4PacketType::ACL_DATA,
      pw::span<uint8_t>(hci_arr.data(),
                        kFirstKFrameOverAclMinSize + k1stPayloadSize)};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes() +
                                 k1stPayloadSize);

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  kframe.pdu_length().Write(kSduLengthFieldSize + k1stPayloadSize);
  kframe.channel_id().Write(local_cid);
  kframe.sdu_length().Write(kSduLength);

  proxy.HandleH4HciFromController(std::move(h4_1st_segment));

  // Send 2nd segment.
  acl->data_total_length().Write(emboss::SubsequentKFrame::MinSizeInBytes() +
                                 k2ndPayloadSize);
  kframe.pdu_length().Write(k2ndPayloadSize);
  H4PacketWithHci h4_2nd_segment{
      emboss::H4PacketType::ACL_DATA,
      pw::span<uint8_t>(hci_arr.data(),
                        emboss::AclDataFrame::MinSizeInBytes() +
                            emboss::SubsequentKFrame::MinSizeInBytes() +
                            k2ndPayloadSize)};

  proxy.HandleH4HciFromController(std::move(h4_2nd_segment));

  EXPECT_EQ(events_received, 1);
}

TEST_F(L2capCocReadTest, NoReadOnStoppedChannel) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 234;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .local_cid = local_cid,
                    .receive_fn = [](multibuf::MultiBuf&&) { FAIL(); }});

  std::array<uint8_t, kFirstKFrameOverAclMinSize> hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes());

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  kframe.pdu_length().Write(kSduLengthFieldSize);
  kframe.channel_id().Write(local_cid);

  channel.Stop();
  proxy.HandleH4HciFromController(std::move(h4_packet));
}

TEST_F(L2capCocReadTest, NoReadOnSameCidDifferentConnectionHandle) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 234;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .local_cid = local_cid,
                    .receive_fn = [](multibuf::MultiBuf&&) { FAIL(); }});

  std::array<uint8_t, kFirstKFrameOverAclMinSize> hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(444);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes());

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  kframe.pdu_length().Write(kSduLengthFieldSize);
  kframe.channel_id().Write(local_cid);

  proxy.HandleH4HciFromController(std::move(h4_packet));
}

TEST_F(L2capCocReadTest, MultipleReadsSameChannel) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  struct {
    int sends_called = 0;
    std::array<uint8_t, 3> payload = {0xAB, 0xCD, 0xEF};
  } capture;

  uint16_t local_cid = 234;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .local_cid = local_cid,
                    .receive_fn = [&capture](multibuf::MultiBuf&& payload) {
                      ++capture.sends_called;
                      ASSERT_FALSE(payload.empty());
                      ConstByteSpan rx_sdu = *payload.ConstChunks().begin();
                      ConstByteSpan expected_sdu = as_bytes(
                          span(capture.payload.data(), capture.payload.size()));
                      EXPECT_TRUE(std::equal(rx_sdu.begin(),
                                             rx_sdu.end(),
                                             expected_sdu.begin(),
                                             expected_sdu.end()));
                    }});

  std::array<uint8_t, kFirstKFrameOverAclMinSize + capture.payload.size()>
      hci_arr;
  hci_arr.fill(0);

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes() +
                                 capture.payload.size());

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  kframe.pdu_length().Write(capture.payload.size() + kSduLengthFieldSize);
  kframe.channel_id().Write(local_cid);
  kframe.sdu_length().Write(capture.payload.size());

  int num_reads = 10;
  for (int i = 0; i < num_reads; ++i) {
    std::copy(capture.payload.begin(),
              capture.payload.end(),
              hci_arr.begin() + kFirstKFrameOverAclMinSize);

    H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};
    proxy.HandleH4HciFromController(std::move(h4_packet));

    std::for_each(capture.payload.begin(),
                  capture.payload.end(),
                  [](uint8_t& byte) { ++byte; });
  }

  EXPECT_EQ(capture.sends_called, num_reads);
}

TEST_F(L2capCocReadTest, MultipleReadsMultipleChannels) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  struct {
    int sends_called = 0;
    std::array<uint8_t, 3> payload = {0xAB, 0xCD, 0xEF};
  } capture;

  constexpr int kNumChannels = 5;
  uint16_t local_cid = 123;
  uint16_t remote_cid = 456;
  auto receive_fn = [&capture](multibuf::MultiBuf&& payload) {
    ++capture.sends_called;
    ASSERT_FALSE(payload.empty());
    ConstByteSpan rx_sdu = *payload.ConstChunks().begin();
    ConstByteSpan expected_sdu =
        as_bytes(span(capture.payload.data(), capture.payload.size()));
    EXPECT_TRUE(std::equal(rx_sdu.begin(),
                           rx_sdu.end(),
                           expected_sdu.begin(),
                           expected_sdu.end()));
  };
  Vector<L2capCoc, kNumChannels> channels;
  for (uint16_t i = 0; i < kNumChannels; ++i) {
    channels.emplace_back(BuildCoc(
        proxy,
        CocParameters{.handle = kConnectionHandle,
                      .local_cid = static_cast<uint16_t>(local_cid + i),
                      .remote_cid = static_cast<uint16_t>(remote_cid + i),
                      .receive_fn = receive_fn}));
  }

  std::array<uint8_t, kFirstKFrameOverAclMinSize + capture.payload.size()>
      hci_arr;
  hci_arr.fill(0);

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes() +
                                 capture.payload.size());

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  kframe.pdu_length().Write(capture.payload.size() + kSduLengthFieldSize);
  kframe.sdu_length().Write(capture.payload.size());

  for (int i = 0; i < kNumChannels; ++i) {
    kframe.channel_id().Write(local_cid + i);

    std::copy(capture.payload.begin(),
              capture.payload.end(),
              hci_arr.begin() + kFirstKFrameOverAclMinSize);

    H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};
    proxy.HandleH4HciFromController(std::move(h4_packet));

    std::for_each(capture.payload.begin(),
                  capture.payload.end(),
                  [](uint8_t& byte) { ++byte; });
  }

  EXPECT_EQ(capture.sends_called, kNumChannels);
}

TEST_F(L2capCocReadTest, ChannelStoppageDoNotAffectOtherChannels) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  struct {
    int sends_called = 0;
    std::array<uint8_t, 3> payload = {0xAB, 0xCD, 0xEF};
  } capture;

  constexpr int kNumChannels = 5;
  uint16_t local_cid = 123;
  uint16_t remote_cid = 456;
  auto receive_fn = [&capture](multibuf::MultiBuf&& payload) {
    ++capture.sends_called;
    ASSERT_FALSE(payload.empty());
    ConstByteSpan rx_sdu = *payload.ConstChunks().begin();
    ConstByteSpan expected_sdu =
        as_bytes(span(capture.payload.data(), capture.payload.size()));
    EXPECT_TRUE(std::equal(rx_sdu.begin(),
                           rx_sdu.end(),
                           expected_sdu.begin(),
                           expected_sdu.end()));
  };
  Vector<L2capCoc, kNumChannels> channels;
  for (uint16_t i = 0; i < kNumChannels; ++i) {
    channels.emplace_back(BuildCoc(
        proxy,
        CocParameters{.handle = kConnectionHandle,
                      .local_cid = static_cast<uint16_t>(local_cid + i),
                      .remote_cid = static_cast<uint16_t>(remote_cid + i),
                      .receive_fn = receive_fn}));
  }

  // Stop the 2nd and 4th of the 5 channels.
  channels[1].Stop();
  channels[3].Stop();

  std::array<uint8_t, kFirstKFrameOverAclMinSize + capture.payload.size()>
      hci_arr;
  hci_arr.fill(0);

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes() +
                                 capture.payload.size());

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  kframe.pdu_length().Write(capture.payload.size() + kSduLengthFieldSize);
  kframe.sdu_length().Write(capture.payload.size());

  for (int i = 0; i < kNumChannels; ++i) {
    // Still send packets to the stopped channels, so we can validate that it
    // does not cause issues.
    kframe.channel_id().Write(local_cid + i);

    std::copy(capture.payload.begin(),
              capture.payload.end(),
              hci_arr.begin() + kFirstKFrameOverAclMinSize);

    H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};
    proxy.HandleH4HciFromController(std::move(h4_packet));

    std::for_each(capture.payload.begin(),
                  capture.payload.end(),
                  [](uint8_t& byte) { ++byte; });
  }

  EXPECT_EQ(capture.sends_called, kNumChannels - 2);
}

TEST_F(L2capCocReadTest, NonCocAclPacketPassesThroughToHost) {
  struct {
    int sends_called = 0;
    std::array<uint8_t, 3> expected_payload = {0xAB, 0xCD, 0xEF};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        if (packet.GetH4Type() == emboss::H4PacketType::EVENT) {
          return;
        }
        ++capture.sends_called;
        Result<emboss::AclDataFrameWriter> acl =
            MakeEmbossWriter<emboss::AclDataFrameWriter>(packet.GetHciSpan());
        EXPECT_EQ(acl->header().handle().Read(), kConnectionHandle);
        emboss::BFrameView bframe =
            emboss::MakeBFrameView(acl->payload().BackingStorage().data(),
                                   acl->data_total_length().Read());
        for (size_t i = 0; i < capture.expected_payload.size(); ++i) {
          EXPECT_EQ(bframe.payload()[i].Read(), capture.expected_payload[i]);
        }
      });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  // Acquire unused CoC to validate that doing so does not interfere.
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle,
                    .receive_fn = [](multibuf::MultiBuf&&) { FAIL(); }});

  std::array<uint8_t,
             emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                 emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
                 capture.expected_payload.size()>
      hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
      capture.expected_payload.size());

  emboss::BFrameWriter bframe = emboss::MakeBFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  bframe.pdu_length().Write(capture.expected_payload.size());
  bframe.channel_id().Write(111);
  std::copy(capture.expected_payload.begin(),
            capture.expected_payload.end(),
            hci_arr.begin() +
                emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                emboss::BasicL2capHeader::IntrinsicSizeInBytes());

  // Send ACL packet that should be forwarded to host.
  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(capture.sends_called, 1);
}

TEST_F(L2capCocReadTest, AclFrameWithIncompleteL2capHeaderForwardedToHost) {
  int sends_to_host_called = 0;
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&sends_to_host_called]([[maybe_unused]] H4PacketWithHci&& packet) {
        if (packet.GetH4Type() == emboss::H4PacketType::ACL_DATA) {
          ++sends_to_host_called;
        }
      });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  L2capCoc channel =
      BuildCoc(proxy, CocParameters{.handle = kConnectionHandle});

  std::array<uint8_t, emboss::AclDataFrameHeader::IntrinsicSizeInBytes()>
      hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(0);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(sends_to_host_called, 1);
}

TEST_F(L2capCocReadTest, FragmentedPduDoesNotInterfereWithOtherChannels) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  uint16_t handle_frag = 0x123, handle_fine = 0x234;
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, handle_frag, emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, handle_fine, emboss::StatusCode::SUCCESS));

  uint16_t cid_frag = 0x345, cid_fine = 0x456;
  int packets_received = 0;
  auto receive_fn = [&packets_received](multibuf::MultiBuf&&) {
    ++packets_received;
  };
  L2capCoc frag_channel = BuildCoc(proxy,
                                   CocParameters{
                                       .handle = handle_frag,
                                       .local_cid = cid_frag,
                                       .receive_fn = receive_fn,
                                   });
  L2capCoc fine_channel = BuildCoc(proxy,
                                   CocParameters{
                                       .handle = handle_fine,
                                       .local_cid = cid_fine,
                                       .receive_fn = receive_fn,
                                   });

  // Order of receptions:
  // 1. 1st of 3 fragments to frag_channel.
  // 2. Non-fragmented PDU to fine_channel.
  // 3. 2nd of 3 fragments to frag_channel.
  // 4. Non-fragmented PDU to fine_channel.
  // 5. 3rd of 3 fragments to frag_channel.
  // 6. Non-fragmented PDU to fine_channel.

  constexpr uint8_t kPduLength = 14;
  ASSERT_GT(kPduLength, kSduLengthFieldSize);
  constexpr uint8_t kSduLength = kPduLength - kSduLengthFieldSize;

  // 1. 1st of 3 fragments to frag_channel.
  std::array<uint8_t, emboss::AclDataFrame::MinSizeInBytes() + kSduLength>
      frag_hci_arr;
  frag_hci_arr.fill(0);
  H4PacketWithHci h4_1st_fragment{
      emboss::H4PacketType::ACL_DATA,
      pw::span(frag_hci_arr.data(), kFirstKFrameOverAclMinSize)};

  Result<emboss::AclDataFrameWriter> acl_frag =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(frag_hci_arr);
  acl_frag->header().handle().Write(handle_frag);
  acl_frag->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes());

  emboss::FirstKFrameWriter kframe_frag =
      emboss::MakeFirstKFrameView(acl_frag->payload().BackingStorage().data(),
                                  acl_frag->data_total_length().Read());
  kframe_frag.pdu_length().Write(kPduLength);
  kframe_frag.channel_id().Write(cid_frag);
  kframe_frag.sdu_length().Write(kSduLength);

  proxy.HandleH4HciFromController(std::move(h4_1st_fragment));

  // 2. Non-fragmented PDU to fine_channel.
  std::array<uint8_t, kFirstKFrameOverAclMinSize> fine_hci_arr;
  fine_hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA,
                            pw::span(fine_hci_arr)};

  Result<emboss::AclDataFrameWriter> acl_fine =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(fine_hci_arr);
  acl_fine->header().handle().Write(handle_fine);
  acl_fine->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes());

  emboss::FirstKFrameWriter kframe_fine =
      emboss::MakeFirstKFrameView(acl_fine->payload().BackingStorage().data(),
                                  acl_fine->data_total_length().Read());
  kframe_fine.pdu_length().Write(kSduLengthFieldSize);
  kframe_fine.channel_id().Write(cid_fine);
  kframe_fine.sdu_length().Write(0);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  // 3. 2nd of 3 fragments to frag_channel.
  acl_frag->header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  acl_frag->header().broadcast_flag().Write(
      emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
  acl_frag->data_total_length().Write(kSduLength / 2);
  H4PacketWithHci h4_2nd_fragment{
      emboss::H4PacketType::ACL_DATA,
      pw::span<uint8_t>(
          frag_hci_arr.data(),
          emboss::AclDataFrame::MinSizeInBytes() + kSduLength / 2)};
  proxy.HandleH4HciFromController(std::move(h4_2nd_fragment));

  // 4. Non-fragmented PDU to fine_channel.
  H4PacketWithHci h4_packet_2{emboss::H4PacketType::ACL_DATA,
                              pw::span(fine_hci_arr)};
  proxy.HandleH4HciFromController(std::move(h4_packet_2));

  // 5. 3rd of 3 fragments to frag_channel.
  if (kSduLength % 2 == 1) {
    acl_frag->data_total_length().Write(kSduLength / 2 + 1);
  }
  H4PacketWithHci h4_3rd_fragment{
      emboss::H4PacketType::ACL_DATA,
      pw::span<uint8_t>(frag_hci_arr.data(),
                        emboss::AclDataFrame::MinSizeInBytes() +
                            kSduLength / 2 + (kSduLength % 2))};
  proxy.HandleH4HciFromController(std::move(h4_3rd_fragment));

  // 6. Non-fragmented PDU to fine_channel.
  H4PacketWithHci h4_packet_3{emboss::H4PacketType::ACL_DATA,
                              pw::span(fine_hci_arr)};
  proxy.HandleH4HciFromController(std::move(h4_packet_3));

  // 3 non-fragmented PDUs plus 1 recombined PDU
  EXPECT_EQ(packets_received, 3 + 1);
}

// ########## L2capCocQueueTest

class L2capCocQueueTest : public ProxyHostTest {};

TEST_F(L2capCocQueueTest, ReadBufferResponseDrainsQueue) {
  const uint16_t kNumSends = L2capCoc::QueueCapacity() - NumBufferedPayloads();
  size_t sends_called = 0;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&sends_called]([[maybe_unused]] H4PacketWithH4&& packet) {
        ++sends_called;
      });
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/kNumSends,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle, .tx_credits = kNumSends});

  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
  for (size_t i = 0; i < kNumSends; ++i) {
    multibuf::MultiBuf empty = MakeEmptyMultiBuf();
    PW_TEST_EXPECT_OK(channel.Write(std::move(empty)).status);
    RunDispatcher();
  }
  multibuf::MultiBuf empty = MakeEmptyMultiBuf();
  EXPECT_EQ(channel.Write(std::move(empty)).status, PW_STATUS_UNAVAILABLE);
  RunDispatcher();
  EXPECT_EQ(sends_called, 0u);

  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, kNumSends));

  EXPECT_EQ(sends_called, kNumSends);
}

TEST_F(L2capCocQueueTest, NocpEventDrainsQueue) {
  const uint16_t kNumSends = L2capCoc::QueueCapacity() - NumBufferedPayloads();
  size_t sends_called = 0;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&sends_called]([[maybe_unused]] H4PacketWithH4&& packet) {
        ++sends_called;
      });
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/kNumSends,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, kNumSends));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  L2capCoc channel =
      BuildCoc(proxy,
               CocParameters{.handle = kConnectionHandle,
                             .tx_credits = uint16_t(kNumSends * 2)});

  for (size_t i = 0; i < kNumSends; ++i) {
    multibuf::MultiBuf empty = MakeEmptyMultiBuf();
    PW_TEST_EXPECT_OK(channel.Write(std::move(empty)).status);
    RunDispatcher();
  }

  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
  for (size_t i = 0; i < kNumSends; ++i) {
    multibuf::MultiBuf empty = MakeEmptyMultiBuf();
    PW_TEST_EXPECT_OK(channel.Write(std::move(empty)).status);
    RunDispatcher();
  }
  multibuf::MultiBuf empty = MakeEmptyMultiBuf();
  EXPECT_EQ(channel.Write(std::move(empty)).status, PW_STATUS_UNAVAILABLE);
  RunDispatcher();
  EXPECT_EQ(sends_called, kNumSends);

  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{kConnectionHandle, kNumSends}}));

  EXPECT_EQ(sends_called, kNumSends * 2u);
}

TEST_F(L2capCocQueueTest, RemovingLrdChannelDoesNotInvalidateRoundRobin) {
  const uint16_t kNumSends = L2capCoc::QueueCapacity() - NumBufferedPayloads();
  size_t sends_called = 0;
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&sends_called]([[maybe_unused]] H4PacketWithH4&& packet) {
        ++sends_called;
      });
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/kNumSends,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, kNumSends));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), kNumSends);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  std::array<uint16_t, 3> local_cids = {101, 102, 103};
  std::array<uint16_t, 3> remote_cids = {104, 105, 106};

  L2capCoc chan_left = BuildCoc(proxy,
                                CocParameters{.handle = kConnectionHandle,
                                              .local_cid = local_cids[0],
                                              .remote_cid = remote_cids[0],
                                              .tx_credits = 1});
  std::optional<L2capCoc> chan_middle =
      BuildCoc(proxy,
               CocParameters{.handle = kConnectionHandle,
                             .local_cid = local_cids[1],
                             .remote_cid = remote_cids[1],
                             .tx_credits = uint16_t(kNumSends + 1)});
  L2capCoc chan_right = BuildCoc(proxy,
                                 CocParameters{.handle = kConnectionHandle,
                                               .local_cid = local_cids[2],
                                               .remote_cid = remote_cids[2],
                                               .tx_credits = 1});

  // We have 3 channels. Make it so LRD channel iterator is on the middle
  // channel, then release that channel and ensure the other two are still
  // reached in the round robin.

  // Queue a packet in middle channel.
  for (size_t i = 0; i < kNumSends; ++i) {
    multibuf::MultiBuf empty = MakeEmptyMultiBuf();
    PW_TEST_EXPECT_OK(chan_middle->Write(std::move(empty)).status);
    RunDispatcher();
  }
  EXPECT_EQ(sends_called, kNumSends);

  // Make middle channel the LRD channel.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{kConnectionHandle, 1}}));
  EXPECT_EQ(sends_called, kNumSends);

  // Queue a packet each in left and right channels.
  multibuf::MultiBuf empty1 = MakeEmptyMultiBuf();
  PW_TEST_EXPECT_OK(chan_left.Write(std::move(empty1)).status);
  multibuf::MultiBuf empty2 = MakeEmptyMultiBuf();
  PW_TEST_EXPECT_OK(chan_right.Write(std::move(empty2)).status);
  RunDispatcher();
  EXPECT_EQ(sends_called, kNumSends + 1u);

  // Drop middle channel. LRD write iterator should still be valid.
  chan_middle.reset();

  // Confirm packets in remaining two channels are sent in round robin.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{kConnectionHandle, 2}}));
  EXPECT_EQ(sends_called, kNumSends + 2u);
}

TEST_F(L2capCocQueueTest, H4BufferReleaseTriggersQueueDrain) {
  constexpr uint8_t kAclLeCredits = 20;
  struct {
    size_t sends_called = 0;
    pw::Vector<H4PacketWithH4, 50> packet_store;
  } capture;
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        ++capture.sends_called;
        capture.packet_store.push_back(std::move(packet));
      });
  allocator::test::AllocatorForTest<4096> allocator;
  ProxyHost proxy(std::move(send_to_host_fn),
                  std::move(send_to_controller_fn),
                  /*le_acl_credits_to_reserve=*/kAclLeCredits,
                  /*br_edr_acl_credits_to_reserve=*/0,
                  &allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kAclLeCredits));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), kAclLeCredits);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  constexpr uint16_t kRemoteCid = 0x456;
  L2capCoc channel = BuildCoc(proxy,
                              CocParameters{
                                  .handle = kConnectionHandle,
                                  .remote_cid = kRemoteCid,
                                  .tx_mtu = 1000,
                                  .tx_mps = 1000,
                                  .tx_credits = kAclLeCredits,
                              });
  EXPECT_EQ(capture.sends_called, 0u);
  // Occupy all buffers. Final Write should queue and not send.
  std::array<uint8_t, 240> payload = {};
  size_t num_writes = 0;
  do {
    auto mbuf_result = multibuf::FromSpan(
        allocator, as_writable_bytes(span(payload)), [](ByteSpan) {});
    ASSERT_TRUE(mbuf_result.has_value());
    multibuf::MultiBuf mbuf = std::move(*mbuf_result);
    PW_TEST_EXPECT_OK(channel.IsWriteAvailable());
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    ++num_writes;
  } while (capture.sends_called == num_writes);
  // The final write should be queued and not sent.
  EXPECT_EQ(capture.sends_called, num_writes - 1);
  // Sending should have stopped because the allocator is full, not because ACL
  // credits ran out.
  EXPECT_LT(capture.sends_called, kAclLeCredits);

  // Destroying an H4 packet will send the next packet in the destructor, so we
  // need to first extract the packet from the vector to avoid appending to
  // the vector during pop_back.
  ASSERT_FALSE(capture.packet_store.empty());
  std::optional<H4PacketWithH4> packet = std::move(capture.packet_store.back());
  capture.packet_store.pop_back();
  // Release a buffer. Queued packet should then send.
  packet.reset();
  RunDispatcher();
  EXPECT_EQ(capture.sends_called, num_writes);

  // Free all buffers before the allocator is destroyed.
  capture.packet_store.clear();
}

TEST_F(L2capCocQueueTest, RoundRobinHandlesMultiplePasses) {
  const uint16_t kNumSends = L2capCoc::QueueCapacity() - NumBufferedPayloads();
  struct {
    size_t sends_called = 0;
    Vector<H4PacketWithH4, kNumSends> packet_store;
  } capture;
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        ++capture.sends_called;
        // We prevent packets from being released in this test because each
        // packet release triggers a round robin.
        capture.packet_store.push_back(std::move(packet));
      });
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/kNumSends,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle, .tx_credits = kNumSends});

  // Occupy all queue slots.
  for (size_t i = 0; i < kNumSends; ++i) {
    multibuf::MultiBuf empty = MakeEmptyMultiBuf();
    PW_TEST_EXPECT_OK(channel.Write(std::move(empty)).status);
    RunDispatcher();
  }
  EXPECT_EQ(capture.sends_called, 0ul);

  // This will provide enough credits for all queued packets. So they all should
  // be drained and sent.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, kNumSends));
  EXPECT_EQ(capture.sends_called, kNumSends);
  capture.packet_store.clear();
}

// ########## L2capCocReassemblyTest

class L2capCocReassemblyTest : public ProxyHostTest {};

TEST_F(L2capCocReassemblyTest, OneSegmentRx) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 0x234;
  struct {
    int sdus_received = 0;
    std::array<uint8_t, 10> expected_payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  } capture;
  L2capCoc channel = BuildCoc(
      proxy,
      {.handle = kConnectionHandle,
       .local_cid = local_cid,
       .receive_fn = [&capture](multibuf::MultiBuf&& payload) {
         ++capture.sdus_received;
         ASSERT_FALSE(payload.empty());
         ConstByteSpan rx_sdu = *payload.ConstChunks().begin();
         ConstByteSpan expected_sdu = as_bytes(span(
             capture.expected_payload.data(), capture.expected_payload.size()));
         EXPECT_TRUE(std::equal(rx_sdu.begin(),
                                rx_sdu.end(),
                                expected_sdu.begin(),
                                expected_sdu.end()));
       }});

  Result<BFrameWithStorage> bframe =
      SetupBFrame(kConnectionHandle,
                  local_cid,
                  capture.expected_payload.size() + kSduLengthFieldSize);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA,
                            bframe->acl.hci_span()};

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      bframe->acl.writer.payload().BackingStorage().data(),
      bframe->acl.writer.payload().SizeInBytes());
  kframe.sdu_length().Write(capture.expected_payload.size());
  PW_CHECK(TryToCopyToEmbossStruct(/*emboss_dest=*/kframe.payload(),
                                   /*src=*/capture.expected_payload));

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(capture.sdus_received, 1);
}

TEST_F(L2capCocReassemblyTest, SduReceivedWhenSegmentedOverFullRangeOfMps) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 0x234;
  struct {
    uint16_t sdus_received = 0;
    std::array<uint8_t, 19> expected_payload = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
  } capture;
  L2capCoc channel = BuildCoc(
      proxy,
      {.handle = kConnectionHandle,
       .local_cid = local_cid,
       .receive_fn = [&capture](multibuf::MultiBuf&& payload) {
         ++capture.sdus_received;
         ASSERT_FALSE(payload.empty());
         ConstByteSpan rx_sdu = *payload.ConstChunks().begin();
         ConstByteSpan expected_sdu = as_bytes(span(
             capture.expected_payload.data(), capture.expected_payload.size()));
         EXPECT_TRUE(std::equal(rx_sdu.begin(),
                                rx_sdu.end(),
                                expected_sdu.begin(),
                                expected_sdu.end()));
       }});

  uint16_t sdus_sent = 0;
  // Test sending payload segmented in every possible way, from MPS of 2 octets
  // to MPS values 5 octets greater than the payload size.
  for (uint16_t mps = 2; mps < capture.expected_payload.size() + 5; ++mps) {
    for (size_t segment_no = 0;; ++segment_no) {
      Result<KFrameWithStorage> kframe =
          SetupKFrame(kConnectionHandle,
                      local_cid,
                      mps,
                      static_cast<uint16_t>(segment_no),
                      span(capture.expected_payload));
      if (!kframe.ok()) {
        break;
      }
      H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA,
                                kframe->acl.hci_span()};
      proxy.HandleH4HciFromController(std::move(h4_packet));
    }
    ++sdus_sent;
  }

  EXPECT_EQ(capture.sdus_received, sdus_sent);
}

TEST_F(L2capCocReassemblyTest, ErrorIfPayloadBytesExceedSduLength) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 0x234;
  int events_received = 0;
  L2capCoc channel =
      BuildCoc(proxy,
               {
                   .handle = kConnectionHandle,
                   .local_cid = local_cid,
                   .receive_fn = [](multibuf::MultiBuf&&) { FAIL(); },
                   .event_fn =
                       [&events_received](L2capChannelEvent event) {
                         ++events_received;
                         EXPECT_EQ(event, L2capChannelEvent::kRxInvalid);
                       },
               });

  constexpr uint16_t kIndicatedSduLength = 5;
  // First PDU will be 2 bytes for SDU length field + 2 payload bytes
  // Second PDU will have 4 payload bytes, which will exceed SDU length by 1.
  constexpr uint16_t kFirstPayloadLength = 2;

  std::array<uint8_t, kFirstKFrameOverAclMinSize + kFirstPayloadLength> hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci first_h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes() +
                                 kFirstPayloadLength);

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  kframe.pdu_length().Write(kSduLengthFieldSize + kFirstPayloadLength);
  kframe.channel_id().Write(local_cid);
  kframe.sdu_length().Write(kIndicatedSduLength);

  proxy.HandleH4HciFromController(std::move(first_h4_packet));

  H4PacketWithHci second_h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};
  proxy.HandleH4HciFromController(std::move(second_h4_packet));

  EXPECT_EQ(events_received, 1);
}

TEST_F(L2capCocReassemblyTest, ErrorIfRxBufferTooSmallForFirstKFrame) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 0x234;
  int events_received = 0;
  L2capCoc channel =
      BuildCoc(proxy,
               {
                   .handle = kConnectionHandle,
                   .local_cid = local_cid,
                   .receive_fn = [](multibuf::MultiBuf&&) { FAIL(); },
                   .event_fn =
                       [&events_received](L2capChannelEvent event) {
                         ++events_received;
                         EXPECT_EQ(event, L2capChannelEvent::kRxInvalid);
                       },
               });

  std::array<uint8_t, kFirstKFrameOverAclMinSize - 1> hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(emboss::FirstKFrame::MinSizeInBytes() - 1);

  emboss::FirstKFrameWriter kframe = emboss::MakeFirstKFrameView(
      acl->payload().BackingStorage().data(), acl->data_total_length().Read());
  EXPECT_TRUE(!kframe.IsComplete());
  kframe.pdu_length().Write(1);
  kframe.channel_id().Write(local_cid);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(events_received, 1);
}

// ########## L2capCocSegmentation

class L2capCocSegmentationTest : public ProxyHostTest {};

TEST_F(L2capCocSegmentationTest, SduSentWhenSegmentedOverFullRangeOfMps) {
  constexpr size_t kPayloadSize = 312;
  struct {
    uint16_t remote_cid = 0x456;
    uint16_t sdus_received = 0;
    uint16_t mps;
    std::array<uint8_t, kPayloadSize> expected_payload;
  } capture;

  for (size_t i = 0; i < capture.expected_payload.size(); ++i) {
    capture.expected_payload[i] = i % UINT8_MAX;
  }

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&& tx_kframe) {
        static uint16_t segment_no = 0;
        static uint16_t pdu_bytes_received = 0;
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            KFrameWithStorage expected_kframe,
            SetupKFrame(kConnectionHandle,
                        capture.remote_cid,
                        capture.mps,
                        segment_no,
                        span(capture.expected_payload)));

        EXPECT_TRUE(std::equal(tx_kframe.GetHciSpan().begin(),
                               tx_kframe.GetHciSpan().end(),
                               expected_kframe.acl.hci_span().begin(),
                               expected_kframe.acl.hci_span().end()));

        pdu_bytes_received +=
            expected_kframe.acl.writer.data_total_length().Read() -
            emboss::BasicL2capHeader::IntrinsicSizeInBytes();

        if (pdu_bytes_received ==
            capture.expected_payload.size() + kSduLengthFieldSize) {
          ++capture.sdus_received;
          segment_no = 0;
          pdu_bytes_received = 0;
        } else {
          ++segment_no;
        }
      });
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  auto* allocator = GetProxyHostAllocator();
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/20,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(
      proxy,
      /*num_credits_to_reserve=*/20,

      /*le_acl_data_packet_length=*/UINT16_MAX));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t sdus_sent = 0;

  // Test sending payload segmented in every possible way, from MPS of 23 octets
  // to MPS values 5 octets greater than the payload size. 23 bytes is the
  // minimum MPS supported for L2CAP channels.
  for (capture.mps = 23; capture.mps < kPayloadSize + 5; ++capture.mps) {
    L2capCoc channel = BuildCoc(proxy,
                                {.handle = kConnectionHandle,
                                 .remote_cid = capture.remote_cid,
                                 .tx_mtu = capture.expected_payload.size(),
                                 .tx_mps = capture.mps,
                                 .tx_credits = 20});
    auto mbuf_result =
        multibuf::FromSpan(*allocator,
                           as_writable_bytes(span(capture.expected_payload)),
                           [](ByteSpan) {});
    ASSERT_TRUE(mbuf_result.has_value());
    multibuf::MultiBuf mbuf = std::move(*mbuf_result);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    ++sdus_sent;

    // Replenish proxy's LE ACL send credits, or else only UINT8_MAX PDUs could
    // be sent in this test.
    PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(
        proxy,
        {{kConnectionHandle,
          static_cast<uint8_t>(20 - proxy.GetNumFreeLeAclPackets())}}));
  }

  EXPECT_EQ(capture.sdus_received, sdus_sent);
}

}  // namespace
}  // namespace pw::bluetooth::proxy
