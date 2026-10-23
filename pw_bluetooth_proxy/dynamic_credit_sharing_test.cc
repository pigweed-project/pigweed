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
  // It should be queued because credits are exhausted. Keep the backing buffer
  // in scope until the packet is dequeued and its ReleaseFn runs.
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
  {
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

TEST_F(DynamicCreditSharingTest,
       HostFragmentedL2capPduIsNotInterleavedByProxyTx) {
  constexpr uint16_t kConnectionHandle = 0x123;
  constexpr uint16_t kControllerMaxCredits = 4;
  constexpr uint16_t kHostCid = 0x0040;
  constexpr uint16_t kFirstFragmentPayloadSize = 2;
  constexpr uint16_t kSecondFragmentPayloadSize = 4;
  constexpr uint16_t kTotalL2capPayloadSize =
      kFirstFragmentPayloadSize + kSecondFragmentPayloadSize;

  struct SentPacketInfo {
    emboss::AclDataPacketBoundaryFlag boundary_flag;
    uint16_t data_total_length;
  };
  pw::Vector<SentPacketInfo, 4> sent_packets;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sent_packets](H4PacketWithH4&& packet) {
        Result<emboss::AclDataFrameHeaderView> acl_view =
            MakeEmbossView<emboss::AclDataFrameHeaderView>(packet.GetHciSpan());
        PW_CHECK_OK(acl_view);
        sent_packets.push_back(SentPacketInfo{
            .boundary_flag = acl_view->packet_boundary_flag().Read(),
            .data_total_length = acl_view->data_total_length().Read(),
        });
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kControllerMaxCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});

  // Host sends first fragment of a 2-fragment L2CAP PDU.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage first_fragment,
      SetupBFrame(kConnectionHandle, kHostCid, kFirstFragmentPayloadSize));
  first_fragment.writer.pdu_length().Write(kTotalL2capPayloadSize);
  proxy.HandleH4HciFromHost(H4PacketWithH4(first_fragment.acl.h4_span()));

  ASSERT_EQ(sent_packets.size(), 1u);
  EXPECT_EQ(sent_packets[0].boundary_flag,
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
  EXPECT_EQ(sent_packets[0].data_total_length,
            kFirstFragmentPayloadSize +
                emboss::BasicL2capHeader::IntrinsicSizeInBytes());

  // Proxy attempts to write a GATT notification while the host L2CAP PDU is
  // mid-fragmentation. It must remain queued and not interleave.
  std::array<uint8_t, 1> attribute_value = {7};
  PW_TEST_EXPECT_OK(channel.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  EXPECT_EQ(sent_packets.size(), 1u);

  // Host sends the continuing fragment that completes the L2CAP PDU.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      AclFrameWithStorage continuing_fragment,
      SetupAcl(kConnectionHandle, kSecondFragmentPayloadSize));
  continuing_fragment.writer.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  proxy.HandleH4HciFromHost(H4PacketWithH4(continuing_fragment.h4_span()));
  RunDispatcher();

  // Both the continuing fragment and the gated proxy packet should now be sent,
  // in that exact order.
  ASSERT_EQ(sent_packets.size(), 3u);
  EXPECT_EQ(sent_packets[1].boundary_flag,
            emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  EXPECT_EQ(sent_packets[1].data_total_length, kSecondFragmentPayloadSize);
  EXPECT_EQ(sent_packets[2].boundary_flag,
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
}

TEST_F(DynamicCreditSharingTest,
       HostThreeFragmentL2capPduIsNotInterleavedByProxyTx) {
  constexpr uint16_t kConnectionHandle = 0x123;
  constexpr uint16_t kControllerMaxCredits = 6;
  constexpr uint16_t kHostCid = 0x0040;
  constexpr uint16_t kFragment1PayloadSize = 2;
  constexpr uint16_t kFragment2PayloadSize = 3;
  constexpr uint16_t kFragment3PayloadSize = 4;
  constexpr uint16_t kTotalL2capPayloadSize =
      kFragment1PayloadSize + kFragment2PayloadSize + kFragment3PayloadSize;

  pw::Vector<emboss::AclDataPacketBoundaryFlag, 6> sent_boundary_flags;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sent_boundary_flags](H4PacketWithH4&& packet) {
        Result<emboss::AclDataFrameHeaderView> acl_view =
            MakeEmbossView<emboss::AclDataFrameHeaderView>(packet.GetHciSpan());
        PW_CHECK_OK(acl_view);
        sent_boundary_flags.push_back(acl_view->packet_boundary_flag().Read());
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kControllerMaxCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});

  // Fragment 1 from host.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage fragment_1,
      SetupBFrame(kConnectionHandle, kHostCid, kFragment1PayloadSize));
  fragment_1.writer.pdu_length().Write(kTotalL2capPayloadSize);
  proxy.HandleH4HciFromHost(H4PacketWithH4(fragment_1.acl.h4_span()));
  ASSERT_EQ(sent_boundary_flags.size(), 1u);

  // Proxy queues first packet after fragment 1 (must be gated).
  std::array<uint8_t, 1> attribute_value = {7};
  PW_TEST_EXPECT_OK(channel.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  EXPECT_EQ(sent_boundary_flags.size(), 1u);

  // Fragment 2 (intermediate continuing fragment) from host.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      AclFrameWithStorage fragment_2,
      SetupAcl(kConnectionHandle, kFragment2PayloadSize));
  fragment_2.writer.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  proxy.HandleH4HciFromHost(H4PacketWithH4(fragment_2.h4_span()));
  RunDispatcher();
  ASSERT_EQ(sent_boundary_flags.size(), 2u);
  EXPECT_EQ(sent_boundary_flags[1],
            emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);

  // Proxy queues second packet after fragment 2 (must still be gated).
  PW_TEST_EXPECT_OK(channel.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  EXPECT_EQ(sent_boundary_flags.size(), 2u);

  // Fragment 3 (final continuing fragment) from host.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      AclFrameWithStorage fragment_3,
      SetupAcl(kConnectionHandle, kFragment3PayloadSize));
  fragment_3.writer.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  proxy.HandleH4HciFromHost(H4PacketWithH4(fragment_3.h4_span()));
  RunDispatcher();

  // All 3 host fragments must precede the 2 proxy packets.
  ASSERT_EQ(sent_boundary_flags.size(), 5u);
  EXPECT_EQ(sent_boundary_flags[0],
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
  EXPECT_EQ(sent_boundary_flags[1],
            emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  EXPECT_EQ(sent_boundary_flags[2],
            emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  EXPECT_EQ(sent_boundary_flags[3],
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
  EXPECT_EQ(sent_boundary_flags[4],
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
}

TEST_F(DynamicCreditSharingTest,
       HostFragmentedL2capPduDrainedBeforeProxyAndPrioritizesHostNocp) {
  constexpr uint16_t kConnectionHandle = 0x123;
  constexpr uint16_t kControllerMaxCredits = 2;
  constexpr uint16_t kSingleCredit = 1;
  constexpr uint16_t kHostCid = 0x0040;
  constexpr uint16_t kFirstFragmentPayloadSize = 2;
  constexpr uint16_t kSecondFragmentPayloadSize = 3;
  constexpr uint16_t kTotalL2capPayloadSize =
      kFirstFragmentPayloadSize + kSecondFragmentPayloadSize;

  struct Capture {
    pw::Vector<emboss::AclDataPacketBoundaryFlag, 6> sent_boundary_flags;
    uint16_t nocps_to_host = 0;
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
        Result<emboss::AclDataFrameHeaderView> acl_view =
            MakeEmbossView<emboss::AclDataFrameHeaderView>(packet.GetHciSpan());
        PW_CHECK_OK(acl_view);
        capture.sent_boundary_flags.push_back(
            acl_view->packet_boundary_flag().Read());
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kControllerMaxCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});

  // 1. Proxy sends 1 packet and Host sends 1 complete packet, then 1 NOCP
  // arrives so that last_reclaimed_ becomes kHost while 1 proxy packet remains
  // in flight.
  std::array<uint8_t, 1> attribute_value = {7};
  PW_TEST_EXPECT_OK(channel.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();

  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage complete_host_frame,
      SetupBFrame(kConnectionHandle, kHostCid, kFirstFragmentPayloadSize));
  proxy.HandleH4HciFromHost(H4PacketWithH4(complete_host_frame.acl.h4_span()));
  ASSERT_EQ(capture.sent_boundary_flags.size(), 2u);

  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(
      proxy, {{kConnectionHandle, kSingleCredit}}));
  RunDispatcher();
  EXPECT_EQ(capture.nocps_to_host, 1u);

  // 2. Host now sends Fragment 1 (uses the 1 available credit) and queues
  // Fragment 2, while Proxy also queues a packet.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage first_fragment,
      SetupBFrame(kConnectionHandle, kHostCid, kFirstFragmentPayloadSize));
  first_fragment.writer.pdu_length().Write(kTotalL2capPayloadSize);
  proxy.HandleH4HciFromHost(H4PacketWithH4(first_fragment.acl.h4_span()));

  PW_TEST_ASSERT_OK_AND_ASSIGN(
      AclFrameWithStorage continuing_fragment,
      SetupAcl(kConnectionHandle, kSecondFragmentPayloadSize));
  continuing_fragment.writer.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  proxy.HandleH4HciFromHost(H4PacketWithH4(continuing_fragment.h4_span()));

  PW_TEST_EXPECT_OK(channel.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  ASSERT_EQ(capture.sent_boundary_flags.size(), 3u);

  // 3. Reclaim 1 credit via NOCP. Even though last_reclaimed_ was kHost and
  // a proxy packet is pending, ReclaimCredit() must prioritize kHost while a
  // fragmented host PDU is in progress, and DrainDynamicQuota must drain the
  // continuing fragment before the proxy packet.
  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(
      proxy, {{kConnectionHandle, kSingleCredit}}));
  RunDispatcher();
  EXPECT_EQ(capture.nocps_to_host, 2u);
  ASSERT_EQ(capture.sent_boundary_flags.size(), 4u);
  EXPECT_EQ(capture.sent_boundary_flags[3],
            emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);

  // 4. Next NOCP drains the queued proxy packet.
  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(
      proxy, {{kConnectionHandle, kSingleCredit}}));
  RunDispatcher();
  ASSERT_EQ(capture.sent_boundary_flags.size(), 5u);
  EXPECT_EQ(capture.sent_boundary_flags[4],
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
}

TEST_F(DynamicCreditSharingTest,
       HostFragmentedL2capPduNotInterleavedInStaticCreditMode) {
  constexpr uint16_t kConnectionHandle = 0x123;
  constexpr uint16_t kControllerMaxCredits = 4;
  constexpr uint16_t kReservedProxyLeCredits = 2;
  constexpr uint16_t kReservedProxyBrEdrCredits = 0;
  constexpr uint16_t kHostCid = 0x0040;
  constexpr uint16_t kFirstFragmentPayloadSize = 2;
  constexpr uint16_t kSecondFragmentPayloadSize = 3;
  constexpr uint16_t kTotalL2capPayloadSize =
      kFirstFragmentPayloadSize + kSecondFragmentPayloadSize;

  pw::Vector<emboss::AclDataPacketBoundaryFlag, 4> sent_boundary_flags;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sent_boundary_flags](H4PacketWithH4&& packet) {
        Result<emboss::AclDataFrameHeaderView> acl_view =
            MakeEmbossView<emboss::AclDataFrameHeaderView>(packet.GetHciSpan());
        PW_CHECK_OK(acl_view);
        sent_boundary_flags.push_back(acl_view->packet_boundary_flag().Read());
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/
                              kReservedProxyLeCredits,
                              /*br_edr_acl_credits_to_reserve=*/
                              kReservedProxyBrEdrCredits,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kControllerMaxCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});

  // Host sends first fragment of a fragmented L2CAP PDU.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage first_fragment,
      SetupBFrame(kConnectionHandle, kHostCid, kFirstFragmentPayloadSize));
  first_fragment.writer.pdu_length().Write(kTotalL2capPayloadSize);
  proxy.HandleH4HciFromHost(H4PacketWithH4(first_fragment.acl.h4_span()));
  ASSERT_EQ(sent_boundary_flags.size(), 1u);

  // Proxy attempts to write while host PDU is mid-fragmentation; must be gated.
  std::array<uint8_t, 1> attribute_value = {7};
  PW_TEST_EXPECT_OK(channel.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  EXPECT_EQ(sent_boundary_flags.size(), 1u);

  // Host sends continuing fragment completing the PDU.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      AclFrameWithStorage continuing_fragment,
      SetupAcl(kConnectionHandle, kSecondFragmentPayloadSize));
  continuing_fragment.writer.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  proxy.HandleH4HciFromHost(H4PacketWithH4(continuing_fragment.h4_span()));
  RunDispatcher();

  ASSERT_EQ(sent_boundary_flags.size(), 3u);
  EXPECT_EQ(sent_boundary_flags[0],
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
  EXPECT_EQ(sent_boundary_flags[1],
            emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  EXPECT_EQ(sent_boundary_flags[2],
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
}

TEST_F(DynamicCreditSharingTest,
       DisconnectDuringHostFragmentedPduUnblocksProxyTx) {
  constexpr uint16_t kConnectionHandle1 = 0x123;
  constexpr uint16_t kConnectionHandle2 = 0x124;
  constexpr uint16_t kControllerMaxCredits = 4;
  constexpr uint16_t kHostCid = 0x0040;
  constexpr uint16_t kFirstFragmentPayloadSize = 2;
  constexpr uint16_t kTotalL2capPayloadSize = kFirstFragmentPayloadSize + 4;

  int sends_called = 0;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sends_called](H4PacketWithH4&&) { ++sends_called; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kControllerMaxCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle1, emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle2, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel2 =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle2});

  // Host sends first fragment of a fragmented L2CAP PDU on connection 1.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage first_fragment,
      SetupBFrame(kConnectionHandle1, kHostCid, kFirstFragmentPayloadSize));
  first_fragment.writer.pdu_length().Write(kTotalL2capPayloadSize);
  proxy.HandleH4HciFromHost(H4PacketWithH4(first_fragment.acl.h4_span()));
  EXPECT_EQ(sends_called, 1);

  // Proxy writes on connection 2; gated while connection 1 is
  // mid-fragmentation.
  std::array<uint8_t, 1> attribute_value = {7};
  PW_TEST_EXPECT_OK(channel2.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  EXPECT_EQ(sends_called, 1);

  // Connection 1 disconnects mid-PDU; connection 2 proxy packet should unblock.
  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, kConnectionHandle1));
  RunDispatcher();
  EXPECT_EQ(sends_called, 2);
}

