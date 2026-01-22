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

#include "pw_bluetooth_proxy/proxy_host.h"

#include <sys/types.h>

#include <array>
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
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_bluetooth_proxy/l2cap_channel_common.h"
#include "pw_bluetooth_proxy/l2cap_status_delegate.h"
#include "pw_bluetooth_proxy_private/test_utils.h"
#include "pw_containers/flat_map.h"
#include "pw_function/function.h"
#include "pw_log/log.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_sync/mutex.h"
#include "pw_thread/test_thread_context.h"
#include "pw_thread/thread.h"
#include "pw_unit_test/framework.h"
#include "pw_unit_test/status_macros.h"

namespace pw::bluetooth::proxy {
namespace {

constexpr uint16_t kConnectionHandle = 0x123;
constexpr uint16_t kConnectionHandle2 = 0x456;
constexpr uint16_t kConnectionHandle3 = 0xABC;

// Return a populated H4 command buffer of a type that proxy host doesn't
// interact with.
Status PopulateNoninteractingToControllerBuffer(H4PacketWithH4& h4_packet) {
  return CreateAndPopulateToControllerView<emboss::InquiryCommandWriter>(
             h4_packet,
             emboss::OpCode::LINK_KEY_REQUEST_REPLY,
             /*parameter_total_size=*/0)
      .status();
}

// Return a populated H4 event buffer of a type that proxy host doesn't interact
// with.
Status CreateNonInteractingToHostBuffer(H4PacketWithHci& h4_packet) {
  return CreateAndPopulateToHostEventWriter<emboss::InquiryCompleteEventWriter>(
             h4_packet, emboss::EventCode::INQUIRY_COMPLETE)
      .status();
}

// ########## Examples

// Example for docs.rst.
// Skip when using async mode.
#if PW_BLUETOOTH_PROXY_ASYNC == 0
TEST(Example, ExampleUsage) {
  pw::allocator::test::AllocatorForTest<4096> allocator;

  // Populate H4 buffer to send towards controller.
  std::array<uint8_t, emboss::InquiryCommandView::SizeInBytes() + 1>
      h4_array_from_host{};
  H4PacketWithH4 h4_packet_from_host{emboss::H4PacketType::UNKNOWN,
                                     h4_array_from_host};
  PW_TEST_EXPECT_OK(
      PopulateNoninteractingToControllerBuffer(h4_packet_from_host));

  // Populate H4 buffer to send towards host.
  std::array<uint8_t, emboss::InquiryCompleteEventView::SizeInBytes() + 1>
      hci_array_from_controller{};
  H4PacketWithHci h4_packet_from_controller{emboss::H4PacketType::UNKNOWN,
                                            hci_array_from_controller};

  PW_TEST_EXPECT_OK(
      CreateNonInteractingToHostBuffer(h4_packet_from_controller));

  pw::Function<void(H4PacketWithHci && packet)> container_send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  pw::Function<void(H4PacketWithH4 && packet)> container_send_to_controller_fn(
      ([]([[maybe_unused]] H4PacketWithH4&& packet) {}));

  // DOCSTAG: [pw_bluetooth_proxy-examples-basic]

#include "pw_bluetooth_proxy/proxy_host.h"

  // Container creates ProxyHost .
  ProxyHost proxy = ProxyHost(std::move(container_send_to_host_fn),
                              std::move(container_send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              &allocator);

  // Container passes H4 packets from host through proxy. Proxy will in turn
  // call the container-provided `container_send_to_controller_fn` to pass them
  // on to the controller. Some packets may be modified, added, or removed.
  proxy.HandleH4HciFromHost(std::move(h4_packet_from_host));

  // Container passes H4 packets from controller through proxy. Proxy will in
  // turn call the container-provided `container_send_to_host_fn` to pass them
  // on to the controller. Some packets may be modified, added, or removed.
  proxy.HandleH4HciFromController(std::move(h4_packet_from_controller));

  // DOCSTAG: [pw_bluetooth_proxy-examples-basic]
}
#endif  // PW_BLUETOOTH_PROXY_ASYNC

// ########## PassthroughTest

class PassthroughTest : public ProxyHostTest {};

// Verify buffer is properly passed (contents unaltered and zero-copy).
TEST_F(PassthroughTest, ToControllerPassesEqualBuffer) {
  std::array<uint8_t, emboss::InquiryCommandView::SizeInBytes() + 1> h4_arr{};
  H4PacketWithH4 h4_packet{emboss::H4PacketType::UNKNOWN, h4_arr};
  PW_TEST_EXPECT_OK(PopulateNoninteractingToControllerBuffer(h4_packet));

  // Struct for capturing because `pw::Function` can't fit multiple captures.
  struct {
    // Use a copy for comparison to catch if proxy incorrectly changes the
    // passed buffer.
    std::array<uint8_t, emboss::InquiryCommandView::SizeInBytes() + 1> h4_arr;
    H4PacketWithH4* h4_packet;
    uint8_t sends_called;
  } send_capture = {h4_arr, &h4_packet, 0};

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&send_capture](H4PacketWithH4&& packet) {
        send_capture.sends_called++;
        EXPECT_EQ(packet.GetH4Type(),
                  emboss::H4PacketType(send_capture.h4_arr[0]));
        EXPECT_TRUE(std::equal(send_capture.h4_packet->GetHciSpan().begin(),
                               send_capture.h4_packet->GetHciSpan().end(),
                               send_capture.h4_arr.begin() + 1,
                               send_capture.h4_arr.end()));
        // Verify no copy by verifying buffer is at the same memory location.
        EXPECT_EQ(packet.GetHciSpan().data(),
                  send_capture.h4_packet->GetHciSpan().data());
      });

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromHost(std::move(h4_packet));

  // Verify to controller callback was called.
  EXPECT_EQ(send_capture.sends_called, 1);
}

// Verify buffer is properly passed (contents unaltered and zero-copy).
TEST_F(PassthroughTest, ToHostPassesEqualBuffer) {
  std::array<uint8_t, emboss::InquiryCompleteEventView::SizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::UNKNOWN, hci_arr};
  PW_TEST_EXPECT_OK(CreateNonInteractingToHostBuffer(h4_packet));

  // Struct for capturing because `pw::Function` can't fit multiple captures.
  struct {
    // Use a copy for comparison to catch if proxy incorrectly changes the
    // passed buffer.
    std::array<uint8_t, emboss::InquiryCompleteEventView::SizeInBytes()>
        hci_arr;
    H4PacketWithHci* h4_packet;
    uint8_t sends_called;
  } send_capture = {hci_arr, &h4_packet, 0};

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&send_capture](H4PacketWithHci&& packet) {
        send_capture.sends_called++;
        EXPECT_EQ(packet.GetH4Type(), send_capture.h4_packet->GetH4Type());
        EXPECT_TRUE(std::equal(send_capture.h4_packet->GetHciSpan().begin(),
                               send_capture.h4_packet->GetHciSpan().end(),
                               send_capture.h4_packet->GetHciSpan().begin(),
                               send_capture.h4_packet->GetHciSpan().end()));
        // Verify no copy by verifying buffer is at the same memory location.
        EXPECT_EQ(packet.GetHciSpan().data(),
                  send_capture.h4_packet->GetHciSpan().data());
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  // Verify to controller callback was called.
  EXPECT_EQ(send_capture.sends_called, 1);
}

// Verify a command complete event (of a type that proxy doesn't act on) is
// properly passed (contents unaltered and zero-copy).
TEST_F(PassthroughTest, ToHostPassesEqualCommandComplete) {
  std::array<
      uint8_t,
      emboss::ReadLocalVersionInfoCommandCompleteEventWriter::SizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::UNKNOWN, hci_arr};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto view,
      CreateAndPopulateToHostEventWriter<
          emboss::ReadLocalVersionInfoCommandCompleteEventWriter>(
          h4_packet, emboss::EventCode::COMMAND_COMPLETE));
  view.command_complete().command_opcode().Write(
      emboss::OpCode::READ_LOCAL_VERSION_INFO);

  // Struct for capturing because `pw::Function` can't fit multiple captures.
  struct {
    std::array<
        uint8_t,
        emboss::ReadLocalVersionInfoCommandCompleteEventWriter::SizeInBytes()>
        hci_arr;
    H4PacketWithHci* h4_packet;
    uint8_t sends_called;
  } send_capture = {hci_arr, &h4_packet, 0};

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&send_capture](H4PacketWithHci&& packet) {
        send_capture.sends_called++;
        EXPECT_EQ(packet.GetH4Type(), send_capture.h4_packet->GetH4Type());
        EXPECT_TRUE(std::equal(send_capture.h4_packet->GetHciSpan().begin(),
                               send_capture.h4_packet->GetHciSpan().end(),
                               send_capture.h4_packet->GetHciSpan().begin(),
                               send_capture.h4_packet->GetHciSpan().end()));
        // Verify no copy by verifying buffer is at the same memory location.
        EXPECT_EQ(packet.GetHciSpan().data(),
                  send_capture.h4_packet->GetHciSpan().data());
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  // Verify to controller callback was called.
  EXPECT_EQ(send_capture.sends_called, 1);
}

// ########## BadPacketTest
// The proxy should not affect buffers it can't process (it should just pass
// them on).

class BadPacketTest : public ProxyHostTest {};

TEST_F(BadPacketTest, BadH4TypeToControllerIsPassedOn) {
  std::array<uint8_t, emboss::InquiryCommandView::SizeInBytes() + 1> h4_arr{};
  H4PacketWithH4 h4_packet{emboss::H4PacketType::UNKNOWN, h4_arr};
  PW_TEST_EXPECT_OK(PopulateNoninteractingToControllerBuffer(h4_packet));
  // Set back to an invalid type (after
  // PopulateNoninteractingToControllerBuffer).
  h4_packet.SetH4Type(emboss::H4PacketType::UNKNOWN);

  // Struct for capturing because `pw::Function` can't fit multiple captures.
  struct {
    // Use a copy for comparison to catch if proxy incorrectly changes the
    // passed buffer.
    std::array<uint8_t, emboss::InquiryCommandView::SizeInBytes() + 1> h4_arr;
    H4PacketWithH4* h4_packet;
    uint8_t sends_called;
  } send_capture = {h4_arr, &h4_packet, 0};

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&send_capture](H4PacketWithH4&& packet) {
        send_capture.sends_called++;
        EXPECT_EQ(packet.GetH4Type(),
                  emboss::H4PacketType(send_capture.h4_arr[0]));
        EXPECT_TRUE(std::equal(send_capture.h4_packet->GetHciSpan().begin(),
                               send_capture.h4_packet->GetHciSpan().end(),
                               send_capture.h4_arr.begin() + 1,
                               send_capture.h4_arr.end()));
        // Verify no copy by verifying buffer is at the same memory location.
        EXPECT_EQ(packet.GetHciSpan().data(),
                  send_capture.h4_packet->GetHciSpan().data());
      });

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromHost(std::move(h4_packet));

  // Verify to controller callback was called.
  EXPECT_EQ(send_capture.sends_called, 1);
}

TEST_F(BadPacketTest, BadH4TypeToHostIsPassedOn) {
  std::array<uint8_t, emboss::InquiryCompleteEventView::SizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::UNKNOWN, hci_arr};
  PW_TEST_EXPECT_OK(CreateNonInteractingToHostBuffer(h4_packet));

  // Set back to an invalid type.
  h4_packet.SetH4Type(emboss::H4PacketType::UNKNOWN);

  // Struct for capturing because `pw::Function` can't fit multiple captures.
  struct {
    // Use a copy for comparison to catch if proxy incorrectly changes the
    // passed buffer.
    std::array<uint8_t, emboss::InquiryCompleteEventView::SizeInBytes()>
        hci_arr;
    H4PacketWithHci* h4_packet;
    uint8_t sends_called = 0;
  } send_capture = {hci_arr, &h4_packet, 0};

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&send_capture](H4PacketWithHci&& packet) {
        send_capture.sends_called++;
        EXPECT_EQ(packet.GetH4Type(), emboss::H4PacketType::UNKNOWN);
        EXPECT_TRUE(std::equal(send_capture.h4_packet->GetHciSpan().begin(),
                               send_capture.h4_packet->GetHciSpan().end(),
                               send_capture.h4_packet->GetHciSpan().begin(),
                               send_capture.h4_packet->GetHciSpan().end()));
        // Verify no copy by verifying buffer is at the same memory location.
        EXPECT_EQ(packet.GetHciSpan().data(),
                  send_capture.h4_packet->GetHciSpan().data());
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  // Verify to controller callback was called.
  EXPECT_EQ(send_capture.sends_called, 1);
}

TEST_F(BadPacketTest, EmptyBufferToControllerIsPassedOn) {
  std::array<uint8_t, 0> h4_arr;
  H4PacketWithH4 h4_packet{emboss::H4PacketType::COMMAND, h4_arr};
  // H4PacketWithH4 use the underlying h4 buffer to store type. Since its length
  // is zero, it can't store it and will always return UNKNOWN.
  EXPECT_EQ(h4_packet.GetH4Type(), emboss::H4PacketType::UNKNOWN);

  uint8_t sends_called = 0;
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sends_called](H4PacketWithH4&& packet) {
        sends_called++;
        EXPECT_EQ(packet.GetH4Type(), emboss::H4PacketType::UNKNOWN);
        EXPECT_TRUE(packet.GetHciSpan().empty());
      });

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromHost(std::move(h4_packet));

  // Verify callback was called.
  EXPECT_EQ(sends_called, 1);
}

TEST_F(BadPacketTest, EmptyBufferToHostIsPassedOn) {
  std::array<uint8_t, 0> hci_arr;
  H4PacketWithHci h4_packet{emboss::H4PacketType::EVENT, hci_arr};

  uint8_t sends_called = 0;
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sends_called](H4PacketWithHci&& packet) {
        sends_called++;
        EXPECT_EQ(packet.GetH4Type(), emboss::H4PacketType::EVENT);
        EXPECT_TRUE(packet.GetHciSpan().empty());
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  // Verify callback was called.
  EXPECT_EQ(sends_called, 1);
}

TEST_F(BadPacketTest, TooShortEventToHostIsPassOn) {
  std::array<uint8_t, emboss::InquiryCompleteEventView::SizeInBytes()>
      valid_hci_arr{};
  H4PacketWithHci valid_packet{emboss::H4PacketType::UNKNOWN, valid_hci_arr};
  PW_TEST_EXPECT_OK(CreateNonInteractingToHostBuffer(valid_packet));

  // Create packet for sending whose span size is one less than a valid command
  // complete event.
  H4PacketWithHci h4_packet{valid_packet.GetH4Type(),
                            valid_packet.GetHciSpan().subspan(
                                0, emboss::EventHeaderView::SizeInBytes() - 1)};

  // Struct for capturing because `pw::Function` can't fit multiple captures.
  struct {
    std::array<uint8_t, emboss::EventHeaderView::SizeInBytes() - 1> hci_arr;
    uint8_t sends_called = 0;
  } send_capture;
  // Copy valid event into a short_array whose size is one less than a valid
  // EventHeader.
  std::copy(h4_packet.GetHciSpan().begin(),
            h4_packet.GetHciSpan().end(),
            send_capture.hci_arr.begin());
  send_capture.sends_called = 0;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&send_capture](H4PacketWithHci&& packet) {
        send_capture.sends_called++;
        EXPECT_TRUE(std::equal(packet.GetHciSpan().begin(),
                               packet.GetHciSpan().end(),
                               send_capture.hci_arr.begin(),
                               send_capture.hci_arr.end()));
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  // Verify callback was called.
  EXPECT_EQ(send_capture.sends_called, 1);
}

TEST_F(BadPacketTest, TooShortCommandCompleteEventToHost) {
  std::array<
      uint8_t,
      emboss::ReadLocalVersionInfoCommandCompleteEventWriter::SizeInBytes()>
      valid_hci_arr{};
  H4PacketWithHci valid_packet{emboss::H4PacketType::UNKNOWN, valid_hci_arr};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto view,
      CreateAndPopulateToHostEventWriter<
          emboss::ReadLocalVersionInfoCommandCompleteEventWriter>(
          valid_packet, emboss::EventCode::COMMAND_COMPLETE));
  view.command_complete().command_opcode().Write(
      emboss::OpCode::READ_LOCAL_VERSION_INFO);

  // Create packet for sending whose span size is one less than a valid command
  // complete event.
  H4PacketWithHci h4_packet{
      valid_packet.GetH4Type(),
      valid_packet.GetHciSpan().subspan(
          0,
          emboss::ReadLocalVersionInfoCommandCompleteEventWriter::
                  SizeInBytes() -
              1)};

  // Struct for capturing because `pw::Function` capture can't fit multiple
  // fields .
  struct {
    std::array<
        uint8_t,
        emboss::ReadLocalVersionInfoCommandCompleteEventWriter::SizeInBytes() -
            1>
        hci_arr;
    uint8_t sends_called = 0;
  } send_capture;
  std::copy(h4_packet.GetHciSpan().begin(),
            h4_packet.GetHciSpan().end(),
            send_capture.hci_arr.begin());
  send_capture.sends_called = 0;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&send_capture](H4PacketWithHci&& packet) {
        send_capture.sends_called++;
        EXPECT_TRUE(std::equal(packet.GetHciSpan().begin(),
                               packet.GetHciSpan().end(),
                               send_capture.hci_arr.begin(),
                               send_capture.hci_arr.end()));
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  // Verify callback was called.
  EXPECT_EQ(send_capture.sends_called, 1);
}

// ########## ReserveLeAclCreditsTest

class ReserveLeAclCreditsTest : public ProxyHostTest {};

// Proxy Host should reserve requested ACL credits from controller's ACL credits
// when using ReadBufferSize command.
TEST_F(ReserveLeAclCreditsTest, ProxyCreditsReserveCreditsWithReadBufferSize) {
  std::array<uint8_t,
             emboss::ReadBufferSizeCommandCompleteEventWriter::SizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::UNKNOWN, hci_arr};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto view,
      CreateAndPopulateToHostEventWriter<
          emboss::ReadBufferSizeCommandCompleteEventWriter>(
          h4_packet, emboss::EventCode::COMMAND_COMPLETE));
  view.command_complete().command_opcode().Write(
      emboss::OpCode::READ_BUFFER_SIZE);
  view.total_num_acl_data_packets().Write(10);
  view.acl_data_packet_length().Write(20);

  uint8_t sends_called = 0;
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sends_called](H4PacketWithHci&& received_packet) {
        sends_called++;
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_view,
            MakeEmbossWriter<emboss::ReadBufferSizeCommandCompleteEventWriter>(
                received_packet.GetHciSpan()));
        // Should reserve 2 credits from original total of 10 (so 8 left for
        // host).
        EXPECT_EQ(event_view.total_num_acl_data_packets().Read(), 8);
        EXPECT_EQ(event_view.acl_data_packet_length().Read(), 20);
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/2,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(proxy.GetNumFreeBrEdrAclPackets(), 2);

  EXPECT_TRUE(proxy.HasSendBrEdrAclCapability());

  // Verify to controller callback was called.
  EXPECT_EQ(sends_called, 1);
}

// Proxy Host should reserve requested ACL LE credits from controller's ACL LE
// credits when using LEReadBufferSizeV1 command.
TEST_F(ReserveLeAclCreditsTest,
       ProxyCreditsReserveCreditsWithLEReadBufferSizeV1) {
  std::array<
      uint8_t,
      emboss::LEReadBufferSizeV1CommandCompleteEventWriter::SizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::UNKNOWN, hci_arr};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto view,
      CreateAndPopulateToHostEventWriter<
          emboss::LEReadBufferSizeV1CommandCompleteEventWriter>(
          h4_packet, emboss::EventCode::COMMAND_COMPLETE));
  view.command_complete().command_opcode().Write(
      emboss::OpCode::LE_READ_BUFFER_SIZE_V1);
  view.total_num_le_acl_data_packets().Write(10);

  uint8_t sends_called = 0;
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sends_called](H4PacketWithHci&& received_packet) {
        sends_called++;
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_view,
            MakeEmbossView<
                emboss::LEReadBufferSizeV1CommandCompleteEventWriter>(
                received_packet.GetHciSpan()));

        // Should reserve 2 credits from original total of 10 (so 8 left for
        // host).
        EXPECT_EQ(event_view.total_num_le_acl_data_packets().Read(), 8);
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 2);

  EXPECT_TRUE(proxy.HasSendLeAclCapability());

  // Verify to controller callback was called.
  EXPECT_EQ(sends_called, 1);
}

