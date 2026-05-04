// Copyright 2023 The Pigweed Authors
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

#include "pw_bluetooth_sapphire/internal/host/transport/command_channel.h"

#include <pw_bluetooth/hci_android.emb.h>
#include <pw_bluetooth/hci_commands.emb.h>
#include <pw_bytes/endian.h>
#include <pw_result/result.h>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/control_packets.h"

namespace bt::hci {
namespace {

using namespace inspect::testing;

using bt::LowerBits;
using bt::UpperBits;
using EventCallbackResult = CommandChannel::EventCallbackResult;

constexpr pw::chrono::SystemClock::duration kCommandTimeout =
    std::chrono::seconds(20);

using TestingBase =
    bt::testing::FakeDispatcherControllerTest<bt::testing::MockController>;

// A reference counted object used to verify that HCI command completion and
// status callbacks are properly cleaned up after the end of a transaction.
class TestCallbackObject {
 public:
  explicit TestCallbackObject(fit::closure deletion_callback)
      : deletion_cb_(std::move(deletion_callback)) {}

  virtual ~TestCallbackObject() { deletion_cb_(); }

 private:
  fit::closure deletion_cb_;
};

class CommandChannelTest : public TestingBase {
 public:
  CommandChannelTest() = default;
  ~CommandChannelTest() override = default;

  pw::async::HeapDispatcher heap_dispatcher() { return heap_dispatcher_; }

  inspect::Inspector inspector_;

 private:
  pw::async::HeapDispatcher heap_dispatcher_{dispatcher()};
};

CommandPacket MakeReadRemoteSupportedFeatures(uint16_t connection_handle) {
  auto packet = CommandPacket::New<
      pw::bluetooth::emboss::ReadRemoteSupportedFeaturesCommandWriter>(
      hci_spec::kReadRemoteSupportedFeatures);
  packet.view_t().connection_handle().Write(connection_handle);
  return packet;
}

TEST_F(CommandChannelTest, SingleRequestResponse) {
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  // Set up expectations:
  // clang-format off
  // HCI_Reset
  StaticByteBuffer req(
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset),  // HCI_Reset opcode
      0x00                                   // parameter_total_size
      );
  // HCI_CommandComplete
 StaticByteBuffer rsp(
      hci_spec::kCommandCompleteEventCode,
      0x04,  // parameter_total_size (4 byte payload)
      0x01,  // num_hci_command_packets (1 can be sent)
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset),  // HCI_Reset opcode
      pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE);
  // clang-format on
  EXPECT_CMD_PACKET_OUT(test_device(), req, &rsp);

  // Send a HCI_Reset command. We attach an instance of TestCallbackObject to
  // the callbacks to verify that it gets cleaned up as expected.
  bool test_obj_deleted = false;
  auto test_obj = std::make_shared<TestCallbackObject>(
      [&test_obj_deleted] { test_obj_deleted = true; });

  auto reset =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  pw::Result<CommandChannel::TransactionId> id = cmd_channel()->SendCommand(
      std::move(reset),
      [&id, test_obj, this](CommandChannel::TransactionId callback_id,
                            const EventPacket& event) {
        auto view =
            event.view<pw::bluetooth::emboss::SimpleCommandCompleteEventView>();
        EXPECT_TRUE(id.ok());
        EXPECT_EQ(id.value(), callback_id);
        EXPECT_EQ(pw::bluetooth::emboss::EventCode::COMMAND_COMPLETE,
                  view.command_complete().header().event_code().Read());
        EXPECT_EQ(
            4, view.command_complete().header().parameter_total_size().Read());
        EXPECT_EQ(1, view.command_complete().num_hci_command_packets().Read());
        EXPECT_EQ(pw::bluetooth::emboss::OpCode::RESET,
                  view.command_complete().command_opcode().Read());
        EXPECT_EQ(pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE,
                  view.status().Read());
        EXPECT_GT(lease_provider().lease_count(), 0u);
      });
  EXPECT_GT(lease_provider().lease_count(), 0u);

  test_obj = nullptr;
  EXPECT_FALSE(test_obj_deleted);
  RunUntilIdle();
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  // Make sure that the I/O thread is no longer holding on to |test_obj|.
  TearDown();

  EXPECT_TRUE(test_obj_deleted);
}

TEST_F(CommandChannelTest, SingleAsynchronousRequest) {
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  // Set up expectations: HCI_Inquiry (general, unlimited, 1s)
  const auto req = testing::InquiryCommandPacket(0x01);
  // HCI_Command_Status
  auto rsp0 =
      testing::CommandStatusPacket(hci_spec::kInquiry,
                                   pw::bluetooth::emboss::StatusCode::SUCCESS,
                                   /*num_packets=*/1);
  // HCI_Inquiry_Complete
  auto rsp1 = StaticByteBuffer(hci_spec::kInquiryCompleteEventCode,
                               0x01,  // parameter_total_size (1 byte payload)
                               pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_CMD_PACKET_OUT(test_device(), req, &rsp0);

  // Send HCI_Inquiry
  pw::Result<CommandChannel::TransactionId> id;
  int cb_count = 0;
  auto cb = [&cb_count, &id, this](CommandChannel::TransactionId callback_id,
                                   const EventPacket& event) {
    cb_count++;
    EXPECT_TRUE(id.ok());
    EXPECT_EQ(callback_id, id.value());
    if (cb_count == 1) {
      ASSERT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
      auto view = event.view<pw::bluetooth::emboss::CommandStatusEventView>();
      EXPECT_EQ(pw::bluetooth::emboss::StatusCode::SUCCESS,
                view.status().Read());
      EXPECT_EQ(pw::bluetooth::emboss::OpCode::INQUIRY,
                view.command_opcode_enum().Read());
      EXPECT_GT(lease_provider().lease_count(), 0u);
    } else {
      EXPECT_EQ(hci_spec::kInquiryCompleteEventCode, event.event_code());
      EXPECT_EQ(fit::ok(), event.ToResult());
    }
  };

  auto packet =
      hci::CommandPacket::New<pw::bluetooth::emboss::InquiryCommandWriter>(
          hci_spec::kInquiry);
  auto view = packet.view_t();
  view.lap().Write(pw::bluetooth::emboss::InquiryAccessCode::GIAC);
  view.inquiry_length().Write(1);
  view.num_responses().Write(0);

  id = cmd_channel()->SendCommand(
      std::move(packet), cb, hci_spec::kInquiryCompleteEventCode);
  EXPECT_GT(lease_provider().lease_count(), 0u);
  RunUntilIdle();
  EXPECT_EQ(1, cb_count);
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  test_device()->SendCommandChannelPacket(rsp1);
  RunUntilIdle();
  EXPECT_EQ(2, cb_count);
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

TEST_F(CommandChannelTest, SingleRequestWithStatusResponse) {
  // Set up expectations
  // clang-format off
  // HCI_Reset for the sake of testing
  auto req = StaticByteBuffer(
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset),  // HCI_Reset opcode
      0x00                                   // parameter_total_size
      );
  // HCI_CommandStatus
  auto rsp = StaticByteBuffer(
       hci_spec::kCommandStatusEventCode,
      0x04,  // parameter_total_size (4 byte payload)
      pw::bluetooth::emboss::StatusCode::SUCCESS, 0x01, // status, num_hci_command_packets (1 can be sent)
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset)  // HCI_Reset opcode
      );
  // clang-format on
  EXPECT_CMD_PACKET_OUT(test_device(), req, &rsp);

  // Send HCI_Reset
  pw::Result<CommandChannel::TransactionId> id;
  auto complete_cb = [&id](CommandChannel::TransactionId callback_id,
                           const EventPacket& event) {
    EXPECT_TRUE(id.ok());
    EXPECT_EQ(callback_id, id.value());
    EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
    auto view = event.view<pw::bluetooth::emboss::CommandStatusEventView>();
    EXPECT_EQ(pw::bluetooth::emboss::StatusCode::SUCCESS, view.status().Read());
    EXPECT_EQ(1, view.num_hci_command_packets().Read());
    EXPECT_EQ(pw::bluetooth::emboss::OpCode::RESET,
              view.command_opcode_enum().Read());
  };

  auto reset =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  id = cmd_channel()->SendCommand(
      std::move(reset), complete_cb, hci_spec::kCommandStatusEventCode);
  RunUntilIdle();
}

// Tests:
//  - Only one HCI command sent until a status is received.
//  - Receiving a status update with a new number of packets available works.
TEST_F(CommandChannelTest, OneSentUntilStatus) {
  // Set up expectations
  // clang-format off
  // HCI_Reset for the sake of testing
  auto req1 = StaticByteBuffer(
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset),  // HCI_Reset opcode
      0x00                                   // parameter_total_size
      );
  auto rsp1 = StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x03,  // parameter_total_size (4 byte payload)
      0x00,  // num_hci_command_packets (None can be sent)
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset)  // HCI_Reset opcode
      );
  auto req2 = StaticByteBuffer(
      LowerBits(hci_spec::kInquiryCancel), UpperBits(hci_spec::kInquiryCancel),  // HCI_InquiryCancel opcode
      0x00                                   // parameter_total_size
      );
  auto rsp2 = StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x03,  // parameter_total_size (4 byte payload)
      0x01,  // num_hci_command_packets (1 can be sent)
      LowerBits(hci_spec::kInquiryCancel), UpperBits(hci_spec::kInquiryCancel)  // HCI_InquiryCancel opcode
      );
  auto rsp_commandsavail = StaticByteBuffer(
       hci_spec::kCommandStatusEventCode,
      0x04,  // parameter_total_size (3 byte payload)
      pw::bluetooth::emboss::StatusCode::SUCCESS, 0x01, // status, num_hci_command_packets (1 can be sent)
      0x00, 0x00 // No associated opcode.
      );
  // clang-format on
  EXPECT_CMD_PACKET_OUT(test_device(), req1, &rsp1);
  EXPECT_CMD_PACKET_OUT(test_device(), req2, &rsp2);

  size_t cb_event_count = 0u;
  size_t transaction_count = 0u;

  test_device()->SetTransactionCallback(
      [&transaction_count]() { transaction_count++; });

  auto cb = [&cb_event_count](CommandChannel::TransactionId,
                              const EventPacket& event) {
    EXPECT_EQ(hci_spec::kCommandCompleteEventCode, event.event_code());
    pw::bluetooth::emboss::OpCode expected_opcode;
    if (cb_event_count == 0u) {
      expected_opcode = pw::bluetooth::emboss::OpCode::RESET;
    } else {
      expected_opcode = pw::bluetooth::emboss::OpCode::INQUIRY_CANCEL;
    }
    EXPECT_EQ(expected_opcode,
              event.view<pw::bluetooth::emboss::CommandCompleteEventView>()
                  .command_opcode()
                  .Read());
    cb_event_count++;
  };

  EXPECT_EQ(lease_provider().lease_count(), 0u);

  auto reset =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  [[maybe_unused]] auto reset_id =
      cmd_channel()->SendCommand(std::move(reset), cb);

  EXPECT_GT(lease_provider().lease_count(), 0u);

  auto inquiry = hci::CommandPacket::New<
      pw::bluetooth::emboss::InquiryCancelCommandWriter>(
      hci_spec::kInquiryCancel);
  [[maybe_unused]] auto inquiry_id =
      cmd_channel()->SendCommand(std::move(inquiry), cb);

  RunUntilIdle();

  EXPECT_EQ(1u, transaction_count);
  EXPECT_EQ(1u, cb_event_count);
  EXPECT_GT(lease_provider().lease_count(), 0u);

  test_device()->SendCommandChannelPacket(rsp_commandsavail);

  RunUntilIdle();

  EXPECT_EQ(2u, transaction_count);
  EXPECT_EQ(2u, cb_event_count);
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

