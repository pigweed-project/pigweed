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

#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "pw_allocator/allocator.h"
#include "pw_allocator/libc_allocator.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/hci_common.emb.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth/hci_events.emb.h"
#include "pw_bluetooth/hci_h4.emb.h"
#include "pw_bluetooth/l2cap_frames.emb.h"
#include "pw_bluetooth_proxy/basic_l2cap_channel.h"
#include "pw_bluetooth_proxy/config.h"
#include "pw_bluetooth_proxy/direction.h"
#include "pw_bluetooth_proxy/gatt_notify_channel.h"
#include "pw_bluetooth_proxy/h4_packet.h"
#include "pw_bluetooth_proxy/internal/logical_transport.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/l2cap_coc.h"
#include "pw_bluetooth_proxy/proxy_host.h"
#include "pw_function/function.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_unit_test/framework.h"

#if PW_MULTIBUF_VERSION == 1
#include "pw_multibuf/simple_allocator.h"
#else  // PW_MULTIBUF_VERSION
#include "pw_allocator/synchronized_allocator.h"
#include "pw_allocator/testing.h"
#endif  // PW_MULTIBUF_VERSION

#if PW_BLUETOOTH_PROXY_ASYNC != 0
#include "pw_async2/notified_dispatcher.h"
#include "pw_sync/mutex.h"
#include "pw_sync/thread_notification.h"
#include "pw_thread/id.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#endif  // PW_BLUETOOTH_PROXY_ASYNC

namespace pw::bluetooth::proxy {

// ########## Util functions

struct AclFrameWithStorage {
  std::vector<uint8_t> storage;
  emboss::AclDataFrameWriter writer;

  static constexpr size_t kH4HeaderSize = 1;
  pw::span<uint8_t> h4_span() { return storage; }
  pw::span<uint8_t> hci_span() {
    return pw::span(storage).subspan(kH4HeaderSize);
  }
};

// Allocate storage and populate an ACL packet header with the given length.
Result<AclFrameWithStorage> SetupAcl(uint16_t handle, uint16_t l2cap_length);

struct BFrameWithStorage {
  AclFrameWithStorage acl;
  emboss::BFrameWriter writer;
};

Result<BFrameWithStorage> SetupBFrame(uint16_t handle,
                                      uint16_t channel_id,
                                      uint16_t bframe_len);

struct CFrameWithStorage {
  AclFrameWithStorage acl;
  emboss::CFrameWriter writer;
};

Result<CFrameWithStorage> SetupCFrame(uint16_t handle,
                                      uint16_t channel_id,
                                      uint16_t cframe_len);

struct KFrameWithStorage {
  AclFrameWithStorage acl;
  std::variant<emboss::FirstKFrameWriter, emboss::SubsequentKFrameWriter>
      writer;
};

// Size of sdu_length field in first K-frames.
inline constexpr uint8_t kSduLengthFieldSize = 2;

// Populate a KFrame that encodes a particular segment of `payload` based on the
// `mps`, or maximum PDU payload size of a segment. `segment_no` is the nth
// segment that would be generated based on the `mps`. The first segment is
// `segment_no == 0` and returns the `FirstKFrameWriter` variant in
// `KFrameWithStorage`.
//
// Returns PW_STATUS_OUT_OF_RANGE if a segment is requested beyond the last
// segment that would be generated based on `mps`.
Result<KFrameWithStorage> SetupKFrame(uint16_t handle,
                                      uint16_t channel_id,
                                      uint16_t mps,
                                      uint16_t segment_no,
                                      span<const uint8_t> payload);

// Populate passed H4 command buffer and return Emboss view on it.
template <typename EmbossT>
Result<EmbossT> CreateAndPopulateToControllerView(H4PacketWithH4& h4_packet,
                                                  emboss::OpCode opcode,
                                                  size_t parameter_total_size) {
  h4_packet.SetH4Type(emboss::H4PacketType::COMMAND);
  PW_TRY_ASSIGN(auto view, MakeEmbossWriter<EmbossT>(h4_packet.GetHciSpan()));
  view.header().opcode().Write(opcode);
  view.header().parameter_total_size().Write(parameter_total_size);
  return view;
}

// Populate passed H4 event buffer and return Emboss writer on it. Suitable for
// use with EmbossT types whose `SizeInBytes()` accurately represents
// the `parameter_total_size` that should be written (minus `EventHeader` size).
template <typename EmbossT>
Result<EmbossT> CreateAndPopulateToHostEventWriter(
    H4PacketWithHci& h4_packet,
    emboss::EventCode event_code,
    size_t parameter_total_size = EmbossT::SizeInBytes() -
                                  emboss::EventHeader::IntrinsicSizeInBytes()) {
  h4_packet.SetH4Type(emboss::H4PacketType::EVENT);
  PW_TRY_ASSIGN(auto view, MakeEmbossWriter<EmbossT>(h4_packet.GetHciSpan()));
  view.header().event_code().Write(event_code);
  view.header().parameter_total_size().Write(parameter_total_size);
  view.status().Write(emboss::StatusCode::SUCCESS);
  EXPECT_TRUE(view.IsComplete());
  return view;
}

//  Returns the state of the given channel.
L2capChannel::State GetState(const internal::GenericL2capChannel& channel);

struct L2capOptions {
  std::optional<MtuOption> mtu;
};

// TODO: https://pwbug.dev/382783733 - Migrate to L2capChannelEvent callback.
struct CocParameters {
  uint16_t handle = 123;
  uint16_t local_cid = 234;
  uint16_t remote_cid = 456;
  uint16_t rx_mtu = 100;
  uint16_t rx_mps = 100;
  uint16_t rx_credits = 1;
  uint16_t tx_mtu = 100;
  uint16_t tx_mps = 100;
  uint16_t tx_credits = 1;
  Function<void(FlatConstMultiBuf&& payload)>&& receive_fn = nullptr;
  ChannelEventCallback&& event_fn = nullptr;
};

struct BasicL2capParameters {
  MultiBufAllocator* rx_multibuf_allocator = nullptr;
  uint16_t handle = 123;
  uint16_t local_cid = 234;
  uint16_t remote_cid = 456;
  AclTransportType transport = AclTransportType::kLe;
  OptionalPayloadReceiveCallback&& payload_from_controller_fn = nullptr;
  OptionalPayloadReceiveCallback&& payload_from_host_fn = nullptr;
  ChannelEventCallback&& event_fn = nullptr;
};

struct BasicChannelProxyParameters {
  ConnectionHandle connection_handle = ConnectionHandle{123};
  uint16_t local_channel_id = 234;
  uint16_t remote_channel_id = 456;
  AclTransportType transport = AclTransportType::kLe;
  L2capChannelManagerInterface::BufferReceiveFunction
      payload_from_controller_fn = [](auto, auto, auto, auto) { return false; };
  L2capChannelManagerInterface::BufferReceiveFunction payload_from_host_fn =
      [](auto, auto, auto, auto) { return false; };
  ChannelEventCallback event_fn = nullptr;
};

struct GattNotifyChannelParameters {
  uint16_t handle = 123;
  uint16_t attribute_handle = 0xBC;
  ChannelEventCallback&& event_fn = nullptr;
};

/// Helper class that can produce an initialized MultiBufAllocator for either
/// Multibuf V1 or V2, depending on the `PW_MULTIBUF_VERSION` config
/// option.
template <size_t kDataCapacity, typename Lock = sync::NoLock>
class MultiBufAllocatorContext {
 public:
  MultiBufAllocator& GetAllocator() { return allocator_; }