// Proxy Host should reserve requested ACL LE credits from controller's ACL LE
// credits when using LEReadBufferSizeV2 command.
TEST_F(ReserveLeAclCreditsTest,
       ProxyCreditsReserveCreditsWithLEReadBufferSizeV2) {
  std::array<
      uint8_t,
      emboss::LEReadBufferSizeV2CommandCompleteEventWriter::SizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::UNKNOWN, hci_arr};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto view,
      CreateAndPopulateToHostEventWriter<
          emboss::LEReadBufferSizeV2CommandCompleteEventWriter>(
          h4_packet, emboss::EventCode::COMMAND_COMPLETE));
  view.command_complete().command_opcode().Write(
      emboss::OpCode::LE_READ_BUFFER_SIZE_V2);
  view.total_num_le_acl_data_packets().Write(10);

  uint8_t sends_called = 0;
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sends_called](H4PacketWithHci&& received_packet) {
        sends_called++;
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_view,
            MakeEmbossView<
                emboss::LEReadBufferSizeV2CommandCompleteEventWriter>(
                received_packet.GetHciSpan()));
        // Should reserve 2 credits from original total of 10 (so 8 left for
        // host).
        EXPECT_EQ(event_view.total_num_le_acl_data_packets().Read(), 8);
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 2);

  EXPECT_TRUE(proxy.HasSendLeAclCapability());

  // Verify to controller callback was called.
  EXPECT_EQ(sends_called, 1);
}

// If controller provides less than wanted credits, we should reserve that
// smaller amount.
TEST_F(ReserveLeAclCreditsTest, ProxyCreditsCappedByControllerCredits) {
  std::array<
      uint8_t,
      emboss::LEReadBufferSizeV1CommandCompleteEventWriter::SizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::UNKNOWN, hci_arr};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto view,
      CreateAndPopulateToHostEventWriter<
          emboss::LEReadBufferSizeV1CommandCompleteEventWriter>(
          h4_packet, emboss::EventCode::COMMAND_COMPLETE));
  view.command_complete().command_opcode().Write(
      emboss::OpCode::LE_READ_BUFFER_SIZE_V1);
  view.total_num_le_acl_data_packets().Write(5);

  uint8_t sends_called = 0;
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sends_called](H4PacketWithHci&& received_packet) {
        sends_called++;
        // We want 7, but can reserve only 5 from original 5 (so 0 left for
        // host).
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_view,
            MakeEmbossView<
                emboss::LEReadBufferSizeV1CommandCompleteEventWriter>(
                received_packet.GetHciSpan()));
        EXPECT_EQ(event_view.total_num_le_acl_data_packets().Read(), 0);
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/7,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 5);

  // Verify to controller callback was called.
  EXPECT_EQ(sends_called, 1);
}

// Proxy Host can reserve zero credits from controller's ACL LE credits.
TEST_F(ReserveLeAclCreditsTest, ProxyCreditsReserveZeroCredits) {
  std::array<
      uint8_t,
      emboss::LEReadBufferSizeV1CommandCompleteEventWriter::SizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::UNKNOWN, hci_arr};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto view,
      CreateAndPopulateToHostEventWriter<
          emboss::LEReadBufferSizeV1CommandCompleteEventWriter>(
          h4_packet, emboss::EventCode::COMMAND_COMPLETE));
  view.command_complete().command_opcode().Write(
      emboss::OpCode::LE_READ_BUFFER_SIZE_V1);
  view.total_num_le_acl_data_packets().Write(10);

  uint8_t sends_called = 0;
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sends_called](H4PacketWithHci&& received_packet) {
        sends_called++;
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_view,
            MakeEmbossView<
                emboss::LEReadBufferSizeV1CommandCompleteEventWriter>(
                received_packet.GetHciSpan()));
        // Should reserve 0 credits from original total of 10 (so 10 left for
        // host).
        EXPECT_EQ(event_view.total_num_le_acl_data_packets().Read(), 10);
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);

  EXPECT_FALSE(proxy.HasSendLeAclCapability());

  // Verify to controller callback was called.
  EXPECT_EQ(sends_called, 1);
}

// If controller has no credits, proxy should reserve none.
TEST_F(ReserveLeAclCreditsTest, ProxyCreditsZeroWhenHostCreditsZero) {
  std::array<
      uint8_t,
      emboss::LEReadBufferSizeV1CommandCompleteEventWriter::SizeInBytes()>
      hci_arr{};
  H4PacketWithHci h4_packet{emboss::H4PacketType::UNKNOWN, hci_arr};
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto view,
      CreateAndPopulateToHostEventWriter<
          emboss::LEReadBufferSizeV1CommandCompleteEventWriter>(
          h4_packet, emboss::EventCode::COMMAND_COMPLETE));
  view.command_complete().command_opcode().Write(
      emboss::OpCode::LE_READ_BUFFER_SIZE_V1);
  view.total_num_le_acl_data_packets().Write(0);

  uint8_t sends_called = 0;
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&sends_called](H4PacketWithHci&& received_packet) {
        sends_called++;
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_view,
            MakeEmbossView<
                emboss::LEReadBufferSizeV1CommandCompleteEventWriter>(
                received_packet.GetHciSpan()));
        // Should reserve 0 credit from original total of 0 (so 0 left for
        // host).
        EXPECT_EQ(event_view.total_num_le_acl_data_packets().Read(), 0);
      });

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);

  EXPECT_TRUE(proxy.HasSendLeAclCapability());

  // Verify to controller callback was called.
  EXPECT_EQ(sends_called, 1);
}

TEST_F(ReserveLeAclCreditsTest, ProxyCreditsZeroWhenNotInitialized) {
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);

  EXPECT_TRUE(proxy.HasSendLeAclCapability());
}

// ########## NumberOfCompletedPacketsTest

class NumberOfCompletedPacketsTest : public ProxyHostTest {};

TEST_F(NumberOfCompletedPacketsTest, TwoOfThreeSentPacketsComplete) {
  constexpr size_t kNumConnections = 3;
  struct {
    int sends_called = 0;
    const std::array<uint16_t, kNumConnections> connection_handles = {
        0x123, 0x456, 0x789};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_header,
            MakeEmbossView<emboss::EventHeaderView>(packet.GetHciSpan().subspan(
                0, emboss::EventHeader::IntrinsicSizeInBytes())));
        capture.sends_called++;
        if (event_header.event_code().Read() !=
            emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS) {
          return;
        }

        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto view,
            MakeEmbossView<emboss::NumberOfCompletedPacketsEventView>(
                packet.GetHciSpan()));
        EXPECT_EQ(packet.GetHciSpan().size(), 15ul);
        EXPECT_EQ(view.num_handles().Read(), capture.connection_handles.size());
        EXPECT_EQ(view.header().event_code().Read(),
                  emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);

        // Proxy should have reclaimed 1 credit from Connection 0 (leaving 0
        // credits in packet), no credits from Connection 1 (meaning 0 will be
        // unchanged), and 1 credit from Connection 2 (leaving 0).
        EXPECT_EQ(view.nocp_data()[0].connection_handle().Read(),
                  capture.connection_handles[0]);
        EXPECT_EQ(view.nocp_data()[0].num_completed_packets().Read(), 0);

        EXPECT_EQ(view.nocp_data()[1].connection_handle().Read(),
                  capture.connection_handles[1]);
        EXPECT_EQ(view.nocp_data()[1].num_completed_packets().Read(), 0);

        EXPECT_EQ(view.nocp_data()[2].connection_handle().Read(),
                  capture.connection_handles[2]);
        EXPECT_EQ(view.nocp_data()[2].num_completed_packets().Read(), 0);
      });
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/kNumConnections,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kNumConnections));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, capture.connection_handles[0], emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, capture.connection_handles[1], emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, capture.connection_handles[2], emboss::StatusCode::SUCCESS));
  EXPECT_EQ(capture.sends_called, 4);

  std::array<uint8_t, 1> attribute_value = {7};

  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 3);

  // Send packet; num free packets should decrement.
  {
    GattNotifyChannel channel = BuildGattNotifyChannel(
        proxy, {.handle = capture.connection_handles[0]});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 2);
    // Proxy host took all credits so will not pass NOCP on to host.
    EXPECT_EQ(capture.sends_called, 4);
  }

  // Send packet over Connection 1, which will not have a packet completed in
  // the Number_of_Completed_Packets event.
  {
    GattNotifyChannel channel = BuildGattNotifyChannel(
        proxy, {.handle = capture.connection_handles[1]});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 1);
  }

  // Send third packet; num free packets should decrement again.
  {
    GattNotifyChannel channel = BuildGattNotifyChannel(
        proxy, {.handle = capture.connection_handles[2]});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
  }

  // Send Number_of_Completed_Packets event that reports 1 packet on Connection
  // 0, 0 packets on Connection 1, and 1 packet on Connection 2. Checks in
  // send_to_host_fn will ensure we have reclaimed 2 of 3 credits.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy,
                                   {{capture.connection_handles[0], 1},
                                    {capture.connection_handles[1], 0},
                                    {capture.connection_handles[2], 1}}));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 2);
  // Proxy host took all credits so will not pass NOCP event on to host.
  EXPECT_EQ(capture.sends_called, 4);
}

TEST_F(NumberOfCompletedPacketsTest,
       ManyMorePacketsCompletedThanPacketsPending) {
  constexpr size_t kNumConnections = 2;
  struct {
    int sends_called = 0;
    const std::array<uint16_t, kNumConnections> connection_handles = {0x123,
                                                                      0x456};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_header,
            MakeEmbossView<emboss::EventHeaderView>(packet.GetHciSpan().subspan(
                0, emboss::EventHeader::IntrinsicSizeInBytes())));
        capture.sends_called++;
        if (event_header.event_code().Read() !=
            emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS) {
          return;
        }

        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto view,
            MakeEmbossView<emboss::NumberOfCompletedPacketsEventView>(
                packet.GetHciSpan()));
        EXPECT_EQ(packet.GetHciSpan().size(), 11ul);
        EXPECT_EQ(view.num_handles().Read(), capture.connection_handles.size());
        EXPECT_EQ(view.header().event_code().Read(),
                  emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);

        // Proxy should have reclaimed 1 credit from Connection 0 (leaving
        // 9 credits in packet) and 1 credit from Connection 2 (leaving 14).
        EXPECT_EQ(view.nocp_data()[0].connection_handle().Read(),
                  capture.connection_handles[0]);
        EXPECT_EQ(view.nocp_data()[0].num_completed_packets().Read(), 9);

        EXPECT_EQ(view.nocp_data()[1].connection_handle().Read(),
                  capture.connection_handles[1]);
        EXPECT_EQ(view.nocp_data()[1].num_completed_packets().Read(), 14);
      });
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 2));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, capture.connection_handles[0], emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, capture.connection_handles[1], emboss::StatusCode::SUCCESS));
  EXPECT_EQ(capture.sends_called, 3);

  std::array<uint8_t, 1> attribute_value = {0};

  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 2);

  // Send packet over Connection 0; num free packets should decrement.
  {
    GattNotifyChannel channel = BuildGattNotifyChannel(
        proxy, {.handle = capture.connection_handles[0]});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 1);
  }

  // Send packet over Connection 1; num free packets should decrement again.
  {
    GattNotifyChannel channel = BuildGattNotifyChannel(
        proxy, {.handle = capture.connection_handles[1]});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
  }

  // Send Number_of_Completed_Packets event that reports 10 packets on
  // Connection 0 and 15 packets on Connection 1. Checks in send_to_host_fn
  // will ensure we have reclaimed exactly 2 credits, 1 from each Connection.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy,
                                   {{capture.connection_handles[0], 10},
                                    {capture.connection_handles[1], 15}}));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 2);
  EXPECT_EQ(capture.sends_called, 4);
}

TEST_F(NumberOfCompletedPacketsTest, ProxyReclaimsOnlyItsUsedCredits) {
  constexpr size_t kNumConnections = 2;
  struct {
    int sends_called = 0;
    const std::array<uint16_t, kNumConnections> connection_handles = {
        kConnectionHandle, kConnectionHandle2};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_header,
            MakeEmbossView<emboss::EventHeaderView>(packet.GetHciSpan().subspan(
                0, emboss::EventHeader::IntrinsicSizeInBytes())));
        capture.sends_called++;
        if (event_header.event_code().Read() !=
            emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS) {
          return;
        }

        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto view,
            MakeEmbossView<emboss::NumberOfCompletedPacketsEventView>(
                packet.GetHciSpan()));
        EXPECT_EQ(packet.GetHciSpan().size(), 11ul);
        EXPECT_EQ(view.num_handles().Read(), 2);
        EXPECT_EQ(view.header().event_code().Read(),
                  emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);

        // Proxy has 4 credits it wants to reclaim, but it should have only
        // reclaimed the 2 credits it used on Connection 0.
        EXPECT_EQ(view.nocp_data()[0].connection_handle().Read(),
                  capture.connection_handles[0]);
        EXPECT_EQ(view.nocp_data()[0].num_completed_packets().Read(), 8);
        EXPECT_EQ(view.nocp_data()[1].connection_handle().Read(),
                  capture.connection_handles[1]);
        EXPECT_EQ(view.nocp_data()[1].num_completed_packets().Read(), 15);
      });
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/4,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 4));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, capture.connection_handles[0], emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, capture.connection_handles[1], emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle3, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(capture.sends_called, 4);

  std::array<uint8_t, 1> attribute_value = {0};

  // Use 2 credits on Connection 0 and 2 credits on random connections that will
  // not be included in the NOCP event.
  {
    GattNotifyChannel channel = BuildGattNotifyChannel(
        proxy, {.handle = capture.connection_handles[0]});
    FlatMultiBufInstance mbuf1_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf1 = MultiBufAdapter::Unwrap(mbuf1_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf1)).status);
    RunDispatcher();

    FlatMultiBufInstance mbuf2_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf2 = MultiBufAdapter::Unwrap(mbuf2_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf2)).status);
    RunDispatcher();
  }
  {
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle3});
    FlatMultiBufInstance mbuf1_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf1 = MultiBufAdapter::Unwrap(mbuf1_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf1)).status);
    RunDispatcher();

    FlatMultiBufInstance mbuf2_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf2 = MultiBufAdapter::Unwrap(mbuf2_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf2)).status);
    RunDispatcher();

    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
  }

  // Send Number_of_Completed_Packets event that reports 10 packets on
  // Connection 0 and 15 packets on Connection 1. Checks in send_to_host_fn
  // will ensure we have reclaimed only 2 credits.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy,
                                   {{capture.connection_handles[0], 10},
                                    {capture.connection_handles[1], 15}}));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 2);
  // NOCP has credits remaining so will be passed on to host.
  EXPECT_EQ(capture.sends_called, 5);
}

TEST_F(NumberOfCompletedPacketsTest, EventUnmodifiedIfNoCreditsInUse) {
  constexpr size_t kNumConnections = 2;
  struct {
    int sends_called = 0;
    const std::array<uint16_t, kNumConnections> connection_handles = {0x123,
                                                                      0x456};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_header,
            MakeEmbossView<emboss::EventHeaderView>(packet.GetHciSpan().subspan(
                0, emboss::EventHeader::IntrinsicSizeInBytes())));
        capture.sends_called++;
        if (event_header.event_code().Read() !=
            emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS) {
          return;
        }

        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto view,
            MakeEmbossView<emboss::NumberOfCompletedPacketsEventView>(
                packet.GetHciSpan()));
        EXPECT_EQ(packet.GetHciSpan().size(), 11ul);
        EXPECT_EQ(view.num_handles().Read(), 2);
        EXPECT_EQ(view.header().event_code().Read(),
                  emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);

        // Event should be unmodified.
        EXPECT_EQ(view.nocp_data()[0].connection_handle().Read(),
                  capture.connection_handles[0]);
        EXPECT_EQ(view.nocp_data()[0].num_completed_packets().Read(), 10);
        EXPECT_EQ(view.nocp_data()[1].connection_handle().Read(),
                  capture.connection_handles[1]);
        EXPECT_EQ(view.nocp_data()[1].num_completed_packets().Read(), 15);
      });
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/10,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 10));
  EXPECT_EQ(capture.sends_called, 1);

  // Send Number_of_Completed_Packets event that reports 10 packets on
  // Connection 0 and 15 packets on Connection 1. Checks in send_to_host_fn
  // will ensure we have not modified the NOCP event.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy,
                                   {{capture.connection_handles[0], 10},
                                    {capture.connection_handles[1], 15}}));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 10);
  // NOCP has credits remaining so will be passed on to host.
  EXPECT_EQ(capture.sends_called, 2);
}

TEST_F(NumberOfCompletedPacketsTest, HandlesUnusualEvents) {
  constexpr size_t kNumConnections = 5;
  struct {
    int sends_called = 0;
    const std::array<uint16_t, kNumConnections> connection_handles = {
        0x123, 0x234, 0x345, 0x456, 0x567};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_header,
            MakeEmbossView<emboss::EventHeaderView>(packet.GetHciSpan().subspan(
                0, emboss::EventHeader::IntrinsicSizeInBytes())));
        capture.sends_called++;
        if (event_header.event_code().Read() !=
            emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS) {
          return;
        }

        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto view,
            MakeEmbossView<emboss::NumberOfCompletedPacketsEventView>(
                packet.GetHciSpan()));
        if (view.num_handles().Read() == 0) {
          return;
        }

        EXPECT_EQ(packet.GetHciSpan().size(), 23ul);
        EXPECT_EQ(view.num_handles().Read(), 5);
        EXPECT_EQ(view.header().event_code().Read(),
                  emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);

        // Event should be unmodified.
        for (int i = 0; i < 5; ++i) {
          EXPECT_EQ(view.nocp_data()[i].connection_handle().Read(),
                    capture.connection_handles[i]);
          EXPECT_EQ(view.nocp_data()[i].num_completed_packets().Read(), 0);
        }
      });
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/10,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 10));
  EXPECT_EQ(capture.sends_called, 1);

  // Send Number_of_Completed_Packets event with no entries.
  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(proxy, {{}}));
  // NOCP has no entries, so will not be passed on to host.
  EXPECT_EQ(capture.sends_called, 1);

  // Send Number_of_Completed_Packets event that reports 0 packets for various
  // connections.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy,
                                   {{capture.connection_handles[0], 0},
                                    {capture.connection_handles[1], 0},
                                    {capture.connection_handles[2], 0},
                                    {capture.connection_handles[3], 0},
                                    {capture.connection_handles[4], 0}}));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 10);
  // Proxy host will not pass on a NOCP with no credits.
  EXPECT_EQ(capture.sends_called, 1);
}

TEST_F(NumberOfCompletedPacketsTest, MultipleChannelsDifferentTransports) {
  static constexpr size_t kPayloadSize = 3;
  const uint16_t kLeConnectionHandle = 0x123;
  const uint16_t kBrEdrConnectionHandle = 0x456;
  struct {
    int sends_called = 0;
    std::array<uint8_t, kPayloadSize> payload = {
        0xAB,
        0xCD,
        0xEF,
    };
  } capture;

  pw::Function<void(H4PacketWithHci&&)>&& send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4&&)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&&) { ++capture.sends_called; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/1,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  // Allow proxy to reserve BR/EDR 1 credit.
  PW_TEST_EXPECT_OK(SendReadBufferResponseFromController(proxy, 1));
  // Allow proxy to reserve LE 1 credit.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kLeConnectionHandle, emboss::StatusCode::SUCCESS));
  PW_TEST_EXPECT_OK(SendConnectionCompleteEvent(
      proxy, kBrEdrConnectionHandle, emboss::StatusCode::SUCCESS));

  // Test that sending on one type of transport doesn't get blocked if the other
  // type of transport is out of credits.

  L2capCoc le_channel = BuildCoc(
      proxy, CocParameters{.handle = kLeConnectionHandle, .tx_credits = 2});
  FlatMultiBufInstance empty = MakeEmptyMultiBuf();
  PW_TEST_EXPECT_OK(
      le_channel.Write(std::move(MultiBufAdapter::Unwrap(empty))).status);
  RunDispatcher();
  EXPECT_EQ(capture.sends_called, 1);

  BasicL2capChannel bredr_channel =
      BuildBasicL2capChannel(proxy,
                             {.handle = kBrEdrConnectionHandle,
                              .transport = AclTransportType::kBrEdr});
  FlatMultiBufInstance mbuf_inst = MultiBufFromSpan(pw::span(capture.payload));
  FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
  PW_TEST_EXPECT_OK(bredr_channel.Write(std::move(mbuf)).status);
  RunDispatcher();

  // Send should succeed even though no LE credits available
  EXPECT_EQ(capture.sends_called, 2);

  // Queue an LE write
  empty = MakeEmptyMultiBuf();
  PW_TEST_EXPECT_OK(
      le_channel.Write(std::move(MultiBufAdapter::Unwrap(empty))).status);
  RunDispatcher();
  EXPECT_EQ(capture.sends_called, 2);

  // Complete previous LE write
  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(proxy, {{0x123, 1}}));
  EXPECT_EQ(capture.sends_called, 3);

  // Complete BR/EDR write
  PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(proxy, {{0x456, 1}}));

  // Write again
  mbuf_inst = MultiBufFromSpan(pw::span(capture.payload));
  PW_TEST_EXPECT_OK(
      bredr_channel.Write(std::move(MultiBufAdapter::Unwrap(mbuf_inst)))
          .status);
  RunDispatcher();
  EXPECT_EQ(capture.sends_called, 4);
}