// Tests:
//  - Different opcodes can be sent concurrently
//  - Same opcodes are queued until a status opcode is sent.
TEST_F(CommandChannelTest, QueuedCommands) {
  // Set up expectations
  // clang-format off
  // HCI_Reset for the sake of testing
  auto req_reset = StaticByteBuffer(
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset),  // HCI_Reset opcode
      0x00                                   // parameter_total_size
      );
  auto rsp_reset = StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x03,  // parameter_total_size (4 byte payload)
      0xFF,  // num_hci_command_packets (255 can be sent)
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset)  // HCI_Reset opcode
      );
  auto req_inqcancel = StaticByteBuffer(
      LowerBits(hci_spec::kInquiryCancel), UpperBits(hci_spec::kInquiryCancel),  // HCI_InquiryCancel opcode
      0x00                                   // parameter_total_size
      );
  auto rsp_inqcancel = StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x03,  // parameter_total_size (4 byte payload)
      0xFF,  // num_hci_command_packets (255 can be sent)
      LowerBits(hci_spec::kInquiryCancel), UpperBits(hci_spec::kInquiryCancel)  // HCI_Reset opcode
      );
  auto rsp_commandsavail = StaticByteBuffer(
       hci_spec::kCommandStatusEventCode,
      0x04,  // parameter_total_size (3 byte payload)
      pw::bluetooth::emboss::StatusCode::SUCCESS, 0xFA, // status, num_hci_command_packets (250 can be sent)
      0x00, 0x00 // No associated opcode.
      );
  // clang-format on

  // We handle our own responses to make sure commands are queued.
  EXPECT_CMD_PACKET_OUT(test_device(), req_reset, );
  EXPECT_CMD_PACKET_OUT(test_device(), req_inqcancel, );
  EXPECT_CMD_PACKET_OUT(test_device(), req_reset, &rsp_reset);

  size_t transaction_count = 0u;
  size_t reset_count = 0u;
  size_t cancel_count = 0u;

  test_device()->SetTransactionCallback(
      [&transaction_count]() { transaction_count++; });

  auto cb = [&reset_count, &cancel_count](CommandChannel::TransactionId /*id*/,
                                          const EventPacket& event) {
    EXPECT_EQ(hci_spec::kCommandCompleteEventCode, event.event_code());
    pw::bluetooth::emboss::OpCode opcode =
        event.view<pw::bluetooth::emboss::CommandCompleteEventView>()
            .command_opcode()
            .Read();
    if (opcode == pw::bluetooth::emboss::OpCode::RESET) {
      reset_count++;
    } else if (opcode == pw::bluetooth::emboss::OpCode::INQUIRY_CANCEL) {
      cancel_count++;
    } else {
      EXPECT_TRUE(false) << "Unexpected opcode in command callback!";
    }
  };

  // CommandChannel only one can be sent - update num_hci_command_packets
  test_device()->SendCommandChannelPacket(rsp_commandsavail);

  auto reset =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  cmd_channel()->SendCommand(std::move(reset), cb).IgnoreError();
  auto inquiry_cancel = hci::CommandPacket::New<
      pw::bluetooth::emboss::InquiryCancelCommandWriter>(
      hci_spec::kInquiryCancel);
  cmd_channel()->SendCommand(std::move(inquiry_cancel), cb).IgnoreError();
  reset = hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
      hci_spec::kReset);
  cmd_channel()->SendCommand(std::move(reset), cb).IgnoreError();

  RunUntilIdle();

  // Different opcodes can be sent without a reply
  EXPECT_EQ(2u, transaction_count);

  // Even if we get a response to one, the duplicate opcode is still queued.
  test_device()->SendCommandChannelPacket(rsp_inqcancel);
  RunUntilIdle();

  EXPECT_EQ(2u, transaction_count);
  EXPECT_EQ(1u, cancel_count);
  EXPECT_EQ(0u, reset_count);

  // Once we get a reset back, the second can be sent (and replied to)
  test_device()->SendCommandChannelPacket(rsp_reset);
  RunUntilIdle();

  EXPECT_EQ(3u, transaction_count);
  EXPECT_EQ(1u, cancel_count);
  EXPECT_EQ(2u, reset_count);
}

// Tests:
//  - Asynchronous commands are handled correctly (two callbacks, one for
//    status, one for complete)
//  - Asynchronous commands with the same event result are queued even if they
//    have different opcodes.
//  - Can't register an event handler when an asynchronous command is waiting.
TEST_F(CommandChannelTest, AsynchronousCommands) {
  constexpr hci_spec::EventCode kTestEventCode0 = 0xFE;
  // Set up expectations
  // clang-format off
  // Using HCI_Reset for testing.
  auto req_reset = StaticByteBuffer(
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset),  // HCI_Reset opcode
      0x00                                   // parameter_total_size
      );
  auto rsp_resetstatus = StaticByteBuffer(
       hci_spec::kCommandStatusEventCode,
      0x04,  // parameter_total_size (4 byte payload)
      pw::bluetooth::emboss::StatusCode::SUCCESS, 0xFA, // status, num_hci_command_packets (250 can be sent)
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset)  // HCI_Reset opcode
      );
  auto req_inqcancel = StaticByteBuffer(
      LowerBits(hci_spec::kInquiryCancel), UpperBits(hci_spec::kInquiryCancel),  // HCI_InquiryCancel opcode
      0x00                                   // parameter_total_size
      );
  auto rsp_inqstatus = StaticByteBuffer(
       hci_spec::kCommandStatusEventCode,
      0x04,  // parameter_total_size (4 byte payload)
      pw::bluetooth::emboss::StatusCode::SUCCESS, 0xFA, // status, num_hci_command_packets (250 can be sent)
      LowerBits(hci_spec::kInquiryCancel), UpperBits(hci_spec::kInquiryCancel)  // HCI_Reset opcode
      );
  auto rsp_bogocomplete = StaticByteBuffer(
      kTestEventCode0,
      0x00 // parameter_total_size (no payload)
      );
  // clang-format on

  EXPECT_CMD_PACKET_OUT(test_device(), req_reset, &rsp_resetstatus);
  EXPECT_CMD_PACKET_OUT(test_device(), req_inqcancel, &rsp_inqstatus);

  pw::Result<CommandChannel::TransactionId> id1, id2;
  size_t cb_count = 0u;

  auto cb = [&id1, &id2, &cb_count, kTestEventCode0](
                CommandChannel::TransactionId callback_id,
                const EventPacket& event) {
    if (cb_count < 2) {
      EXPECT_TRUE(id1.ok());
      EXPECT_EQ(id1.value(), callback_id);
    } else {
      EXPECT_TRUE(id2.ok());
      EXPECT_EQ(id2.value(), callback_id);
    }
    if ((cb_count % 2) == 0) {
      EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
      EXPECT_EQ(pw::bluetooth::emboss::StatusCode::SUCCESS,
                event.view<pw::bluetooth::emboss::CommandStatusEventView>()
                    .status()
                    .Read());
    } else if ((cb_count % 2) == 1) {
      EXPECT_EQ(kTestEventCode0, event.event_code());
    }
    cb_count++;
  };

  hci::CommandPacket packet =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  id1 = cmd_channel()->SendCommand(std::move(packet), cb, kTestEventCode0);

  RunUntilIdle();

  // Should have received the Status but not the result.
  EXPECT_EQ(1u, cb_count);

  // Setting another event up with different opcode will still queue the command
  // because we don't want to have two commands waiting on an event.
  packet = hci::CommandPacket::New<
      pw::bluetooth::emboss::InquiryCancelCommandWriter>(
      hci_spec::kInquiryCancel);
  id2 = cmd_channel()->SendCommand(std::move(packet), cb, kTestEventCode0);
  RunUntilIdle();

  EXPECT_EQ(1u, cb_count);

  // Sending the complete will release the queue and send the next command.
  test_device()->SendCommandChannelPacket(rsp_bogocomplete);
  RunUntilIdle();

  EXPECT_EQ(3u, cb_count);

  // Should not be able to register an event handler now, we're still waiting on
  // the asynchronous command.
  auto event_id0 = cmd_channel()->AddEventHandler(
      kTestEventCode0,
      [](const EventPacket&) { return EventCallbackResult::kContinue; });
  EXPECT_EQ(0u, event_id0);

  // Finish out the commands.
  test_device()->SendCommandChannelPacket(rsp_bogocomplete);
  RunUntilIdle();

  EXPECT_EQ(4u, cb_count);
}

// Tests:
//  - Updating to say no commands can be sent works. (commands are queued)
//  - Can't add an event handler once a SendCommand() succeeds watiing on
//    the same event code. (even if they are queued)
TEST_F(CommandChannelTest, AsyncQueueWhenBlocked) {
  constexpr hci_spec::EventCode kTestEventCode0 = 0xF0;
  // Set up expectations
  // clang-format off
  // Using HCI_Reset for testing.
  auto req_reset = StaticByteBuffer(
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset),  // HCI_Reset opcode
      0x00                                   // parameter_total_size
      );
  auto rsp_resetstatus = StaticByteBuffer(
       hci_spec::kCommandStatusEventCode,
      0x04,  // parameter_total_size (4 byte payload)
      pw::bluetooth::emboss::StatusCode::SUCCESS, 0xFA, // status, num_hci_command_packets (250 can be sent)
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset)  // HCI_Reset opcode
      );
  auto rsp_bogocomplete = StaticByteBuffer(
      kTestEventCode0,
      0x00 // parameter_total_size (no payload)
      );
  auto rsp_nocommandsavail = StaticByteBuffer(
       hci_spec::kCommandStatusEventCode,
      0x04,  // parameter_total_size (3 byte payload)
      pw::bluetooth::emboss::StatusCode::SUCCESS, 0x00, // status, num_hci_command_packets (none can be sent)
      0x00, 0x00 // No associated opcode.
      );
  auto rsp_commandsavail = StaticByteBuffer(
       hci_spec::kCommandStatusEventCode,
      0x04,  // parameter_total_size (3 byte payload)
      pw::bluetooth::emboss::StatusCode::SUCCESS, 0x01, // status, num_hci_command_packets (one can be sent)
      0x00, 0x00 // No associated opcode.
      );
  // clang-format on

  size_t transaction_count = 0u;

  test_device()->SetTransactionCallback(
      [&transaction_count]() { transaction_count++; });

  EXPECT_CMD_PACKET_OUT(
      test_device(), req_reset, &rsp_resetstatus, &rsp_bogocomplete);

  test_device()->SendCommandChannelPacket(rsp_nocommandsavail);

  RunUntilIdle();

  pw::Result<CommandChannel::TransactionId> id;
  size_t cb_count = 0;
  auto cb = [&cb_count, &id, kTestEventCode0](
                CommandChannel::TransactionId callback_id,
                const EventPacket& event) {
    cb_count++;
    EXPECT_TRUE(id.ok());
    EXPECT_EQ(callback_id, id.value());
    if (cb_count == 1) {
      ASSERT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
      auto view = event.view<pw::bluetooth::emboss::CommandStatusEventView>();
      EXPECT_EQ(pw::bluetooth::emboss::StatusCode::SUCCESS,
                view.status().Read());
      EXPECT_EQ(pw::bluetooth::emboss::OpCode::RESET,
                view.command_opcode_enum().Read());
    } else {
      EXPECT_EQ(kTestEventCode0, event.event_code());
    }
  };

  auto packet =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  id = cmd_channel()->SendCommand(std::move(packet), cb, kTestEventCode0);

  RunUntilIdle();

  ASSERT_TRUE(id.ok());
  ASSERT_EQ(0u, transaction_count);

  // This returns invalid because an async command is registered.
  auto invalid_id = cmd_channel()->AddEventHandler(
      kTestEventCode0,
      [](const EventPacket&) { return EventCallbackResult::kContinue; });

  RunUntilIdle();

  ASSERT_EQ(0u, invalid_id);

  // Commands become available and the whole transaction finishes.
  test_device()->SendCommandChannelPacket(rsp_commandsavail);

  RunUntilIdle();

  ASSERT_EQ(1u, transaction_count);
  ASSERT_EQ(2u, cb_count);
}

