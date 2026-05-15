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

#include <sys/types.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>

#include "pw_allocator/libc_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/check.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/hci_commands.emb.h"
#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/hci_events.emb.h"
#include "pw_bluetooth/hci_h4.emb.h"
#include "pw_bluetooth/l2cap_frames.emb.h"
#include "pw_bluetooth_proxy/direction.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/l2cap_status_delegate.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_bluetooth_proxy_private/test_utils.h"
#include "pw_containers/flat_map.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"
#include "pw_log/log.h"
#include "pw_multibuf/from_span.h"
#include "pw_multibuf/multibuf.h"
#include "pw_span/cast.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_sync/binary_semaphore.h"
#include "pw_sync/mutex.h"
#include "pw_thread/sleep.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"
#include "pw_unit_test/status_macros.h"

namespace pw::bluetooth::proxy {
namespace {

class DynamicCreditSharingTest : public ProxyHostTest {
 public:
  using ProxyHostTest::SendNumberOfCompletedPackets;
};

TEST_F(DynamicCreditSharingTest, DynamicQueueingAndDrainingLE) {
  struct {
    int sends_called = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&&) { capture.sends_called++; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  // Set total credits to 2.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 2));

  constexpr uint16_t connection_handle = 0x123;
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, connection_handle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = connection_handle});

  std::array<uint8_t, 1> attribute_value = {7};

  // Send first packet.
  {
    multibuf::MultiBuf mbuf = MultiBufFromArray(attribute_value);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, 1);
  }

  // Send second packet.
  {
    multibuf::MultiBuf mbuf = MultiBufFromArray(attribute_value);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, 2);
  }

  // Send third packet (should be queued).
  {
    multibuf::MultiBuf mbuf = MultiBufFromArray(attribute_value);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    // Verify it was NOT sent to controller yet.
    EXPECT_EQ(capture.sends_called, 2);
  }

  // Reclaim 1 credit.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{connection_handle, 1}}));

  // The queued packet should be sent out.
  EXPECT_EQ(capture.sends_called, 3);
}

TEST_F(DynamicCreditSharingTest, DisconnectClearsQueuedPackets) {
  struct {
    int sends_called = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&&) { capture.sends_called++; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  // Set total credits to 1.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));

  constexpr uint16_t connection_handle = 0x123;
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, connection_handle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = connection_handle});

  std::array<uint8_t, 1> attribute_value = {7};

  // Send first packet (uses 1 credit).
  {
    multibuf::MultiBuf mbuf = MultiBufFromArray(attribute_value);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, 1);
  }

  // Send second packet (should be queued in AclDataChannel).
  {
    multibuf::MultiBuf mbuf = MultiBufFromArray(attribute_value);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, 1);  // Still 1
  }

  // Send Disconnection_Complete event.
  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, connection_handle));

  // Reclaim 1 credit.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{connection_handle, 1}}));

  // The queued packet should NOT be sent out because it was cleared.
  EXPECT_EQ(capture.sends_called, 1);
}