// ########## DisconnectionCompleteTest

class DisconnectionCompleteTest : public ProxyHostTest {};

TEST_F(DisconnectionCompleteTest, DisconnectionReclaimsCredits) {
  struct {
    int sends_called = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_header,
            MakeEmbossView<emboss::EventHeaderView>(packet.GetHciSpan().subspan(
                0, emboss::EventHeader::IntrinsicSizeInBytes())));
        capture.sends_called++;
        if (event_header.event_code().Read() !=
            emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS) {
          return;
        }

        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto view,
            MakeEmbossView<emboss::NumberOfCompletedPacketsEventView>(
                packet.GetHciSpan()));
        EXPECT_EQ(packet.GetHciSpan().size(), 7ul);
        EXPECT_EQ(view.num_handles().Read(), 1);
        EXPECT_EQ(view.header().event_code().Read(),
                  emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);

        // Event should be unmodified.
        EXPECT_EQ(view.nocp_data()[0].connection_handle().Read(),
                  kConnectionHandle);
        EXPECT_EQ(view.nocp_data()[0].num_completed_packets().Read(), 10);
      });
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/10,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 10));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle3, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(capture.sends_called, 3);

  std::array<uint8_t, 1> attribute_value = {0};

  {
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});

    // Use up 3 of the 10 credits on the Connection that will be disconnected.
    for (int i = 0; i < 3; ++i) {
      FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
      FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
      PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
      RunDispatcher();
    }
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 7);
  }

  // Use up 2 credits on a random Connection.
  {
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle3});

    for (int i = 0; i < 2; ++i) {
      FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
      FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
      PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
      RunDispatcher();
    }
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 5);
  }

  // Send Disconnection_Complete event, which should reclaim 3 credits.
  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, kConnectionHandle));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 8);
  EXPECT_EQ(capture.sends_called, 4);

  // Use 1 credit and reclaim it on a bunch of random channels. Then send
  // disconnect and ensure it was cleaned up in connections list. The send will
  // fail if disconnect doesn't cleanup properly.
  //
  // We already have an active connection at this point in the test, so loop
  // over the remaining slots + 1 which would otherwise fail if cleanup wasn't
  // working right.
  size_t kNumRemainingConnections = ProxyHost::GetMaxNumAclConnections() - 2;
  for (uint16_t i = 0; i < kNumRemainingConnections; ++i) {
    uint16_t handle = 0x234 + i;
    PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
        proxy, handle, emboss::StatusCode::SUCCESS));
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = handle});

    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();

    PW_TEST_EXPECT_OK(SendNumberOfCompletedPackets(proxy, {{handle, 1}}));
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 8);
    PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, handle));
  }

  // Send Number_of_Completed_Packets event that reports 10 packets, none of
  // which should be reclaimed because this Connection has disconnected. Checks
  // in send_to_host_fn will ensure we have not modified the NOCP event.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{kConnectionHandle, 10}}));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 8);
  // NOCP has credits remaining so will be passed on to host.
  EXPECT_EQ(static_cast<size_t>(capture.sends_called),
            4 + (2 * kNumRemainingConnections) + 1);
}

TEST_F(DisconnectionCompleteTest, FailedDisconnectionHasNoEffect) {
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  std::array<uint8_t, 1> attribute_value = {0};

  // Use sole credit.
  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});
  FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
  FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
  PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
  RunDispatcher();
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);

  // Send failed Disconnection_Complete event, should not reclaim credit.
  PW_TEST_EXPECT_OK(
      SendDisconnectionCompleteEvent(proxy,
                                     kConnectionHandle,
                                     /*direction=*/Direction::kFromController,
                                     /*successful=*/false));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
}

TEST_F(DisconnectionCompleteTest, DisconnectionOfUnusedConnectionHasNoEffect) {
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  std::array<uint8_t, 1> attribute_value = {0};

  // Use sole credit.
  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});
  FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
  FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
  PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
  RunDispatcher();
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);

  // Send Disconnection_Complete event to random Connection, should have no
  // effect.
  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, 0x456));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
}

TEST_F(DisconnectionCompleteTest, CanReuseConnectionHandleAfterDisconnection) {
  struct {
    int sends_called = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_header,
            MakeEmbossView<emboss::EventHeaderView>(packet.GetHciSpan().subspan(
                0, emboss::EventHeader::IntrinsicSizeInBytes())));
        capture.sends_called++;
        if (event_header.event_code().Read() !=
            emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS) {
          return;
        }

        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto view,
            MakeEmbossView<emboss::NumberOfCompletedPacketsEventView>(
                packet.GetHciSpan()));
        EXPECT_EQ(packet.GetHciSpan().size(), 7ul);
        EXPECT_EQ(view.num_handles().Read(), 1);
        EXPECT_EQ(view.header().event_code().Read(),
                  emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);

        // Should have reclaimed the 1 packet.
        EXPECT_EQ(view.nocp_data()[0].connection_handle().Read(),
                  kConnectionHandle);
        EXPECT_EQ(view.nocp_data()[0].num_completed_packets().Read(), 0);
      });
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(capture.sends_called, 2);

  std::array<uint8_t, 1> attribute_value = {0};

  {
    // Establish connection over `connection_handle`.
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
  }

  // Disconnect
  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, kConnectionHandle));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 1);
  EXPECT_EQ(capture.sends_called, 3);

  // Reconnect
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  {
    // Re-establish connection over `connection_handle`.
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
  }

  // Send Number_of_Completed_Packets event that reports 1 packet. Checks in
  // send_to_host_fn will ensure packet has been reclaimed.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{kConnectionHandle, 1}}));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 1);
  // Since proxy reclaimed the one credit, it does not pass event on to host.
  EXPECT_EQ(capture.sends_called, 4);
}

TEST_F(DisconnectionCompleteTest, DisconnectionErasesAclConnection) {
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  int sends_called = 0;
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sends_called](H4PacketWithH4&&) { ++sends_called; });
  pw::allocator::test::AllocatorForTest<34000> allocator;
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              &allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));

  uint16_t connection_handle = 0x567;
  pw::Vector<L2capCoc, ProxyHost::GetMaxNumAclConnections()> channels;
  for (size_t i = 0; i < ProxyHost::GetMaxNumAclConnections(); ++i) {
    uint16_t handle = ++connection_handle;
    PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
        proxy, handle, emboss::StatusCode::SUCCESS));
    channels.push_back(BuildCoc(proxy, CocParameters{.handle = handle}));
  }
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, connection_handle + 1, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(
      BuildCocWithResult(
          proxy,
          CocParameters{.handle = static_cast<uint16_t>(connection_handle + 1)})
          .status(),
      Status::Unavailable());

  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, connection_handle++));
  // After erasing the last ACL connection, there should be space for a new one.
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, connection_handle, emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      L2capCoc channel,
      BuildCocWithResult(proxy, CocParameters{.handle = connection_handle}));
  // Confirm signaling channels are functional.
  PW_TEST_EXPECT_OK(channel.SendAdditionalRxCredits(3));
  RunDispatcher();

  EXPECT_EQ(sends_called, 1);
  channels.clear();
}

// ########## DestructionTest

class DestructionTest : public ProxyHostTest {};

// This test can deadlock on failure.
TEST_F(DestructionTest, CanDestructWhenPacketsQueuedInSignalingChannel) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle2, emboss::StatusCode::SUCCESS));

  L2capCoc channel =
      BuildCoc(proxy, CocParameters{.handle = kConnectionHandle});
  L2capCoc channel2 =
      BuildCoc(proxy, CocParameters{.handle = kConnectionHandle2});

  PW_TEST_EXPECT_OK(channel.SendAdditionalRxCredits(1));
}

// ########## ResetTest

class ResetTest : public ProxyHostTest {};

TEST_F(ResetTest, ResetClearsActiveConnections) {
  struct {
    int sends_called = 0;
  } host_capture;
  struct {
    int sends_called = 0;
  } controller_capture;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&host_capture](H4PacketWithHci&& packet) {
        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto event_header,
            MakeEmbossView<emboss::EventHeaderView>(packet.GetHciSpan().subspan(
                0, emboss::EventHeader::IntrinsicSizeInBytes())));
        host_capture.sends_called++;
        if (event_header.event_code().Read() !=
            emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS) {
          return;
        }

        PW_TEST_ASSERT_OK_AND_ASSIGN(
            auto view,
            MakeEmbossView<emboss::NumberOfCompletedPacketsEventView>(
                packet.GetHciSpan()));
        EXPECT_EQ(packet.GetHciSpan().size(), 7ul);
        EXPECT_EQ(view.num_handles().Read(), 1);
        EXPECT_EQ(view.header().event_code().Read(),
                  emboss::EventCode::NUMBER_OF_COMPLETED_PACKETS);

        // Should be unchanged.
        EXPECT_EQ(view.nocp_data()[0].connection_handle().Read(),
                  kConnectionHandle);
        EXPECT_EQ(view.nocp_data()[0].num_completed_packets().Read(), 1);
      });
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&controller_capture]([[maybe_unused]] H4PacketWithH4&& packet) {
        ++controller_capture.sends_called;
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 2));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(host_capture.sends_called, 2);

  std::array<uint8_t, 1> attribute_value = {0};

  {
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(controller_capture.sends_called, 1);
  }

  proxy.Reset();

  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
  // Reset should not have cleared `le_acl_credits_to_reserve`, so proxy should
  // still indicate the capability.
  EXPECT_TRUE(proxy.HasSendLeAclCapability());

  // Re-initialize AclDataChannel with 2 credits.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 2));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle2, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(host_capture.sends_called, 4);

  {
    // Send ACL on random handle to expend one credit.
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle2});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(controller_capture.sends_called, 2);
  }

  // This should have no effect, as the reset has cleared our active connection
  // on this handle.
  PW_TEST_EXPECT_OK(
      SendNumberOfCompletedPackets(proxy, {{kConnectionHandle, 1}}));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 1);
  // NOCP has credits remaining so will be passed on to host.
  EXPECT_EQ(host_capture.sends_called, 5);
}

TEST_F(ResetTest, ProxyHandlesMultipleResets) {
  int sends_called = 0;

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&sends_called]([[maybe_unused]] H4PacketWithH4&& packet) {
        ++sends_called;
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));

  proxy.Reset();
  proxy.Reset();

  std::array<uint8_t, 1> attribute_value = {0};
  // Validate state after double reset.
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
  EXPECT_TRUE(proxy.HasSendLeAclCapability());
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  {
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
  }
  EXPECT_EQ(sends_called, 1);

  proxy.Reset();

  // Validate state after third reset.
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 0);
  EXPECT_TRUE(proxy.HasSendLeAclCapability());
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  {
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
  }
  EXPECT_EQ(sends_called, 2);
}

TEST_F(ResetTest, HandleHciReset) {
  struct {
    int sends_called = 0;
  } host_capture;
  struct {
    int sends_called = 0;
  } controller_capture;

  pw::Function<void(H4PacketWithHci&&)> send_to_host_fn(
      [&host_capture](H4PacketWithHci&&) { ++host_capture.sends_called; });
  pw::Function<void(H4PacketWithH4&&)> send_to_controller_fn(
      [&controller_capture](H4PacketWithH4&&) {
        ++controller_capture.sends_called;
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/2,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 2));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(host_capture.sends_called, 2);

  // Use 1 credit.
  std::array<uint8_t, 1> attribute_value = {0};
  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});
  FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
  FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
  PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
  RunDispatcher();
  EXPECT_EQ(controller_capture.sends_called, 1);
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 1);

  // Send HCI_Reset. This should cause proxy to reset and our free credits as
  // well.
  std::array<uint8_t, emboss::ResetCommandView::SizeInBytes() + 1>
      h4_array_from_host{};
  H4PacketWithH4 h4_packet_from_host{emboss::H4PacketType::UNKNOWN,
                                     h4_array_from_host};
  PW_TEST_EXPECT_OK(
      CreateAndPopulateToControllerView<emboss::ResetCommandWriter>(
          h4_packet_from_host,
          emboss::OpCode::RESET,
          /*parameter_total_size=*/0));
  proxy.HandleH4HciFromHost(std::move(h4_packet_from_host));

  // Send new buffer response which shouldn't crash.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 2));
  EXPECT_EQ(host_capture.sends_called, 3);
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), 2);
}

// ########## MultiSendTest

class MultiSendTest : public ProxyHostTest {};

TEST_F(MultiSendTest, CanOccupyAllThenReuseEachBuffer) {
  constexpr uint8_t kAclCredits = 255;
  struct {
    size_t sends_called = 0;
    // These are packets that have been sent towards controller, but not
    // released yet by container.
    pw::Vector<H4PacketWithH4, 50> in_flight_packets{};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        // Capture all packets to prevent their destruction.
        capture.sends_called++;
        capture.in_flight_packets.push_back(std::move(packet));
      });

  allocator::test::AllocatorForTest<4096> allocator;
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/kAclCredits,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              &allocator);
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, kAclCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  std::optional<GattNotifyChannel> channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});

  std::array<uint8_t, 240> attribute_value = {};

  // Occupy all H4 buffers and queue 1 packet.
  size_t num_writes_0 = 0;
  do {
    FlatMultiBufInstance mbuf = MultiBufFromSpan(span(attribute_value));
    PW_TEST_EXPECT_OK(
        channel->Write(std::move(MultiBufAdapter::Unwrap(mbuf))).status);
    RunDispatcher();
    ++num_writes_0;
  } while (capture.sends_called == num_writes_0);
  // The final write should be queued and not sent.
  EXPECT_EQ(capture.sends_called, num_writes_0 - 1);

  // Release an H4 buffer, which should result in the queued packet being sent.
  // Move the packet first to avoid a push_back to in_flight_packets during a
  // pop_back.
  std::optional<H4PacketWithH4> last_packet =
      std::move(capture.in_flight_packets.back());
  capture.in_flight_packets.pop_back();
  last_packet.reset();
  RunDispatcher();
  EXPECT_EQ(capture.sends_called, num_writes_0);

  // Free up remaining slots.
  capture.in_flight_packets.clear();

  // There should have been no more sends since there were no payloads queued.
  EXPECT_EQ(capture.sends_called, num_writes_0);

  // Confirm we can now reoccupy each H4 buffer slot.
  size_t num_writes_1 = 0;
  capture.sends_called = 0;
  do {
    FlatMultiBufInstance mbuf = MultiBufFromSpan(span(attribute_value));
    PW_TEST_EXPECT_OK(
        channel->Write(std::move(MultiBufAdapter::Unwrap(mbuf))).status);
    RunDispatcher();
    ++num_writes_1;
  } while (capture.sends_called == num_writes_1);
  EXPECT_EQ(num_writes_0, num_writes_1);
  // The final write should be queued and not sent.
  EXPECT_EQ(capture.sends_called, num_writes_1 - 1);

  // Free all packets before destroying ProxyHost to avoid UAF.
  channel.reset();
  capture.in_flight_packets.clear();
}

TEST_F(MultiSendTest, CanRepeatedlyReuseOneBuffer) {
  constexpr size_t kAclBuffersSize = 10;
  struct {
    size_t sends_called = 0;
    // These are packets that have been sent towards controller, but not
    // released yet by container.
    pw::Vector<H4PacketWithH4, kAclBuffersSize> in_flight_packets{};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        ++capture.sends_called;
        capture.in_flight_packets.push_back(std::move(packet));
      });

  // Allow proxy to reserve enough credits for all the sends we do below.
  // simultaneous sends supported by proxy.
  constexpr size_t kTotalAclCredits = 2 * kAclBuffersSize;

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/kTotalAclCredits,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, kTotalAclCredits));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel =
      BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});

  std::array<uint8_t, 1> attribute_value = {0xF};

  // Occupy all H4 buffers.
  for (size_t sent = 1; sent <= kAclBuffersSize; ++sent) {
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    // Each write is sent towards controller
    EXPECT_EQ(capture.sends_called, sent);
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), kTotalAclCredits - sent);
    // Container holds on to each H4 buffer.
    EXPECT_EQ(capture.in_flight_packets.size(), sent);
  }

  // This was already verified in last iteration of loop above, but we EXPECT
  // explicitly here to provide reader context for EXPECTs in the loop below.
  EXPECT_EQ(capture.sends_called, kAclBuffersSize);
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), kTotalAclCredits - kAclBuffersSize);
  EXPECT_EQ(capture.in_flight_packets.size(), kAclBuffersSize);

  // Repeatedly free and reoccupy last buffer.
  for (size_t sent = 1; sent <= kAclBuffersSize; ++sent) {
    capture.in_flight_packets.pop_back();
    // No send due to release of H4 buffer since no payloads were queued.
    EXPECT_EQ(capture.sends_called, kAclBuffersSize + sent - 1);
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(),
              kTotalAclCredits - kAclBuffersSize - (sent - 1));
    // In flight packets has one free slot (which should align with one free H4
    // buffer slot).
    EXPECT_EQ(capture.in_flight_packets.size(), kAclBuffersSize - 1);

    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();

    // Send happened using that one free H4 buffer slot.
    EXPECT_EQ(capture.sends_called, kAclBuffersSize + sent);
    EXPECT_EQ(proxy.GetNumFreeLeAclPackets(),
              kTotalAclCredits - kAclBuffersSize - sent);
    // In flight packets full again, which should align with H4
    // buffers being full.
    EXPECT_EQ(capture.in_flight_packets.size(), kAclBuffersSize);
  }

  // If captured packets are not reset here, they may destruct after the proxy
  // and lead to a crash when trying to lock the proxy's destructed mutex.
  capture.in_flight_packets.clear();
}

TEST_F(MultiSendTest, CanSendOverManyDifferentConnections) {
  std::array<uint8_t, 1> attribute_value = {0xF};
  struct {
    uint16_t sends_called = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [&capture]([[maybe_unused]] H4PacketWithH4&& packet) {
        ++capture.sends_called;
      });

  pw::allocator::test::AllocatorForTest<32768> allocator;
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              ProxyHost::GetMaxNumAclConnections(),
                              /*br_edr_acl_credits_to_reserve=*/0,
                              &allocator);
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(
      proxy, ProxyHost::GetMaxNumAclConnections()));

  for (uint16_t send = 1; send <= ProxyHost::GetMaxNumAclConnections();
       send++) {
    // Use current send count as the connection handle.
    uint16_t conn_handle = send;
    PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
        proxy, conn_handle, emboss::StatusCode::SUCCESS));
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = conn_handle});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, send);
  }
}

TEST_F(MultiSendTest, AttemptToCreateOverMaxConnectionsFails) {
  constexpr uint16_t kSends = ProxyHost::GetMaxNumAclConnections() + 1;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  pw::allocator::test::AllocatorForTest<34000> allocator;
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/kSends,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              &allocator);
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, kSends));

  std::vector<GattNotifyChannel> channels;

  for (uint16_t send = 1; send <= ProxyHost::GetMaxNumAclConnections();
       send++) {
    // Use current send count as the connection handle.
    uint16_t conn_handle = send;
    PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
        proxy, conn_handle, emboss::StatusCode::SUCCESS));
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = conn_handle});
    channels.push_back(std::move(channel));
  }

  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, /*handle=*/kSends, emboss::StatusCode::SUCCESS));
  // Last one should fail
  EXPECT_EQ(
      BuildGattNotifyChannelWithResult(proxy, {.handle = kSends}).status(),
      Status::Unavailable());
}

// ########## BasicL2capChannelTest

class BasicL2capChannelTest : public ProxyHostTest {};