// Tests:
//  - Events are routed to the event handler.
//  - Can't queue a command on the same event that is already in an event
//  handler.
TEST_F(CommandChannelTest, EventHandlerBasic) {
  constexpr hci_spec::EventCode kTestEventCode0 = 0xFD;
  constexpr hci_spec::EventCode kTestEventCode1 = 0xFE;
  StaticByteBuffer cmd_status(
      hci_spec::kCommandStatusEventCode, 0x04, 0x00, 0x01, 0x00, 0x00);
  auto cmd_complete = StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode, 0x03, 0x01, 0x00, 0x00);
  auto event0 = StaticByteBuffer(kTestEventCode0, 0x00);
  auto event1 = StaticByteBuffer(kTestEventCode1, 0x00);

  int event_count0 = 0;
  auto event_cb0 = [&event_count0, kTestEventCode0](const EventPacket& event) {
    event_count0++;
    EXPECT_EQ(kTestEventCode0, event.event_code());
    return EventCallbackResult::kContinue;
  };

  int event_count1 = 0;
  auto event_cb1 = [&event_count1, kTestEventCode0](const EventPacket& event) {
    event_count1++;
    EXPECT_EQ(kTestEventCode0, event.event_code());
    return EventCallbackResult::kContinue;
  };

  int event_count2 = 0;
  auto event_cb2 = [&event_count2, kTestEventCode1](const EventPacket& event) {
    event_count2++;
    EXPECT_EQ(kTestEventCode1, event.event_code());
    return EventCallbackResult::kContinue;
  };
  auto id0 = cmd_channel()->AddEventHandler(kTestEventCode0, event_cb0);
  EXPECT_NE(0u, id0);

  // Can register a handler for the same event code more than once.
  auto id1 = cmd_channel()->AddEventHandler(kTestEventCode0, event_cb1);
  EXPECT_NE(0u, id1);
  EXPECT_NE(id0, id1);

  // Add a handler for a different event code.
  auto id2 = cmd_channel()->AddEventHandler(kTestEventCode1, event_cb2);
  EXPECT_NE(0u, id2);

  auto reset =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  auto transaction_id = cmd_channel()->SendCommand(
      std::move(reset), [](auto, const hci::EventPacket&) {}, kTestEventCode0);

  EXPECT_FALSE(transaction_id.ok());

  test_device()->SendCommandChannelPacket(cmd_status);
  test_device()->SendCommandChannelPacket(cmd_complete);
  test_device()->SendCommandChannelPacket(event1);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(cmd_complete);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(cmd_status);
  test_device()->SendCommandChannelPacket(event1);

  RunUntilIdle();

  EXPECT_EQ(3, event_count0);
  EXPECT_EQ(3, event_count1);
  EXPECT_EQ(2, event_count2);

  event_count0 = 0;
  event_count1 = 0;
  event_count2 = 0;

  // Remove the first event handler.
  cmd_channel()->RemoveEventHandler(id0);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event1);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event1);

  RunUntilIdle();

  EXPECT_EQ(0, event_count0);
  EXPECT_EQ(7, event_count1);
  EXPECT_EQ(2, event_count2);

  event_count0 = 0;
  event_count1 = 0;
  event_count2 = 0;

  // Remove the second event handler.
  cmd_channel()->RemoveEventHandler(id1);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event1);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event1);
  test_device()->SendCommandChannelPacket(event1);

  RunUntilIdle();

  EXPECT_EQ(0, event_count0);
  EXPECT_EQ(0, event_count1);
  EXPECT_EQ(3, event_count2);
}

// Tests:
//  - can't send a command that masks an event handler.
//  - can send a command without a callback.
TEST_F(CommandChannelTest, EventHandlerEventWhileTransactionPending) {
  // clang-format off
  // HCI_Reset
  auto req = StaticByteBuffer(
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset),  // HCI_Reset opcode
      0x00                                   // parameter_total_size
      );

  auto req_complete = StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x03,  // parameter_total_size (3 byte payload)
      0x01, // num_hci_command_packets (1 can be sent)
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset)  // HCI_Reset opcode
      );
  // clang-format on

  constexpr hci_spec::EventCode kTestEventCode = 0xFE;
  auto event = StaticByteBuffer(kTestEventCode, 0x01, 0x00);

  // We will send the HCI_Reset command with kTestEventCode as the completion
  // event. The event handler we register below should only get invoked once and
  // after the pending transaction completes.
  EXPECT_CMD_PACKET_OUT(test_device(), req, &req_complete, &event, &event);

  int event_count = 0;
  auto event_cb = [&event_count, kTestEventCode](const EventPacket& event) {
    event_count++;
    EXPECT_EQ(kTestEventCode, event.event_code());
    EXPECT_EQ(1u,
              event.view<pw::bluetooth::emboss::EventHeaderView>()
                  .parameter_total_size()
                  .Read());
    EXPECT_EQ(pw::bluetooth::emboss::EventHeader::IntrinsicSizeInBytes() + 1u,
              event.size());
    return EventCallbackResult::kContinue;
  };

  cmd_channel()->AddEventHandler(kTestEventCode, event_cb);

  auto reset =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  pw::Result<CommandChannel::TransactionId> id =
      cmd_channel()->SendCommand(std::move(reset),
                                 hci::CommandChannel::CommandCallback(nullptr),
                                 kTestEventCode);
  EXPECT_FALSE(id.ok());

  reset = hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
      hci_spec::kReset);
  id = cmd_channel()->SendCommand(
      std::move(reset), hci::CommandChannel::CommandCallback(nullptr));
  EXPECT_TRUE(id.ok());

  RunUntilIdle();

  EXPECT_EQ(2, event_count);
}

// Tests:
//  - Calling RemoveQueuedCommand on a synchronous command that has already been
//    sent to the controller returns false.
//  - The command still completes and notifies the callback.
TEST_F(CommandChannelTest, RemoveQueuedSyncCommandPendingStatus) {
  auto req_reset =
      StaticByteBuffer(LowerBits(hci_spec::kReset),
                       UpperBits(hci_spec::kReset),  // HCI_Reset opcode
                       0x00                          // parameter_total_size
      );
  auto rsp_reset =
      StaticByteBuffer(hci_spec::kCommandCompleteEventCode,
                       0x03,  // parameter_total_size (3 byte payload)
                       0xFF,  // num_hci_command_packets (255 can be sent)
                       LowerBits(hci_spec::kReset),
                       UpperBits(hci_spec::kReset)  // HCI_Reset opcode
      );
  EXPECT_CMD_PACKET_OUT(test_device(), req_reset, );

  int transaction_count = 0u;
  test_device()->SetTransactionCallback(
      [&transaction_count]() { transaction_count++; });

  auto cmd = hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
      hci_spec::kReset);
  int cmd_cb_count = 0;
  auto cmd_cb = [&cmd_cb_count](auto, const hci::EventPacket&) {
    cmd_cb_count++;
  };
  auto cmd_id = cmd_channel()->SendCommand(std::move(cmd), std::move(cmd_cb));
  EXPECT_TRUE(cmd_id.ok());

  RunUntilIdle();

  EXPECT_EQ(1, transaction_count);
  EXPECT_FALSE(cmd_channel()->RemoveQueuedCommand(cmd_id.value()));
  test_device()->SendCommandChannelPacket(rsp_reset);

  RunUntilIdle();

  EXPECT_EQ(1, transaction_count);
  EXPECT_EQ(1, cmd_cb_count);
}

// Tests:
//  - Remove a synchronous command that is queued up behind another command with
//  the same opcode.
//  - The first command (after removal) does not receive the update event for
//  the second command.
TEST_F(CommandChannelTest, RemoveQueuedQueuedSyncCommand) {
  using namespace std::placeholders;
  auto req_reset =
      StaticByteBuffer(LowerBits(hci_spec::kReset),
                       UpperBits(hci_spec::kReset),  // HCI_Reset opcode
                       0x00                          // parameter_total_size
      );
  auto rsp_reset =
      StaticByteBuffer(hci_spec::kCommandCompleteEventCode,
                       0x03,  // parameter_total_size (4 byte payload)
                       0xFF,  // num_hci_command_packets (255 can be sent)
                       LowerBits(hci_spec::kReset),
                       UpperBits(hci_spec::kReset)  // HCI_Reset opcode
      );
  EXPECT_CMD_PACKET_OUT(test_device(), req_reset, );

  int transaction_count = 0u;
  test_device()->SetTransactionCallback(
      [&transaction_count]() { transaction_count++; });

  auto event_cb = [](CommandChannel::TransactionId /*id*/,
                     const EventPacket& event,
                     int* event_count) {
    EXPECT_EQ(hci_spec::kCommandCompleteEventCode, event.event_code());
    (*event_count)++;
  };

  // Send two reset commands so that the second one is queued up.
  auto reset =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  int event_count0 = 0;
  auto id0 = cmd_channel()->SendCommand(
      std::move(reset), std::bind(event_cb, _1, _2, &event_count0));
  EXPECT_TRUE(id0.ok());
  reset = hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
      hci_spec::kReset);
  int event_count1 = 0;
  auto id1 = cmd_channel()->SendCommand(
      std::move(reset), std::bind(event_cb, _1, _2, &event_count1));
  EXPECT_TRUE(id1.ok());

  RunUntilIdle();

  EXPECT_EQ(1, transaction_count);
  EXPECT_TRUE(cmd_channel()->RemoveQueuedCommand(id1.value()));

  RunUntilIdle();

  EXPECT_EQ(0, event_count0);
  test_device()->SendCommandChannelPacket(rsp_reset);

  RunUntilIdle();

  // Only one command should have been sent.
  EXPECT_EQ(1, transaction_count);
  // The queued (then canceled) command should never have gotten an event.
  EXPECT_EQ(0, event_count1);
  // The sent command should have gotten one event (CommandComplete).
  EXPECT_EQ(1, event_count0);
}

// Read Remote Supported Features
const StaticByteBuffer kReadRemoteSupportedFeaturesCmd(
    LowerBits(hci_spec::kReadRemoteSupportedFeatures),
    UpperBits(hci_spec::kReadRemoteSupportedFeatures),
    0x02,  // parameter_total_size
    0x01,
    0x00  // connection_handle
);

// Command Status for Read Remote Supported Features
const auto kReadRemoteSupportedFeaturesRsp = StaticByteBuffer(
    hci_spec::kCommandStatusEventCode,
    0x04,  // parameter_total_size (4 byte payload)
    pw::bluetooth::emboss::StatusCode::SUCCESS,  // status
    0xFF,                                        // num_hci_command_packets
    LowerBits(hci_spec::kReadRemoteSupportedFeatures),
    UpperBits(hci_spec::kReadRemoteSupportedFeatures)  // opcode
);