 private:
  // Use libc allocators so msan can detect use after frees.
#if PW_MULTIBUF_VERSION == 1

  std::array<std::byte, kDataCapacity> buffer_{};
  pw::multibuf::SimpleAllocator allocator_{
      /*data_area=*/buffer_,
      /*metadata_alloc=*/allocator::GetLibCAllocator()};

#elif PW_MULTIBUF_VERSION == 2
  using BlockType = allocator::test::AllocatorForTest<0>::BlockType;

  static constexpr size_t kDataSize =
      AlignUp(kDataCapacity + BlockType::kBlockOverhead, BlockType::kAlignment);

  allocator::test::AllocatorForTest<kDataSize> data_alloc_;
  allocator::SynchronizedAllocator<Lock> synced_{data_alloc_};
  MultiBufAllocator allocator_{
      /*data_alloc=*/synced_,
      /*metadata_alloc=*/allocator::GetLibCAllocator()};

#endif  // PW_MULTIBUF_VERSION
};

// ########## Async support
#if PW_BLUETOOTH_PROXY_ASYNC != 0

/// An RAII wrapper for a thread that runs an async task dispatcher.
///
/// This is useful for multithreaded unit tests of proxies that may have async
/// mode enabled.
///
/// In sync mode, this type has no fields and all methods return trivially.
class ProxyHostTestDispatcher final {
 public:
  /// A routine that can be invoked on the dispatcher thread.
  ///
  /// In async mode, packets need to be generated on the dispatcher/receive
  /// thread. Each `ProxyHostTest::Send...` method can detect when it is called
  /// from another thread and `NeedsDispatcherThread`. In this case, it adds its
  /// parameters to a lambda capture that can be passed to the dispatcher thread
  /// to re-evaluate the method.
  ~ProxyHostTestDispatcher();

  ProxyHost& proxy() { return *proxy_; }

 private:
  friend class ProxyHostTest;