TEST_F(BasicL2capChannelTest, BasicWrite) {
  struct {
    int sends_called = 0;
    // First four bits 0x0 encode PB & BC flags
    uint16_t handle = 0x0ACB;
    // Length of L2CAP PDU
    uint16_t acl_data_total_length = 0x0007;
    // L2CAP header PDU length field
    uint16_t pdu_length = 0x0003;
    // Random CID
    uint16_t channel_id = 0x1234;
    // L2CAP information payload
    std::array<uint8_t, 3> payload = {0xAB, 0xCD, 0xEF};

    // Built from the preceding values in little endian order (except payload in
    // big endian).
    std::array<uint8_t, 11> expected_hci_packet = {
        0xCB, 0x0A, 0x07, 0x00, 0x03, 0x00, 0x34, 0x12, 0xAB, 0xCD, 0xEF};
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
        emboss::BFrameView bframe = emboss::MakeBFrameView(
            acl.payload().BackingStorage().data(), acl.SizeInBytes());
        EXPECT_EQ(bframe.pdu_length().Read(), capture.pdu_length);
        EXPECT_EQ(bframe.channel_id().Read(), capture.channel_id);
        for (size_t i = 0; i < 3; ++i) {
          EXPECT_EQ(bframe.payload()[i].Read(), capture.payload[i]);
        }
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  // Allow proxy to reserve 1 LE credit.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, capture.handle, emboss::StatusCode::SUCCESS));

  BasicL2capChannel channel =
      BuildBasicL2capChannel(proxy,
                             {.handle = capture.handle,
                              .local_cid = 0x123,
                              .remote_cid = capture.channel_id,
                              .transport = AclTransportType::kLe});

  FlatMultiBufInstance mbuf_inst = MultiBufFromSpan(pw::span(capture.payload));
  FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
  PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
  RunDispatcher();
  EXPECT_EQ(capture.sends_called, 1);
}

TEST_F(BasicL2capChannelTest, ErrorOnWriteTooLarge) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) { FAIL(); });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  // Allow proxy to reserve 1 credit.
  const uint16_t kAclDataPacketLength = 100;
  PW_TEST_EXPECT_OK(SendReadBufferResponseFromController(
      proxy, /*num_credits_to_reserve=*/1, kAclDataPacketLength));
  PW_TEST_ASSERT_OK(SendConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  std::array<uint8_t,
             kAclDataPacketLength -
                 emboss::BasicL2capHeader::IntrinsicSizeInBytes() + 1>
      one_byte_too_big_sdu;

  BasicL2capChannel channel =
      BuildBasicL2capChannel(proxy,
                             {.handle = kConnectionHandle,
                              .local_cid = 0x123,
                              .remote_cid = 0x123,
                              .transport = AclTransportType::kBrEdr});

  FlatMultiBufInstance mbuf_inst =
      MultiBufFromSpan(pw::span(one_byte_too_big_sdu));
  FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
  EXPECT_EQ(channel.Write(std::move(mbuf)).status, Status::InvalidArgument());
  RunDispatcher();
}

TEST_F(BasicL2capChannelTest, CannotCreateChannelWithInvalidArgs) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  // Connection handle too large by 1.
  Result<BasicL2capChannel> channel =
      BuildBasicL2capChannelWithResult(proxy,
                                       {.handle = 0x0FFF,
                                        .local_cid = 0x123,
                                        .remote_cid = 0x123,
                                        .transport = AclTransportType::kLe});
  EXPECT_EQ(channel.status(), Status::InvalidArgument());

  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  // Local CID invalid (0).
  channel =
      BuildBasicL2capChannelWithResult(proxy,
                                       BasicL2capParameters{
                                           .handle = kConnectionHandle,
                                           .local_cid = 0,
                                           .remote_cid = 0x123,
                                           .transport = AclTransportType::kLe,
                                       });
  EXPECT_EQ(channel.status(), Status::InvalidArgument());
}

TEST_F(BasicL2capChannelTest, BasicRead) {
  struct {
    int sends_called = 0;
    int to_host_called = 0;
    std::array<uint8_t, 3> expected_payload = {0xAB, 0xCD, 0xEF};
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        if (packet.GetH4Type() == emboss::H4PacketType::ACL_DATA) {
          ++capture.to_host_called;
        }
      });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  uint16_t handle = 334;
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, handle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 443;
  BasicL2capChannel channel = BuildBasicL2capChannel(
      proxy,
      BasicL2capParameters{
          .handle = handle,
          .local_cid = local_cid,
          .remote_cid = 0x123,
          .transport = AclTransportType::kLe,
          .payload_from_controller_fn =
              [&capture](FlatConstMultiBuf&& buffer) {
                ++capture.sends_called;
                PW_ASSERT(!buffer.empty());
                ConstByteSpan payload = *buffer.ConstChunks().begin();
                ConstByteSpan expected_bytes =
                    as_bytes(span(capture.expected_payload.data(),
                                  capture.expected_payload.size()));
                EXPECT_TRUE(std::equal(payload.begin(),
                                       payload.end(),
                                       expected_bytes.begin(),
                                       expected_bytes.end()));
                return std::nullopt;
              },
      });

  std::array<uint8_t,
             emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                 emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
                 capture.expected_payload.size()>
      hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, hci_arr};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(handle);
  acl->data_total_length().Write(
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
      capture.expected_payload.size());

  emboss::BFrameWriter bframe = emboss::MakeBFrameView(
      acl->payload().BackingStorage().data(), acl->payload().SizeInBytes());
  bframe.pdu_length().Write(capture.expected_payload.size());
  bframe.channel_id().Write(local_cid);
  std::copy(capture.expected_payload.begin(),
            capture.expected_payload.end(),
            hci_arr.begin() +
                emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                emboss::BasicL2capHeader::IntrinsicSizeInBytes());

  // Send ACL data packet destined for the CoC we registered.
  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(capture.sends_called, 1);
  EXPECT_EQ(capture.to_host_called, 0);
}

TEST_F(BasicL2capChannelTest, BasicForward) {
  struct {
    int sends_called = 0;
    int to_host_called = 0;
    std::array<uint8_t, 3> expected_payload = {0xAB, 0xCD, 0xEF};
    std::array<uint8_t,
               emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                   emboss::BasicL2capHeader::IntrinsicSizeInBytes() + 3>
        hci_arr{};
  } capture;

  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, capture.hci_arr};

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        if (packet.GetH4Type() != emboss::H4PacketType::ACL_DATA) {
          return;
        }
        ++capture.to_host_called;
        EXPECT_TRUE(std::equal(packet.GetHciSpan().begin(),
                               packet.GetHciSpan().end(),
                               capture.hci_arr.begin(),
                               capture.hci_arr.end()));
      });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  uint16_t handle = 334;
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, handle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 443;
  BasicL2capChannel channel = BuildBasicL2capChannel(
      proxy,
      BasicL2capParameters{
          .handle = handle,
          .local_cid = local_cid,
          .remote_cid = 0x123,
          .transport = AclTransportType::kLe,
          .payload_from_controller_fn =
              [&capture](FlatConstMultiBuf&& buffer) {
                ++capture.sends_called;
                // Forward to host.
                return FlatConstMultiBufInstance(std::move(buffer));
              },
      });

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(capture.hci_arr);
  acl->header().handle().Write(handle);
  acl->data_total_length().Write(
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
      capture.expected_payload.size());

  emboss::BFrameWriter bframe = emboss::MakeBFrameView(
      acl->payload().BackingStorage().data(), acl->payload().SizeInBytes());
  bframe.pdu_length().Write(capture.expected_payload.size());
  bframe.channel_id().Write(local_cid);
  std::copy(capture.expected_payload.begin(),
            capture.expected_payload.end(),
            capture.hci_arr.begin() +
                emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                emboss::BasicL2capHeader::IntrinsicSizeInBytes());

  // Send ACL data packet destined for the CoC we registered.
  proxy.HandleH4HciFromController(std::move(h4_packet));

  EXPECT_EQ(capture.sends_called, 1);
  EXPECT_EQ(capture.to_host_called, 1);
}

TEST_F(BasicL2capChannelTest, ReadPacketToController) {
  struct {
    int sends_called = 0;
    int from_host_called = 0;
    std::array<uint8_t, 3> expected_payload = {0xAB, 0xCD, 0xEF};
    std::array<uint8_t,
               emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                   emboss::BasicL2capHeader::IntrinsicSizeInBytes() + 3>
        hci_arr{};
  } capture;

  std::array<uint8_t, sizeof(emboss::H4PacketType) + capture.hci_arr.size()>
      h4_arr;
  h4_arr[0] = cpp23::to_underlying(emboss::H4PacketType::ACL_DATA);
  H4PacketWithH4 h4_packet{h4_arr};

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        ++capture.from_host_called;
        EXPECT_TRUE(std::equal(packet.GetHciSpan().begin(),
                               packet.GetHciSpan().end(),
                               capture.hci_arr.begin(),
                               capture.hci_arr.end()));
      });
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  uint16_t handle = 0x334;
  PW_TEST_ASSERT_OK(
      SendConnectionCompleteEvent(proxy, handle, emboss::StatusCode::SUCCESS));
  uint16_t local_cid = 0x443;
  uint16_t remote_cid = 0x123;
  BasicL2capChannel channel = BuildBasicL2capChannel(
      proxy,
      BasicL2capParameters{
          .handle = handle,
          .local_cid = local_cid,
          .remote_cid = remote_cid,
          .transport = AclTransportType::kBrEdr,
          .payload_from_host_fn =
              [&capture](FlatConstMultiBuf&& buffer) {
                ++capture.sends_called;
                return FlatConstMultiBufInstance(std::move(buffer));
              },
      });

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(capture.hci_arr);
  acl->header().handle().Write(handle);
  acl->data_total_length().Write(
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
      capture.expected_payload.size());

  emboss::BasicL2capHeaderWriter l2cap_header =
      emboss::MakeBasicL2capHeaderView(
          acl->payload().BackingStorage().data(),
          acl->payload().BackingStorage().SizeInBytes());
  l2cap_header.pdu_length().Write(capture.expected_payload.size());
  l2cap_header.channel_id().Write(remote_cid);

  std::copy(capture.expected_payload.begin(),
            capture.expected_payload.end(),
            capture.hci_arr.begin() +
                emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                emboss::BasicL2capHeader::IntrinsicSizeInBytes());

  std::copy(capture.hci_arr.begin(), capture.hci_arr.end(), h4_arr.begin() + 1);

  proxy.HandleH4HciFromHost(std::move(h4_packet));

  EXPECT_EQ(capture.from_host_called, 1);
  EXPECT_EQ(capture.sends_called, 1);
}

TEST_F(BasicL2capChannelTest, BasicModifyForwardToHost) {
  struct {
    int from_controller_called = 0;
    int to_host_called = 0;
    std::array<uint8_t, 3> expected_payload_to_host = {0xAB, 0xCD, 0xEF};
    std::array<uint8_t, 3> payload_from_controller = {0xDE, 0xAD, 0xBE};
    std::array<uint8_t,
               emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                   emboss::BasicL2capHeader::IntrinsicSizeInBytes() + 3>
        hci_arr{};
  } capture;

  H4PacketWithHci h4_packet{emboss::H4PacketType::ACL_DATA, capture.hci_arr};

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        if (packet.GetH4Type() != emboss::H4PacketType::ACL_DATA) {
          return;
        }
        ++capture.to_host_called;
        auto offset = emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                      emboss::BasicL2capHeader::IntrinsicSizeInBytes();
        EXPECT_TRUE(std::equal(packet.GetHciSpan().begin() + offset,
                               packet.GetHciSpan().end(),
                               capture.expected_payload_to_host.begin(),
                               capture.expected_payload_to_host.end()));
      });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  uint16_t handle = 334;
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, handle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 443;
  BasicL2capChannel channel = BuildBasicL2capChannel(
      proxy,
      BasicL2capParameters{
          .handle = handle,
          .local_cid = local_cid,
          .remote_cid = 0x123,
          .transport = AclTransportType::kLe,
          .payload_from_controller_fn =
              [&capture](FlatMultiBuf&& buffer) {
                ++capture.from_controller_called;
                PW_ASSERT(!buffer.empty());
                // Forward to host.
                auto payload =
                    pw::as_writable_bytes(MultiBufAdapter::AsSpan(buffer));
                ConstByteSpan modified_bytes =
                    as_bytes(span(capture.expected_payload_to_host.data(),
                                  capture.expected_payload_to_host.size()));

                std::copy(modified_bytes.begin(),
                          modified_bytes.end(),
                          payload.begin());
                return FlatConstMultiBufInstance(std::move(buffer));
              },
      });

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(capture.hci_arr);
  acl->header().handle().Write(handle);
  acl->data_total_length().Write(
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
      capture.payload_from_controller.size());

  emboss::BFrameWriter bframe = emboss::MakeBFrameView(
      acl->payload().BackingStorage().data(), acl->payload().SizeInBytes());
  bframe.pdu_length().Write(capture.payload_from_controller.size());
  bframe.channel_id().Write(local_cid);
  std::copy(capture.payload_from_controller.begin(),
            capture.payload_from_controller.end(),
            capture.hci_arr.begin() +
                emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                emboss::BasicL2capHeader::IntrinsicSizeInBytes());

  // Send ACL data packet destined for the CoC we registered.
  proxy.HandleH4HciFromController(std::move(h4_packet));
  EXPECT_EQ(capture.from_controller_called, 1);
  EXPECT_EQ(capture.to_host_called, 1);
}

TEST_F(BasicL2capChannelTest, BasicModifyForwardToController) {
  struct {
    int from_host_called = 0;
    int to_controller_called = 0;
    std::array<uint8_t, 3> expected_payload_to_controller = {0xAB, 0xCD, 0xEF};
    std::array<uint8_t, 3> payload_from_host = {0xDE, 0xAD, 0xBE};
    std::array<uint8_t,
               emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                   emboss::BasicL2capHeader::IntrinsicSizeInBytes() + 3>
        hci_arr{};
  } capture;

  std::array<uint8_t, sizeof(emboss::H4PacketType) + capture.hci_arr.size()>
      h4_arr;
  h4_arr[0] = cpp23::to_underlying(emboss::H4PacketType::ACL_DATA);
  H4PacketWithH4 h4_packet{h4_arr};

  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        if (packet.GetH4Type() != emboss::H4PacketType::ACL_DATA) {
          return;
        }
        ++capture.to_controller_called;
        auto offset = emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                      emboss::BasicL2capHeader::IntrinsicSizeInBytes();
        EXPECT_TRUE(std::equal(packet.GetHciSpan().begin() + offset,
                               packet.GetHciSpan().end(),
                               capture.expected_payload_to_controller.begin(),
                               capture.expected_payload_to_controller.end()));
      });
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [](H4PacketWithHci&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  uint16_t handle = 334;
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, handle, emboss::StatusCode::SUCCESS));

  uint16_t local_cid = 443;
  uint16_t remote_cid = 0x123;

  BasicL2capChannel channel = BuildBasicL2capChannel(
      proxy,
      BasicL2capParameters{
          .handle = handle,
          .local_cid = local_cid,
          .remote_cid = 0x123,
          .transport = AclTransportType::kLe,
          .payload_from_host_fn =
              [&capture](FlatMultiBuf&& buffer) {
                ++capture.from_host_called;
                PW_ASSERT(!buffer.empty());
                // Forward to host.
                auto payload =
                    pw::as_writable_bytes(MultiBufAdapter::AsSpan(buffer));
                ConstByteSpan modified_bytes = as_bytes(
                    span(capture.expected_payload_to_controller.data(),
                         capture.expected_payload_to_controller.size()));

                std::copy(modified_bytes.begin(),
                          modified_bytes.end(),
                          payload.begin());
                return FlatConstMultiBufInstance(std::move(buffer));
              },
      });

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(capture.hci_arr);
  acl->header().handle().Write(handle);
  acl->data_total_length().Write(
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
      capture.payload_from_host.size());

  emboss::BasicL2capHeaderWriter l2cap_header =
      emboss::MakeBasicL2capHeaderView(
          acl->payload().BackingStorage().data(),
          acl->payload().BackingStorage().SizeInBytes());
  l2cap_header.pdu_length().Write(capture.payload_from_host.size());
  l2cap_header.channel_id().Write(remote_cid);

  std::copy(capture.payload_from_host.begin(),
            capture.payload_from_host.end(),
            capture.hci_arr.begin() +
                emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                emboss::BasicL2capHeader::IntrinsicSizeInBytes());

  std::copy(capture.hci_arr.begin(), capture.hci_arr.end(), h4_arr.begin() + 1);
  proxy.HandleH4HciFromHost(std::move(h4_packet));

  EXPECT_EQ(capture.from_host_called, 1);
  EXPECT_EQ(capture.to_controller_called, 1);
}