// Read Remote Supported Features Complete
const auto kReadRemoteSupportedFeaturesComplete = StaticByteBuffer(
    hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode,
    0x0B,  // parameter_total_size (11 bytes)
    pw::bluetooth::emboss::StatusCode::SUCCESS,  // status
    0x01,
    0x00,  // connection_handle
    0xFF,
    0x00,
    0x00,
    0x00,
    0x02,
    0x00,
    0x00,
    0x80  // lmp_features
          // Set: 3 slot packets, 5 slot packets, Encryption, Timing Accuracy,
          // Role Switch, Hold Mode, Sniff Mode, LE Supported, Extended Features
);

// Tests:
//  - Remove an asynchronous command that is queued up behind another command
//  with the same opcode.
//  - The first command (after removal) does not receive the update event for
//  the second command.
TEST_F(CommandChannelTest, RemoveQueuedQueuedAsyncCommand) {
  using namespace std::placeholders;
  EXPECT_CMD_PACKET_OUT(test_device(), kReadRemoteSupportedFeaturesCmd, );

  int transaction_count = 0u;
  test_device()->SetTransactionCallback(
      [&transaction_count]() { transaction_count++; });

  auto event_cb = [](CommandChannel::TransactionId /*id*/,
                     const EventPacket& /*event*/,
                     int* event_count) { (*event_count)++; };

  // Send two read commands so that the second one is queued up.
  auto packet = MakeReadRemoteSupportedFeatures(0x0001);
  int event_count0 = 0;
  auto id0 = cmd_channel()->SendCommand(
      std::move(packet),
      std::bind(event_cb, _1, _2, &event_count0),
      hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode);
  EXPECT_TRUE(id0.ok());
  packet = MakeReadRemoteSupportedFeatures(0x0001);
  int event_count1 = 0;
  auto id1 = cmd_channel()->SendCommand(
      std::move(packet),
      std::bind(event_cb, _1, _2, &event_count1),
      hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode);
  EXPECT_TRUE(id1.ok());

  RunUntilIdle();

  EXPECT_EQ(1, transaction_count);
  EXPECT_TRUE(cmd_channel()->RemoveQueuedCommand(id1.value()));

  RunUntilIdle();

  EXPECT_EQ(0, event_count0);
  test_device()->SendCommandChannelPacket(kReadRemoteSupportedFeaturesRsp);
  test_device()->SendCommandChannelPacket(kReadRemoteSupportedFeaturesComplete);

  RunUntilIdle();

  // Only one command should have been sent.
  EXPECT_EQ(1, transaction_count);
  // The queued (then canceled) command should never have gotten an event.
  EXPECT_EQ(0, event_count1);
  // The sent command should have gotten two events (Command Status, Read Remote
  // Supported Features Complete).
  EXPECT_EQ(2, event_count0);
}

// Tests:
//  - Calling RemoveQueuedCommand on an asynchronous command that has received
//  both Command Status
//    and command completion events returns false and has no effect.
TEST_F(CommandChannelTest, RemoveQueuedCompletedAsyncCommand) {
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteSupportedFeaturesCmd,
                        &kReadRemoteSupportedFeaturesRsp,
                        &kReadRemoteSupportedFeaturesComplete);

  int transaction_count = 0;
  test_device()->SetTransactionCallback(
      [&transaction_count] { transaction_count++; });

  int event_count = 0;
  auto event_cb = [&event_count](CommandChannel::TransactionId /*id*/,
                                 const EventPacket& /*event*/) {
    event_count++;
  };

  auto packet = MakeReadRemoteSupportedFeatures(0x0001);
  auto id = cmd_channel()->SendCommand(
      std::move(packet),
      std::move(event_cb),
      hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode);
  EXPECT_TRUE(id.ok());

  RunUntilIdle();

  EXPECT_EQ(2, event_count);
  EXPECT_FALSE(cmd_channel()->RemoveQueuedCommand(id.value()));

  RunUntilIdle();

  // Only one command should have been sent.
  EXPECT_EQ(1, transaction_count);
  // The sent command should have received CommandStatus and InquiryComplete.
  EXPECT_EQ(2, event_count);
}

// Tests:
//  - Calling RemoveQueuedCommand on an asynchronous command that has already
//  been sent to the
//    controller returns false.
//  - The command still notifies the callback for update and completion events.
TEST_F(CommandChannelTest, RemoveQueuedAsyncCommandPendingUpdate) {
  EXPECT_CMD_PACKET_OUT(test_device(), kReadRemoteSupportedFeaturesCmd, );

  int transaction_count = 0;
  test_device()->SetTransactionCallback(
      [&transaction_count] { transaction_count++; });

  pw::Result<CommandChannel::TransactionId> cmd_id;
  int cmd_events = 0;
  auto cmd_cb = [&cmd_id, &cmd_events](CommandChannel::TransactionId id,
                                       const EventPacket& event) {
    EXPECT_TRUE(cmd_id.ok());
    EXPECT_EQ(cmd_id.value(), id);
    if (cmd_events == 0) {
      EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
    }
    cmd_events++;
  };

  auto cmd_packet = MakeReadRemoteSupportedFeatures(0x0001);
  cmd_id = cmd_channel()->SendCommand(
      std::move(cmd_packet),
      std::move(cmd_cb),
      hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode);
  EXPECT_TRUE(cmd_id.ok());

  RunUntilIdle();

  EXPECT_EQ(0, cmd_events);
  EXPECT_FALSE(cmd_channel()->RemoveQueuedCommand(cmd_id.value()));

  RunUntilIdle();

  test_device()->SendCommandChannelPacket(kReadRemoteSupportedFeaturesRsp);
  test_device()->SendCommandChannelPacket(kReadRemoteSupportedFeaturesComplete);

  RunUntilIdle();

  EXPECT_EQ(1, transaction_count);
  // The command should have gotten update and complete events.
  EXPECT_EQ(2, cmd_events);
}

// Tests:
//  - Calling RemoveQueuedCommand on an asynchronous command that has already
//  been sent to the
//    controller and gotten Command Status returns false.
//  - The command still notifies the callback for completion event.
TEST_F(CommandChannelTest, RemoveQueuedAsyncCommandPendingCompletion) {
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteSupportedFeaturesCmd,
                        &kReadRemoteSupportedFeaturesRsp);

  int transaction_count = 0;
  test_device()->SetTransactionCallback(
      [&transaction_count] { transaction_count++; });

  pw::Result<CommandChannel::TransactionId> cmd_id;
  int cmd_events = 0;
  auto cmd_cb = [&cmd_id, &cmd_events](CommandChannel::TransactionId id,
                                       const EventPacket& event) {
    EXPECT_EQ(cmd_id.value(), id);
    if (cmd_events == 0) {
      EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
    }
    cmd_events++;
  };

  auto cmd_packet = MakeReadRemoteSupportedFeatures(0x0001);
  cmd_id = cmd_channel()->SendCommand(
      std::move(cmd_packet),
      std::move(cmd_cb),
      hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode);
  EXPECT_TRUE(cmd_id.ok());

  RunUntilIdle();

  EXPECT_EQ(1, cmd_events);
  EXPECT_FALSE(cmd_channel()->RemoveQueuedCommand(cmd_id.value()));

  RunUntilIdle();

  test_device()->SendCommandChannelPacket(kReadRemoteSupportedFeaturesComplete);

  RunUntilIdle();

  EXPECT_EQ(1, transaction_count);
  // The command should have gotten update and complete events.
  EXPECT_EQ(2, cmd_events);
}

TEST_F(CommandChannelTest, VendorEventHandler) {
  constexpr hci_spec::EventCode kTestSubeventCode0 = 0x10;
  constexpr hci_spec::EventCode kTestSubeventCode1 = 0x12;
  StaticByteBuffer vendor_event_bytes0(
      hci_spec::kVendorDebugEventCode, 0x01, kTestSubeventCode0);
  auto vendor_event_bytes1 = StaticByteBuffer(
      hci_spec::kVendorDebugEventCode, 0x01, kTestSubeventCode1);

  int event_count0 = 0;
  auto event_cb0 = [&event_count0,
                    kTestSubeventCode0](const EventPacket& event) {
    event_count0++;
    EXPECT_EQ(hci_spec::kVendorDebugEventCode, event.event_code());
    EXPECT_EQ(kTestSubeventCode0,
              event.view<pw::bluetooth::emboss::VendorDebugEventView>()
                  .subevent_code()
                  .Read());
    return EventCallbackResult::kContinue;
  };

  int event_count1 = 0;
  auto event_cb1 = [&event_count1,
                    kTestSubeventCode1](const EventPacket& event) {
    event_count1++;
    EXPECT_EQ(hci_spec::kVendorDebugEventCode, event.event_code());
    EXPECT_EQ(kTestSubeventCode1,
              event.view<pw::bluetooth::emboss::VendorDebugEventView>()
                  .subevent_code()
                  .Read());
    return EventCallbackResult::kContinue;
  };

  auto id0 =
      cmd_channel()->AddVendorEventHandler(kTestSubeventCode0, event_cb0);
  EXPECT_NE(0u, id0);

  // Can register a handler for the same event code more than once.
  auto id1 =
      cmd_channel()->AddVendorEventHandler(kTestSubeventCode0, event_cb0);
  EXPECT_NE(0u, id1);
  EXPECT_NE(id0, id1);

  // Add a handler for a different event code.
  auto id2 =
      cmd_channel()->AddVendorEventHandler(kTestSubeventCode1, event_cb1);
  EXPECT_NE(0u, id2);

  test_device()->SendCommandChannelPacket(vendor_event_bytes0);
  RunUntilIdle();
  EXPECT_EQ(2, event_count0);
  EXPECT_EQ(0, event_count1);

  test_device()->SendCommandChannelPacket(vendor_event_bytes0);
  RunUntilIdle();
  EXPECT_EQ(4, event_count0);
  EXPECT_EQ(0, event_count1);

  test_device()->SendCommandChannelPacket(vendor_event_bytes1);
  RunUntilIdle();
  EXPECT_EQ(4, event_count0);
  EXPECT_EQ(1, event_count1);

  // Remove the first event handler.
  cmd_channel()->RemoveEventHandler(id0);
  test_device()->SendCommandChannelPacket(vendor_event_bytes0);
  test_device()->SendCommandChannelPacket(vendor_event_bytes1);
  RunUntilIdle();
  EXPECT_EQ(5, event_count0);
  EXPECT_EQ(2, event_count1);
}