TEST_F(DynamicCreditSharingTest,
       HostThreeFragmentL2capPduNotInterleavedInStaticCreditMode) {
  constexpr uint16_t kConnectionHandle = 0x123;
  constexpr uint16_t kControllerMaxCredits = 6;
  constexpr uint16_t kReservedProxyLeCredits = 2;
  constexpr uint16_t kReservedProxyBrEdrCredits = 0;
  constexpr uint16_t kHostCid = 0x0040;
  constexpr uint16_t kFragment1PayloadSize = 2;
  constexpr uint16_t kFragment2PayloadSize = 3;
  constexpr uint16_t kFragment3PayloadSize = 4;
  constexpr uint16_t kTotalL2capPayloadSize =
      kFragment1PayloadSize + kFragment2PayloadSize + kFragment3PayloadSize;

  pw::Vector<emboss::AclDataPacketBoundaryFlag, 6> sent_boundary_flags;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sent_boundary_flags](H4PacketWithH4&& packet) {
        Result<emboss::AclDataFrameHeaderView> acl_view =
            MakeEmbossView<emboss::AclDataFrameHeaderView>(packet.GetHciSpan());
        PW_CHECK_OK(acl_view);
        sent_boundary_flags.push_back(acl_view->packet_boundary_flag().Read());
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/
                              kReservedProxyLeCredits,
                              /*br_edr_acl_credits_to_reserve=*/
                              kReservedProxyBrEdrCredits,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kControllerMaxCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});

  // Fragment 1 from host.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage fragment_1,
      SetupBFrame(kConnectionHandle, kHostCid, kFragment1PayloadSize));
  fragment_1.writer.pdu_length().Write(kTotalL2capPayloadSize);
  proxy.HandleH4HciFromHost(H4PacketWithH4(fragment_1.acl.h4_span()));
  ASSERT_EQ(sent_boundary_flags.size(), 1u);

  // Proxy queues first packet after fragment 1 (must be gated).
  std::array<uint8_t, 1> attribute_value = {7};
  PW_TEST_EXPECT_OK(channel.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  EXPECT_EQ(sent_boundary_flags.size(), 1u);

  // Fragment 2 (intermediate continuing fragment) from host.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      AclFrameWithStorage fragment_2,
      SetupAcl(kConnectionHandle, kFragment2PayloadSize));
  fragment_2.writer.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  proxy.HandleH4HciFromHost(H4PacketWithH4(fragment_2.h4_span()));
  RunDispatcher();
  ASSERT_EQ(sent_boundary_flags.size(), 2u);
  EXPECT_EQ(sent_boundary_flags[1],
            emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);

  // Proxy queues second packet after fragment 2 (must still be gated).
  PW_TEST_EXPECT_OK(channel.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  EXPECT_EQ(sent_boundary_flags.size(), 2u);

  // Fragment 3 (final continuing fragment) from host.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      AclFrameWithStorage fragment_3,
      SetupAcl(kConnectionHandle, kFragment3PayloadSize));
  fragment_3.writer.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  proxy.HandleH4HciFromHost(H4PacketWithH4(fragment_3.h4_span()));
  RunDispatcher();

  // All 3 host fragments must precede the 2 proxy packets.
  ASSERT_EQ(sent_boundary_flags.size(), 5u);
  EXPECT_EQ(sent_boundary_flags[0],
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
  EXPECT_EQ(sent_boundary_flags[1],
            emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  EXPECT_EQ(sent_boundary_flags[2],
            emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  EXPECT_EQ(sent_boundary_flags[3],
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
  EXPECT_EQ(sent_boundary_flags[4],
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
}

TEST_F(DynamicCreditSharingTest,
       HostFragmentedL2capPduPrioritizesHostNocpInStaticCreditMode) {
  constexpr uint16_t kConnectionHandle = 0x123;
  constexpr uint16_t kControllerMaxCredits = 2;
  constexpr uint16_t kReservedProxyLeCredits = 1;
  constexpr uint16_t kReservedProxyBrEdrCredits = 0;
  constexpr uint16_t kSingleCredit = 1;
  constexpr uint16_t kHostCid = 0x0040;
  constexpr uint16_t kFirstFragmentPayloadSize = 2;
  constexpr uint16_t kSecondFragmentPayloadSize = 3;
  constexpr uint16_t kTotalL2capPayloadSize =
      kFirstFragmentPayloadSize + kSecondFragmentPayloadSize;

  struct Capture {
    pw::Vector<emboss::AclDataPacketBoundaryFlag, 6> sent_boundary_flags;
    uint16_t nocps_to_host = 0;
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
        Result<emboss::AclDataFrameHeaderView> acl_view =
            MakeEmbossView<emboss::AclDataFrameHeaderView>(packet.GetHciSpan());
        PW_CHECK_OK(acl_view);
        capture.sent_boundary_flags.push_back(
            acl_view->packet_boundary_flag().Read());
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/
                              kReservedProxyLeCredits,
                              /*br_edr_acl_credits_to_reserve=*/
                              kReservedProxyBrEdrCredits,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kControllerMaxCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});

  // 1. Proxy sends 1 packet (exhausting its 1 reserved credit) and Host sends
  // 1 complete packet, then 1 NOCP arrives so last_reclaimed_ becomes kHost
  // while 1 proxy packet remains in flight.
  std::array<uint8_t, 1> attribute_value = {7};
  PW_TEST_EXPECT_OK(channel.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();

  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage complete_host_frame,
      SetupBFrame(kConnectionHandle, kHostCid, kFirstFragmentPayloadSize));
  proxy.HandleH4HciFromHost(H4PacketWithH4(complete_host_frame.acl.h4_span()));
  ASSERT_EQ(capture.sent_boundary_flags.size(), 2u);

  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(
      proxy, {{kConnectionHandle, kSingleCredit}}));
  RunDispatcher();
  EXPECT_EQ(capture.nocps_to_host, 1u);

  // 2. Host sends Fragment 1 of a fragmented PDU. Proxy also queues a packet.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage first_fragment,
      SetupBFrame(kConnectionHandle, kHostCid, kFirstFragmentPayloadSize));
  first_fragment.writer.pdu_length().Write(kTotalL2capPayloadSize);
  proxy.HandleH4HciFromHost(H4PacketWithH4(first_fragment.acl.h4_span()));

  PW_TEST_EXPECT_OK(channel.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  ASSERT_EQ(capture.sent_boundary_flags.size(), 3u);

  // 3. Reclaim 1 credit via NOCP while Host PDU is mid-fragmentation. Even
  // though last_reclaimed_ was kHost and 1 proxy packet is pending, the credit
  // must be returned to the Host so the Host is not starved of credits needed
  // to send Fragment 2.
  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(
      proxy, {{kConnectionHandle, kSingleCredit}}));
  RunDispatcher();
  EXPECT_EQ(capture.nocps_to_host, 2u);
  EXPECT_EQ(capture.sent_boundary_flags.size(), 3u);

  // 4. Next NOCP reclaims the proxy's in-flight credit, but proxy TX remains
  // gated until Host sends Fragment 2.
  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(
      proxy, {{kConnectionHandle, kSingleCredit}}));
  RunDispatcher();
  EXPECT_EQ(capture.sent_boundary_flags.size(), 3u);

  // 5. Host sends Fragment 2; both Fragment 2 and the queued proxy packet are
  // sent in order.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      AclFrameWithStorage continuing_fragment,
      SetupAcl(kConnectionHandle, kSecondFragmentPayloadSize));
  continuing_fragment.writer.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  proxy.HandleH4HciFromHost(H4PacketWithH4(continuing_fragment.h4_span()));
  RunDispatcher();

  ASSERT_EQ(capture.sent_boundary_flags.size(), 5u);
  EXPECT_EQ(capture.sent_boundary_flags[3],
            emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  EXPECT_EQ(capture.sent_boundary_flags[4],
            emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
}

TEST_F(DynamicCreditSharingTest,
       DisconnectDuringHostFragmentedPduUnblocksProxyTxInStaticCreditMode) {
  constexpr uint16_t kConnectionHandle1 = 0x123;
  constexpr uint16_t kConnectionHandle2 = 0x124;
  constexpr uint16_t kControllerMaxCredits = 4;
  constexpr uint16_t kReservedProxyLeCredits = 2;
  constexpr uint16_t kReservedProxyBrEdrCredits = 0;
  constexpr uint16_t kHostCid = 0x0040;
  constexpr uint16_t kFirstFragmentPayloadSize = 2;
  constexpr uint16_t kTotalL2capPayloadSize = kFirstFragmentPayloadSize + 4;

  int sends_called = 0;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sends_called](H4PacketWithH4&&) { ++sends_called; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/
                              kReservedProxyLeCredits,
                              /*br_edr_acl_credits_to_reserve=*/
                              kReservedProxyBrEdrCredits,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kControllerMaxCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle1, emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle2, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel2 =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle2});

  // Host sends first fragment of a fragmented L2CAP PDU on connection 1.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage first_fragment,
      SetupBFrame(kConnectionHandle1, kHostCid, kFirstFragmentPayloadSize));
  first_fragment.writer.pdu_length().Write(kTotalL2capPayloadSize);
  proxy.HandleH4HciFromHost(H4PacketWithH4(first_fragment.acl.h4_span()));
  EXPECT_EQ(sends_called, 1);

  // Proxy writes on connection 2; gated while connection 1 is
  // mid-fragmentation.
  std::array<uint8_t, 1> attribute_value = {7};
  PW_TEST_EXPECT_OK(channel2.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  EXPECT_EQ(sends_called, 1);

  // Connection 1 disconnects mid-PDU; connection 2 proxy packet should unblock.
  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, kConnectionHandle1));
  RunDispatcher();
  EXPECT_EQ(sends_called, 2);
}