// TODO: https://pwbug.dev/365161669 - Disable test at build-level once
// joinability is a build-system constraint.
#if PW_THREAD_JOINING_ENABLED
// Have multiple threads write to a BasicL2cap channel. Verify all resulting ACL
// packets are sent towards controller in the correct order per channel.
// This test be run repetitively with googletest by using:
// clang-format off
// bazelisk --config=googletest //pw_bluetooth_proxy:pw_bluetooth_proxy_test --
//   --gtest_filter=BasicL2capChannelTest.MultithreadedWrite --gtest_repeat=1000
// clang-format on
TEST_F(BasicL2capChannelTest, MultithreadedWrite) {
  constexpr unsigned int kNumThreads = 40;
  constexpr unsigned int kPacketsPerThread =
      L2capChannel::QueueCapacity() - NumBufferedPayloads();

  constexpr uint16_t kBaseLocalCid = 0xb000;   // 176
  constexpr uint16_t kBaseRemoteCid = 0xc000;  // 192
  constexpr uint16_t kPayloadSize = 10;

  struct {
    const uint16_t kExpectedPduLength = kPayloadSize;
    const uint16_t kTestHandle = 0xaa;  // 170
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
        EXPECT_EQ(acl.header().handle().Read(), capture.kTestHandle);
        EXPECT_EQ(acl.header().packet_boundary_flag().Read(),
                  emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
        EXPECT_EQ(acl.header().broadcast_flag().Read(),
                  emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
        EXPECT_EQ(acl.data_total_length().Read(),
                  capture.kExpectedAclDataTotalLength);

        PW_TEST_ASSERT_OK_AND_ASSIGN(
            emboss::BFrameView bframe,
            MakeEmbossView<emboss::BFrameView>(
                acl.payload().BackingStorage().data(), acl.SizeInBytes()));

        EXPECT_EQ(bframe.pdu_length().Read(), capture.kExpectedPduLength);

        // Each channel's remote cid has the thread index as its LSB.
        uint16_t current_remote_cid = bframe.channel_id().Read();
        unsigned int current_thread_id = current_remote_cid & ~kBaseRemoteCid;
        {
          std::lock_guard lock(capture.sends_by_channel_mutex);

          // Each payload byte should match the send count (to verify ordering).
          for (size_t i = 0; i < kPayloadSize; ++i) {
            EXPECT_EQ(bframe.payload()[i].Read(),
                      capture.sends_by_channel[current_thread_id]);
          }
          capture.sends_by_channel[current_thread_id]++;
        }
      });

  allocator::test::AllocatorForTest<65536> allocator;
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
      proxy, capture.kTestHandle, emboss::StatusCode::SUCCESS));

  MultiBufAllocatorContext<200 * 1024, sync::Mutex> packet_allocator_context;
  struct ThreadCapture {
    BasicL2capChannel channel;
    MultiBufAllocator& packet_allocator;
  };

  pw::Vector<ThreadCapture, kNumThreads> captures;
  pw::thread::test::TestThreadContext context;
  pw::Vector<pw::Thread, kNumThreads> threads;

  for (unsigned int i = 0; i < kNumThreads; ++i) {
    uint16_t local_cid = kBaseLocalCid + i;
    // Each channel's remote cid has the thread index as its LSB. That is
    // used to track packet ordering per channel.
    uint16_t remote_cid = kBaseRemoteCid + i;
    // TODO: https://pwbug.dev/422222575 -  Move channel creation, close, and
    // destruction inside each thread once we have proper channel lifecycle
    // locking.
    ThreadCapture thread_capture{
        BuildBasicL2capChannel(
            proxy,
            BasicL2capParameters{.handle = capture.kTestHandle,
                                 .local_cid = local_cid,
                                 .remote_cid = remote_cid}),
        packet_allocator_context.GetAllocator(),
    };
    captures.emplace_back(std::move(thread_capture));
  }

  for (unsigned int thread_numb = 0; thread_numb < kNumThreads; ++thread_numb) {
    ThreadCapture& thread_capture = captures[thread_numb];
    threads.emplace_back(context.options(), [&thread_capture]() {
      for (unsigned int packet_numb = 0; packet_numb < kPacketsPerThread;
           ++packet_numb) {
        std::array<uint8_t, kPayloadSize> payload = {};
        std::fill(payload.begin(), payload.end(), packet_numb);
        FlatMultiBufInstance mbuf_inst = MultiBufFromSpan(
            pw::span(payload), thread_capture.packet_allocator);
        PW_TEST_EXPECT_OK(
            thread_capture.channel
                .Write(std::move(MultiBufAdapter::Unwrap(mbuf_inst)))
                .status);
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

// ########## L2capSignalingTest

class L2capSignalingTest : public ProxyHostTest {};

TEST_F(L2capSignalingTest, FlowControlCreditIndDrainsQueue) {
  size_t sends_called = 0;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&sends_called]([[maybe_unused]] H4PacketWithH4&& packet) {
        ++sends_called;
      });
  ProxyHost proxy =
      ProxyHost(std::move(send_to_host_fn),
                std::move(send_to_controller_fn),
                /*le_acl_credits_to_reserve=*/L2capCoc::QueueCapacity(),
                /*br_edr_acl_credits_to_reserve=*/0,
                GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_EXPECT_OK(
      SendLeReadBufferResponseFromController(proxy, L2capCoc::QueueCapacity()));
  EXPECT_EQ(proxy.GetNumFreeLeAclPackets(), L2capCoc::QueueCapacity());

  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t remote_cid = 456;
  L2capCoc channel = BuildCoc(proxy,
                              CocParameters{.handle = kConnectionHandle,
                                            .remote_cid = remote_cid,
                                            .tx_credits = 0});

  for (size_t i = 0; i < L2capCoc::QueueCapacity(); ++i) {
    FlatMultiBufInstance empty = MakeEmptyMultiBuf();
    PW_TEST_EXPECT_OK(
        channel.Write(std::move(MultiBufAdapter::Unwrap(empty))).status);
    RunDispatcher();
  }
  FlatMultiBufInstance empty = MakeEmptyMultiBuf();
  EXPECT_EQ(channel.Write(std::move(MultiBufAdapter::Unwrap(empty))).status,
            Status::Unavailable());
  RunDispatcher();
  EXPECT_EQ(sends_called, 0u);

  constexpr size_t kL2capLength =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
      emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes();
  constexpr size_t kHciLength =
      emboss::AclDataFrame::MinSizeInBytes() + kL2capLength;
  std::array<uint8_t, kHciLength> hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci flow_control_credit_ind{emboss::H4PacketType::ACL_DATA,
                                          pw::span(hci_arr.data(), kHciLength)};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(kL2capLength);

  emboss::CFrameWriter l2cap = emboss::MakeCFrameView(
      acl->payload().BackingStorage().data(), kL2capLength);
  l2cap.pdu_length().Write(
      emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes());
  // 0x0005 = LE-U fixed signaling channel ID.
  l2cap.channel_id().Write(0x0005);

  emboss::L2capFlowControlCreditIndWriter ind =
      emboss::MakeL2capFlowControlCreditIndView(
          l2cap.payload().BackingStorage().data(),
          emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes());
  ind.command_header().code().Write(
      emboss::L2capSignalingPacketCode::FLOW_CONTROL_CREDIT_IND);
  ind.command_header().data_length().Write(
      emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes() -
      emboss::L2capSignalingCommandHeader::IntrinsicSizeInBytes());
  ind.cid().Write(remote_cid);
  ind.credits().Write(L2capCoc::QueueCapacity());

  proxy.HandleH4HciFromController(std::move(flow_control_credit_ind));
  RunDispatcher();
  EXPECT_EQ(sends_called, L2capCoc::QueueCapacity());
}

TEST_F(L2capSignalingTest, ChannelClosedWithErrorIfCreditsExceeded) {
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy =
      ProxyHost(std::move(send_to_host_fn),
                std::move(send_to_controller_fn),
                /*le_acl_credits_to_reserve=*/L2capCoc::QueueCapacity(),
                /*br_edr_acl_credits_to_reserve=*/0,
                GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  uint16_t remote_cid = 456;
  int events_received = 0;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{
          .handle = kConnectionHandle,
          .remote_cid = remote_cid,
          // Initialize with max credit count.
          .tx_credits =
              emboss::L2capLeCreditBasedConnectionReq::max_credit_value(),
          .event_fn = [&events_received](L2capChannelEvent event) {
            EXPECT_EQ(event, L2capChannelEvent::kRxInvalid);
            ++events_received;
          }});

  constexpr size_t kL2capLength =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
      emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes();
  constexpr size_t kHciLength =
      emboss::AclDataFrame::MinSizeInBytes() + kL2capLength;
  std::array<uint8_t, kHciLength> hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci flow_control_credit_ind{emboss::H4PacketType::ACL_DATA,
                                          pw::span(hci_arr.data(), kHciLength)};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(kL2capLength);

  emboss::CFrameWriter l2cap =
      emboss::MakeCFrameView(acl->payload().BackingStorage().data(),
                             emboss::BasicL2capHeader::IntrinsicSizeInBytes());
  l2cap.pdu_length().Write(
      emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes());
  // 0x0005 = LE-U fixed signaling channel ID.
  l2cap.channel_id().Write(0x0005);

  emboss::L2capFlowControlCreditIndWriter ind =
      emboss::MakeL2capFlowControlCreditIndView(
          l2cap.payload().BackingStorage().data(),
          emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes());
  ind.command_header().code().Write(
      emboss::L2capSignalingPacketCode::FLOW_CONTROL_CREDIT_IND);
  ind.command_header().data_length().Write(
      emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes() -
      emboss::L2capSignalingCommandHeader::IntrinsicSizeInBytes());
  ind.cid().Write(remote_cid);
  // Exceed max credit count by 1.
  ind.credits().Write(1);

  proxy.HandleH4HciFromController(std::move(flow_control_credit_ind));

  EXPECT_EQ(events_received, 1);
}

TEST_F(L2capSignalingTest, SignalsArePassedOnToHost) {
  int forwards_to_host = 0;
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&forwards_to_host](H4PacketWithHci&&) { ++forwards_to_host; });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  EXPECT_EQ(forwards_to_host, 0);

  PW_TEST_EXPECT_OK(
      SendL2capConnectionReq(proxy, Direction::kFromController, 44, 55, 56));
  EXPECT_EQ(forwards_to_host, 1);
}

TEST_F(L2capSignalingTest, SignalsArePassedOnToHostAfterAclDisconnect) {
  int sends_to_host = 0;
  int sends_to_controller = 0;
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&sends_to_host](H4PacketWithHci&&) { ++sends_to_host; });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&sends_to_controller](H4PacketWithH4&&) { ++sends_to_controller; });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  // Allow proxy to reserve 1 credit.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(sends_to_host, 2);

  std::array<uint8_t, 1> attribute_value = {0};
  {
    GattNotifyChannel channel =
        BuildGattNotifyChannel(proxy, {.handle = kConnectionHandle});
    FlatMultiBufInstance mbuf_inst = MultiBufFromArray(attribute_value);
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
  }
  EXPECT_EQ(sends_to_controller, 1);

  // Disconnect that connection.
  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, kConnectionHandle));
  EXPECT_EQ(sends_to_host, 3);

  // Send signal again using the same connection. Signal should be passed on
  // to host.
  PW_TEST_EXPECT_OK(SendL2capConnectionReq(
      proxy, Direction::kFromController, kConnectionHandle, 55, 56));
  EXPECT_EQ(sends_to_host, 4);

  // Trigger credit send for L2capCoc to verify new signalling channel
  // object is present and working.
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  {
    L2capCoc channel =
        BuildCoc(proxy, CocParameters{.handle = kConnectionHandle});
    PW_TEST_EXPECT_OK(channel.SendAdditionalRxCredits(7));
    RunDispatcher();
  }
  EXPECT_EQ(sends_to_controller, 2);
}

TEST_F(L2capSignalingTest,
       CreditIndAddressedToNonManagedChannelForwardedToHost) {
  int forwards_to_host = 0;
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&forwards_to_host](H4PacketWithHci&&) { ++forwards_to_host; });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) {});

  ProxyHost proxy =
      ProxyHost(std::move(send_to_host_fn),
                std::move(send_to_controller_fn),
                /*le_acl_credits_to_reserve=*/L2capCoc::QueueCapacity(),
                /*br_edr_acl_credits_to_reserve=*/0,
                GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(forwards_to_host, 1);

  uint16_t remote_cid = 456;
  L2capCoc channel = BuildCoc(
      proxy,
      CocParameters{.handle = kConnectionHandle, .remote_cid = remote_cid});

  constexpr size_t kL2capLength =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
      emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes();
  constexpr size_t kHciLength =
      emboss::AclDataFrame::MinSizeInBytes() + kL2capLength;
  std::array<uint8_t, kHciLength> hci_arr;
  hci_arr.fill(0);
  H4PacketWithHci flow_control_credit_ind{emboss::H4PacketType::ACL_DATA,
                                          pw::span(hci_arr.data(), kHciLength)};

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr);
  acl->header().handle().Write(kConnectionHandle);
  acl->data_total_length().Write(kL2capLength);

  emboss::CFrameWriter l2cap =
      emboss::MakeCFrameView(acl->payload().BackingStorage().data(),
                             emboss::BasicL2capHeader::IntrinsicSizeInBytes());
  l2cap.pdu_length().Write(
      emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes());
  // 0x0005 = LE-U fixed signaling channel ID.
  l2cap.channel_id().Write(0x0005);

  emboss::L2capFlowControlCreditIndWriter ind =
      emboss::MakeL2capFlowControlCreditIndView(
          l2cap.payload().BackingStorage().data(),
          emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes());
  ind.command_header().code().Write(
      emboss::L2capSignalingPacketCode::FLOW_CONTROL_CREDIT_IND);
  ind.command_header().data_length().Write(
      emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes() -
      emboss::L2capSignalingCommandHeader::IntrinsicSizeInBytes());
  // Address packet to different CID on same connection.
  ind.cid().Write(remote_cid + 1);

  proxy.HandleH4HciFromController(std::move(flow_control_credit_ind));

  EXPECT_EQ(forwards_to_host, 2);
}

TEST_F(L2capSignalingTest, RxAdditionalCreditsSent) {
  struct {
    uint16_t local_cid = 456;
    uint16_t credits = 3;
    int sends_called = 0;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [](H4PacketWithHci&&) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        ++capture.sends_called;
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
        // TODO: https://pwbug.dev/382553099 - Test to ensure we are properly
        // incrementing Identifier when sending multiple signaling packets.
        EXPECT_EQ(ind.command_header().identifier().Read(), 1);
        EXPECT_EQ(
            ind.command_header().data_length().Read(),
            emboss::L2capFlowControlCreditInd::IntrinsicSizeInBytes() -
                emboss::L2capSignalingCommandHeader::IntrinsicSizeInBytes());
        EXPECT_EQ(ind.cid().Read(), capture.local_cid);
        EXPECT_EQ(ind.credits().Read(), capture.credits);
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  // Allow proxy to reserve 1 LE credit.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 1));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  // Build channel so ACL connection is registered.
  L2capCoc channel = BuildCoc(proxy,
                              CocParameters{.handle = kConnectionHandle,
                                            .local_cid = capture.local_cid});

  PW_TEST_EXPECT_OK(channel.SendAdditionalRxCredits(capture.credits));
  RunDispatcher();

  EXPECT_EQ(capture.sends_called, 1);
}

TEST_F(L2capSignalingTest, RemoteLocalCidCollisionBetweenProfiles) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/5,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  // We are receiving the connection and disconnection request for two different
  // channels where one channel has same local CID and the others remote CID.
  constexpr uint8_t kPsm = 0x19;
  constexpr uint16_t kRemoteCID = 0x65;
  constexpr uint16_t kLocalCID = 0x46;

  constexpr uint8_t kPsm2 = 0x1B;
  constexpr uint16_t kRemoteCID2 = 0x46;
  constexpr uint16_t kLocalCID2 = 0x4f;

  // Receive L2capConnectionReq on first PSM
  EXPECT_EQ(SendL2capConnectionReq(proxy,
                                   Direction::kFromController,
                                   kConnectionHandle,
                                   kRemoteCID,
                                   kPsm),
            pw::OkStatus());
  EXPECT_EQ(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromHost,
                             kConnectionHandle,
                             kRemoteCID,
                             kLocalCID,
                             emboss::L2capConnectionRspResultCode::SUCCESSFUL),
      pw::OkStatus());

  // Acquire first channel with the event_fn_
  uint8_t reset_called = 0;

  auto event_fn([&reset_called](L2capChannelEvent event) -> void {
    switch (event) {
      case L2capChannelEvent::kChannelClosedByOther:
      case L2capChannelEvent::kReset:
        reset_called++;
        break;
      case L2capChannelEvent::kRxInvalid:
      case L2capChannelEvent::kRxOutOfMemory:
      case L2capChannelEvent::kRxWhileStopped:
      case L2capChannelEvent::kWriteAvailable:
      default:
        break;
    }
  });
  BasicL2capChannel channel =
      BuildBasicL2capChannel(proxy,
                             {.handle = kConnectionHandle,
                              .local_cid = kLocalCID,
                              .remote_cid = kRemoteCID,
                              .transport = AclTransportType::kBrEdr,
                              .event_fn = event_fn});

  // Receive L2capConnectionReq on second PSM
  EXPECT_EQ(SendL2capConnectionReq(proxy,
                                   Direction::kFromController,
                                   kConnectionHandle,
                                   kRemoteCID2,
                                   kPsm2),
            pw::OkStatus());
  EXPECT_EQ(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromHost,
                             kConnectionHandle,
                             kRemoteCID2,
                             kLocalCID2,
                             emboss::L2capConnectionRspResultCode::SUCCESSFUL),
      pw::OkStatus());

  // Send Disconnect second channel (Received this disconnect rsp)
  EXPECT_EQ(SendL2capDisconnectRsp(proxy,
                                   Direction::kFromController,
                                   AclTransportType::kBrEdr,
                                   kConnectionHandle,
                                   kLocalCID2,
                                   kRemoteCID2),
            pw::OkStatus());

  // Assert first event channel wasn't called
  EXPECT_EQ(reset_called, 0);

  // Send Disconnect first channel (Received this disconnect rsp)
  EXPECT_EQ(SendL2capDisconnectRsp(proxy,
                                   Direction::kFromController,
                                   AclTransportType::kBrEdr,
                                   kConnectionHandle,
                                   kLocalCID,
                                   kRemoteCID),
            pw::OkStatus());

  // Assert first event channel was called
  EXPECT_EQ(reset_called, 1);
}

// ########## AcluSignalingChannelTest

class AcluSignalingChannelTest : public ProxyHostTest {};

TEST_F(AcluSignalingChannelTest, HandlesMultipleCommands) {
  std::optional<H4PacketWithHci> host_packet;
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&host_packet](H4PacketWithHci&& packet) {
        host_packet = std::move(packet);
      });
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  constexpr uint16_t kHandle = 123;

  // Test that the proxy can parse a CFrame containing multiple commands and
  // pass it through. We pack 3 CONNECTION_REQ commands into one CFrame.
  constexpr size_t kNumCommands = 3;
  constexpr size_t kCmdLen = emboss::L2capConnectionReq::IntrinsicSizeInBytes();
  constexpr size_t kL2capLength =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes() + kCmdLen * kNumCommands;
  constexpr size_t kHciLength =
      emboss::AclDataFrame::MinSizeInBytes() + kL2capLength;
  std::array<uint8_t, kHciLength> hci_arr{};
  H4PacketWithHci l2cap_cframe_packet{emboss::H4PacketType::ACL_DATA,
                                      pw::span(hci_arr.data(), kHciLength)};

  // ACL header
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto acl, MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr));
  acl.header().handle().Write(kHandle);
  acl.data_total_length().Write(kL2capLength);
  EXPECT_EQ(kL2capLength, acl.payload().BackingStorage().SizeInBytes());

  // L2CAP header
  auto l2cap =
      emboss::MakeCFrameView(acl.payload().BackingStorage().data(),
                             acl.payload().BackingStorage().SizeInBytes());
  l2cap.pdu_length().Write(kNumCommands * kCmdLen);
  l2cap.channel_id().Write(
      cpp23::to_underlying(emboss::L2capFixedCid::ACL_U_SIGNALING));
  EXPECT_TRUE(l2cap.Ok());

  auto command_buffer =
      pw::span(l2cap.payload().BackingStorage().data(),
               l2cap.payload().BackingStorage().SizeInBytes());
  EXPECT_EQ(l2cap.payload().BackingStorage().SizeInBytes(),
            kCmdLen * kNumCommands);

  do {
    // CONNECTION_REQ
    auto cmd_writer = emboss::MakeL2capConnectionReqView(command_buffer.data(),
                                                         command_buffer.size());
    cmd_writer.command_header().code().Write(
        emboss::L2capSignalingPacketCode::CONNECTION_REQ);
    // Note data_length doesn't include command header.
    cmd_writer.command_header().data_length().Write(
        kCmdLen - emboss::L2capSignalingCommandHeader::IntrinsicSizeInBytes());
    cmd_writer.psm().Write(1);
    cmd_writer.source_cid().Write(1);
    EXPECT_TRUE(cmd_writer.Ok());
    EXPECT_EQ(cmd_writer.SizeInBytes(), kCmdLen);
    command_buffer = command_buffer.subspan(cmd_writer.SizeInBytes());
  } while (!command_buffer.empty());

  proxy.HandleH4HciFromController(std::move(l2cap_cframe_packet));
  // We should get back what we sent, since the proxy doesn't consume
  // CONNECTION_REQ commands. It would be nice to also verify the individual
  // commands were parsed out but hooks don't exist for that at the time of
  // writing.
  EXPECT_TRUE(host_packet.has_value());
  EXPECT_EQ(host_packet->GetHciSpan().size(), kHciLength);
}

TEST_F(AcluSignalingChannelTest, InvalidPacketForwarded) {
  std::optional<H4PacketWithHci> host_packet;
  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&host_packet](H4PacketWithHci&& packet) {
        host_packet = std::move(packet);
      });
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/1,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  constexpr uint16_t kHandle = 123;

  // Test that the proxy forwards on invalid L2cap B-frames destined for
  // signaling channel.

  constexpr size_t kL2capLength =
      emboss::BasicL2capHeader::IntrinsicSizeInBytes();
  constexpr size_t kHciLength =
      emboss::AclDataFrame::MinSizeInBytes() + kL2capLength;
  std::array<uint8_t, kHciLength> hci_arr{};
  H4PacketWithHci l2cap_cframe_packet{emboss::H4PacketType::ACL_DATA,
                                      pw::span(hci_arr.data(), kHciLength)};

  // ACL header
  PW_TEST_ASSERT_OK_AND_ASSIGN(
      auto acl, MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_arr));
  acl.header().handle().Write(kHandle);
  acl.data_total_length().Write(kL2capLength);
  EXPECT_EQ(kL2capLength, acl.payload().BackingStorage().SizeInBytes());

  // L2CAP header
  auto l2cap =
      emboss::MakeCFrameView(acl.payload().BackingStorage().data(),
                             acl.payload().BackingStorage().SizeInBytes());
  // Invalid length, since we aren't encoding a payload.
  l2cap.pdu_length().Write(1);
  l2cap.channel_id().Write(
      cpp23::to_underlying(emboss::L2capFixedCid::ACL_U_SIGNALING));
  EXPECT_FALSE(l2cap.Ok());

  proxy.HandleH4HciFromController(std::move(l2cap_cframe_packet));
  // We should get back what we sent.
  EXPECT_TRUE(host_packet.has_value());
  EXPECT_EQ(host_packet->GetHciSpan().size(), kHciLength);
}

// ########## ProxyHostConnectionEventTest

class ProxyHostConnectionEventTest : public ProxyHostTest {};

TEST_F(ProxyHostConnectionEventTest, ConnectionCompletePassthroughOk) {
  size_t host_called = 0;
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&host_called]([[maybe_unused]] H4PacketWithHci&& packet) {
        ++host_called;
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendConnectionCompleteEvent(proxy, 1, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(host_called, 1U);

  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, 1));
  EXPECT_EQ(host_called, 2U);
}

TEST_F(ProxyHostConnectionEventTest,
       ConnectionCompleteWithErrorStatusPassthroughOk) {
  size_t host_called = 0;
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&host_called]([[maybe_unused]] H4PacketWithHci&& packet) {
        ++host_called;
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(SendConnectionCompleteEvent(
      proxy, 1, emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED));
  EXPECT_EQ(host_called, 1U);
}