TEST_F(DynamicCreditSharingTest, DynamicQueueingAndDrainingBrEdr) {
  struct {
    int sends_called = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&&) { capture.sends_called++; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  // Set total credits to 2 for BR/EDR.
  PW_TEST_EXPECT_OK(SendReadBufferResponseFromController(proxy, 2));

  constexpr uint16_t connection_handle = 0x456;
  PW_TEST_ASSERT_OK(SendConnectionCompleteEvent(
      proxy, connection_handle, emboss::StatusCode::SUCCESS));

  BasicL2capChannel channel = BuildBasicL2capChannel(
      proxy,
      {.handle = connection_handle, .transport = AclTransportType::kBrEdr});

  std::array<uint8_t, 1> attribute_value = {7};

  // Send first packet.
  {
    auto mbuf_result =
        multibuf::FromSpan(*GetProxyHostAllocator(),
                           as_writable_bytes(span(attribute_value)),
                           [](ByteSpan) {});
    ASSERT_TRUE(mbuf_result.has_value());
    PW_TEST_EXPECT_OK(channel.Write(std::move(*mbuf_result)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, 1);
  }

  // Send second packet.
  {
    auto mbuf_result =
        multibuf::FromSpan(*GetProxyHostAllocator(),
                           as_writable_bytes(span(attribute_value)),
                           [](ByteSpan) {});
    ASSERT_TRUE(mbuf_result.has_value());
    PW_TEST_EXPECT_OK(channel.Write(std::move(*mbuf_result)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, 2);
  }

  // Send third packet (should be queued).
  {
    auto mbuf_result =
        multibuf::FromSpan(*GetProxyHostAllocator(),
                           as_writable_bytes(span(attribute_value)),
                           [](ByteSpan) {});
    ASSERT_TRUE(mbuf_result.has_value());
    PW_TEST_EXPECT_OK(channel.Write(std::move(*mbuf_result)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, 2);
  }

  // Reclaim 1 credit.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{connection_handle, 1}}));

  // The queued packet should be sent out.
  EXPECT_EQ(capture.sends_called, 3);
}

TEST_F(DynamicCreditSharingTest, HostCreditsExhaustedThenProxyQueuesLE) {
  struct {
    int sends_called = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&&) { capture.sends_called++; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  // Set total credits to 2.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 2));

  constexpr uint16_t connection_handle = 0x123;
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, connection_handle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = connection_handle});

  // Simulate Host sending 2 packets.
  {
    std::array<uint8_t, 9> host_packet_arr = {
        0x02,
        0x23,
        0x01,
        0x04,
        0x00,  // H4 Type, Connection Handle 0x123, Length = 4
        0x00,
        0x00,
        0x01,
        0x00  // L2CAP Header: Length = 0, CID = 1
    };
    proxy.HandleH4HciFromHost(H4PacketWithH4(host_packet_arr));
    proxy.HandleH4HciFromHost(H4PacketWithH4(host_packet_arr));
  }

  EXPECT_EQ(capture.sends_called, 2);

  std::array<uint8_t, 1> attribute_value = {7};

  // Send packet from proxy (should be queued).
  {
    multibuf::MultiBuf mbuf = MultiBufFromArray(attribute_value);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, 2);
  }

  // Reclaim 1 credit.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{connection_handle, 1}}));

  // The queued packet should be sent out.
  EXPECT_EQ(capture.sends_called, 3);
}

TEST_F(DynamicCreditSharingTest, HostCreditsExhaustedThenProxyQueuesBrEdr) {
  struct {
    int sends_called = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&&) { capture.sends_called++; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  // Set total credits to 2 for BR/EDR.
  PW_TEST_EXPECT_OK(SendReadBufferResponseFromController(proxy, 2));

  constexpr uint16_t connection_handle = 0x456;
  PW_TEST_ASSERT_OK(SendConnectionCompleteEvent(
      proxy, connection_handle, emboss::StatusCode::SUCCESS));

  BasicL2capChannel channel = BuildBasicL2capChannel(
      proxy,
      {.handle = connection_handle, .transport = AclTransportType::kBrEdr});

  // Simulate Host sending 2 packets.
  {
    std::array<uint8_t, 9> host_packet_arr = {
        0x02,
        0x56,
        0x04,
        0x04,
        0x00,  // H4 Type, Connection Handle 0x456, Length = 4
        0x00,
        0x00,
        0x01,
        0x00  // L2CAP Header: Length = 0, CID = 1
    };
    proxy.HandleH4HciFromHost(H4PacketWithH4(host_packet_arr));
    proxy.HandleH4HciFromHost(H4PacketWithH4(host_packet_arr));
  }

  EXPECT_EQ(capture.sends_called, 2);

  std::array<uint8_t, 1> attribute_value = {7};

  // Send packet from proxy (should be queued).
  {
    auto mbuf_result =
        multibuf::FromSpan(*GetProxyHostAllocator(),
                           as_writable_bytes(span(attribute_value)),
                           [](ByteSpan) {});
    ASSERT_TRUE(mbuf_result.has_value());
    PW_TEST_EXPECT_OK(channel.Write(std::move(*mbuf_result)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, 2);
  }

  // Reclaim 1 credit.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{connection_handle, 1}}));

  // The queued packet should be sent out.
  EXPECT_EQ(capture.sends_called, 3);
}

TEST_F(DynamicCreditSharingTest, FairRoundRobinDrainingAndNocp) {
  struct {
    int sends_to_controller = 0;
    int proxy_packets_sent = 0;
    int host_packets_sent = 0;
    int nocps_to_host = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        pw::span<uint8_t> hci_buffer = packet.GetHciSpan();
        Result<emboss::EventHeaderView> event =
            MakeEmbossView<emboss::EventHeaderView>(hci_buffer);
        if (event.ok() && event->event_code().Read() ==
                              emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS) {
          Result<emboss::NumberOfCompletedPacketsEventView> nocp =
              MakeEmbossView<emboss::NumberOfCompletedPacketsEventView>(
                  hci_buffer);
          if (nocp.ok() && nocp->num_handles().Read() > 0) {
            capture.nocps_to_host +=
                nocp->nocp_data()[0].num_completed_packets().Read();
          }
        }
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        capture.sends_to_controller++;
        Result<emboss::AclDataFrameHeaderView> acl_view =
            MakeEmbossView<emboss::AclDataFrameHeaderView>(packet.GetHciSpan());
        if (acl_view.ok()) {
          // Proxy GattNotifyChannel packets in this test have data length 8.
          // Host packets have length 6.
          if (acl_view->data_total_length().Read() == 8) {
            capture.proxy_packets_sent++;
          } else {
            PW_LOG_INFO("ACL length: %d", acl_view->data_total_length().Read());
            capture.host_packets_sent++;
          }
        }
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 2));

  constexpr uint16_t connection_handle = 0x123;
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, connection_handle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = connection_handle});

  // Host sends 4 packets (length = 4 + 2 = 6, data length = 2)
  std::array<std::array<uint8_t, 11>, 4> host_packets;
  for (size_t i = 0; i < 4; ++i) {
    host_packets[i] = {
        0x02,
        0x23,
        0x01,
        0x06,
        0x00,  // H4, Handle 0x123, Length = 6
        0x02,
        0x00,
        0x01,
        0x00,  // L2CAP: Length = 2, CID = 1
        0xAA,
        0xBB  // Payload
    };
    proxy.HandleH4HciFromHost(H4PacketWithH4(host_packets[i]));
  }

  // Proxy sends 4 packets (length = 4 + 1 = 5, data length = 1)
  std::array<uint8_t, 1> attribute_value = {7};
  for (int i = 0; i < 4; ++i) {
    multibuf::MultiBuf mbuf = MultiBufFromArray(attribute_value);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
  }
  RunDispatcher();

  // Initially, controller max is 2. 2 host packets sent, 2 queued. 4 proxy
  // queued.
  EXPECT_EQ(capture.sends_to_controller, 2);
  EXPECT_EQ(capture.host_packets_sent, 2);
  EXPECT_EQ(capture.proxy_packets_sent, 0);
  EXPECT_EQ(capture.nocps_to_host, 0);

  // Reclaim 1
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{connection_handle, 1}}));
  RunDispatcher();

  // We want to see round-robin. Either proxy or host sends.
  EXPECT_EQ(capture.sends_to_controller, 3);

  // Reclaim 3 more
  for (int i = 0; i < 3; ++i) {
    PW_TEST_EXPECT_OK(
        SendNumberOfCompletedPackets(proxy, {{connection_handle, 1}}));
    RunDispatcher();
  }

  // After 4 reclaims, we should have sent a mix of host and proxy packets.
  // Total packets sent = 2 + 4 = 6.
  EXPECT_EQ(capture.sends_to_controller, 6);
  EXPECT_TRUE(capture.proxy_packets_sent > 0);
  EXPECT_TRUE(capture.host_packets_sent > 2);  // At least one from queue
}