TEST_F(DynamicCreditSharingTest,
       AbortedHostFragmentedPduByNewUnfragmentedPduUnblocksProxyTx) {
  constexpr uint16_t kConnectionHandle = 0x123;
  constexpr uint16_t kControllerMaxCredits = 4;
  constexpr uint16_t kHostCid = 0x0040;
  constexpr uint16_t kFirstFragmentPayloadSize = 2;
  constexpr uint16_t kTotalL2capPayloadSize = kFirstFragmentPayloadSize + 4;

  int sends_called = 0;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sends_called](H4PacketWithH4&&) { ++sends_called; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              *GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kControllerMaxCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});

  // 1. Host sends first fragment of a fragmented L2CAP PDU.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage first_fragment,
      SetupBFrame(kConnectionHandle, kHostCid, kFirstFragmentPayloadSize));
  first_fragment.writer.pdu_length().Write(kTotalL2capPayloadSize);
  proxy.HandleH4HciFromHost(H4PacketWithH4(first_fragment.acl.h4_span()));
  EXPECT_EQ(sends_called, 1);

  // 2. Proxy queues a packet; gated while host fragmented PDU is in progress.
  std::array<uint8_t, 1> attribute_value = {7};
  PW_TEST_EXPECT_OK(channel.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  EXPECT_EQ(sends_called, 1);

  // 3. Host aborts the incomplete PDU by sending a new unfragmented FIRST_*
  // packet. Once sent, proxy TX must be unblocked.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage unfragmented_pdu,
      SetupBFrame(kConnectionHandle, kHostCid, kFirstFragmentPayloadSize));
  proxy.HandleH4HciFromHost(H4PacketWithH4(unfragmented_pdu.acl.h4_span()));
  RunDispatcher();
  EXPECT_EQ(sends_called, 3);
}