TEST_F(ProxyHostConnectionEventTest, LeConnectionCompletePassthroughOk) {
  size_t host_called = 0;
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&host_called]([[maybe_unused]] H4PacketWithHci&& packet) {
        ++host_called;
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(
      SendLeConnectionCompleteEvent(proxy, 1, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(host_called, 1U);
}

class L2capStatusTrackerTest : public ProxyHostTest,
                               public L2capStatusDelegate {
 public:
  static constexpr uint16_t kPsm = 1;

  bool ShouldTrackPsm(uint16_t psm) override { return psm == kPsm; }
  void HandleConnectionComplete(const L2capChannelConnectionInfo& i) override {
    EXPECT_FALSE(info.has_value());
    PW_CHECK(proxy_ptr);
    info.emplace(i);
    // Test we can create channel directly in callback.
    l2cap_channel =
        BuildBasicL2capChannel(*proxy_ptr,
                               {.handle = i.connection_handle,
                                .local_cid = i.local_cid,
                                .remote_cid = i.remote_cid,
                                .transport = AclTransportType::kBrEdr});
  }
  void HandleDisconnectionComplete(
      const L2capChannelConnectionInfo& i) override {
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->direction, i.direction);
    EXPECT_EQ(info->connection_handle, i.connection_handle);
    EXPECT_EQ(info->remote_cid, i.remote_cid);
    EXPECT_EQ(info->local_cid, i.local_cid);
    info.reset();
  }

  void HandleConfigurationChanged(
      const L2capChannelConfigurationInfo& i) override {
    configuration_called++;
    PW_CHECK(proxy_ptr);

    EXPECT_EQ(config_info->direction, i.direction);
    EXPECT_EQ(config_info->connection_handle, i.connection_handle);
    EXPECT_EQ(config_info->local_cid, i.local_cid);
    EXPECT_EQ(config_info->mtu, i.mtu);
  }

  ProxyHost* proxy_ptr = nullptr;
  uint8_t configuration_called = 0;
  std::optional<L2capChannelConnectionInfo> info;
  std::optional<BasicL2capChannel> l2cap_channel;
  std::optional<L2capChannelConfigurationInfo> config_info;
};

// TODO(b/405201804): Add test that check MTU value in the response
TEST_F(L2capStatusTrackerTest, L2capConfigurationMTUCalled) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy_ptr = &proxy;

  constexpr uint16_t kLocalCid = 30;
  constexpr uint16_t kRemoteCid = 31;
  constexpr uint16_t kHandle = 123;

  proxy.RegisterL2capStatusDelegate(*this);

  PW_TEST_EXPECT_OK(
      SendConnectionCompleteEvent(proxy, kHandle, emboss::StatusCode::SUCCESS));

  // Receive new connection req
  PW_TEST_EXPECT_OK(SendL2capConnectionReq(
      proxy, Direction::kFromController, kHandle, kRemoteCid, kPsm));
  EXPECT_FALSE(info.has_value());
  // Send success rsp
  PW_TEST_EXPECT_OK(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromHost,
                             kHandle,
                             kRemoteCid,
                             kLocalCid,
                             emboss::L2capConnectionRspResultCode::SUCCESSFUL));

  auto l2cap_options = L2capOptions{
      .mtu = MtuOption{1024},
  };

  // Send Configure Request
  auto expected_sent_l2cap_configuration = L2capChannelConfigurationInfo{
      .direction = Direction::kFromHost,
      .connection_handle = kHandle,
      .remote_cid = kRemoteCid,
      .local_cid = kLocalCid,
      .mtu = MtuOption{1024},
  };
  config_info.emplace(expected_sent_l2cap_configuration);

  PW_TEST_EXPECT_OK(SendL2capConfigureReq(
      proxy, Direction::kFromHost, kHandle, kRemoteCid, l2cap_options));

  PW_TEST_EXPECT_OK(
      SendL2capConfigureRsp(proxy,
                            Direction::kFromController,
                            kHandle,
                            kLocalCid,
                            emboss::L2capConfigurationResult::SUCCESS));
  ASSERT_EQ(this->configuration_called, 1);

  // Receive Configure Request
  auto expected_recv_l2cap_configuration = L2capChannelConfigurationInfo{
      .direction = Direction::kFromController,
      .connection_handle = kHandle,
      .remote_cid = kRemoteCid,
      .local_cid = kLocalCid,
      .mtu = MtuOption{1024},
  };

  config_info.emplace(expected_recv_l2cap_configuration);

  PW_TEST_EXPECT_OK(SendL2capConfigureReq(
      proxy, Direction::kFromController, kHandle, kLocalCid, l2cap_options));

  PW_TEST_EXPECT_OK(
      SendL2capConfigureRsp(proxy,
                            Direction::kFromHost,
                            kHandle,
                            kRemoteCid,
                            emboss::L2capConfigurationResult::SUCCESS));
  ASSERT_EQ(this->configuration_called, 2);

  proxy.UnregisterL2capStatusDelegate(*this);
}

TEST_F(L2capStatusTrackerTest, L2capConfigurationNoOption) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  proxy_ptr = &proxy;

  constexpr uint16_t kSourceCid = 30;
  constexpr uint16_t kDestinationCid = 31;
  constexpr uint16_t kHandle = 123;

  proxy.RegisterL2capStatusDelegate(*this);

  PW_TEST_EXPECT_OK(
      SendConnectionCompleteEvent(proxy, kHandle, emboss::StatusCode::SUCCESS));

  // Send new connection req
  PW_TEST_EXPECT_OK(SendL2capConnectionReq(
      proxy, Direction::kFromController, kHandle, kSourceCid, kPsm));
  EXPECT_FALSE(info.has_value());

  // Send success rsp
  PW_TEST_EXPECT_OK(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromHost,
                             kHandle,
                             kSourceCid,
                             kDestinationCid,
                             emboss::L2capConnectionRspResultCode::SUCCESSFUL));

  // Send Configure Request
  auto expected_l2cap_configuration = L2capChannelConfigurationInfo{
      .direction = Direction::kFromController,
      .connection_handle = kHandle,
      .remote_cid = kSourceCid,
      .local_cid = kDestinationCid,
      .mtu = std::nullopt,
  };

  config_info.emplace(expected_l2cap_configuration);

  auto l2cap_options = L2capOptions{
      .mtu = std::nullopt,
  };

  PW_TEST_EXPECT_OK(SendL2capConfigureReq(proxy,
                                          Direction::kFromController,
                                          kHandle,
                                          kDestinationCid,
                                          l2cap_options));

  PW_TEST_EXPECT_OK(
      SendL2capConfigureRsp(proxy,
                            Direction::kFromHost,
                            kHandle,
                            kSourceCid,
                            emboss::L2capConfigurationResult::SUCCESS));

  proxy.UnregisterL2capStatusDelegate(*this);
}

TEST_F(L2capStatusTrackerTest, L2capEventsControllerInitiated) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  proxy_ptr = &proxy;

  constexpr uint16_t kSourceCid = 30;
  constexpr uint16_t kDestinationCid = 31;
  constexpr uint16_t kHandle = 123;

  proxy.RegisterL2capStatusDelegate(*this);

  PW_TEST_EXPECT_OK(
      SendConnectionCompleteEvent(proxy, kHandle, emboss::StatusCode::SUCCESS));

  // First send CONNECTION_REQ to setup partial connection
  PW_TEST_EXPECT_OK(SendL2capConnectionReq(
      proxy, Direction::kFromController, kHandle, kSourceCid, kPsm));
  EXPECT_FALSE(info.has_value());

  // Send non-successful connection response.
  PW_TEST_EXPECT_OK(SendL2capConnectionRsp(
      proxy,
      Direction::kFromHost,
      kHandle,
      kSourceCid,
      kDestinationCid,
      emboss::L2capConnectionRspResultCode::INVALID_SOURCE_CID));
  EXPECT_FALSE(info.has_value());

  // Send successful connection response, but expect that it will not have
  // called listener since the connection was closed with error already.
  PW_TEST_EXPECT_OK(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromHost,
                             kHandle,
                             kSourceCid,
                             kDestinationCid,
                             emboss::L2capConnectionRspResultCode::SUCCESSFUL));
  EXPECT_FALSE(info.has_value());

  // Send new connection req
  PW_TEST_EXPECT_OK(SendL2capConnectionReq(
      proxy, Direction::kFromController, kHandle, kSourceCid, kPsm));
  EXPECT_FALSE(info.has_value());

  // Send rsp with PENDING set.
  PW_TEST_EXPECT_OK(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromHost,
                             kHandle,
                             kSourceCid,
                             kDestinationCid,
                             emboss::L2capConnectionRspResultCode::PENDING));
  EXPECT_FALSE(info.has_value());

  // Send success rsp
  PW_TEST_EXPECT_OK(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromHost,
                             kHandle,
                             kSourceCid,
                             kDestinationCid,
                             emboss::L2capConnectionRspResultCode::SUCCESSFUL));
  EXPECT_TRUE(info.has_value());
  EXPECT_EQ(info->direction, Direction::kFromController);
  EXPECT_EQ(info->connection_handle, kHandle);
  EXPECT_EQ(info->local_cid, kDestinationCid);
  EXPECT_EQ(info->remote_cid, kSourceCid);

  // Send disconnect
  PW_TEST_EXPECT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromHost,
                                           AclTransportType::kBrEdr,
                                           kHandle,
                                           kSourceCid,
                                           kDestinationCid));
  EXPECT_FALSE(info.has_value());

  proxy.UnregisterL2capStatusDelegate(*this);

  // Send successful connection sequence with no listeners.
  PW_TEST_EXPECT_OK(SendL2capConnectionReq(
      proxy, Direction::kFromController, kHandle, kSourceCid, kPsm));
  PW_TEST_EXPECT_OK(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromHost,
                             kHandle,
                             kSourceCid,
                             kDestinationCid,
                             emboss::L2capConnectionRspResultCode::SUCCESSFUL));
  EXPECT_FALSE(info.has_value());
}

TEST_F(L2capStatusTrackerTest, L2capEventsHostInitiated) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  proxy_ptr = &proxy;

  constexpr uint16_t kSourceCid = 30;
  constexpr uint16_t kDestinationCid = 31;
  constexpr uint16_t kHandle = 123;

  proxy.RegisterL2capStatusDelegate(*this);

  PW_TEST_EXPECT_OK(
      SendConnectionCompleteEvent(proxy, kHandle, emboss::StatusCode::SUCCESS));

  // First send CONNECTION_REQ to setup partial connection
  PW_TEST_EXPECT_OK(SendL2capConnectionReq(
      proxy, Direction::kFromHost, kHandle, kSourceCid, kPsm));
  EXPECT_FALSE(info.has_value());

  // Send non-successful connection response.
  PW_TEST_EXPECT_OK(SendL2capConnectionRsp(
      proxy,
      Direction::kFromController,
      kHandle,
      kSourceCid,
      kDestinationCid,
      emboss::L2capConnectionRspResultCode::INVALID_SOURCE_CID));
  EXPECT_FALSE(info.has_value());

  // Send successful connection response, but expect that it will not have
  // called listener since the connection was closed with error already.
  PW_TEST_EXPECT_OK(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromController,
                             kHandle,
                             kSourceCid,
                             kDestinationCid,
                             emboss::L2capConnectionRspResultCode::SUCCESSFUL));
  EXPECT_FALSE(info.has_value());

  // Send new connection req
  PW_TEST_EXPECT_OK(SendL2capConnectionReq(
      proxy, Direction::kFromHost, kHandle, kSourceCid, kPsm));
  EXPECT_FALSE(info.has_value());

  // Send rsp with PENDING set.
  PW_TEST_EXPECT_OK(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromController,
                             kHandle,
                             kSourceCid,
                             kDestinationCid,
                             emboss::L2capConnectionRspResultCode::PENDING));
  EXPECT_FALSE(info.has_value());

  // Send success rsp
  PW_TEST_EXPECT_OK(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromController,
                             kHandle,
                             kSourceCid,
                             kDestinationCid,
                             emboss::L2capConnectionRspResultCode::SUCCESSFUL));
  EXPECT_TRUE(info.has_value());
  EXPECT_EQ(info->direction, Direction::kFromHost);
  EXPECT_EQ(info->connection_handle, kHandle);
  EXPECT_EQ(info->local_cid, kSourceCid);
  EXPECT_EQ(info->remote_cid, kDestinationCid);

  // Send disconnect rsp
  PW_TEST_EXPECT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kBrEdr,
                                           kHandle,
                                           kSourceCid,
                                           kDestinationCid));
  EXPECT_FALSE(info.has_value());

  proxy.UnregisterL2capStatusDelegate(*this);

  // Send successful connection sequence with no listeners.
  PW_TEST_EXPECT_OK(SendL2capConnectionReq(
      proxy, Direction::kFromHost, kHandle, kSourceCid, kPsm));
  PW_TEST_EXPECT_OK(
      SendL2capConnectionRsp(proxy,
                             Direction::kFromController,
                             kHandle,
                             kSourceCid,
                             kDestinationCid,
                             emboss::L2capConnectionRspResultCode::SUCCESSFUL));
  EXPECT_FALSE(info.has_value());
}

TEST_F(ProxyHostConnectionEventTest, HciDisconnectionAlertsListeners) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  constexpr uint16_t kPsm = 1;

  class TestStatusDelegate final : public L2capStatusDelegate {
   public:
    bool ShouldTrackPsm(uint16_t psm) override { return psm == kPsm; }
    void HandleConnectionComplete(const L2capChannelConnectionInfo&) override {
      ++connections_received;
    }
    void HandleDisconnectionComplete(
        const L2capChannelConnectionInfo&) override {
      ++disconnections_received;
    }

    void HandleConfigurationChanged(
        const L2capChannelConfigurationInfo&) override {
      ++configuration_received;
    }

    int connections_received = 0;
    int disconnections_received = 0;
    int configuration_received = 0;
  };

  TestStatusDelegate test_delegate;
  proxy.RegisterL2capStatusDelegate(test_delegate);

  constexpr uint16_t Handle1 = 0x123, Handle2 = 0x124;
  PW_TEST_EXPECT_OK(
      SendConnectionCompleteEvent(proxy, Handle1, emboss::StatusCode::SUCCESS));
  PW_TEST_EXPECT_OK(
      SendConnectionCompleteEvent(proxy, Handle2, emboss::StatusCode::SUCCESS));

  // Establish three connected_channels:
  // handle = 0x123, PSM = 1 | handle = 0x124, PSM = 1 | handle = 0x123, PSM =
  // 1
  constexpr uint16_t kStartSourceCid = 0x111;
  constexpr uint16_t kStartDestinationCid = 0x211;

  auto l2cap_options = L2capOptions{
      .mtu = MtuOption{1024},
  };

  for (size_t i = 0; i < 3; ++i) {
    PW_TEST_EXPECT_OK(SendL2capConnectionReq(proxy,
                                             Direction::kFromController,
                                             i == 1 ? Handle2 : Handle1,
                                             kStartSourceCid + i,
                                             kPsm));
    PW_TEST_EXPECT_OK(SendL2capConnectionRsp(
        proxy,
        Direction::kFromHost,
        i == 1 ? Handle2 : Handle1,
        kStartSourceCid + i,
        kStartDestinationCid + i,
        emboss::L2capConnectionRspResultCode::SUCCESSFUL));
    PW_TEST_EXPECT_OK(SendL2capConfigureReq(proxy,
                                            Direction::kFromController,
                                            i == 1 ? Handle2 : Handle1,
                                            kStartDestinationCid + i,
                                            l2cap_options));
    PW_TEST_EXPECT_OK(
        SendL2capConfigureRsp(proxy,
                              Direction::kFromHost,
                              i == 1 ? Handle2 : Handle1,
                              kStartSourceCid + i,
                              emboss::L2capConfigurationResult::SUCCESS));
  }

  EXPECT_EQ(test_delegate.connections_received, 3);
  EXPECT_EQ(test_delegate.configuration_received, 3);
  EXPECT_EQ(test_delegate.disconnections_received, 0);

  // Disconnect handle1, which should disconnect first and third channel.
  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, Handle1));
  EXPECT_EQ(test_delegate.disconnections_received, 2);

  // Confirm remaining channel can still be disconnected properly.
  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, Handle2));
  EXPECT_EQ(test_delegate.disconnections_received, 3);

  proxy.UnregisterL2capStatusDelegate(test_delegate);
}

TEST_F(ProxyHostConnectionEventTest,
       HciDisconnectionFromControllerClosesChannels) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle + 1, emboss::StatusCode::SUCCESS));

  constexpr uint16_t kStartingCid = 0x111;
  int events_received = 0;
  auto event_fn = [&events_received](L2capChannelEvent event) {
    ++events_received;
    EXPECT_EQ(event, L2capChannelEvent::kChannelClosedByOther);
  };
  BasicL2capChannel chan1 = BuildBasicL2capChannel(proxy,
                                                   {.handle = kConnectionHandle,
                                                    .local_cid = kStartingCid,
                                                    .remote_cid = kStartingCid,
                                                    .event_fn = event_fn});

  // chan2 is on a different connection so should not be closed
  BasicL2capChannel chan2 =
      BuildBasicL2capChannel(proxy,
                             {.handle = kConnectionHandle + 1,
                              .local_cid = kStartingCid + 1,
                              .remote_cid = kStartingCid + 1,
                              .event_fn = event_fn});

  BasicL2capChannel chan3 =
      BuildBasicL2capChannel(proxy,
                             {.handle = kConnectionHandle,
                              .local_cid = kStartingCid + 2,
                              .remote_cid = kStartingCid + 2,
                              .event_fn = event_fn});

  EXPECT_EQ(GetState(chan1), L2capChannel::State::kRunning);
  EXPECT_EQ(GetState(chan2), L2capChannel::State::kRunning);
  EXPECT_EQ(GetState(chan3), L2capChannel::State::kRunning);

  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, kConnectionHandle));

  EXPECT_EQ(events_received, 2);
  EXPECT_EQ(GetState(chan1), L2capChannel::State::kClosed);
  EXPECT_EQ(GetState(chan2), L2capChannel::State::kRunning);
  EXPECT_EQ(GetState(chan3), L2capChannel::State::kClosed);

  // Confirm L2CAP_DISCONNECTION_RSP packet does not result in another event.
  PW_TEST_EXPECT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromHost,
                                           AclTransportType::kLe,
                                           kConnectionHandle,
                                           kStartingCid,
                                           kStartingCid));
  EXPECT_EQ(events_received, 2);
}

TEST_F(ProxyHostConnectionEventTest,
       L2capDisconnectionRspFromHostClosesChannels) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  constexpr uint16_t kStartingSourceCid = 0x111;
  constexpr uint16_t kStartingDestinationCid = 0x211;
  int events_received = 0;
  auto event_fn = [&events_received](L2capChannelEvent event) {
    ++events_received;
    EXPECT_EQ(event, L2capChannelEvent::kChannelClosedByOther);
  };
  BasicL2capChannel chan1 =
      BuildBasicL2capChannel(proxy,
                             {.handle = kConnectionHandle,
                              .local_cid = kStartingDestinationCid,
                              .remote_cid = kStartingSourceCid,
                              .event_fn = event_fn});

  BasicL2capChannel chan2 =
      BuildBasicL2capChannel(proxy,
                             {.handle = kConnectionHandle,
                              .local_cid = kStartingDestinationCid + 1,
                              .remote_cid = kStartingSourceCid + 1,
                              .event_fn = event_fn});

  BasicL2capChannel chan3 =
      BuildBasicL2capChannel(proxy,
                             {.handle = kConnectionHandle,
                              .local_cid = kStartingDestinationCid + 2,
                              .remote_cid = kStartingSourceCid + 2,
                              .event_fn = event_fn});

  EXPECT_EQ(GetState(chan1), L2capChannel::State::kRunning);
  EXPECT_EQ(GetState(chan2), L2capChannel::State::kRunning);
  EXPECT_EQ(GetState(chan3), L2capChannel::State::kRunning);

  // Close chan1's & chan2's underlying L2CAP connections.
  PW_TEST_EXPECT_OK(
      SendL2capDisconnectRsp(proxy,
                             Direction::kFromHost,
                             AclTransportType::kLe,
                             kConnectionHandle,
                             /*source_cid=*/kStartingSourceCid,
                             /*destination_cid=*/kStartingDestinationCid));
  PW_TEST_EXPECT_OK(
      SendL2capDisconnectRsp(proxy,
                             Direction::kFromHost,
                             AclTransportType::kLe,
                             kConnectionHandle,
                             /*source_cid=*/kStartingSourceCid + 2,
                             /*destination_cid=*/kStartingDestinationCid + 2));

  EXPECT_EQ(events_received, 2);
  EXPECT_EQ(GetState(chan1), L2capChannel::State::kClosed);
  EXPECT_EQ(GetState(chan2), L2capChannel::State::kRunning);
  EXPECT_EQ(GetState(chan3), L2capChannel::State::kClosed);

  // Confirm HCI disconnection only closes remaining channel.
  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, kConnectionHandle));
  EXPECT_EQ(GetState(chan2), L2capChannel::State::kClosed);
  EXPECT_EQ(events_received, 3);
}