#if PW_THREAD_JOINING_ENABLED
TEST_F(DynamicCreditSharingTest, StressMultiThreadedSend) {
  constexpr unsigned int kNumThreads = 4;
  constexpr unsigned int kPacketsPerThread = 20;
  constexpr uint16_t kTestHandle = 0x123;
  constexpr uint16_t kPayloadSize = 10;
  constexpr uint16_t kTotalCredits = 5;
  constexpr uint16_t kBaseLocalCid = 0xb000;
  constexpr uint16_t kBaseRemoteCid = 0xc000;

  struct {
    std::atomic<int> sends_to_controller{0};
    std::atomic<int> proxy_packets_sent{0};
    std::atomic<int> host_packets_sent{0};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        capture.sends_to_controller++;
        Result<emboss::AclDataFrameHeaderView> acl_view =
            MakeEmbossView<emboss::AclDataFrameHeaderView>(packet.GetHciSpan());
        if (acl_view.ok()) {
          if (acl_view->data_total_length().Read() == kPayloadSize + 4) {
            capture.proxy_packets_sent++;
          } else {
            capture.host_packets_sent++;
          }
        }
      });

  // Use libc allocators so msan can detect use after frees.
  std::array<std::byte, 20 * 1024> packet_buffer{};
  pw::multibuf::SimpleAllocator multibuf_allocator{
      /*data_area=*/packet_buffer,
      /*metadata_alloc=*/allocator::GetLibCAllocator()};

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnNewThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kTotalCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kTestHandle, emboss::StatusCode::SUCCESS));

  struct ThreadCapture {
    BasicL2capChannel channel;
    multibuf::MultiBufAllocator& packet_allocator;
  };

  pw::Vector<ThreadCapture, kNumThreads> captures;
  for (unsigned int i = 0; i < kNumThreads; ++i) {
    captures.emplace_back(ThreadCapture{
        BuildBasicL2capChannel(
            proxy,
            BasicL2capParameters{
                .handle = kTestHandle,
                .local_cid = static_cast<uint16_t>(kBaseLocalCid + i),
                .remote_cid = static_cast<uint16_t>(kBaseRemoteCid + i)}),
        multibuf_allocator,
    });
  }

  pw::thread::test::TestThreadContext context;
  pw::Vector<pw::Thread, kNumThreads + 2> threads;

  // Proxy writer threads
  for (unsigned int i = 0; i < kNumThreads; ++i) {
    ThreadCapture& thread_capture = captures[i];
    threads.emplace_back(context.options(), [&thread_capture]() {
      std::array<uint8_t, kPayloadSize> attribute_value{};
      for (unsigned int j = 0; j < kPacketsPerThread; ++j) {
        std::fill(attribute_value.begin(), attribute_value.end(), j);
        auto mbuf_result =
            thread_capture.packet_allocator.AllocateContiguous(kPayloadSize);
        if (mbuf_result.has_value()) {
          multibuf::MultiBuf mbuf_inst = std::move(*mbuf_result);
          auto it = mbuf_inst.begin();
          for (uint8_t val : attribute_value) {
            *it = static_cast<std::byte>(val);
            ++it;
          }
          thread_capture.channel.Write(std::move(mbuf_inst));
        }
        pw::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  // Host writer thread
  std::atomic<bool> stop_host{false};
  struct HostContext {
    ProxyHost& proxy;
    std::atomic<bool>& stop;
  } host_ctx{proxy, stop_host};

  threads.emplace_back(context.options(), [&host_ctx]() {
    std::array<uint8_t, 11> host_packet_arr = {
        0x02,
        0x23,
        0x01,
        0x06,
        0x00,  // H4, Handle 0x123, Length = 6
        0x02,
        0x00,
        0x01,
        0x00,  // L2CAP: Length = 2, CID = 1
        0xAA,
        0xBB  // Payload
    };
    while (!host_ctx.stop) {
      host_ctx.proxy.HandleH4HciFromHost(H4PacketWithH4(host_packet_arr));
      pw::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  // NOCP reclaimer thread
  std::atomic<bool> stop_nocp{false};
  struct NocpContext {
    ProxyHost& proxy;
    std::atomic<bool>& stop;
    uint16_t handle;
    DynamicCreditSharingTest* test;
  } nocp_ctx{proxy, stop_nocp, kTestHandle, this};

  threads.emplace_back(context.options(), [&nocp_ctx]() {
    while (!nocp_ctx.stop) {
      (void)nocp_ctx.test->SendNumberOfCompletedPackets(nocp_ctx.proxy,
                                                        {{nocp_ctx.handle, 1}});
      pw::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
  });

  // Wait for proxy writers
  for (unsigned int i = 0; i < kNumThreads; ++i) {
    threads[i].join();
  }

  // Stop host and NOCP threads
  stop_host = true;
  stop_nocp = true;
  threads[kNumThreads].join();
  threads[kNumThreads + 1].join();

  captures.clear();
  JoinDispatcherThread();

  EXPECT_GT(capture.sends_to_controller, 0);
  EXPECT_GT(capture.proxy_packets_sent, 0);
  EXPECT_GT(capture.host_packets_sent, 0);
}
#endif  // PW_THREAD_JOINING_ENABLED

TEST_F(DynamicCreditSharingTest, HostPacketsOrderingStress) {
  constexpr unsigned int kPacketsPerThread = 100;
  constexpr uint16_t kTestHandle = 0x123;
  constexpr uint16_t kInitialCredits = 5;

  struct {
    pw::Vector<uint8_t, kPacketsPerThread> sequence;
    pw::sync::Mutex mutex;
    int credits = kInitialCredits;
    size_t nocp_sent = 0;
    std::atomic<bool> stop_controller{false};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        pw::span<uint8_t> hci_buffer = packet.GetHciSpan();
        Result<emboss::EventHeaderView> event =
            MakeEmbossView<emboss::EventHeaderView>(hci_buffer);
        if (event.ok() && event->event_code().Read() ==
                              emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS) {
          Result<emboss::NumberOfCompletedPacketsEventView> nocp =
              MakeEmbossView<emboss::NumberOfCompletedPacketsEventView>(
                  hci_buffer);
          if (nocp.ok() && nocp->num_handles().Read() > 0) {
            std::lock_guard lock(capture.mutex);
            capture.credits +=
                nocp->nocp_data()[0].num_completed_packets().Read();
          }
        }
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        pw::span<uint8_t> span = packet.GetHciSpan();
        if (span.size() >= 10) {
          uint8_t seq = span[9];
          std::lock_guard lock(capture.mutex);
          capture.sequence.push_back(seq);
        }
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnNewThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kInitialCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kTestHandle, emboss::StatusCode::SUCCESS));

  pw::thread::test::TestThreadContext context;
  pw::Vector<pw::Thread, 2> threads;

  // Host thread
  struct HostThreadCtx {
    ProxyHost& proxy;
    decltype(capture)& cap;
  } host_thread_ctx{proxy, capture};

  threads.emplace_back(context.options(), [&host_thread_ctx]() {
    for (unsigned int j = 0; j < kPacketsPerThread; ++j) {
      bool has_credits = false;
      while (!has_credits) {
        {
          std::lock_guard lock(host_thread_ctx.cap.mutex);
          if (host_thread_ctx.cap.credits > 0) {
            host_thread_ctx.cap.credits--;
            has_credits = true;
          }
        }
        if (!has_credits) {
          pw::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }

      std::array<uint8_t, 11> host_packet_arr = {0x02,
                                                 0x23,
                                                 0x01,
                                                 0x06,
                                                 0x00,
                                                 0x02,
                                                 0x00,
                                                 0x01,
                                                 0x00,
                                                 0,  // Thread idx (unused)
                                                 static_cast<uint8_t>(j)};
      host_thread_ctx.proxy.HandleH4HciFromHost(
          H4PacketWithH4(host_packet_arr));
    }
  });

  // Controller thread (sends NOCPs)
  struct ControllerCtx {
    uint16_t test_handle;
    ProxyHost& proxy;
    decltype(capture)& cap;
    DynamicCreditSharingTest* test;
  } controller_ctx{kTestHandle, proxy, capture, this};

  threads.emplace_back(context.options(), [&controller_ctx]() {
    while (!controller_ctx.cap.stop_controller) {
      pw::this_thread::sleep_for(std::chrono::milliseconds(1));
      bool send_nocp = false;
      {
        std::lock_guard lock(controller_ctx.cap.mutex);
        if (controller_ctx.cap.nocp_sent < controller_ctx.cap.sequence.size()) {
          controller_ctx.cap.nocp_sent++;
          send_nocp = true;
        }
      }
      if (send_nocp) {
        (void)controller_ctx.test->SendNumberOfCompletedPackets(
            controller_ctx.proxy, {{controller_ctx.test_handle, 1}});
      }
    }
  });

  // Wait for host thread to finish sending all packets.
  threads[0].join();

  // Stop controller thread.
  capture.stop_controller = true;
  threads[1].join();

  JoinDispatcherThread();

  // Verify ordering.
  std::lock_guard lock(capture.mutex);
  EXPECT_EQ(capture.sequence.size(), kPacketsPerThread);
  for (size_t j = 1; j < capture.sequence.size(); ++j) {
    EXPECT_GT(capture.sequence[j], capture.sequence[j - 1]);
  }
}

TEST_F(DynamicCreditSharingTest, ReleaseFnDoesNotDeadlock) {
  struct {
    int sends_called = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&&) { capture.sends_called++; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  // Set total credits to 1.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));

  constexpr uint16_t connection_handle = 0x123;
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, connection_handle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = connection_handle});

  std::array<uint8_t, 1> attribute_value = {7};

  // Send first packet (uses 1 credit).
  {
    multibuf::MultiBuf mbuf = MultiBufFromArray(attribute_value);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, 1);
  }

  // Send second packet from host with a ReleaseFn that calls into proxy.
  // It should be queued because credits are exhausted.
  {
    std::array<uint8_t, 9> host_packet_arr = {
        0x02,
        0x23,
        0x01,
        0x04,
        0x00,  // H4 Type, Connection Handle 0x123, Length = 4
        0x00,
        0x00,
        0x01,
        0x00  // L2CAP Header: Length = 0, CID = 1
    };

    // Create a packet with a ReleaseFn that calls GetNumFreeLeAclPackets
    // TODO: b/505912880 - It would be better to call HandleH4HciFromHost
    // instead of GetNumFreeLeAclPackets, but that currently deadlocks.
    H4PacketWithH4 host_packet(host_packet_arr, [&proxy](const uint8_t*) {
      // This calls into proxy and acquires credit_mutex_
      (void)proxy.GetNumFreeLeAclPackets();
    });

    proxy.HandleH4HciFromHost(std::move(host_packet));
    EXPECT_EQ(capture.sends_called, 1);  // Still 1, packet is queued
  }

  // Reclaim 1 credit. This will trigger DrainDynamicQuota and dequeue the host
  // packet. If the packet is sent or dropped, its ReleaseFn will be called. If
  // called under locks, it will deadlock.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{connection_handle, 1}}));

  // The queued packet should be sent out.
  EXPECT_EQ(capture.sends_called, 2);
}

}  // namespace
}  // namespace pw::bluetooth::proxy