TEST_F(DynamicCreditSharingTest,
       TwoConnectionsWithFragmentedHostPdusKeepProxyGatedUntilBothComplete) {
  constexpr uint16_t kConnectionHandle1 = 0x123;
  constexpr uint16_t kConnectionHandle2 = 0x124;
  constexpr uint16_t kControllerMaxCredits = 6;
  constexpr uint16_t kReservedProxyLeCredits = 2;
  constexpr uint16_t kReservedProxyBrEdrCredits = 0;
  constexpr uint16_t kHostCid = 0x0040;
  constexpr uint16_t kFirstFragmentPayloadSize = 2;
  constexpr uint16_t kSecondFragmentPayloadSize = 4;
  constexpr uint16_t kTotalL2capPayloadSize =
      kFirstFragmentPayloadSize + kSecondFragmentPayloadSize;

  int sends_called = 0;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sends_called](H4PacketWithH4&&) { ++sends_called; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/
                              kReservedProxyLeCredits,
                              /*br_edr_acl_credits_to_reserve=*/
                              kReservedProxyBrEdrCredits,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kControllerMaxCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle1, emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle2, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel1 =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle1});

  // 1. Both connections start a fragmented host L2CAP PDU.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage first_frag_conn1,
      SetupBFrame(kConnectionHandle1, kHostCid, kFirstFragmentPayloadSize));
  first_frag_conn1.writer.pdu_length().Write(kTotalL2capPayloadSize);
  proxy.HandleH4HciFromHost(H4PacketWithH4(first_frag_conn1.acl.h4_span()));

  PW_TEST_ASSERT_OK_AND_ASSIGN(
      BFrameWithStorage first_frag_conn2,
      SetupBFrame(kConnectionHandle2, kHostCid, kFirstFragmentPayloadSize));
  first_frag_conn2.writer.pdu_length().Write(kTotalL2capPayloadSize);
  proxy.HandleH4HciFromHost(H4PacketWithH4(first_frag_conn2.acl.h4_span()));
  EXPECT_EQ(sends_called, 2);

  // 2. Proxy queues a packet; gated while either connection has a fragmented
  // host PDU in progress.
  std::array<uint8_t, 1> attribute_value = {7};
  PW_TEST_EXPECT_OK(channel1.Write(MultiBufFromArray(attribute_value)).status);
  RunDispatcher();
  EXPECT_EQ(sends_called, 2);

  // 3. Connection 1 completes its fragmented PDU; proxy TX remains gated
  // because Connection 2 still has a fragmented PDU in progress.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      AclFrameWithStorage cont_frag_conn1,
      SetupAcl(kConnectionHandle1, kSecondFragmentPayloadSize));
  cont_frag_conn1.writer.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  proxy.HandleH4HciFromHost(H4PacketWithH4(cont_frag_conn1.h4_span()));
  RunDispatcher();
  EXPECT_EQ(sends_called, 3);

  // 4. Connection 2 completes its fragmented PDU; now proxy TX is unblocked.
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      AclFrameWithStorage cont_frag_conn2,
      SetupAcl(kConnectionHandle2, kSecondFragmentPayloadSize));
  cont_frag_conn2.writer.header().packet_boundary_flag().Write(
      emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
  proxy.HandleH4HciFromHost(H4PacketWithH4(cont_frag_conn2.h4_span()));
  RunDispatcher();
  EXPECT_EQ(sends_called, 5);
}

}  // namespace
}  // namespace pw::bluetooth::proxy