TEST_F(ProxyHostConnectionEventTest,
       L2capDisconnectionRspFromControllerClosesChannels) {
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [](H4PacketWithHci&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));

  constexpr uint16_t kStartingCid = 0x111;
  int events_received = 0;
  auto event_fn = [&events_received](L2capChannelEvent event) {
    ++events_received;
    EXPECT_EQ(event, L2capChannelEvent::kChannelClosedByOther);
  };
  BasicL2capChannel chan1 = BuildBasicL2capChannel(proxy,
                                                   {.handle = kConnectionHandle,
                                                    .local_cid = kStartingCid,
                                                    .remote_cid = kStartingCid,
                                                    .event_fn = event_fn});

  BasicL2capChannel chan2 =
      BuildBasicL2capChannel(proxy,
                             {.handle = kConnectionHandle,
                              .local_cid = kStartingCid + 1,
                              .remote_cid = kStartingCid + 1,
                              .event_fn = event_fn});

  BasicL2capChannel chan3 =
      BuildBasicL2capChannel(proxy,
                             {.handle = kConnectionHandle,
                              .local_cid = kStartingCid + 2,
                              .remote_cid = kStartingCid + 2,
                              .event_fn = event_fn});

  EXPECT_EQ(GetState(chan1), L2capChannel::State::kRunning);
  EXPECT_EQ(GetState(chan2), L2capChannel::State::kRunning);
  EXPECT_EQ(GetState(chan3), L2capChannel::State::kRunning);

  // Close chan1's & chan2's underlying L2CAP connections.
  PW_TEST_EXPECT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kConnectionHandle,
                                           kStartingCid,
                                           kStartingCid));
  PW_TEST_EXPECT_OK(SendL2capDisconnectRsp(proxy,
                                           Direction::kFromController,
                                           AclTransportType::kLe,
                                           kConnectionHandle,
                                           kStartingCid + 2,
                                           kStartingCid + 2));

  EXPECT_EQ(events_received, 2);
  EXPECT_EQ(GetState(chan1), L2capChannel::State::kClosed);
  EXPECT_EQ(GetState(chan2), L2capChannel::State::kRunning);
  EXPECT_EQ(GetState(chan3), L2capChannel::State::kClosed);

  // Confirm HCI disconnection only closes remaining channel.
  PW_TEST_EXPECT_OK(SendDisconnectionCompleteEvent(proxy, kConnectionHandle));
  EXPECT_EQ(GetState(chan2), L2capChannel::State::kClosed);
  EXPECT_EQ(events_received, 3);
}

// ########## AclFragTest

// TODO: https://pwbug.dev/438553038 - Update tests to also test in to-host
// direction.
class AclFragTest : public ProxyHostTest {
 protected:
  static constexpr uint16_t kHandle = 0x04AD;
  static constexpr uint16_t kLocalCid = 0xC1D;

  ProxyHost GetProxy() {
    // We can't add a ProxyHost member because it makes the test fixture too
    // large, so we provide a helper function instead.
    return ProxyHost(pw::bind_member<&AclFragTest::SendToHost>(this),
                     pw::bind_member<&AclFragTest::SendToController>(this),
                     /*le_acl_credits_to_reserve=*/0,
                     /*br_edr_acl_credits_to_reserve=*/0,
                     GetProxyHostAllocator());
  }

  BasicL2capChannel GetL2capChannel(
      ProxyHost& proxy, MultiBufAllocator* rx_multibuf_allocator = nullptr) {
    return BuildBasicL2capChannel(
        proxy,
        BasicL2capParameters{
            .rx_multibuf_allocator = rx_multibuf_allocator,
            .handle = kHandle,
            .local_cid = kLocalCid,
            .remote_cid = 0x123,
            .transport = AclTransportType::kLe,
            .payload_from_controller_fn =
                [this](FlatConstMultiBuf&& buffer) {
                  payloads_from_controller_.emplace_back(std::move(buffer));
                  return std::nullopt;  // Consume
                },
        });
  }

  // Verify the payloads the client received.
  // Also dtor them (in some cases they may have been allocated in the test).
  void ExpectClientReceivedPayloadsAndClear(
      std::initializer_list<ConstByteSpan> expected_payloads) {
    ASSERT_EQ(payloads_from_controller_.size(), expected_payloads.size());
    auto payloads_iter = payloads_from_controller_.begin();
    for (ConstByteSpan expected : expected_payloads) {
      const FlatConstMultiBuf& mbuf = *payloads_iter++;
      ConstByteSpan payload = as_bytes(MultiBufAdapter::AsSpan(mbuf));
      EXPECT_TRUE(std::equal(
          payload.begin(), payload.end(), expected.begin(), expected.end()));
    }
    payloads_from_controller_.clear();
  }

  void VerifyNormalOperationAfterRecombination(ProxyHost& proxy) {
    // Verify things work normally after recombination ends.
    static constexpr std::array<uint8_t, 4> kPayload = {'D', 'o', 'n', 'e'};
    payloads_from_controller_.clear();
    SendL2capBFrame(proxy, kHandle, kPayload, kPayload.size(), kLocalCid);
    ExpectClientReceivedPayloadsAndClear({
        as_bytes(span(kPayload)),
    });
  }

  int packets_sent_to_host_ = 0;
  int packets_sent_to_controller_ = 0;

 private:
  void SendToHost(H4PacketWithHci&& /*packet*/) { ++packets_sent_to_host_; }

  void SendToController(H4PacketWithH4&& /*packet*/) {
    ++packets_sent_to_controller_;
  }

  std::vector<FlatConstMultiBufInstance> payloads_from_controller_;
};

TEST_F(AclFragTest, AclBiggerThanL2capDropped) {
  ProxyHost proxy = GetProxy();
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(packets_sent_to_host_, 1);
  BasicL2capChannel channel = GetL2capChannel(proxy);

  // Send an ACL packet with more data than L2CAP header indicates.
  static constexpr std::array<uint8_t, 4> kPayload{};
  SendL2capBFrame(proxy, kHandle, kPayload, 1, kLocalCid);

  // Should be dropped.
  EXPECT_EQ(packets_sent_to_host_, 1);
  ExpectClientReceivedPayloadsAndClear({});
}

TEST_F(AclFragTest, RecombinationWorksWithEmptyFirstPayload) {
  ProxyHost proxy = GetProxy();
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(packets_sent_to_host_, 1);
  BasicL2capChannel channel = GetL2capChannel(proxy);

  static constexpr std::array<uint8_t, 4> kPayload = {0xA1, 0xB2, 0xC3, 0xD2};

  // Fragment 1: ACL Header + L2CAP B-Frame Header + (no payload)
  PW_LOG_INFO("Sending frag 1: ACL + L2CAP header");
  SendL2capBFrame(proxy, kHandle, {}, kPayload.size(), kLocalCid);

  // Fragment 2: ACL Header + Payload frag 2
  PW_LOG_INFO("Sending frag 2: ACL(CONT) + payload2");
  SendAclContinuingFrag(proxy, kHandle, kPayload);

  EXPECT_EQ(packets_sent_to_host_, 1);
  ExpectClientReceivedPayloadsAndClear({
      as_bytes(span(kPayload)),
  });

  VerifyNormalOperationAfterRecombination(proxy);
}

// If a client channel is dropped between first and last
// packet of a fragmented PDU, then packet should be dropped.
// Under msan this test also verifies code is not trying to access channel
// allocator's memory after channel dtor.
TEST_F(AclFragTest, ChannelDtorDuringRecombinationDropsPdu) {
  ProxyHost proxy = GetProxy();
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(packets_sent_to_host_, 1);
  static constexpr std::array<uint8_t, 4> kPayload = {0xA1, 0xB2, 0xC3, 0xD2};

  {
    BasicL2capChannel channel = GetL2capChannel(proxy);

    // Fragment 1: ACL Header + L2CAP B-Frame Header + (no payload)
    PW_LOG_INFO("Sending frag 1: ACL + L2CAP header");

    SendL2capBFrame(proxy, kHandle, {}, kPayload.size(), kLocalCid);

    // Dtor of channel and allocator.
  }

  // Fragment 2: ACL Header + Payload frag 2
  PW_LOG_INFO("Sending frag 2: ACL(CONT) + payload2");
  // Since channel was destroyed before this, channel allocator's memory should
  // not be accessed (msan will verify).
  SendAclContinuingFrag(proxy, kHandle, kPayload);

  // Since channel was destroyed before 2nd fragment was sent, PDU should have
  // been dropped.
  EXPECT_EQ(packets_sent_to_host_, 1);
  ExpectClientReceivedPayloadsAndClear({});

  // Open up channel again to verify rx still works after completing above.
  BasicL2capChannel channel2 = GetL2capChannel(proxy);
  VerifyNormalOperationAfterRecombination(proxy);
}

// During recombination dtor first channel, but then create new channel with
// same cid. Verify recombination is properly dropped.
TEST_F(AclFragTest, ChannelDtorAndNewChannelDuringRecombination) {
  ProxyHost proxy = GetProxy();
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(packets_sent_to_host_, 1);
  static constexpr std::array<uint8_t, 4> kPayload = {0xA1, 0xB2, 0xC3, 0xD2};

  {
    BasicL2capChannel channel = GetL2capChannel(proxy);

    // Fragment 1: ACL Header + L2CAP B-Frame Header + (no payload)
    PW_LOG_INFO("Sending frag 1: ACL + L2CAP header");

    SendL2capBFrame(proxy, kHandle, {}, kPayload.size(), kLocalCid);

    // Dtor of channel and allocator.
  }

  // Open up L2CAP channel with same channel id on same connection.
  BasicL2capChannel channel2 = GetL2capChannel(proxy);

  // Fragment 2: ACL Header + Payload frag 2
  PW_LOG_INFO("Sending frag 2: ACL(CONT) + payload2");
  // Since channel1 was destroyed before this, channel1 allocator's
  // memory should not be accessed (msan will verify).
  SendAclContinuingFrag(proxy, kHandle, kPayload);

  // Since channel1 was destroyed before 2nd fragment was sent, its PDU should
  // have been dropped even though channel2 with same cid was created.
  EXPECT_EQ(packets_sent_to_host_, 1);
  ExpectClientReceivedPayloadsAndClear({});

  // Verify rx to channel2 still works.
  VerifyNormalOperationAfterRecombination(proxy);
}

// Ensure expected handling of channel not having enough allocator space to fit
// the recombined buffer. Current behavior is to pass first and any continuing
// packets to AP.
// TODO: https://pwbug.dev/404275508 - We should probably do something different
// in this case (like stopping channel or at least sending it an event).
TEST_F(AclFragTest, ChannelCantAllocateMultibuf) {
  ProxyHost proxy = GetProxy();
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(packets_sent_to_host_, 1);

  // Intentionally use allocator without enough room for PDU buf.
  MultiBufAllocatorContext<1> allocator_context;
  MultiBufAllocator& allocator = allocator_context.GetAllocator();
  BasicL2capChannel channel = GetL2capChannel(proxy, &allocator);

  static constexpr std::array<uint8_t, 4> kPayload = {0xA1, 0xB2, 0xC3, 0xD2};

  // Fragment 1: ACL Header + L2CAP B-Frame Header + (no payload)
  PW_LOG_INFO("Sending frag 1: ACL + L2CAP header");
  SendL2capBFrame(proxy, kHandle, {}, kPayload.size(), kLocalCid);

  // Fragment 2: ACL Header + Payload frag 2
  PW_LOG_INFO("Sending frag 2: ACL(CONT) + payload2");
  SendAclContinuingFrag(proxy, kHandle, kPayload);

  // Both packets should have been sent to host.
  EXPECT_EQ(packets_sent_to_host_, 3);
  // No payloads should have been sent to the client.
  ExpectClientReceivedPayloadsAndClear({});
}