TEST_F(CommandChannelTest, LEMetaEventHandler) {
  constexpr hci_spec::EventCode kTestSubeventCode0 = 0xFE;
  constexpr hci_spec::EventCode kTestSubeventCode1 = 0xFF;
  auto le_meta_event_bytes0 =
      StaticByteBuffer(hci_spec::kLEMetaEventCode, 0x01, kTestSubeventCode0);
  auto le_meta_event_bytes1 =
      StaticByteBuffer(hci_spec::kLEMetaEventCode, 0x01, kTestSubeventCode1);

  int event_count0 = 0;
  auto event_cb0 = [&event_count0,
                    kTestSubeventCode0](const EventPacket& event) {
    event_count0++;
    EXPECT_EQ(hci_spec::kLEMetaEventCode, event.event_code());
    EXPECT_EQ(kTestSubeventCode0,
              event.view<pw::bluetooth::emboss::LEMetaEventView>()
                  .subevent_code()
                  .Read());
    return EventCallbackResult::kContinue;
  };

  int event_count1 = 0;
  auto event_cb1 = [&event_count1,
                    kTestSubeventCode1](const EventPacket& event) {
    event_count1++;
    EXPECT_EQ(hci_spec::kLEMetaEventCode, event.event_code());
    EXPECT_EQ(kTestSubeventCode1,
              event.view<pw::bluetooth::emboss::LEMetaEventView>()
                  .subevent_code()
                  .Read());
    return EventCallbackResult::kContinue;
  };

  auto id0 =
      cmd_channel()->AddLEMetaEventHandler(kTestSubeventCode0, event_cb0);
  EXPECT_NE(0u, id0);

  // Can register a handler for the same event code more than once.
  auto id1 =
      cmd_channel()->AddLEMetaEventHandler(kTestSubeventCode0, event_cb0);
  EXPECT_NE(0u, id1);
  EXPECT_NE(id0, id1);

  // Add a handler for a different event code.
  auto id2 =
      cmd_channel()->AddLEMetaEventHandler(kTestSubeventCode1, event_cb1);
  EXPECT_NE(0u, id2);

  test_device()->SendCommandChannelPacket(le_meta_event_bytes0);
  RunUntilIdle();
  EXPECT_EQ(2, event_count0);
  EXPECT_EQ(0, event_count1);

  test_device()->SendCommandChannelPacket(le_meta_event_bytes0);
  RunUntilIdle();
  EXPECT_EQ(4, event_count0);
  EXPECT_EQ(0, event_count1);

  test_device()->SendCommandChannelPacket(le_meta_event_bytes1);
  RunUntilIdle();
  EXPECT_EQ(4, event_count0);
  EXPECT_EQ(1, event_count1);

  // Remove the first event handler.
  cmd_channel()->RemoveEventHandler(id0);
  test_device()->SendCommandChannelPacket(le_meta_event_bytes0);
  test_device()->SendCommandChannelPacket(le_meta_event_bytes1);
  RunUntilIdle();
  EXPECT_EQ(5, event_count0);
  EXPECT_EQ(2, event_count1);
}

TEST_F(CommandChannelTest, EventHandlerIdsDontCollide) {
  // Add a LE Meta event handler and a event handler and make sure that IDs are
  // generated correctly across the two methods.
  EXPECT_EQ(
      1u,
      cmd_channel()->AddLEMetaEventHandler(
          hci_spec::kLEConnectionCompleteSubeventCode,
          [](const EventPacket&) { return EventCallbackResult::kContinue; }));
  EXPECT_EQ(
      2u,
      cmd_channel()->AddEventHandler(
          hci_spec::kDisconnectionCompleteEventCode,
          [](const EventPacket&) { return EventCallbackResult::kContinue; }));
}

// Tests:
//  - Can't register an event handler for CommandStatus or CommandComplete
TEST_F(CommandChannelTest, EventHandlerRestrictions) {
  auto id0 = cmd_channel()->AddEventHandler(
      hci_spec::kCommandStatusEventCode,
      [](const EventPacket&) { return EventCallbackResult::kContinue; });
  EXPECT_EQ(0u, id0);
  id0 = cmd_channel()->AddEventHandler(
      hci_spec::kCommandCompleteEventCode,
      [](const EventPacket&) { return EventCallbackResult::kContinue; });
  EXPECT_EQ(0u, id0);
}

// Tests that an asynchronous command with a completion event code does not
// remove an existing handler for colliding LE meta subevent code.
TEST_F(CommandChannelTest,
       AsyncEventHandlersAndLeMetaEventHandlersDoNotInterfere) {
  // Set up expectations for the asynchronous command and its corresponding
  // command status event.
  auto cmd =
      StaticByteBuffer(LowerBits(hci_spec::kInquiry),
                       UpperBits(hci_spec::kInquiry),  // HCI_Inquiry opcode
                       0x00                            // parameter_total_size
      );
  auto cmd_status = testing::CommandStatusPacket(
      hci_spec::kInquiry, pw::bluetooth::emboss::StatusCode::SUCCESS);

  EXPECT_CMD_PACKET_OUT(test_device(), cmd, &cmd_status);

  constexpr hci_spec::EventCode kTestEventCode = 0x01;

  // Add LE event handler for kTestEventCode
  int le_event_count = 0;
  auto le_event_cb = [&](const EventPacket& event) {
    EXPECT_EQ(hci_spec::kLEMetaEventCode, event.event_code());
    EXPECT_EQ(kTestEventCode,
              event.view<pw::bluetooth::emboss::LEMetaEventView>()
                  .subevent_code()
                  .Read());
    le_event_count++;
    return EventCallbackResult::kContinue;
  };
  cmd_channel()->AddLEMetaEventHandler(
      hci_spec::kLEConnectionCompleteSubeventCode, std::move(le_event_cb));

  // Initiate the async transaction with kTestEventCode as its completion code
  // (we use hci_spec::kInquiry as a test opcode).
  int async_cmd_cb_count = 0;
  auto async_cmd_cb = [&](auto /*id*/, const EventPacket& event) {
    if (async_cmd_cb_count == 0) {
      EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
    } else {
      EXPECT_EQ(kTestEventCode, event.event_code());
    }
    async_cmd_cb_count++;
  };

  auto packet = CommandPacket::New<pw::bluetooth::emboss::InquiryCommandView>(
      hci_spec::kInquiry,
      pw::bluetooth::emboss::CommandHeader::IntrinsicSizeInBytes());
  cmd_channel()
      ->SendCommand(std::move(packet), std::move(async_cmd_cb), kTestEventCode)
      .IgnoreError();

  // clang-format off
  auto event_bytes = StaticByteBuffer(
      kTestEventCode,
      0x01,  // parameter_total_size
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto le_event_bytes = StaticByteBuffer(
      hci_spec::kLEMetaEventCode,
      0x01,  // parameter_total_size
      kTestEventCode);
  // clang-format on

  // Send a spurious LE event before processing the Command Status event. This
  // should get routed to the correct event handler.
  test_device()->SendCommandChannelPacket(le_event_bytes);

  // Process the async command expectation.
  RunUntilIdle();

  // End the asynchronous transaction. This should NOT unregister the LE event
  // handler.
  test_device()->SendCommandChannelPacket(event_bytes);

  // Send more LE events. These should get routed to the LE event handler.
  test_device()->SendCommandChannelPacket(le_event_bytes);
  test_device()->SendCommandChannelPacket(le_event_bytes);

  RunUntilIdle();

  // Should have received 3 LE events.
  EXPECT_EQ(3, le_event_count);

  // The async command handler should have been called twice: once for Command
  // Status and once for the completion event.
  EXPECT_EQ(2, async_cmd_cb_count);
}

TEST_F(CommandChannelTest, TransportClosedCallback) {
  bool error_cb_called = false;
  auto error_cb = [&error_cb_called] { error_cb_called = true; };
  transport()->SetTransportErrorCallback(error_cb);

  (void)heap_dispatcher().Post(
      [this](pw::async::Context /*ctx*/, pw::Status status) {
        if (status.ok()) {
          test_device()->Stop();
        }
      });
  RunUntilIdle();
  EXPECT_TRUE(error_cb_called);
}

TEST_F(CommandChannelTest, CommandTimeoutCallback) {
  auto req_reset =
      StaticByteBuffer(LowerBits(hci_spec::kReset),
                       UpperBits(hci_spec::kReset),  // HCI_Reset opcode
                       0x00                          // parameter_total_size
      );

  // Expect the HCI_Reset command but dont send a reply back to make the command
  // time out.
  EXPECT_CMD_PACKET_OUT(test_device(), req_reset, );

  size_t timeout_cb_count = 0;
  auto timeout_cb = [&] { timeout_cb_count++; };
  cmd_channel()->set_channel_timeout_cb(timeout_cb);

  size_t cmd_cb_count = 0;
  auto cb = [&](auto, const hci::EventPacket&) { cmd_cb_count++; };

  auto packet =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  auto id1 = cmd_channel()->SendCommand(std::move(packet), cb);
  ASSERT_TRUE(id1.ok());

  packet = hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
      hci_spec::kReset);
  auto id2 = cmd_channel()->SendCommand(std::move(packet), cb);
  ASSERT_TRUE(id2.ok());

  // Run the loop until the command timeout task gets scheduled.
  RunUntilIdle();
  EXPECT_EQ(0u, timeout_cb_count);
  EXPECT_EQ(0u, cmd_cb_count);

  RunFor(kCommandTimeout);

  EXPECT_EQ(1u, timeout_cb_count);
  EXPECT_EQ(0u, cmd_cb_count);

  DeleteTransport();
  EXPECT_EQ(0u, cmd_cb_count);
}

TEST_F(CommandChannelTest, DestroyChannelInTimeoutCallback) {
  auto req_reset =
      StaticByteBuffer(LowerBits(hci_spec::kReset),
                       UpperBits(hci_spec::kReset),  // HCI_Reset opcode
                       0x00                          // parameter_total_size
      );

  // Expect the HCI_Reset command but dont send a reply back to make the command
  // time out.
  EXPECT_CMD_PACKET_OUT(test_device(), req_reset, );

  size_t timeout_cb_count = 0;
  auto timeout_cb = [&] {
    timeout_cb_count++;
    DeleteTransport();
  };
  cmd_channel()->set_channel_timeout_cb(timeout_cb);

  size_t cmd_cb_count = 0;
  auto cb = [&](auto, const hci::EventPacket&) { cmd_cb_count++; };

  auto packet =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  auto id1 = cmd_channel()->SendCommand(std::move(packet), cb);
  ASSERT_TRUE(id1.ok());

  packet = hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
      hci_spec::kReset);
  auto id2 = cmd_channel()->SendCommand(std::move(packet), cb);
  ASSERT_TRUE(id2.ok());

  RunFor(kCommandTimeout);
  EXPECT_EQ(1u, timeout_cb_count);
}

TEST_F(CommandChannelTest, CommandsAndEventsIgnoredAfterCommandTimeout) {
  size_t timeout_cb_count = 0;
  auto timeout_cb = [&] { timeout_cb_count++; };
  cmd_channel()->set_channel_timeout_cb(timeout_cb);

  size_t cmd_cb_count = 0;
  auto cb = [&](auto, const hci::EventPacket&) { cmd_cb_count++; };

  // Expect the HCI_Reset command but dont send a reply back to make the command
  // time out.
  auto req_reset =
      StaticByteBuffer(LowerBits(hci_spec::kReset),
                       UpperBits(hci_spec::kReset),  // HCI_Reset opcode
                       0x00                          // parameter_total_size
      );
  EXPECT_CMD_PACKET_OUT(test_device(), req_reset);
  auto packet =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  auto id1 = cmd_channel()->SendCommand(std::move(packet), cb);
  ASSERT_TRUE(id1.ok());

  // Run the loop until the command timeout task gets scheduled.
  RunUntilIdle();
  EXPECT_EQ(0u, timeout_cb_count);
  RunFor(kCommandTimeout);
  EXPECT_EQ(1u, timeout_cb_count);
  EXPECT_EQ(0u, cmd_cb_count);

  // Additional commands should be ignored.
  packet = hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
      hci_spec::kReset);
  auto id2 = cmd_channel()->SendCommand(std::move(packet), cb);
  EXPECT_FALSE(id2.ok());
  // No command should be sent.
  RunUntilIdle();

  // Events should be ignored.
  test_device()->SendCommandChannelPacket(bt::testing::CommandCompletePacket(
      hci_spec::kReset, pw::bluetooth::emboss::StatusCode::SUCCESS));
  RunUntilIdle();
  EXPECT_EQ(0u, cmd_cb_count);
}

