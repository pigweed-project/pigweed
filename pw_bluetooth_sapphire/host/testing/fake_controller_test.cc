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

#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"

#include <pw_async/fake_dispatcher_fixture.h>
#include <pw_bluetooth/hci_android.emb.h>
#include <pw_bluetooth/hci_events.emb.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/vendor_protocol.h"
#include "pw_unit_test/framework.h"

namespace bt::testing {

namespace android_hci = hci_spec::vendor::android;
namespace android_emb = pw::bluetooth::vendor::android_hci;
namespace pwemb = pw::bluetooth::emboss;

using FakeControllerTest = pw::async::test::FakeDispatcherFixture;

TEST_F(FakeControllerTest, TestInquiryCommand) {
  FakeController controller(dispatcher());

  int event_cb_count = 0;
  controller.SetEventFunction(
      [&event_cb_count](pw::span<const std::byte> packet_bytes) {
        auto header_view = pw::bluetooth::emboss::MakeEventHeaderView(
            reinterpret_cast<const uint8_t*>(packet_bytes.data()),
            packet_bytes.size());

        if (header_view.event_code_uint().Read() ==
            hci_spec::kCommandStatusEventCode) {
          auto command_status_view =
              pw::bluetooth::emboss::MakeCommandStatusEventView(
                  reinterpret_cast<const uint8_t*>(packet_bytes.data()),
                  packet_bytes.size());
          EXPECT_EQ(command_status_view.status().Read(),
                    pw::bluetooth::emboss::StatusCode::SUCCESS);
          ++event_cb_count;
        } else if (header_view.event_code_uint().Read() ==
                   hci_spec::kInquiryCompleteEventCode) {
          auto inquiry_complete_view =
              pw::bluetooth::emboss::MakeInquiryCompleteEventView(
                  reinterpret_cast<const uint8_t*>(packet_bytes.data()),
                  packet_bytes.size());
          EXPECT_EQ(inquiry_complete_view.status().Read(),
                    pw::bluetooth::emboss::StatusCode::SUCCESS);
          ++event_cb_count;
        }

        else {
          ADD_FAILURE() << "Unexpected Event packet received";
        }
      });

  auto inquiry =
      hci::CommandPacket::New<pw::bluetooth::emboss::InquiryCommandWriter>(
          hci_spec::kInquiry);
  auto view = inquiry.view_t();
  view.lap().Write(pw::bluetooth::emboss::InquiryAccessCode::GIAC);
  view.inquiry_length().Write(8);
  view.num_responses().Write(0);
  controller.SendCommand(inquiry.data().subspan());

  // The maximum amount of time before Inquiry is halted is calculated as
  // inquiry_length * 1.28 s. FakeController:OnInquiry simulates this by posting
  // the InquiryCompleteEvent to be returned after this duration.
  RunFor(std::chrono::milliseconds(
      static_cast<int64_t>(view.inquiry_length().Read()) * 1280));

  EXPECT_EQ(event_cb_count, 2);
}

namespace {

template <typename WriterT, size_t Size, typename... ViewArgs>
void BuildAndSendApcfCmd(FakeController& controller,
                         android_emb::ApcfSubOpcode sub_opcode,
                         uint8_t filter_index,
                         ViewArgs... view_args) {
  auto cmd = hci::CommandPacket::New<WriterT>(android_hci::kLEApcf, Size);
  auto view = cmd.template view<WriterT>(view_args...);
  view.vendor_command().sub_opcode().Write(static_cast<uint8_t>(sub_opcode));
  view.action().Write(android_emb::ApcfAction::ADD);
  view.filter_index().Write(filter_index);

  if constexpr (std::is_same_v<WriterT,
                               android_emb::LEApcfAdTypeCommandWriter>) {
    view.ad_type().Write(pw::bluetooth::emboss::CommonDataType::FLAGS);
    view.ad_data_length().Write(0);
  }

  controller.SendCommand(cmd.data().subspan());
}

void SendAddFilterCommand(FakeController& controller,
                          android_emb::ApcfSubOpcode sub_opcode,
                          uint8_t filter_index) {
#define SEND_APCF_ADD_CMD(opcode_name, struct_name, ...)             \
  case android_emb::ApcfSubOpcode::opcode_name:                      \
    BuildAndSendApcfCmd<android_emb::struct_name##Writer,            \
                        android_emb::struct_name::MinSizeInBytes()>( \
        controller, sub_opcode, filter_index, ##__VA_ARGS__);        \
    break;

  switch (sub_opcode) {
    SEND_APCF_ADD_CMD(SET_FILTERING_PARAMETERS,
                      LEApcfSetFilteringParametersCommand)
    SEND_APCF_ADD_CMD(BROADCAST_ADDRESS, LEApcfBroadcastAddressCommand)
    SEND_APCF_ADD_CMD(SERVICE_UUID, LEApcfServiceUUID16Command)
    SEND_APCF_ADD_CMD(SOLICITATION_UUID, LEApcfSolicitationUUID16Command)
    SEND_APCF_ADD_CMD(
        LOCAL_NAME, LEApcfLocalNameCommand, static_cast<uint8_t>(0))
    SEND_APCF_ADD_CMD(MANUFACTURER_DATA,
                      LEApcfManufacturerDataCommand,
                      static_cast<uint8_t>(0))
    SEND_APCF_ADD_CMD(
        SERVICE_DATA, LEApcfServiceDataCommand, static_cast<uint8_t>(0))
    SEND_APCF_ADD_CMD(AD_TYPE_FILTER, LEApcfAdTypeCommand)
    case android_emb::ApcfSubOpcode::ENABLE:
    case android_emb::ApcfSubOpcode::TRANSPORT_DISCOVERY_DATA:
    case android_emb::ApcfSubOpcode::READ_EXTENDED_FEATURES:
      PW_CRASH("Unsupported sub-opcode for addition");
  }
#undef SEND_APCF_ADD_CMD
}

const std::vector<android_emb::ApcfSubOpcode> kApcfAddSubOpcodes = {
    android_emb::ApcfSubOpcode::SET_FILTERING_PARAMETERS,
    android_emb::ApcfSubOpcode::BROADCAST_ADDRESS,
    android_emb::ApcfSubOpcode::SERVICE_UUID,
    android_emb::ApcfSubOpcode::SOLICITATION_UUID,
    android_emb::ApcfSubOpcode::LOCAL_NAME,
    android_emb::ApcfSubOpcode::MANUFACTURER_DATA,
    android_emb::ApcfSubOpcode::SERVICE_DATA,
    android_emb::ApcfSubOpcode::AD_TYPE_FILTER,
};

}  // namespace

TEST_F(FakeControllerTest, TestApcfAllAdditionMethods) {
  FakeController controller(dispatcher());

  FakeController::Settings settings;
  settings.max_apcf_filters = 10;
  settings.ApplyAndroidVendorExtensionDefaults();
  controller.set_settings(settings);

  int event_cb_count = 0;
  android_emb::ApcfSubOpcode expected_sub_opcode;
  uint8_t expected_available_spaces = 0;

  controller.SetEventFunction([&](pw::span<const std::byte> packet_bytes) {
    auto header_view = pw::bluetooth::emboss::MakeEventHeaderView(
        packet_bytes.data(), packet_bytes.size());
    if (header_view.event_code_uint().Read() !=
        hci_spec::kCommandCompleteEventCode) {
      return;
    }

    auto command_complete_view =
        pw::bluetooth::emboss::MakeCommandCompleteEventView(
            packet_bytes.data(), packet_bytes.size());
    if (command_complete_view.command_opcode().Read() !=
        pwemb::OpCode::ANDROID_APCF) {
      return;
    }

    auto apcf_view = android_emb::MakeLEApcfCommandCompleteEventView(
        packet_bytes.data(), packet_bytes.size());

    EXPECT_EQ(apcf_view.status().Read(), pwemb::StatusCode::SUCCESS);
    EXPECT_EQ(apcf_view.sub_opcode().Read(), expected_sub_opcode);
    EXPECT_EQ(apcf_view.available_spaces().Read(), expected_available_spaces);
    ++event_cb_count;
  });

  int expected_event_cb_count = 0;
  expected_available_spaces = 9;
  for (auto sub_opcode : kApcfAddSubOpcodes) {
    expected_sub_opcode = sub_opcode;
    SendAddFilterCommand(controller, sub_opcode, /*filter_index=*/1);
    RunUntilIdle();
    EXPECT_EQ(event_cb_count, ++expected_event_cb_count);
  }

  expected_available_spaces = 8;
  for (auto sub_opcode : {android_emb::ApcfSubOpcode::SET_FILTERING_PARAMETERS,
                          android_emb::ApcfSubOpcode::LOCAL_NAME}) {
    expected_sub_opcode = sub_opcode;
    SendAddFilterCommand(controller, sub_opcode, /*filter_index=*/2);
    RunUntilIdle();
    EXPECT_EQ(event_cb_count, ++expected_event_cb_count);
  }
}

TEST_F(FakeControllerTest, TestApcfAvailableFiltersUnderflow) {
  FakeController controller(dispatcher());

  int event_cb_count = 0;
  android_emb::ApcfSubOpcode expected_sub_opcode;

  controller.SetEventFunction([&](pw::span<const std::byte> packet_bytes) {
    auto header_view = pw::bluetooth::emboss::MakeEventHeaderView(
        packet_bytes.data(), packet_bytes.size());
    if (header_view.event_code_uint().Read() !=
        hci_spec::kCommandCompleteEventCode) {
      return;
    }

    auto command_complete_view =
        pw::bluetooth::emboss::MakeCommandCompleteEventView(
            packet_bytes.data(), packet_bytes.size());
    if (command_complete_view.command_opcode().Read() !=
        pwemb::OpCode::ANDROID_APCF) {
      return;
    }

    auto apcf_view = android_emb::MakeLEApcfCommandCompleteEventView(
        packet_bytes.data(), packet_bytes.size());

    EXPECT_EQ(apcf_view.status().Read(), pwemb::StatusCode::SUCCESS);
    EXPECT_EQ(apcf_view.sub_opcode().Read(), expected_sub_opcode);

    // available_spaces should be 0, not 255 (underflow)
    EXPECT_EQ(apcf_view.available_spaces().Read(), 0);

    ++event_cb_count;
  });

  int expected_event_cb_count = 0;
  for (auto sub_opcode : kApcfAddSubOpcodes) {
    expected_sub_opcode = sub_opcode;
    SendAddFilterCommand(controller, sub_opcode, /*filter_index=*/1);
    RunUntilIdle();
    EXPECT_EQ(event_cb_count, ++expected_event_cb_count);
  }
}

}  // namespace bt::testing