// When an ACL fragmented payload is received for a channel with no rx allocator
// the original ACL packets are passed to the host.
// This currently can happen for signal and GATT channels.
// TODO: https://pwbug.dev/423695410 - In future we should always support
// recombination so the client has the option to reject.
TEST_F(AclFragTest, ChannelHasNoRxAllocator) {
  // GATT fixed CID is 0x04.
  uint16_t local_cid = 0x0004;

  constexpr uint8_t kPayloadFragmentSize = 3;
  constexpr uint8_t kRecombinedPayloadSize = kPayloadFragmentSize * 2;
  std::array<uint8_t, kPayloadFragmentSize> payload_fragment = {
      0xAB, 0xCD, 0xEF};

  std::array<uint8_t,
             emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                 emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
                 kPayloadFragmentSize>
      hci_first{};
  std::array<uint8_t,
             emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                 emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
                 kPayloadFragmentSize>
      hci_cont{};

  struct {
    int to_host_called = 0;
    std::array<H4PacketWithHci, 2> h4s;
  } capture{.h4s = {H4PacketWithHci{emboss::H4PacketType::ACL_DATA, hci_first},
                    H4PacketWithHci{emboss::H4PacketType::ACL_DATA, hci_cont}}};

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        if (packet.GetH4Type() != emboss::H4PacketType::ACL_DATA) {
          return;
        }
        auto expected_hci = capture.h4s[capture.to_host_called].GetHciSpan();
        EXPECT_TRUE(std::equal(packet.GetHciSpan().begin(),
                               packet.GetHciSpan().end(),
                               expected_hci.begin(),
                               expected_hci.end()));
        ++capture.to_host_called;
      });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));

  GattNotifyChannel channel = BuildGattNotifyChannel(proxy,
                                                     {
                                                         .handle = kHandle,
                                                     });

  {
    // Define and send first fragment.
    Result<emboss::AclDataFrameWriter> acl =
        MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_first);
    acl->header().handle().Write(kHandle);
    acl->header().packet_boundary_flag().Write(
        emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
    acl->header().broadcast_flag().Write(
        emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
    acl->data_total_length().Write(
        emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
        kPayloadFragmentSize);

    emboss::BFrameWriter bframe = emboss::MakeBFrameView(
        acl->payload().BackingStorage().data(), acl->payload().SizeInBytes());
    // We are going to send twice the expected payload (over two fragments).
    bframe.pdu_length().Write(kRecombinedPayloadSize);
    bframe.channel_id().Write(local_cid);
    std::copy(payload_fragment.begin(),
              payload_fragment.end(),
              bframe.payload().BackingStorage().begin());

    std::array<uint8_t, hci_first.size()> hci_first_send{};
    std::copy(hci_first.begin(), hci_first.end(), hci_first_send.begin());

    H4PacketWithHci h4_send{emboss::H4PacketType::ACL_DATA, hci_first_send};
    proxy.HandleH4HciFromController(std::move(h4_send));

    // ACL fragment should be delivered to host since channel can't
    // recombine. An error should be logged also, but we don't have way in
    // Pigweed to test that here.
    EXPECT_EQ(capture.to_host_called, 1);
    ExpectClientReceivedPayloadsAndClear({});
  }

  {
    // Define and send 2nd fragment.
    Result<emboss::AclDataFrameWriter> acl =
        MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_first);
    acl->header().handle().Write(kHandle);
    acl->header().packet_boundary_flag().Write(
        emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
    acl->header().broadcast_flag().Write(
        emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
    // Just contains the 2nd payload with no l2cap headers.
    acl->data_total_length().Write(kPayloadFragmentSize);

    // Entire ACL payload is just the fragment.
    std::copy(payload_fragment.begin(),
              payload_fragment.end(),
              acl->payload().BackingStorage().begin());

    std::array<uint8_t, hci_cont.size()> hci_cont_send{};
    std::copy(hci_cont.begin(), hci_cont.end(), hci_cont_send.begin());

    H4PacketWithHci h4_send{emboss::H4PacketType::ACL_DATA, hci_cont_send};
    proxy.HandleH4HciFromController(std::move(h4_send));

    // ACL fragment should be delivered to host since channel can't recombine.
    // The fact there were two fragments also verifies that recombination didn't
    // happen.
    EXPECT_EQ(capture.to_host_called, 2);
    ExpectClientReceivedPayloadsAndClear({});
  }
}

// If a recombined PDU is unhandled, then a ACL packet containing the entire
// PDU should be passed on to host.
TEST_F(AclFragTest, UnhandledRecombinedPdu) {
  constexpr uint8_t kPayloadFragmentSize = 3;
  constexpr uint8_t kRecombinedPayloadSize = kPayloadFragmentSize * 2;
  // rename to fragment
  std::array<uint8_t, kPayloadFragmentSize> payload_first = {0x04, 0x05, 0x06};
  std::array<uint8_t, kPayloadFragmentSize> payload_cont = {0x07, 0x08, 0x09};

  std::array<uint8_t,
             emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                 emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
                 kPayloadFragmentSize>
      hci_first{};
  std::array<uint8_t,
             emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                 emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
                 kPayloadFragmentSize>
      hci_cont{};

  std::array<uint8_t,
             emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                 emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
                 kRecombinedPayloadSize>
      hci_recombined{};

  {
    // Fill expected combined packet.
    Result<emboss::AclDataFrameWriter> acl =
        MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_recombined);
    acl->header().handle().Write(kHandle);
    // Controller to Host are always flushable (except for loopback), per
    // Volume 4, Part E, 5.4.2, Packet_Boundary_Flag table.
    acl->header().packet_boundary_flag().Write(
        emboss::AclDataPacketBoundaryFlag::FIRST_FLUSHABLE);
    acl->header().broadcast_flag().Write(
        emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
    acl->data_total_length().Write(
        emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
        kRecombinedPayloadSize);

    emboss::BFrameWriter bframe = emboss::MakeBFrameView(
        acl->payload().BackingStorage().data(), acl->payload().SizeInBytes());
    // We are going to send twice the expected payload (over two fragments).
    bframe.pdu_length().Write(kRecombinedPayloadSize);
    bframe.channel_id().Write(kLocalCid);
    std::copy(payload_first.begin(),
              payload_first.end(),
              bframe.payload().BackingStorage().begin());
    std::copy(payload_cont.begin(),
              payload_cont.end(),
              bframe.payload().BackingStorage().begin() + kPayloadFragmentSize);
  }

  struct {
    int channel_pdus_received = 0;
    int to_host_acls = 0;
    H4PacketWithHci h4;
  } capture{
      .h4 = H4PacketWithHci{emboss::H4PacketType::ACL_DATA, hci_recombined}};

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        if (packet.GetH4Type() == emboss::H4PacketType::EVENT) {
          // No need to verify the READ_BUFFER event.
          return;
        }
        ++capture.to_host_acls;
        EXPECT_EQ(packet.GetH4Type(), emboss::H4PacketType::ACL_DATA);
        auto expected_hci = capture.h4.GetHciSpan();
        EXPECT_TRUE(std::equal(packet.GetHciSpan().begin(),
                               packet.GetHciSpan().end(),
                               expected_hci.begin(),
                               expected_hci.end()));
      });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);

  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(
      proxy, /*num_credits_to_reserve=*/0));
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(capture.to_host_acls, 0);

  BasicL2capChannel channel = BuildBasicL2capChannel(
      proxy,
      BasicL2capParameters{
          .handle = kHandle,
          .local_cid = kLocalCid,
          .payload_from_controller_fn =
              [&capture](FlatConstMultiBuf&& buffer) {
                capture.channel_pdus_received++;
                // Unhandled
                return FlatConstMultiBufInstance(std::move(buffer));
              },
      });

  EXPECT_EQ(capture.channel_pdus_received, 0);

  {
    // Define and send first fragment.
    Result<emboss::AclDataFrameWriter> acl =
        MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_first);
    acl->header().handle().Write(kHandle);
    acl->header().packet_boundary_flag().Write(
        emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
    acl->header().broadcast_flag().Write(
        emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
    acl->data_total_length().Write(
        emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
        kPayloadFragmentSize);

    emboss::BFrameWriter bframe = emboss::MakeBFrameView(
        acl->payload().BackingStorage().data(), acl->payload().SizeInBytes());
    bframe.pdu_length().Write(kRecombinedPayloadSize);
    bframe.channel_id().Write(kLocalCid);
    std::copy(payload_first.begin(),
              payload_first.end(),
              bframe.payload().BackingStorage().begin());

    std::array<uint8_t, hci_first.size()> hci_send{};
    std::copy(hci_first.begin(), hci_first.end(), hci_send.begin());

    H4PacketWithHci h4_send{emboss::H4PacketType::ACL_DATA, hci_send};
    proxy.HandleH4HciFromController(std::move(h4_send));

    // ACL fragment should be added to partial recombined PDU. Nothing passed
    // on.
    EXPECT_EQ(capture.to_host_acls, 0);
    EXPECT_EQ(capture.channel_pdus_received, 0);
  }

  {
    // Define and send 2nd fragment.
    Result<emboss::AclDataFrameWriter> acl =
        MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_cont);
    acl->header().handle().Write(kHandle);
    acl->header().packet_boundary_flag().Write(
        emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
    acl->header().broadcast_flag().Write(
        emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
    // Just contains the 2nd payload with no l2cap headers.
    acl->data_total_length().Write(kPayloadFragmentSize);

    // Entire ACL payload is just the fragment.
    std::copy(payload_cont.begin(),
              payload_cont.end(),
              acl->payload().BackingStorage().begin());

    std::array<uint8_t, hci_cont.size()> hci_send{};
    std::copy(hci_cont.begin(), hci_cont.end(), hci_send.begin());

    H4PacketWithHci h4_send{emboss::H4PacketType::ACL_DATA, hci_send};
    proxy.HandleH4HciFromController(std::move(h4_send));

    // Recombined ACL packet should be delivered to host since channel rejected
    // it.
    EXPECT_EQ(capture.to_host_acls, 1);
    // Channel received the PDU, but rejected it.
    EXPECT_EQ(capture.channel_pdus_received, 1);
  }
}

TEST_F(AclFragTest, RecombinationWorksWithSplitPayloads) {
  ProxyHost proxy = GetProxy();
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(packets_sent_to_host_, 1);
  BasicL2capChannel channel = GetL2capChannel(proxy);

  static constexpr std::array<uint8_t, 2> kPayloadFrag1 = {0xA1, 0xB2};
  static constexpr std::array<uint8_t, 2> kPayloadFrag2 = {0xC3, 0xD2};
  static constexpr std::array<uint8_t, 4> kPayload = {0xA1, 0xB2, 0xC3, 0xD2};

  constexpr int kNumIter = 4;

  for (int i = 0; i < kNumIter; ++i) {
    // Fragment 1: ACL Header + L2CAP B-Frame Header + Payload frag 1
    PW_LOG_INFO("Sending frag 1: ACL + L2CAP header + payload1");
    SendL2capBFrame(proxy, kHandle, kPayloadFrag1, kPayload.size(), kLocalCid);

    // Fragment 2: ACL Header + Payload frag 2
    PW_LOG_INFO("Sending frag 2: ACL(CONT) + payload2");
    SendAclContinuingFrag(proxy, kHandle, kPayloadFrag2);
  }

  EXPECT_EQ(packets_sent_to_host_, 1);
  ExpectClientReceivedPayloadsAndClear({
      as_bytes(span(kPayload)),
      as_bytes(span(kPayload)),
      as_bytes(span(kPayload)),
      as_bytes(span(kPayload)),
  });

  VerifyNormalOperationAfterRecombination(proxy);
}

TEST_F(AclFragTest, UnexpectedContinuingFragment) {
  ProxyHost proxy = GetProxy();
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(packets_sent_to_host_, 1);
  BasicL2capChannel channel = GetL2capChannel(proxy);

  static constexpr std::array<uint8_t, 4> kPayload = {0xA1, 0xB2, 0xC3, 0xD2};

  // Send an unexpected CONTINUING_FRAGMENT
  PW_LOG_INFO("Sending frag 1: ACL(CONT) + payload");
  SendAclContinuingFrag(proxy, kHandle, kPayload);

  ExpectClientReceivedPayloadsAndClear({});
  EXPECT_EQ(packets_sent_to_host_, 2);  // Should be passed on to host

  VerifyNormalOperationAfterRecombination(proxy);
}

TEST_F(AclFragTest, UnexpectedFirstFragment) {
  ProxyHost proxy = GetProxy();
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(packets_sent_to_host_, 1);
  BasicL2capChannel channel = GetL2capChannel(proxy);

  static constexpr std::array<uint8_t, 2> kPayloadFrag1 = {0xA1, 0xB2};
  static constexpr std::array<uint8_t, 2> kPayloadFrag2 = {0xC3, 0xD2};
  static constexpr std::array<uint8_t, 4> kPayload = {0xA1, 0xB2, 0xC3, 0xD2};

  // PDU A: Fragment 1: Start recombination by sending first fragment.
  PW_LOG_INFO("Sending frag 1: ACL + L2CAP header + payload1");
  SendL2capBFrame(proxy, kHandle, {}, 100, kLocalCid);

  // We never send the 100 byte payload here.

  // So this new first-fragment is unexpected:
  // PDU B: Fragment 1: ACL Header + L2CAP B-Frame Header + Payload frag 1
  PW_LOG_INFO("Sending frag 1: ACL + L2CAP header + payload1");
  SendL2capBFrame(proxy, kHandle, kPayloadFrag1, kPayload.size(), kLocalCid);

  // PDU B: Fragment 2: ACL Header + Payload frag 2
  PW_LOG_INFO("Sending frag 2: ACL(CONT) + payload2");
  SendAclContinuingFrag(proxy, kHandle, kPayloadFrag2);

  // Nothing should be sent to the host. The first fragment of PDU A is dropped.
  EXPECT_EQ(packets_sent_to_host_, 1);

  // PDU B is delivered.
  ExpectClientReceivedPayloadsAndClear({
      as_bytes(span(kPayload)),
  });

  VerifyNormalOperationAfterRecombination(proxy);
}

TEST_F(AclFragTest, ContinuingFragmentTooLarge) {
  ProxyHost proxy = GetProxy();
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(packets_sent_to_host_, 1);
  BasicL2capChannel channel = GetL2capChannel(proxy);

  static constexpr std::array<uint8_t, 2> kPayloadFrag1 = {0xA1, 0xB2};
  static constexpr std::array<uint8_t, 5> kPayloadFrag2TooBig = {
      0xC3, 0xD2, 0xBA, 0xAA, 0xAD};
  static constexpr std::array<uint8_t, 4> kPayload = {0xA1, 0xB2, 0xC3, 0xD2};

  // Fragment 1: ACL Header + L2CAP B-Frame Header + Payload frag 1
  PW_LOG_INFO("Sending frag 1: ACL + L2CAP header + payload1");
  SendL2capBFrame(proxy, kHandle, kPayloadFrag1, kPayload.size(), kLocalCid);

  // Fragment 2: ACL Header + Payload frag 2
  PW_LOG_INFO("Sending frag 2: ACL(CONT) + payload2 (too big)");
  SendAclContinuingFrag(proxy, kHandle, kPayloadFrag2TooBig);

  ExpectClientReceivedPayloadsAndClear({});

  // This was for a channel owned by the proxy so it should have been dropped.
  EXPECT_EQ(packets_sent_to_host_, 1);

  VerifyNormalOperationAfterRecombination(proxy);
}

TEST_F(AclFragTest,
       CanReceiveUnfragmentedPduOnOneChannelWhileRecombiningOnAnother) {
  ProxyHost proxy = GetProxy();
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(packets_sent_to_host_, 1);

  // Channel 1
  static constexpr std::array<uint8_t, 2> kPayload1Frag1 = {0xA1, 0xB2};
  static constexpr std::array<uint8_t, 2> kPayload1Frag2 = {0xC3, 0xD2};
  static constexpr std::array<uint8_t, 4> kPayload1 = {0xA1, 0xB2, 0xC3, 0xD2};

  int channel1_sends_called = 0;
  BasicL2capChannel channel = BuildBasicL2capChannel(
      proxy,
      BasicL2capParameters{
          .handle = kHandle,
          .local_cid = kLocalCid,
          .remote_cid = 0x123,
          .transport = AclTransportType::kLe,
          .payload_from_controller_fn =
              [&channel1_sends_called](FlatConstMultiBuf&& buffer) {
                ++channel1_sends_called;
                PW_ASSERT(!buffer.empty());
                ConstByteSpan payload = *buffer.ConstChunks().begin();
                ConstByteSpan expected_bytes = as_bytes(span(kPayload1));
                EXPECT_TRUE(std::equal(payload.begin(),
                                       payload.end(),
                                       expected_bytes.begin(),
                                       expected_bytes.end()));
                return std::nullopt;
              },
      });

  // Channel 2
  static constexpr uint16_t kHandle2 = 0x4D2;
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle2, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(packets_sent_to_host_, 2);
  static constexpr uint16_t kLocalCid2 = 0xC2D;
  static constexpr std::array<uint8_t, 4> kPayload2 = {0x33, 0x66, 0x99, 0xCC};

  int channel2_sends_called = 0;
  BasicL2capChannel channel2 = BuildBasicL2capChannel(
      proxy,
      BasicL2capParameters{
          .handle = kHandle2,
          .local_cid = kLocalCid2,
          .remote_cid = 0x321,
          .transport = AclTransportType::kLe,
          .payload_from_controller_fn =
              [&channel2_sends_called](FlatConstMultiBuf&& buffer) {
                ++channel2_sends_called;
                PW_ASSERT(!buffer.empty());
                ConstByteSpan payload = *buffer.ConstChunks().begin();
                ConstByteSpan expected_bytes = as_bytes(span(kPayload2));
                EXPECT_TRUE(std::equal(payload.begin(),
                                       payload.end(),
                                       expected_bytes.begin(),
                                       expected_bytes.end()));
                return std::nullopt;
              },
      });

  // Channel 1: Fragment 1: ACL Header + L2CAP B-Frame Header + Payload frag 1
  PW_LOG_INFO("Sending frag 1: ACL + L2CAP header + payload1");
  SendL2capBFrame(proxy, kHandle, kPayload1Frag1, kPayload1.size(), kLocalCid);

  // Channel 2: Send full PDU
  SendL2capBFrame(proxy, kHandle2, kPayload2, kPayload2.size(), kLocalCid2);
  EXPECT_EQ(channel2_sends_called, 1);

  // Channel 1: Fragment 2: ACL Header + Payload frag 2
  PW_LOG_INFO("Sending frag 2: ACL(CONT) + payload2");
  SendAclContinuingFrag(proxy, kHandle, kPayload1Frag2);

  EXPECT_EQ(channel1_sends_called, 1);
  EXPECT_EQ(packets_sent_to_host_, 2);
}

TEST_F(ProxyHostTest, ClientProvidedAllocatorUsedForH4) {
  std::array<uint8_t, 100> payload = {};
  pw::allocator::test::AllocatorForTest<4096> allocator;

  struct {
    int sends_called = 0;
    pw::Vector<H4PacketWithH4, 5> sent_packets;
  } capture;

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      []([[maybe_unused]] H4PacketWithHci&& packet) {});
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [&capture](H4PacketWithH4&& packet) {
        ++capture.sends_called;
        capture.sent_packets.emplace_back(std::move(packet));
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/10,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              &allocator);
  StartDispatcherOnCurrentThread(proxy);
  size_t initial = allocator.GetAllocated();

  // Allow proxy to reserve 1 LE credit.
  PW_TEST_EXPECT_OK(SendLeReadBufferResponseFromController(proxy, 10));

  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  EXPECT_GT(allocator.GetAllocated(), initial);
  size_t allocated_for_connection = allocator.GetAllocated();

  constexpr uint16_t remote_channel_id = 0x1234;
  constexpr uint16_t local_channel_id = 0x4321;

  {
    BasicL2capChannel channel =
        BuildBasicL2capChannel(proxy,
                               {.handle = kConnectionHandle,
                                .local_cid = local_channel_id,
                                .remote_cid = remote_channel_id,
                                .transport = AclTransportType::kLe});

    FlatMultiBufInstance mbuf_inst = MultiBufFromSpan(pw::span(payload));
    FlatMultiBuf& mbuf = MultiBufAdapter::Unwrap(mbuf_inst);
    PW_TEST_EXPECT_OK(channel.Write(std::move(mbuf)).status);
    RunDispatcher();
    EXPECT_EQ(capture.sends_called, 1);
  }
  EXPECT_GT(allocator.GetAllocated(),
            initial + allocated_for_connection + payload.size());

  capture.sent_packets.clear();
  proxy.Reset();
  EXPECT_EQ(allocator.GetAllocated(), initial);
}

TEST_F(ProxyHostTest, NotEnoughMemoryToAllocateConnection) {
  // Allocator is too small to hold connection state.
  pw::allocator::test::AllocatorForTest<0x600> allocator;

  size_t host_called = 0;
  pw::Function<void(H4PacketWithH4 && packet)> send_to_controller_fn(
      []([[maybe_unused]] H4PacketWithH4&& packet) {});

  pw::Function<void(H4PacketWithHci && packet)> send_to_host_fn(
      [&host_called]([[maybe_unused]] H4PacketWithHci&& packet) {
        ++host_called;
      });

  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              &allocator);
  StartDispatcherOnCurrentThread(proxy);

  // Connection should silently fail for connection complete events.
  allocator.Exhaust();
  PW_TEST_EXPECT_OK(SendLeConnectionCompleteEvent(
      proxy, kConnectionHandle, emboss::StatusCode::SUCCESS));
  EXPECT_EQ(host_called, 1U);

  Result<BasicL2capChannel> channel =
      BuildBasicL2capChannelWithResult(proxy,
                                       {.handle = kConnectionHandle,
                                        .local_cid = 0x123,
                                        .remote_cid = 0x456,
                                        .transport = AclTransportType::kLe});
  EXPECT_EQ(channel.status(), Status::InvalidArgument());
}

TEST_F(AclFragTest, UnhandledRecombinedPduBeforeMaxLeAclLengthKnown) {
  constexpr uint8_t kPayloadFragmentSize = 3;
  constexpr uint8_t kRecombinedPayloadSize = kPayloadFragmentSize * 2;
  std::array<uint8_t, kPayloadFragmentSize> payload_first = {0x04, 0x05, 0x06};
  std::array<uint8_t, kPayloadFragmentSize> payload_cont = {0x07, 0x08, 0x09};

  std::array<uint8_t,
             emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                 emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
                 kPayloadFragmentSize>
      hci_first{};
  std::array<uint8_t,
             emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                 emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
                 kPayloadFragmentSize>
      hci_cont{};

  std::array<uint8_t,
             emboss::AclDataFrameHeader::IntrinsicSizeInBytes() +
                 emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
                 kRecombinedPayloadSize>
      hci_recombined{};

  {
    Result<emboss::AclDataFrameWriter> acl =
        MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_recombined);
    acl->header().handle().Write(kHandle);
    acl->header().packet_boundary_flag().Write(
        emboss::AclDataPacketBoundaryFlag::FIRST_FLUSHABLE);
    acl->header().broadcast_flag().Write(
        emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
    acl->data_total_length().Write(
        emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
        kRecombinedPayloadSize);

    emboss::BFrameWriter bframe = emboss::MakeBFrameView(
        acl->payload().BackingStorage().data(), acl->payload().SizeInBytes());
    // We are going to send twice the expected payload (over two fragments).
    bframe.pdu_length().Write(kRecombinedPayloadSize);
    bframe.channel_id().Write(kLocalCid);
    std::copy(payload_first.begin(),
              payload_first.end(),
              bframe.payload().BackingStorage().begin());
    std::copy(payload_cont.begin(),
              payload_cont.end(),
              bframe.payload().BackingStorage().begin() + kPayloadFragmentSize);
  }

  struct {
    int channel_pdus_received = 0;
    int to_host_acls = 0;
    H4PacketWithHci h4;
  } capture{
      .h4 = H4PacketWithHci{emboss::H4PacketType::ACL_DATA, hci_recombined}};

  pw::Function<void(H4PacketWithHci && packet)>&& send_to_host_fn(
      [&capture](H4PacketWithHci&& packet) {
        if (packet.GetH4Type() == emboss::H4PacketType::EVENT) {
          return;
        }
        EXPECT_EQ(packet.GetH4Type(), emboss::H4PacketType::ACL_DATA);
        ++capture.to_host_acls;
        auto expected_hci = capture.h4.GetHciSpan();
        EXPECT_TRUE(std::equal(packet.GetHciSpan().begin(),
                               packet.GetHciSpan().end(),
                               expected_hci.begin(),
                               expected_hci.end()));
      });
  pw::Function<void(H4PacketWithH4 && packet)>&& send_to_controller_fn(
      [](H4PacketWithH4&&) {});
  ProxyHost proxy = ProxyHost(std::move(send_to_host_fn),
                              std::move(send_to_controller_fn),
                              /*le_acl_credits_to_reserve=*/0,
                              /*br_edr_acl_credits_to_reserve=*/0,
                              GetProxyHostAllocator());
  StartDispatcherOnCurrentThread(proxy);
  PW_TEST_ASSERT_OK(SendLeConnectionCompleteEvent(
      proxy, kHandle, emboss::StatusCode::SUCCESS));

  // IMPORTANT: No LE Read Buffer Size response is sent from the controller!

  BasicL2capChannel channel = BuildBasicL2capChannel(
      proxy,
      BasicL2capParameters{
          .handle = kHandle,
          .local_cid = kLocalCid,
          .payload_from_controller_fn =
              [&capture](FlatConstMultiBuf&& buffer) {
                capture.channel_pdus_received++;
                // Unhandled
                return FlatConstMultiBufInstance(std::move(buffer));
              },
      });

  EXPECT_EQ(capture.channel_pdus_received, 0);

  {
    // Define and send first fragment.
    Result<emboss::AclDataFrameWriter> acl =
        MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_first);
    acl->header().handle().Write(kHandle);
    acl->header().packet_boundary_flag().Write(
        emboss::AclDataPacketBoundaryFlag::FIRST_NON_FLUSHABLE);
    acl->header().broadcast_flag().Write(
        emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
    acl->data_total_length().Write(
        emboss::BasicL2capHeader::IntrinsicSizeInBytes() +
        kPayloadFragmentSize);

    emboss::BFrameWriter bframe = emboss::MakeBFrameView(
        acl->payload().BackingStorage().data(), acl->payload().SizeInBytes());
    bframe.pdu_length().Write(kRecombinedPayloadSize);
    bframe.channel_id().Write(kLocalCid);
    std::copy(payload_first.begin(),
              payload_first.end(),
              bframe.payload().BackingStorage().begin());

    std::array<uint8_t, hci_first.size()> hci_send{};
    std::copy(hci_first.begin(), hci_first.end(), hci_send.begin());

    H4PacketWithHci h4_send{emboss::H4PacketType::ACL_DATA, hci_send};
    proxy.HandleH4HciFromController(std::move(h4_send));
    EXPECT_EQ(capture.to_host_acls, 0);
    EXPECT_EQ(capture.channel_pdus_received, 0);
  }

  {
    // Define and send 2nd fragment.
    Result<emboss::AclDataFrameWriter> acl =
        MakeEmbossWriter<emboss::AclDataFrameWriter>(hci_cont);
    acl->header().handle().Write(kHandle);
    acl->header().packet_boundary_flag().Write(
        emboss::AclDataPacketBoundaryFlag::CONTINUING_FRAGMENT);
    acl->header().broadcast_flag().Write(
        emboss::AclDataPacketBroadcastFlag::POINT_TO_POINT);
    // Just contains the 2nd payload with no l2cap headers.
    acl->data_total_length().Write(kPayloadFragmentSize);

    // Entire ACL payload is just the fragment.
    std::copy(payload_cont.begin(),
              payload_cont.end(),
              acl->payload().BackingStorage().begin());

    std::array<uint8_t, hci_cont.size()> hci_send{};
    std::copy(hci_cont.begin(), hci_cont.end(), hci_send.begin());

    H4PacketWithHci h4_send{emboss::H4PacketType::ACL_DATA, hci_send};
    proxy.HandleH4HciFromController(std::move(h4_send));

    // Recombined ACL packet should be delivered to host since channel rejected
    // it.
    EXPECT_EQ(capture.to_host_acls, 1);
    // Channel received the PDU, but rejected it.
    EXPECT_EQ(capture.channel_pdus_received, 1);
  }
}

}  // namespace
}  // namespace pw::bluetooth::proxy