// Tests:
//  - Asynchronous commands should be able to schedule another asynchronous
//    command in their callback.
TEST_F(CommandChannelTest, AsynchronousCommandChaining) {
  constexpr size_t kExpectedCallbacksPerCommand = 2;
  constexpr hci_spec::EventCode kTestEventCode0 = 0xFE;
  // Set up expectations
  // clang-format off
  // Using HCI_Reset for testing.
  auto req_reset = StaticByteBuffer(
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset), // HCI_Reset opcode
      0x00                                  // parameter_total_size (no payload)
      );
  auto rsp_resetstatus = StaticByteBuffer(
       hci_spec::kCommandStatusEventCode,
      0x04,                        // parameter_total_size (4 byte payload)
      pw::bluetooth::emboss::StatusCode::SUCCESS, 0xFA,  // status, num_hci_command_packets (250)
      LowerBits(hci_spec::kReset), UpperBits(hci_spec::kReset)  // HCI_Reset opcode
  );
  auto req_inqcancel = StaticByteBuffer(
      LowerBits(hci_spec::kInquiryCancel), UpperBits(hci_spec::kInquiryCancel), // HCI_InquiryCancel
      0x00                        // parameter_total_size (no payload)
  );
  auto rsp_inqstatus = StaticByteBuffer(
       hci_spec::kCommandStatusEventCode,
      0x04,                        // parameter_total_size (4 byte payload)
      pw::bluetooth::emboss::StatusCode::SUCCESS, 0xFA,  // status, num_hci_command_packets (250)
      LowerBits(hci_spec::kInquiryCancel), UpperBits(hci_spec::kInquiryCancel) // HCI_InquiryCanacel
  );
  auto rsp_bogocomplete = StaticByteBuffer(
      kTestEventCode0,
      0x00 // parameter_total_size (no payload)
      );
  // clang-format on

  EXPECT_CMD_PACKET_OUT(test_device(), req_reset, &rsp_resetstatus);
  EXPECT_CMD_PACKET_OUT(test_device(), req_reset, &rsp_resetstatus);

  pw::Result<CommandChannel::TransactionId> id1, id2;
  CommandChannel::CommandCallback cb;
  size_t cb_count = 0u;

  cb = [&cb,
        cmd_channel = cmd_channel(),
        &id1,
        &id2,
        &cb_count,
        kTestEventCode0](CommandChannel::TransactionId callback_id,
                         const EventPacket& event) {
    if (cb_count < kExpectedCallbacksPerCommand) {
      EXPECT_TRUE(id1.ok());
      EXPECT_EQ(id1.value(), callback_id);
    } else {
      EXPECT_TRUE(id2.ok());
      EXPECT_EQ(id2.value(), callback_id);
    }
    if ((cb_count % 2) == 0) {
      // First event from each command - CommandStatus
      EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
      EXPECT_EQ(pw::bluetooth::emboss::StatusCode::SUCCESS,
                event.view<pw::bluetooth::emboss::CommandStatusEventView>()
                    .status()
                    .Read());
    } else {
      // Second event from each command - completion event
      EXPECT_EQ(kTestEventCode0, event.event_code());
      if (cb_count < 2) {
        // Add the second command when the first one completes.
        auto packet =
            hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
                hci_spec::kReset);
        id2 = cmd_channel->SendCommand(
            std::move(packet), cb.share(), kTestEventCode0);
      }
    }
    cb_count++;
  };

  auto packet =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  id1 = cmd_channel()->SendCommand(
      std::move(packet), cb.share(), kTestEventCode0);

  RunUntilIdle();

  // Should have received the Status but not the result.
  EXPECT_EQ(1u, cb_count);

  // Sending the complete will finish the command and add the next command.
  test_device()->SendCommandChannelPacket(rsp_bogocomplete);
  RunUntilIdle();

  EXPECT_EQ(3u, cb_count);

  // Finish out the command.
  test_device()->SendCommandChannelPacket(rsp_bogocomplete);
  RunUntilIdle();

  EXPECT_EQ(4u, cb_count);
}

// Tests:
//  - Commands that are exclusive of other commands cannot run together, and
//    instead wait until the exclusive commands finish.
//  - Exclusive Commands in the queue still get started in order
//  - Commands that aren't exclusive run as normal even when an exclusive one is
//    waiting.
TEST_F(CommandChannelTest, ExclusiveCommands) {
  constexpr hci_spec::EventCode kExclOneCompleteEvent = 0xFE;
  constexpr hci_spec::EventCode kExclTwoCompleteEvent = 0xFD;
  constexpr hci_spec::OpCode kExclusiveOne = hci_spec::DefineOpCode(0x01, 0x01);
  constexpr hci_spec::OpCode kExclusiveTwo = hci_spec::DefineOpCode(0x01, 0x02);
  constexpr hci_spec::OpCode kNonExclusive = hci_spec::DefineOpCode(0x01, 0x03);

  // Set up expectations
  //  - kExclusiveOne can't run at the same time as kExclusiveTwo, and
  //  vice-versa.
  //  - kExclusiveOne finishes with kExclOneCompleteEvent
  //  - kExclusiveTwo finishes with kExclTwoCompleteEvent
  //  - kNonExclusive can run whenever it wants.
  //  - For testing, we omit the payloads of all commands.
  auto excl_one_cmd = StaticByteBuffer(
      LowerBits(kExclusiveOne), UpperBits(kExclusiveOne), 0x00  // (no payload)
  );
  auto rsp_excl_one_status =
      StaticByteBuffer(hci_spec::kCommandStatusEventCode,
                       0x04,  // parameter_total_size (4 byte payload)
                       pw::bluetooth::emboss::StatusCode::SUCCESS,
                       0xFA,  // status, num_hci_command_packets (250)
                       LowerBits(kExclusiveOne),
                       UpperBits(kExclusiveOne)  // HCI opcode
      );
  auto rsp_one_complete = StaticByteBuffer(
      kExclOneCompleteEvent, 0x00  // parameter_total_size (no payload)
  );

  auto excl_two_cmd = StaticByteBuffer(
      LowerBits(kExclusiveTwo), UpperBits(kExclusiveTwo), 0x00  // (no payload)
  );
  auto rsp_excl_two_status =
      StaticByteBuffer(hci_spec::kCommandStatusEventCode,
                       0x04,  // parameter_total_size (4 byte payload)
                       pw::bluetooth::emboss::StatusCode::SUCCESS,
                       0xFA,  // status, num_hci_command_packets (250)
                       LowerBits(kExclusiveTwo),
                       UpperBits(kExclusiveTwo)  // HCI opcode
      );
  auto rsp_two_complete = StaticByteBuffer(
      kExclTwoCompleteEvent, 0x00  // parameter_total_size (no payload)
  );

  auto nonexclusive_cmd =
      StaticByteBuffer(LowerBits(kNonExclusive),
                       UpperBits(kNonExclusive),  // HCI opcode
                       0x00  // parameter_total_size (no payload)
      );
  auto nonexclusive_complete = StaticByteBuffer(
      hci_spec::kCommandCompleteEventCode,
      0x04,  // parameter_total_size (4 byte payload)
      0xFA,  // num_hci_command_packets (250)
      LowerBits(kNonExclusive),
      UpperBits(kNonExclusive),                   // HCI opcode
      pw::bluetooth::emboss::StatusCode::SUCCESS  // Command succeeded
  );

  pw::Result<CommandChannel::TransactionId> id1, id2, id3;
  CommandChannel::CommandCallback exclusive_cb;
  size_t exclusive_cb_count = 0u;

  size_t nonexclusive_cb_count = 0;
  CommandChannel::CommandCallback nonexclusive_cb =
      [&nonexclusive_cb_count](auto /*callback_id*/, const EventPacket& event) {
        EXPECT_EQ(hci_spec::kCommandCompleteEventCode, event.event_code());
        nonexclusive_cb_count++;
      };

  exclusive_cb = [&exclusive_cb,
                  &nonexclusive_cb,
                  cmd_channel = cmd_channel(),
                  &id1,
                  &id2,
                  &id3,
                  &exclusive_cb_count,
                  kExclOneCompleteEvent,
                  kExclTwoCompleteEvent](
                     CommandChannel::TransactionId callback_id,
                     const EventPacket& event) {
    // Expected event -> Action in response
    // 0. Status for kExclusiveOne -> Send a kExclusiveTwo
    // 1. Complete for kExclusiveOne -> Send Another kExclusiveOne and
    // kNonExclusive
    // 2. Status for kExclusiveTwo -> Nothing
    // 3. Complete for kExclusiveTwo -> Nothing
    // 4. Status for kExclusiveOne -> Nothing
    // 5. Complete for kExclusiveOne -> Nothing
    switch (exclusive_cb_count) {
      case 0: {
        // Status for kExclusiveOne -> Send kExclusiveTwo (queued)
        EXPECT_TRUE(id1.ok());
        EXPECT_EQ(id1.value(), callback_id);
        EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
        EXPECT_EQ(pw::bluetooth::emboss::StatusCode::SUCCESS,
                  event.view<pw::bluetooth::emboss::CommandStatusEventView>()
                      .status()
                      .Read());
        auto packet =
            CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(
                kExclusiveTwo);
        id2 = cmd_channel->SendExclusiveCommand(std::move(packet),
                                                exclusive_cb.share(),
                                                kExclTwoCompleteEvent,
                                                {kExclusiveOne});
        std::cout << "queued Exclusive Two: " << id2.value() << std::endl;
        break;
      }
      case 1: {
        // Complete for kExclusiveOne -> Resend kExclusiveOne
        EXPECT_TRUE(id1.ok());
        EXPECT_EQ(id1.value(), callback_id);
        EXPECT_EQ(kExclOneCompleteEvent, event.event_code());
        // Add the second command when the first one completes.
        auto packet =
            CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(
                kExclusiveOne);
        id3 = cmd_channel->SendExclusiveCommand(std::move(packet),
                                                exclusive_cb.share(),
                                                kExclOneCompleteEvent,
                                                {kExclusiveTwo});
        std::cout << "queued Second Exclusive One: " << id3.value()
                  << std::endl;
        packet = CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(
            kNonExclusive);
        cmd_channel->SendCommand(std::move(packet), nonexclusive_cb.share())
            .IgnoreError();

        break;
      }
      case 2: {  // Status for kExclusiveTwo
        EXPECT_TRUE(id2.ok());
        EXPECT_EQ(id2.value(), callback_id);
        EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
        EXPECT_EQ(pw::bluetooth::emboss::StatusCode::SUCCESS,
                  event.view<pw::bluetooth::emboss::CommandStatusEventView>()
                      .status()
                      .Read());
        break;
      }
      case 3: {  // Complete for kExclusiveTwo
        EXPECT_TRUE(id2.ok());
        EXPECT_EQ(id2.value(), callback_id);
        EXPECT_EQ(kExclTwoCompleteEvent, event.event_code());
        break;
      }
      case 4: {  // Status for Second kExclusiveOne
        EXPECT_TRUE(id3.ok());
        EXPECT_EQ(id3.value(), callback_id);
        EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
        EXPECT_EQ(pw::bluetooth::emboss::StatusCode::SUCCESS,
                  event.view<pw::bluetooth::emboss::CommandStatusEventView>()
                      .status()
                      .Read());
        break;
      }
      case 5: {  // Complete for Second kExclusiveOne
        EXPECT_TRUE(id3.ok());
        EXPECT_EQ(id3.value(), callback_id);
        EXPECT_EQ(kExclOneCompleteEvent, event.event_code());
        break;
      }
      default: {
        ASSERT_TRUE(false);  // Should never be called more than 6 times.
        break;
      }
    }
    exclusive_cb_count++;
  };

  EXPECT_CMD_PACKET_OUT(test_device(), excl_one_cmd, &rsp_excl_one_status);
  EXPECT_CMD_PACKET_OUT(
      test_device(), nonexclusive_cmd, &nonexclusive_complete);
  id1 = cmd_channel()->SendExclusiveCommand(
      CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(
          kExclusiveOne),
      exclusive_cb.share(),
      kExclOneCompleteEvent,
      {kExclusiveTwo});
  cmd_channel()
      ->SendCommand(
          CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(
              kNonExclusive),
          nonexclusive_cb.share())
      .IgnoreError();
  RunUntilIdle();
  // Should have received the ExclusiveOne status but not the complete.
  // ExclusiveTwo should be queued.
  EXPECT_EQ(1u, exclusive_cb_count);
  // NonExclusive should be completed.
  EXPECT_EQ(1u, nonexclusive_cb_count);

  // Sending the ExclusiveOne complete will send the ExclusiveTwo command, queue
  // another ExclusiveOne command, and send a NonExclusive command.
  EXPECT_CMD_PACKET_OUT(test_device(), excl_two_cmd, &rsp_excl_two_status);
  EXPECT_CMD_PACKET_OUT(
      test_device(), nonexclusive_cmd, &nonexclusive_complete);
  test_device()->SendCommandChannelPacket(rsp_one_complete);
  RunUntilIdle();
  EXPECT_EQ(3u,
            exclusive_cb_count);  // +2: rsp_one_complete, rsp_excl_two_status
  EXPECT_EQ(2u, nonexclusive_cb_count);  // +1: nonexclusive_complete

  // Complete ExclusiveTwo and send a NonExclusive. The queued ExclusiveOne
  // should be sent.
  EXPECT_CMD_PACKET_OUT(
      test_device(), nonexclusive_cmd, &nonexclusive_complete);
  EXPECT_CMD_PACKET_OUT(test_device(), excl_one_cmd, &rsp_excl_one_status);
  test_device()->SendCommandChannelPacket(rsp_two_complete);
  cmd_channel()
      ->SendCommand(
          CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(
              kNonExclusive),
          nonexclusive_cb.share())
      .IgnoreError();
  RunUntilIdle();
  EXPECT_EQ(5u,
            exclusive_cb_count);  // +2: rsp_two_complete, rsp_excl_one_status
  EXPECT_EQ(3u, nonexclusive_cb_count);  // +1: nonexclusive_complete

  // Finish the second ExclusiveOne
  test_device()->SendCommandChannelPacket(rsp_one_complete);
  RunUntilIdle();
  EXPECT_EQ(6u, exclusive_cb_count);  // +1: rsp_one_complete
  EXPECT_EQ(3u, nonexclusive_cb_count);
}