  ProxyHostTestDispatcher() : dispatcher_(rx_) {}

  /// Registers the dispatcher with the proxy without creating a thread.
  void StartOnCurrentThread(ProxyHost& proxy);

  /// Returns true if dispatcher is running on the curren thread.
  [[nodiscard]] bool IsRunningOnThisThread() const PW_LOCKS_EXCLUDED(mutex_);

  /// Runs the dispatcher until it is stalled.
  void Run();

#if PW_THREAD_JOINING_ENABLED

  /// Starts a dedicated thread to run the dispatcher.
  void StartOnNewThread(ProxyHost& proxy);

  /// If running on a dedicated thread, stops and joins the thread.
  void JoinThread();

#endif  // PW_BLUETOOTH_PROXY_ASYNC != 0

  async2::NotifiedDispatcher dispatcher_;
  ProxyHost* proxy_ = nullptr;

  thread::test::TestThreadContext context_;
  std::optional<Thread> thread_;

  sync::ThreadNotification rx_;
  sync::ThreadNotification tx_;

  mutable sync::Mutex mutex_;
  std::optional<Thread::id> id_ PW_GUARDED_BY(mutex_);
};

#endif  // PW_BLUETOOTH_PROXY_ASYNC != 0

// ########## Test Suites

class ProxyHostTest : public testing::Test {
 protected:
  // Returns the number of payloads that can be dequeued without being sent.
  // This is 0 in sync mode, and 1 in async mode.
  static constexpr uint16_t NumBufferedPayloads() {
    return PW_BLUETOOTH_PROXY_ASYNC == 0 ? 0 : 1;
  }

  Allocator* GetProxyHostAllocator();

  //////////////////////////////////////////////////////////////////////////////
  // Async support. In sync mode, these methods do nothing.

  /// Sets up the dispatcher for the `proxy`.
  ///
  /// In async mode, this registers the dispatcher that will be used to process
  /// events on the current thread. Either this method or
  /// `StartDispatcherOnNewThread` must be called exactly once before using the
  /// proxy in a test. If this method is used, the dispatcher must be manually
  /// run using `RunDispatcher`.
  ///
  /// In sync mode, this is a no-op.
  void StartDispatcherOnCurrentThread(ProxyHost& proxy);

  /// Starts a dedicated thread to run the async task dispatcher.
  ///
  /// In async mode, this registers the dispatcher that will be used to run
  /// tasks on a separate thread. Either this method or
  /// `StartDispatcherOnCurrentThread` must be called exactly once before using
  /// the proxy in a test.
  ///
  /// In sync mode, this is a no-op.
  void StartDispatcherOnNewThread(ProxyHost& proxy);

  /// Runs the dispatcher if in async mode.
  ///
  /// This processes any pending events.
  ///
  /// In sync mode, this is a no-op.
  void RunDispatcher();

  /// Stops the dispatcher thread.
  ///
  /// If omitted, the dispatcher thread will be joined on test destruction.
  ///
  /// In sync mode, this is a no-op.
  void JoinDispatcherThread();

  //////////////////////////////////////////////////////////////////////////////
  // Packet generation.

  // Send an LE_Read_Buffer_Size (V2) CommandComplete event to `proxy` to
  // request the reservation of a number of LE ACL send credits.
  Status SendLeReadBufferResponseFromController(
      ProxyHost& proxy,
      uint8_t num_credits_to_reserve,
      uint16_t le_acl_data_packet_length = 251);

  Status SendReadBufferResponseFromController(
      ProxyHost& proxy,
      uint8_t num_credits_to_reserve,
      uint16_t acl_data_packet_length = 0xFFFF);

  // Send a Number_of_Completed_Packets event to `proxy` that reports each
  // {connection handle, number of completed packets} entry provided.
  Status SendNumberOfCompletedPackets(
      ProxyHost& proxy,
      std::initializer_list<std::pair<uint16_t, uint16_t>>
          packets_per_connection);

  // Send a Connection_Complete event to `proxy` indicating the provided
  // `handle` has disconnected.
  Status SendConnectionCompleteEvent(ProxyHost& proxy,
                                     uint16_t handle,
                                     emboss::StatusCode status);

  // Send a LE_Connection_Complete event to `proxy` indicating the provided
  // `handle` has disconnected.
  Status SendLeConnectionCompleteEvent(ProxyHost& proxy,
                                       uint16_t handle,
                                       emboss::StatusCode status);

  // Send a Disconnection_Complete event to `proxy` indicating the provided
  // `handle` has disconnected.
  Status SendDisconnectionCompleteEvent(
      ProxyHost& proxy,
      uint16_t handle,
      Direction direction = Direction::kFromController,
      bool successful = true);

  Status SendL2capConnectionReq(ProxyHost& proxy,
                                Direction direction,
                                uint16_t handle,
                                uint16_t source_cid,
                                uint16_t psm);