TEST_F(CommandChannelTest, SendCommandFailsIfEventHandlerInstalled) {
  constexpr hci_spec::EventCode kTestEventCode0 = 0xFE;

  // Register event handler for kTestEventCode0.
  auto id0 = cmd_channel()->AddEventHandler(
      kTestEventCode0, [](const EventPacket& /*event*/) {
        return EventCallbackResult::kContinue;
      });
  EXPECT_NE(0u, id0);

  // Try to send a command for kTestEventCode0. SendCommand should fail for a
  // code already registered with "AddEventHandler".
  auto reset =
      hci::CommandPacket::New<pw::bluetooth::emboss::ResetCommandWriter>(
          hci_spec::kReset);
  auto transaction_id = cmd_channel()->SendCommand(
      std::move(reset), [](auto, const hci::EventPacket&) {}, kTestEventCode0);
  EXPECT_FALSE(transaction_id.ok());
}

TEST_F(CommandChannelTest, EventHandlerResults) {
  constexpr hci_spec::EventCode kTestEventCode0 = 0xFE;

  int event_count = 0;
  auto event_cb = [&event_count, kTestEventCode0](const EventPacket& event) {
    event_count++;
    EXPECT_EQ(kTestEventCode0, event.event_code());

    if (event_count == 1) {
      return EventCallbackResult::kContinue;
    }

    return EventCallbackResult::kRemove;
  };

  EXPECT_NE(cmd_channel()->AddEventHandler(kTestEventCode0, event_cb), 0u);

  // Send three requests, and process the callbacks immediately. The second
  // callback returns "remove" before the third event callback has been called.
  auto event0 = StaticByteBuffer(kTestEventCode0, 0x00);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event0);
  test_device()->SendCommandChannelPacket(event0);
  RunUntilIdle();
  EXPECT_EQ(2, event_count);
}

TEST_F(CommandChannelTest, SendCommandWithLEMetaEventSubeventRsp) {
  constexpr hci_spec::OpCode kOpCode = hci_spec::kLEReadRemoteFeatures;
  constexpr hci_spec::EventCode kSubeventCode =
      hci_spec::kLEReadRemoteFeaturesCompleteSubeventCode;

  auto cmd = StaticByteBuffer(LowerBits(kOpCode),
                              UpperBits(kOpCode),
                              // parameter total size (0 byte payload)
                              0x00);

  auto cmd_status_event =
      StaticByteBuffer(hci_spec::kCommandStatusEventCode,
                       // parameter total size (4 byte payload)
                       0x04,
                       // status, num_hci_command_packets (250)
                       pw::bluetooth::emboss::StatusCode::SUCCESS,
                       0xFA,
                       // HCI opcode
                       LowerBits(kOpCode),
                       UpperBits(kOpCode));
  auto cmd_complete_subevent =
      StaticByteBuffer(hci_spec::kLEMetaEventCode,
                       0x01,  // parameter total size (1 byte payload)
                       kSubeventCode);

  EXPECT_CMD_PACKET_OUT(test_device(), cmd, &cmd_status_event);

  auto cmd_packet =
      CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(kOpCode);

  size_t event_count = 0;
  auto event_cb = [&event_count](auto, const EventPacket& event) {
    switch (event_count) {
      case 0: {
        EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
        break;
      }
      case 1: {
        EXPECT_EQ(hci_spec::kLEMetaEventCode, event.event_code());
        break;
      }
      default: {
        FAIL();
      }
    }
    event_count++;
  };
  auto id = cmd_channel()->SendLeAsyncCommand(
      std::move(cmd_packet), std::move(event_cb), kSubeventCode);
  EXPECT_TRUE(id.ok());

  RunUntilIdle();
  EXPECT_EQ(1u, event_count);

  // Handler should be removed when subevent received.
  test_device()->SendCommandChannelPacket(cmd_complete_subevent);
  RunUntilIdle();
  EXPECT_EQ(2u, event_count);

  // This seconod complete event should be ignored because the handler should
  // have been removed.
  test_device()->SendCommandChannelPacket(cmd_complete_subevent);
  RunUntilIdle();
  EXPECT_EQ(2u, event_count);
}

TEST_F(
    CommandChannelTest,
    SendingLECommandAfterAddingLEMetaEventHandlerFailsForSameSubeventCodeAndSucceedsForDifferentSubeventCode) {
  constexpr hci_spec::EventCode kSubeventCode =
      hci_spec::kLEReadRemoteFeaturesCompleteSubeventCode;
  constexpr hci_spec::OpCode kOpCode =
      hci_spec::kLEReadRemoteFeatures;  // LE Read Remote Features

  EXPECT_NE(0u,
            cmd_channel()->AddLEMetaEventHandler(
                kSubeventCode, [](const EventPacket&) {
                  return EventCallbackResult::kContinue;
                }));
  EXPECT_FALSE(
      cmd_channel()
          ->SendLeAsyncCommand(
              CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(
                  kOpCode),
              [](auto, const EventPacket&) {},
              kSubeventCode)
          .ok());

  auto cmd = StaticByteBuffer(LowerBits(kOpCode),
                              UpperBits(kOpCode),
                              // parameter total size (0 byte payload)
                              0x00);
  EXPECT_CMD_PACKET_OUT(test_device(), std::move(cmd), );
  EXPECT_TRUE(
      cmd_channel()
          ->SendLeAsyncCommand(
              CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(
                  kOpCode),
              [](auto, const EventPacket&) {},
              kSubeventCode + 1)
          .ok());
  RunUntilIdle();
}

TEST_F(CommandChannelTest,
       SendingSecondLECommandWithSameSubeventShouldWaitForFirstToComplete) {
  // Commands have different op codes but same subevent code so that second
  // command is not blocked because of matching op codes (which would not test
  // LE command handling).
  constexpr hci_spec::OpCode kOpCode0 = hci_spec::kLEReadRemoteFeatures;
  constexpr hci_spec::OpCode kOpCode1 = hci_spec::kLEReadBufferSizeV1;
  constexpr hci_spec::EventCode kSubeventCode =
      hci_spec::kLEReadRemoteFeaturesCompleteSubeventCode;

  auto cmd0 = StaticByteBuffer(LowerBits(kOpCode0),
                               UpperBits(kOpCode0),
                               // parameter total size (0 byte payload)
                               0x00);
  auto cmd0_status_event =
      StaticByteBuffer(hci_spec::kCommandStatusEventCode,
                       // parameter total size (4 byte payload)
                       0x04,
                       // status, num_hci_command_packets (250)
                       pw::bluetooth::emboss::StatusCode::SUCCESS,
                       0xFA,
                       // HCI opcode
                       LowerBits(kOpCode0),
                       UpperBits(kOpCode0));
  auto cmd1 = StaticByteBuffer(LowerBits(kOpCode1),
                               UpperBits(kOpCode1),
                               // parameter total size (0 byte payload)
                               0x00);
  auto cmd1_status_event =
      StaticByteBuffer(hci_spec::kCommandStatusEventCode,
                       // parameter total size (4 byte payload)
                       0x04,
                       // status, num_hci_command_packets (250)
                       pw::bluetooth::emboss::StatusCode::SUCCESS,
                       0xFA,
                       // HCI opcode
                       LowerBits(kOpCode1),
                       UpperBits(kOpCode1));

  auto cmd_complete_subevent =
      StaticByteBuffer(hci_spec::kLEMetaEventCode,
                       0x01,  // parameter total size (1 byte payload)
                       kSubeventCode);

  EXPECT_CMD_PACKET_OUT(test_device(), cmd0, &cmd0_status_event);

  size_t event_count_0 = 0;
  auto event_cb_0 = [&event_count_0](auto, const EventPacket& event) {
    switch (event_count_0) {
      case 0: {
        EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
        break;
      }
      case 1: {
        EXPECT_EQ(hci_spec::kLEMetaEventCode, event.event_code());
        break;
      }
      default: {
        FAIL();
      }
    }
    event_count_0++;
  };
  auto id_0 = cmd_channel()->SendLeAsyncCommand(
      CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(kOpCode0),
      std::move(event_cb_0),
      kSubeventCode);
  EXPECT_TRUE(id_0.ok());

  RunUntilIdle();
  EXPECT_EQ(1u, event_count_0);

  size_t event_count_1 = 0;
  auto event_cb_1 = [&event_count_1](auto, const EventPacket& event) {
    switch (event_count_1) {
      case 0: {
        EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
        break;
      }
      case 1: {
        EXPECT_EQ(hci_spec::kLEMetaEventCode, event.event_code());
        break;
      }
      default: {
        FAIL();
      }
    }
    event_count_1++;
  };
  // Command should be queued and not sent until after first complete event
  // received.
  auto id_1 = cmd_channel()->SendLeAsyncCommand(
      CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(kOpCode1),
      std::move(event_cb_1),
      kSubeventCode);
  EXPECT_TRUE(id_1.ok());
  RunUntilIdle();
  EXPECT_EQ(0u, event_count_1);

  // When first command complete event is received, second command should be
  // sent.
  EXPECT_CMD_PACKET_OUT(test_device(), cmd1, &cmd1_status_event);
  test_device()->SendCommandChannelPacket(cmd_complete_subevent);
  RunUntilIdle();
  EXPECT_EQ(2u, event_count_0);
  EXPECT_EQ(1u, event_count_1);

  // Second complete event should be received by second command event handler
  // only.
  test_device()->SendCommandChannelPacket(cmd_complete_subevent);
  RunUntilIdle();
  EXPECT_EQ(2u, event_count_0);
  EXPECT_EQ(2u, event_count_1);
}

TEST_F(
    CommandChannelTest,
    RegisteringLEMetaEventHandlerWhileLECommandPendingFailsForSameSubeventAndSucceedsForDifferentSubevent) {
  constexpr hci_spec::OpCode kOpCode = hci_spec::kLEReadRemoteFeatures;
  constexpr hci_spec::EventCode kSubeventCode =
      hci_spec::kLEReadRemoteFeaturesCompleteSubeventCode;

  auto cmd = StaticByteBuffer(LowerBits(kOpCode),
                              UpperBits(kOpCode),
                              // parameter total size (0 byte payload)
                              0x00);

  auto cmd_status_event =
      StaticByteBuffer(hci_spec::kCommandStatusEventCode,
                       // parameter total size (4 byte payload)
                       0x04,
                       // status, num_hci_command_packets (250)
                       pw::bluetooth::emboss::StatusCode::SUCCESS,
                       0xFA,
                       // HCI opcode
                       LowerBits(kOpCode),
                       UpperBits(kOpCode));

  EXPECT_CMD_PACKET_OUT(test_device(), cmd, &cmd_status_event);

  size_t event_count = 0;
  auto event_cb = [&event_count](auto, const EventPacket& event) {
    EXPECT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
    event_count++;
  };
  auto id1 = cmd_channel()->SendLeAsyncCommand(
      CommandPacket::New<pw::bluetooth::emboss::CommandHeaderView>(kOpCode),
      std::move(event_cb),
      kSubeventCode);
  EXPECT_TRUE(id1.ok());
  RunUntilIdle();
  EXPECT_EQ(1u, event_count);

  // Async LE command for subevent is already pending, so registering event
  // handler should fail by returning 0.
  CommandChannel::EventHandlerId id2 = cmd_channel()->AddLEMetaEventHandler(
      kSubeventCode,
      [](const EventPacket&) { return EventCallbackResult::kContinue; });
  EXPECT_EQ(0u, id2);

  // Registering event handler for different subevent code should succeed.
  CommandChannel::EventHandlerId id3 = cmd_channel()->AddLEMetaEventHandler(
      static_cast<hci_spec::EventCode>(kSubeventCode + 1),
      [](const EventPacket&) { return EventCallbackResult::kContinue; });
  EXPECT_NE(0u, id3);
}

#ifndef NINSPECT
TEST_F(CommandChannelTest, InspectHierarchy) {
  cmd_channel()->AttachInspect(inspector_.GetRoot(), "command_channel");

  auto transactions_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("transactions"))));

  auto stats_matcher = AllOf(NodeMatches(
      AllOf(NameMatches("command_response_stats"),
            PropertyList(UnorderedElementsAre(UintIs("avg_ms", 0),
                                              UintIs("p95_ms", 0),
                                              UintIs("p99_ms", 0),
                                              UintIs("variance_ms_sq", 0),
                                              StringIs("outliers", ""))))));

  auto command_channel_matcher = AllOf(
      NodeMatches(AllOf(NameMatches("command_channel"),
                        PropertyList(UnorderedElementsAre(
                            UintIs("allowed_command_packets", 1),
                            UintIs("next_event_handler_id", 1))))),
      ChildrenMatch(UnorderedElementsAre(transactions_matcher, stats_matcher)));

  EXPECT_THAT(inspect::ReadFromVmo(inspector_.DuplicateVmo()).value(),
              ChildrenMatch(ElementsAre(command_channel_matcher)));

  const auto req = testing::InquiryCommandPacket(0x01);
  auto rsp0 =
      testing::CommandStatusPacket(hci_spec::kInquiry,
                                   pw::bluetooth::emboss::StatusCode::SUCCESS,
                                   /*num_packets=*/1);
  auto rsp1 = StaticByteBuffer(hci_spec::kInquiryCompleteEventCode,
                               0x01,  // parameter_total_size (1 byte payload)
                               pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(test_device(), req, &rsp0, &rsp1);

  int cb_count = 0;
  auto cb = [&cb_count, &stats_matcher, this](
                CommandChannel::TransactionId callback_id,
                const EventPacket& event) {
    cb_count++;
    if (cb_count == 1) {
      ASSERT_EQ(hci_spec::kCommandStatusEventCode, event.event_code());
      auto inquiry_matcher_complete = AllOf(NodeMatches(AllOf(
          NameMatches("transaction_1"),
          PropertyList(UnorderedElementsAre(UintIs("opcode", 1025),
                                            UintIs("complete_event_code", 1),
                                            StringIs("state", "complete"))))));
      auto transactions_matcher_complete =
          AllOf(NodeMatches(AllOf(NameMatches("transactions"))),
                ChildrenMatch(ElementsAre(inquiry_matcher_complete)));
      auto command_channel_matcher_complete =
          AllOf(NodeMatches(AllOf(NameMatches("command_channel"),
                                  PropertyList(UnorderedElementsAre(
                                      UintIs("allowed_command_packets", 1),
                                      UintIs("next_event_handler_id", 2))))),
                ChildrenMatch(UnorderedElementsAre(
                    transactions_matcher_complete, stats_matcher)));

      EXPECT_THAT(inspect::ReadFromVmo(inspector_.DuplicateVmo()).value(),
                  ChildrenMatch(ElementsAre(command_channel_matcher_complete)));
    } else {
      EXPECT_EQ(hci_spec::kInquiryCompleteEventCode, event.event_code());
      auto transactions_matcher_empty =
          AllOf(NodeMatches(AllOf(NameMatches("transactions"))),
                ChildrenMatch(::testing::IsEmpty()));
      auto command_channel_matcher_complete2 =
          AllOf(NodeMatches(AllOf(NameMatches("command_channel"),
                                  PropertyList(UnorderedElementsAre(
                                      UintIs("allowed_command_packets", 1),
                                      UintIs("next_event_handler_id", 2))))),
                ChildrenMatch(
                    ElementsAre(transactions_matcher_empty, stats_matcher)));
    }
  };

  auto packet =
      hci::CommandPacket::New<pw::bluetooth::emboss::InquiryCommandWriter>(
          hci_spec::kInquiry);
  auto view = packet.view_t();
  view.lap().Write(pw::bluetooth::emboss::InquiryAccessCode::GIAC);
  view.inquiry_length().Write(1);
  view.num_responses().Write(0);
  cmd_channel()
      ->SendCommand(std::move(packet), cb, hci_spec::kInquiryCompleteEventCode)
      .IgnoreError();

  auto inquiry_matcher = AllOf(NodeMatches(
      AllOf(NameMatches("transaction_1"),
            PropertyList(UnorderedElementsAre(UintIs("opcode", 1025),
                                              UintIs("complete_event_code", 1),
                                              StringIs("state", "pending"))))));
  auto transactions_matcher_after_send =
      AllOf(NodeMatches(AllOf(NameMatches("transactions"))),
            ChildrenMatch(ElementsAre(inquiry_matcher)));
  auto command_channel_matcher_after_send =
      AllOf(NodeMatches(AllOf(NameMatches("command_channel"),
                              PropertyList(UnorderedElementsAre(
                                  UintIs("allowed_command_packets", 0),
                                  UintIs("next_event_handler_id", 2))))),
            ChildrenMatch(UnorderedElementsAre(transactions_matcher_after_send,
                                               stats_matcher)));

  EXPECT_THAT(inspect::ReadFromVmo(inspector_.DuplicateVmo()).value(),
              ChildrenMatch(ElementsAre(command_channel_matcher_after_send)));

  RunUntilIdle();
  EXPECT_EQ(2, cb_count);
}

TEST_F(CommandChannelTest, ResponseTimeMetrics) {
  cmd_channel()->AttachInspect(inspector_.GetRoot(), "command_channel");

  auto send_cmd_with_delay = [&](int delay_seconds) {
    const auto req = testing::InquiryCommandPacket(0x01);
    const auto rsp_status = StaticByteBuffer(
        hci_spec::kCommandStatusEventCode,
        0x04,  // parameter_total_size (3 byte payload)
        pw::bluetooth::emboss::StatusCode::SUCCESS,
        0x01,  // status, num_hci_command_packets (1 can be sent)
        LowerBits(hci_spec::kInquiry),
        UpperBits(hci_spec::kInquiry));
    auto rsp_complete =
        StaticByteBuffer(hci_spec::kInquiryCompleteEventCode,
                         0x01,  // parameter_total_size (1 byte payload)
                         pw::bluetooth::emboss::StatusCode::SUCCESS);

    EXPECT_CMD_PACKET_OUT(test_device(), req, );
    auto packet =
        hci::CommandPacket::New<pw::bluetooth::emboss::InquiryCommandWriter>(
            hci_spec::kInquiry);
    auto view = packet.view_t();
    view.lap().Write(pw::bluetooth::emboss::InquiryAccessCode::GIAC);
    view.inquiry_length().Write(1);
    view.num_responses().Write(0);

    EXPECT_TRUE(cmd_channel()
                    ->SendCommand(
                        std::move(packet),
                        [](auto, const auto&) {},
                        hci_spec::kInquiryCompleteEventCode)
                    .ok());

    RunFor(std::chrono::seconds(delay_seconds));
    // for async commands, the measurement should be from initiate -> status
    // not initiate -> finish
    test_device()->SendCommandChannelPacket(rsp_status);
    // Inquiry should run for 1 secoond
    RunFor(std::chrono::seconds(1));
    test_device()->SendCommandChannelPacket(rsp_complete);
    RunUntilIdle();
  };

  send_cmd_with_delay(1);
  send_cmd_with_delay(2);
  send_cmd_with_delay(3);
  send_cmd_with_delay(4);
  send_cmd_with_delay(6);  // Outlier!

  // Verify that metrics are updated in Inspect.
  // Average should be (1000 + 2000 + 3000 + 4000 + 6000) / 5 = 3200 ms.
  // Samples: [1000, 2000, 3000, 4000, 6000]
  // Given the avg (3200ms) and variance, the p95 and p99 were calculated
  // manually to verify.
  auto stats_after_send_matcher = AllOf(NodeMatches(AllOf(
      NameMatches("command_response_stats"),
      PropertyList(UnorderedElementsAre(UintIs("avg_ms", 3200),
                                        UintIs("p95_ms", 6030),
                                        UintIs("p99_ms", 7201),
                                        UintIs("variance_ms_sq", 2960000),
                                        StringIs("outliers", "[6000]"))))));
  auto transactions_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("transactions"))));
  auto command_channel_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("command_channel"),
                              PropertyList(UnorderedElementsAre(
                                  UintIs("allowed_command_packets", 1),
                                  UintIs("next_event_handler_id", 6))))),
            ChildrenMatch(UnorderedElementsAre(transactions_matcher,
                                               stats_after_send_matcher)));

  EXPECT_THAT(inspect::ReadFromVmo(inspector_.DuplicateVmo()).value(),
              ChildrenMatch(ElementsAre(command_channel_matcher)));
}
#endif  // NINSPECT

}  // namespace
}  // namespace bt::hci