  Status SendL2capConfigureReq(ProxyHost& proxy,
                               Direction direction,
                               uint16_t handle,
                               uint16_t destination_cid,
                               L2capOptions& l2cap_options);

  Status SendL2capConfigureRsp(ProxyHost& proxy,
                               Direction direction,
                               uint16_t handle,
                               uint16_t local_cid,
                               emboss::L2capConfigurationResult result);

  Status SendL2capConnectionRsp(
      ProxyHost& proxy,
      Direction direction,
      uint16_t handle,
      uint16_t source_cid,
      uint16_t destination_cid,

      emboss::L2capConnectionRspResultCode result_code);

  Status SendL2capDisconnectRsp(ProxyHost& proxy,
                                Direction direction,
                                AclTransportType transport,
                                uint16_t handle,
                                uint16_t source_cid,
                                uint16_t destination_cid);

  /// Sends an L2CAP B-Frame.
  ///
  /// This can be either a complete PDU (pdu_length == payload.size()) or an
  /// initial fragment (pdu_length > payload.size()).
  void SendL2capBFrame(ProxyHost& proxy,
                       uint16_t handle,
                       pw::span<const uint8_t> payload,
                       size_t pdu_length,
                       uint16_t channel_id);

  /// Sends an ACL frame with CONTINUING_FRAGMENT boundary flag.
  ///
  /// No L2CAP header is included.
  void SendAclContinuingFrag(ProxyHost& proxy,
                             uint16_t handle,
                             pw::span<const uint8_t> payload);

  //////////////////////////////////////////////////////////////////////////////
  // Channel acquisition.

  pw::Result<L2capCoc> BuildCocWithResult(ProxyHost& proxy,
                                          CocParameters params);

  L2capCoc BuildCoc(ProxyHost& proxy, CocParameters params);

  Result<BasicL2capChannel> BuildBasicL2capChannelWithResult(
      ProxyHost& proxy, BasicL2capParameters params);

  BasicL2capChannel BuildBasicL2capChannel(ProxyHost& proxy,
                                           BasicL2capParameters params);

  Result<GattNotifyChannel> BuildGattNotifyChannelWithResult(
      ProxyHost& proxy, GattNotifyChannelParameters params);

  GattNotifyChannel BuildGattNotifyChannel(ProxyHost& proxy,
                                           GattNotifyChannelParameters params);

  Result<UniquePtr<ChannelProxy>> BuildBasicModeChannelProxyWithResult(
      ProxyHost& proxy, BasicChannelProxyParameters&& params);

  UniquePtr<ChannelProxy> BuildBasicModeChannelProxy(
      ProxyHost& proxy, BasicChannelProxyParameters&& params);

  //////////////////////////////////////////////////////////////////////////////
  // MultiBuf utilities.

  template <typename T, size_t N>
  static FlatMultiBufInstance MultiBufFromSpan(span<T, N> buf,
                                               MultiBufAllocator& allocator) {
    ConstByteSpan bytes = as_bytes(buf);
    auto result = MultiBufAdapter::Create(allocator, bytes.size());
    PW_ASSERT(result.has_value());
    FlatMultiBufInstance buffer = std::move(result.value());
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(buffer);
    EXPECT_EQ(MultiBufAdapter::Copy(mbuf, 0, bytes), bytes.size());
    return buffer;
  }

  template <typename T, size_t N>
  FlatMultiBufInstance MultiBufFromSpan(span<T, N> buf) {
    return MultiBufFromSpan(buf, test_multibuf_allocator_);
  }

  template <typename T, size_t N>
  FlatMultiBufInstance MultiBufFromArray(const std::array<T, N>& arr) {
    return MultiBufFromSpan(pw::span{arr});
  }

  FlatMultiBufInstance MakeEmptyMultiBuf() {
    return MultiBufFromSpan(span<uint8_t>{});
  }

 private:
#if PW_BLUETOOTH_PROXY_ASYNC != 0
  ProxyHostTestDispatcher dispatcher_;
#endif  // PW_BLUETOOTH_PROXY_ASYNC

  // MultiBuf allocator for creating objects to pass to the system under
  // test (e.g. creating test packets to send to proxy host).
  MultiBufAllocatorContext<2048> test_allocator_context_;
  MultiBufAllocator& test_multibuf_allocator_ =
      test_allocator_context_.GetAllocator();

  // Default MultiBuf allocator to be passed to system under test (e.g.
  // to pass to AcquireL2capCoc).
  MultiBufAllocatorContext<2048> sut_allocator_context_;
  MultiBufAllocator& sut_multibuf_allocator_ =
      sut_allocator_context_.GetAllocator();
};

}  // namespace pw::bluetooth::proxy
