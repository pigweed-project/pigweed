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

#include "pw_bluetooth_sapphire/internal/host/sdp/server.h"

#include <pw_bytes/endian.h>

#include <cstdint>
#include <vector>

#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel_test.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_l2cap.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/data_element.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/pdu.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/sdp.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::sdp {

using RegistrationHandle = Server::RegistrationHandle;

namespace {

using namespace inspect::testing;

using TestingBase = l2cap::testing::FakeChannelTest;

constexpr hci_spec::ConnectionHandle kTestHandle1 = 1;
constexpr hci_spec::ConnectionHandle kTestHandle2 = 2;

void NopConnectCallback(l2cap::Channel::WeakPtr, const DataElement&) {}

// Returns true if the |psm| is in the valid dynamic PSM range.
bool isValidDynamicPsm(uint16_t psm) {
  const auto kMinDynamicPSM = 0x1001;
  const auto kMaxDynamicPSM = 0xfffe;  // Server::kDynamicPsm == 0xffff.
  return ((psm >= kMinDynamicPSM) && (psm <= kMaxDynamicPSM));
}

constexpr l2cap::ChannelParameters kChannelParams;

class ServerTest : public TestingBase {
 public:
  ServerTest() = default;
  ~ServerTest() = default;

 protected:
  void SetUp() override {
    l2cap_ = std::make_unique<l2cap::testing::FakeL2cap>(dispatcher());
    l2cap_->set_channel_callback([this](auto fake_chan) {
      channel_ = std::move(fake_chan);
      set_fake_chan(channel_);
    });
    l2cap_->AddACLConnection(kTestHandle1,
                             pw::bluetooth::emboss::ConnectionRole::PERIPHERAL,
                             nullptr,
                             nullptr,
                             [](l2cap::ChannelManager::BrEdrFixedChannels) {});
    l2cap_->AddACLConnection(kTestHandle2,
                             pw::bluetooth::emboss::ConnectionRole::PERIPHERAL,
                             nullptr,
                             nullptr,
                             [](l2cap::ChannelManager::BrEdrFixedChannels) {});
    server_ = std::make_unique<Server>(l2cap_.get());
  }

  void TearDown() override {
    channel_.reset();
    server_ = nullptr;
    l2cap_ = nullptr;
  }

  Server* server() const { return server_.get(); }

  l2cap::testing::FakeL2cap* l2cap() const { return l2cap_.get(); }

  RegistrationHandle AddSPP(
      sdp::Server::ConnectCallback cb = NopConnectCallback) {
    ServiceRecord record;

    record.SetServiceClassUUIDs({profile::kSerialPort});
    record.AddProtocolDescriptor(
        ServiceRecord::kPrimaryProtocolList, protocol::kL2CAP, DataElement());
    record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                 protocol::kRFCOMM,
                                 DataElement(uint8_t{0}));
    record.AddProfile(profile::kSerialPort, 1, 2);
    record.AddInfo("en", "FAKE", "", "");
    std::vector<ServiceRecord> records;
    records.emplace_back(std::move(record));
    RegistrationHandle handle = server()->RegisterService(
        std::move(records), kChannelParams, std::move(cb));
    EXPECT_TRUE(handle);
    return handle;
  }

  RegistrationHandle AddA2DPSink(
      sdp::Server::ConnectCallback cb = NopConnectCallback) {
    ServiceRecord record;
    record.SetServiceClassUUIDs({profile::kAudioSink});
    record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                 protocol::kL2CAP,
                                 DataElement(l2cap::kAVDTP));
    record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                 protocol::kAVDTP,
                                 DataElement(uint16_t{0x0103}));  // Version
    record.AddProfile(profile::kAdvancedAudioDistribution, 1, 3);
    record.SetAttribute(kA2DP_SupportedFeatures,
                        DataElement(uint16_t{0x0001}));  // Headphones
    std::vector<ServiceRecord> records;
    records.emplace_back(std::move(record));
    RegistrationHandle handle = server()->RegisterService(
        std::move(records), kChannelParams, std::move(cb));
    EXPECT_TRUE(handle);
    return handle;
  }

  RegistrationHandle AddL2capService(
      l2cap::Psm channel,
      l2cap::ChannelParameters chan_params = kChannelParams,
      sdp::Server::ConnectCallback cb = NopConnectCallback) {
    ServiceRecord record;
    record.SetServiceClassUUIDs({profile::kAudioSink});
    record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                 protocol::kL2CAP,
                                 DataElement(channel));
    record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                 protocol::kAVDTP,
                                 DataElement(uint16_t{0x0103}));  // Version
    record.AddProfile(profile::kAdvancedAudioDistribution, 1, 3);
    record.SetAttribute(kA2DP_SupportedFeatures,
                        DataElement(uint16_t{0x0001}));  // Headphones
    std::vector<ServiceRecord> records;
    records.emplace_back(std::move(record));
    RegistrationHandle handle = server()->RegisterService(
        std::move(records), chan_params, std::move(cb));
    EXPECT_TRUE(handle);
    return handle;
  }

  void TriggerInboundL2capChannelsForAllocated(size_t expected_psm_count) {
    auto allocated = server()->AllocatedPsmsForTest();
    // The SDP PSM is special and will always exist. Not connectable.
    EXPECT_TRUE(allocated.erase(l2cap::kSDP));
    EXPECT_EQ(expected_psm_count, allocated.size());

    // Trigger inbound L2CAP channels for the remaining. |id|, |remote_id|
    // aren't important.
    l2cap::ChannelId id = 0x40;
    for (auto it = allocated.begin(); it != allocated.end(); it++) {
      EXPECT_TRUE(
          l2cap()->TriggerInboundL2capChannel(kTestHandle1, *it, id, id));
      id++;
    }

    RunUntilIdle();
  }

 private:
  l2cap::testing::FakeChannel::WeakPtr channel_;
  std::unique_ptr<l2cap::testing::FakeL2cap> l2cap_;
  std::unique_ptr<Server> server_;
};

constexpr l2cap::ChannelId kSdpChannel = 0x0041;

#define SDP_ERROR_RSP(t_id, code)             \
  StaticByteBuffer(0x01,                      \
                   UpperBits(t_id),           \
                   LowerBits(t_id),           \
                   0x00,                      \
                   0x02,                      \
                   UpperBits(uint16_t(code)), \
                   LowerBits(uint16_t(code)));

#define UINT32_AS_BE_BYTES(x)                                    \
  UpperBits(x >> 16), LowerBits(x >> 16), UpperBits(x & 0xFFFF), \
      LowerBits(x & 0xFFFF)

// Test:
//  - Accepts channels and holds channel open correctly.
//  - More than one channel from the same peer can be open at once.
//  - Packets that are the wrong length are responded to with kInvalidSize
//  - Answers with the same TransactionID as sent
TEST_F(ServerTest, BasicError) {
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad));
  RunUntilIdle();
  ASSERT_TRUE(fake_chan().is_alive());
  EXPECT_TRUE(fake_chan()->activated());

  const auto kRspErrSize = SDP_ERROR_RSP(0x1001, ErrorCode::kInvalidSize);

  const StaticByteBuffer kTooSmall(0x01,  // SDP_ServiceSearchRequest
                                   0x10,
                                   0x01,  // Transaction ID (0x1001)
                                   0x00,
                                   0x09  // Parameter length (9 bytes)
  );

  const auto kRspTooSmall = SDP_ERROR_RSP(0x1001, ErrorCode::kInvalidSize);

  const StaticByteBuffer kTooBig(0x01,  // SDP_ServiceSearchRequest
                                 0x20,
                                 0x10,  // Transaction ID (0x2010)
                                 0x00,
                                 0x02,  // Parameter length (2 bytes)
                                 0x01,
                                 0x02,
                                 0x03  // 3 bytes of parameters
  );

  const auto kRspTooBig = SDP_ERROR_RSP(0x2010, ErrorCode::kInvalidSize);

  EXPECT_TRUE(ReceiveAndExpect(kTooSmall, kRspTooSmall));
  EXPECT_TRUE(ReceiveAndExpect(kTooBig, kRspTooBig));

  const auto kRspInvalidSyntax =
      SDP_ERROR_RSP(0x2010, ErrorCode::kInvalidRequestSyntax);

  // Responses aren't valid requests
  EXPECT_TRUE(ReceiveAndExpect(kRspTooBig, kRspInvalidSyntax));

  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel + 1, 0x0bad));
  RunUntilIdle();
  ASSERT_TRUE(fake_chan().is_alive());
  EXPECT_TRUE(fake_chan()->activated());

  EXPECT_TRUE(ReceiveAndExpect(kTooSmall, kRspTooSmall));
}

// Test:
//  - Passes an initialized ServiceRecord that has a matching ServiceHandle
//  - Doesn't add a service that doesn't contain a ServiceClassIDList
//  - Adds a service that is valid.
//  - Services can be Unregistered.
TEST_F(ServerTest, RegisterService) {
  std::vector<ServiceRecord> records;
  EXPECT_FALSE(
      server()->RegisterService(std::move(records), kChannelParams, {}));

  ServiceRecord record = ServiceRecord();
  std::vector<ServiceRecord> records0;
  records0.emplace_back(std::move(record));
  EXPECT_FALSE(
      server()->RegisterService(std::move(records0), kChannelParams, {}));

  ServiceRecord record1;
  record1.SetAttribute(kServiceClassIdList, DataElement(uint16_t{42}));
  std::vector<ServiceRecord> records1;
  records1.emplace_back(std::move(record1));
  EXPECT_FALSE(
      server()->RegisterService(std::move(records1), kChannelParams, {}));

  ServiceRecord has_handle;
  has_handle.SetHandle(42);
  std::vector<ServiceRecord> records2;
  records2.emplace_back(std::move(has_handle));
  EXPECT_FALSE(
      server()->RegisterService(std::move(records2), kChannelParams, {}));

  ServiceRecord valid;
  valid.SetServiceClassUUIDs({profile::kAVRemoteControl});
  std::vector<ServiceRecord> records3;
  records3.emplace_back(std::move(valid));
  RegistrationHandle handle =
      server()->RegisterService(std::move(records3), kChannelParams, {});

  EXPECT_TRUE(handle);

  EXPECT_TRUE(server()->UnregisterService(handle));
  EXPECT_FALSE(server()->UnregisterService(handle));
}

// Test:
// - Adds a protocol-only service to the server.
// - Tests registration of the PSM is successful.
// - Tests callback correctness when inbound l2cap channels are connected.
TEST_F(ServerTest, RegisterProtocolOnlyService) {
  ServiceRecord protocol_only;
  l2cap::Psm test_psm = 500;
  protocol_only.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                      protocol::kL2CAP,
                                      DataElement(uint16_t{test_psm}));

  std::vector<uint16_t> protocols_discovered;
  auto service_connect_cb = [&](auto /*channel*/,
                                const DataElement& protocol_list) {
    EXPECT_EQ(DataElement::Type::kSequence, protocol_list.type());
    auto* psm = protocol_list.At(0);
    EXPECT_EQ(DataElement::Type::kSequence, psm->type());
    psm = psm->At(1);
    EXPECT_EQ(DataElement::Type::kUnsignedInt, psm->type());
    protocols_discovered.emplace_back(*psm->template Get<uint16_t>());
  };

  std::vector<ServiceRecord> records;
  records.emplace_back(std::move(protocol_only));
  auto handle = server()->RegisterService(
      std::move(records), kChannelParams, std::move(service_connect_cb));

  EXPECT_TRUE(handle);

  EXPECT_TRUE(
      l2cap()->TriggerInboundL2capChannel(kTestHandle1, test_psm, 0x40, 0x41));
  RunUntilIdle();

  ASSERT_EQ(1u, protocols_discovered.size());
  // There should be one connection (and therefore protocol_list) per psm
  // registered.
  ASSERT_EQ(test_psm, protocols_discovered[0]);

  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad));
  RunUntilIdle();

  // Searching for the service record doesn't work
  //
  // By asking for everything L2CAP
  const StaticByteBuffer kL2capSearch(
      0x02,  // SDP_ServiceSearchRequest
      0x10,
      0x01,  // Transaction ID (0x1001)
      0x00,
      0x08,  // Parameter length (8 bytes)
      // ServiceSearchPattern
      0x35,
      0x03,  // Sequence uint8 3 bytes
      0x19,
      0x01,
      0x00,  // UUID: Protocol: L2CAP
      0xFF,
      0xFF,  // MaximumServiceRecordCount: (none)
      0x00   // Continuation State: none
  );
  bool responded = false;
  auto service_search_cb = [&responded](auto cb_packet) {
    EXPECT_LE(sizeof(Header), cb_packet->size());
    PacketView<Header> packet(cb_packet.get());
    EXPECT_EQ(kServiceSearchResponse, packet.header().pdu_id);
    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    packet.Resize(len);
    ServiceSearchResponse resp;
    fit::result<Error<>> result = resp.Parse(packet.payload_data());
    EXPECT_EQ(fit::ok(), result);
    EXPECT_EQ(0u, resp.service_record_handle_list().size());
    responded = true;
  };

  fake_chan()->SetSendCallback(service_search_cb, dispatcher());
  fake_chan()->Receive(kL2capSearch);
  RunUntilIdle();
  EXPECT_TRUE(responded);

  // By asking for everything L2CAP with all attributes
  const auto kServiceSearchAttributeRequest =
      StaticByteBuffer(0x06,  // SDP_ServiceAttributeRequest
                       0x10,
                       0x01,  // Transaction ID (0x1001)
                       0x00,
                       0x12,  // Parameter length (18 bytes)
                       // ServiceSearchPattern
                       0x35,
                       0x03,  // Sequence uint8 3 bytes
                       0x19,
                       0x01,
                       0x00,  // UUID: Protocol: L2CAP
                       0xFF,
                       0xFF,  // MaximumAttributeByteCount (no max)
                       // AttributeIDList
                       0x35,
                       0x08,  // Sequence uint8 8 bytes
                       0x09,  // uint16_t, single attribute
                       0x00,
                       0x00,  // ServiceRecordHandle
                       0x0A,  // uint32_t, which is a range (0x3000 - 0xf000)
                       0x00,
                       0x00,  // low end of range
                       0xff,
                       0xff,  // high end of range
                       0x00   // Continuation State: none
      );

  responded = false;
  auto service_search_attribute_cb = [&responded](auto cb_packet) {
    EXPECT_LE(sizeof(Header), cb_packet->size());
    PacketView<sdp::Header> packet(cb_packet.get());
    ASSERT_EQ(kServiceSearchAttributeResponse, packet.header().pdu_id);
    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    packet.Resize(len);
    ServiceSearchAttributeResponse rsp;
    fit::result<Error<>> result = rsp.Parse(packet.payload_data());
    EXPECT_EQ(fit::ok(), result);
    EXPECT_EQ(0u, rsp.num_attribute_lists());
    responded = true;
  };

  fake_chan()->SetSendCallback(service_search_attribute_cb, dispatcher());
  fake_chan()->Receive(kServiceSearchAttributeRequest);
  RunUntilIdle();
  EXPECT_TRUE(responded);

  // Asking for the service handle directly will also not work, and gives an
  // InvalidRecordHandle
  const auto kServiceAttributeRequest =
      StaticByteBuffer(0x04,  // SDP_ServiceAttributeRequest
                       0x10,
                       0x01,  // Transaction ID (0x1001)
                       0x00,
                       0x11,  // Parameter length (17 bytes)
                       UINT32_AS_BE_BYTES(handle),  // ServiceRecordHandle
                       0xFF,
                       0xFF,  // MaximumAttributeByteCount (10 bytes max)
                       // AttributeIDList
                       0x35,
                       0x08,  // Sequence uint8 8 bytes
                       0x09,  // uint16_t, single attribute
                       0x00,
                       0x01,  // ServiceClassIDList
                       0x0A,  // uint32_t, which is a range (0x3000 - 0xf000)
                       0x30,
                       0x00,  // low end of range
                       0xf0,
                       0x00,  // high end of range
                       0x00   // Continuation State: none
      );

  responded = false;
  auto service_attribute_cb = [&responded](auto cb_packet) {
    EXPECT_LE(sizeof(Header), cb_packet->size());
    PacketView<sdp::Header> packet(cb_packet.get());
    ASSERT_EQ(0x01, packet.header().pdu_id);
    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    ASSERT_GE(sizeof(Header) + len, cb_packet->size());
    packet.Resize(len);
    ErrorResponse rsp;
    fit::result<Error<>> result = rsp.Parse(packet.payload_data());
    EXPECT_FALSE(result.is_error());
    EXPECT_EQ(rsp.error_code(), ErrorCode::kInvalidRecordHandle);
    responded = true;
  };

  fake_chan()->SetSendCallback(service_attribute_cb, dispatcher());
  fake_chan()->Receive(kServiceAttributeRequest);
  RunUntilIdle();

  EXPECT_TRUE(responded);

  // Should still be able to unregister it by the handle.
  EXPECT_TRUE(server()->UnregisterService(handle));
}

// Test:
// - Adds a primary protocol to the service definition.
// - Adds multiple additional protocols to the service definition.
// - Tests registration and removal are successful.
// - Tests callback correctness when inbound l2cap channels are connected.
TEST_F(ServerTest, RegisterServiceWithAdditionalProtocol) {
  std::vector<l2cap::Psm> psms{500, 27, 29};

  ServiceRecord psm_additional;
  psm_additional.SetServiceClassUUIDs({profile::kAVRemoteControl});
  psm_additional.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                       protocol::kL2CAP,
                                       DataElement(uint16_t{psms[0]}));
  psm_additional.AddProtocolDescriptor(
      1, protocol::kL2CAP, DataElement(uint16_t{psms[1]}));
  psm_additional.AddProtocolDescriptor(
      2, protocol::kL2CAP, DataElement(uint16_t{psms[2]}));

  std::vector<uint16_t> protocols_discovered;
  auto cb = [&](auto /*channel*/, const DataElement& protocol_list) {
    EXPECT_EQ(DataElement::Type::kSequence, protocol_list.type());
    auto* psm = protocol_list.At(0);
    EXPECT_EQ(DataElement::Type::kSequence, psm->type());
    psm = psm->At(1);
    EXPECT_EQ(DataElement::Type::kUnsignedInt, psm->type());
    protocols_discovered.emplace_back(*psm->template Get<uint16_t>());
  };

  std::vector<ServiceRecord> records;
  records.emplace_back(std::move(psm_additional));
  auto handle = server()->RegisterService(
      std::move(records), kChannelParams, std::move(cb));

  EXPECT_TRUE(handle);

  EXPECT_TRUE(
      l2cap()->TriggerInboundL2capChannel(kTestHandle1, psms[0], 0x40, 0x41));
  EXPECT_TRUE(
      l2cap()->TriggerInboundL2capChannel(kTestHandle1, psms[1], 0x42, 0x43));
  EXPECT_TRUE(
      l2cap()->TriggerInboundL2capChannel(kTestHandle1, psms[2], 0x44, 0x44));
  RunUntilIdle();

  ASSERT_EQ(3u, protocols_discovered.size());
  // There should be one connection (and therefore protocol_list) per psm
  // registered.
  for (auto& psm : psms) {
    ASSERT_EQ(
        1u,
        std::count(
            protocols_discovered.begin(), protocols_discovered.end(), psm));
  }

  EXPECT_TRUE(server()->UnregisterService(handle));
}

// Test:
// - Adds a primary protocol to the service definition.
// - Adds an additional protocol to the service definition.
// - Adds an additional protocol with missing information.
// - Tests that none of protocols are registered.
TEST_F(ServerTest, RegisterServiceWithIncompleteAdditionalProtocol) {
  ServiceRecord psm_additional;
  psm_additional.SetServiceClassUUIDs({profile::kAVRemoteControl});
  psm_additional.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                       protocol::kL2CAP,
                                       DataElement(uint16_t{500}));
  psm_additional.AddProtocolDescriptor(
      1, protocol::kL2CAP, DataElement(uint16_t{27}));
  psm_additional.AddProtocolDescriptor(2, protocol::kL2CAP, DataElement());

  size_t cb_count = 0;
  auto cb = [&](auto /*channel*/, auto& /* protocol_list */) { cb_count++; };

  std::vector<ServiceRecord> records;
  records.emplace_back(std::move(psm_additional));
  RegistrationHandle handle = server()->RegisterService(
      std::move(records), kChannelParams, std::move(cb));

  EXPECT_FALSE(handle);
  EXPECT_FALSE(
      l2cap()->TriggerInboundL2capChannel(kTestHandle1, 500, 0x40, 0x41));
  RunUntilIdle();

  // Despite an incoming L2CAP connection, the callback should never be
  // triggered since no services should be registered.
  EXPECT_EQ(0u, cb_count);

  EXPECT_FALSE(server()->UnregisterService(handle));
}

TEST_F(ServerTest, PsmVerification) {
  ServiceRecord no_psm;
  no_psm.SetServiceClassUUIDs({profile::kAVRemoteControl});
  no_psm.AddProtocolDescriptor(
      ServiceRecord::kPrimaryProtocolList, protocol::kL2CAP, DataElement());

  std::vector<ServiceRecord> records;
  records.emplace_back(std::move(no_psm));
  EXPECT_FALSE(
      server()->RegisterService(std::move(records), kChannelParams, {}));

  ServiceRecord psm_wrong_argtype;
  psm_wrong_argtype.SetServiceClassUUIDs({profile::kAVRemoteControl});
  psm_wrong_argtype.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                          protocol::kL2CAP,
                                          DataElement(bool(true)));

  std::vector<ServiceRecord> records2;
  records2.emplace_back(std::move(psm_wrong_argtype));
  EXPECT_FALSE(
      server()->RegisterService(std::move(records2), kChannelParams, {}));

  ServiceRecord psm_wrong_intsize;
  psm_wrong_intsize.SetServiceClassUUIDs({profile::kAVRemoteControl});
  psm_wrong_intsize.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                          protocol::kL2CAP,
                                          DataElement(uint8_t{5}));

  std::vector<ServiceRecord> records_wrong_intsize;
  records_wrong_intsize.emplace_back(std::move(psm_wrong_intsize));
  EXPECT_FALSE(server()->RegisterService(
      std::move(records_wrong_intsize), kChannelParams, {}));

  ServiceRecord psm_rfcomm;
  psm_rfcomm.SetServiceClassUUIDs({profile::kAVRemoteControl});
  psm_rfcomm.AddProtocolDescriptor(
      ServiceRecord::kPrimaryProtocolList, protocol::kL2CAP, DataElement());
  psm_rfcomm.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                   protocol::kRFCOMM,
                                   DataElement(uint16_t{5}));

  std::vector<ServiceRecord> records3;
  records3.emplace_back(std::move(psm_rfcomm));
  EXPECT_TRUE(server()->RegisterService(
      std::move(records3), kChannelParams, NopConnectCallback));

  // Another RFCOMM should fail, even with a different channel.
  ServiceRecord psm_rfcomm2;
  psm_rfcomm2.SetServiceClassUUIDs({profile::kAVRemoteControl});
  psm_rfcomm2.AddProtocolDescriptor(
      ServiceRecord::kPrimaryProtocolList, protocol::kL2CAP, DataElement());
  psm_rfcomm2.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                    protocol::kRFCOMM,
                                    DataElement(uint16_t{7}));

  std::vector<ServiceRecord> records4;
  records4.emplace_back(std::move(psm_rfcomm2));
  EXPECT_FALSE(server()->RegisterService(
      std::move(records4), kChannelParams, NopConnectCallback));

  ServiceRecord psm_ok;
  psm_ok.SetServiceClassUUIDs({profile::kAVRemoteControl});
  psm_ok.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                               protocol::kL2CAP,
                               DataElement(uint16_t{500}));

  std::vector<ServiceRecord> records5;
  records5.emplace_back(std::move(psm_ok));
  auto handle = server()->RegisterService(
      std::move(records5), kChannelParams, NopConnectCallback);
  EXPECT_TRUE(handle);

  ServiceRecord psm_same;
  psm_same.SetServiceClassUUIDs({profile::kAVRemoteControl});
  psm_same.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                 protocol::kL2CAP,
                                 DataElement(uint16_t{500}));

  std::vector<ServiceRecord> records6;
  records6.emplace_back(std::move(psm_same));
  EXPECT_FALSE(server()->RegisterService(
      std::move(records6), kChannelParams, NopConnectCallback));

  // Unregistering allows us to re-register with PSM.
  server()->UnregisterService(handle);
  ServiceRecord psm_readd;
  psm_readd.SetServiceClassUUIDs({profile::kAVRemoteControl});
  psm_readd.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                  protocol::kL2CAP,
                                  DataElement(uint16_t{500}));

  std::vector<ServiceRecord> records7;
  records7.emplace_back(std::move(psm_readd));
  EXPECT_TRUE(server()->RegisterService(
      std::move(records7), kChannelParams, NopConnectCallback));
}

// Test:
// - Registering multiple ServiceRecords from the same client is successful.
// - Inbound L2CAP connections on the registered PSMs trigger the callback.
TEST_F(ServerTest, RegisterServiceMultipleRecordsSuccess) {
  ServiceRecord record1;
  record1.SetServiceClassUUIDs({profile::kAVRemoteControl});
  record1.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                protocol::kL2CAP,
                                DataElement(uint16_t{7}));
  record1.AddProtocolDescriptor(1, protocol::kL2CAP, DataElement(uint16_t{8}));

  ServiceRecord record2;
  record2.SetServiceClassUUIDs({profile::kAudioSink});
  record2.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                protocol::kL2CAP,
                                DataElement(uint16_t{9}));
  record2.AddProtocolDescriptor(1, protocol::kL2CAP, DataElement(uint16_t{10}));

  std::vector<ServiceRecord> records;
  records.emplace_back(std::move(record1));
  records.emplace_back(std::move(record2));

  size_t cb_count = 0;
  auto cb = [&](auto /*channel*/, auto& /*protocol_list*/) { cb_count++; };

  auto handle = server()->RegisterService(
      std::move(records), kChannelParams, std::move(cb));
  EXPECT_TRUE(handle);

  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(kTestHandle1, 7, 0x40, 0x41));
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(kTestHandle1, 8, 0x42, 0x43));
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(kTestHandle1, 9, 0x44, 0x44));
  EXPECT_TRUE(
      l2cap()->TriggerInboundL2capChannel(kTestHandle1, 10, 0x45, 0x46));
  RunUntilIdle();

  EXPECT_EQ(4u, cb_count);

  EXPECT_TRUE(server()->UnregisterService(handle));
}

// Test:
// - Registering multiple ServiceRecords with the same PSM from the same client
// is successful.
// - Inbound L2CAP connections on the registered PSMs trigger the same callback.
// - Attempting to register a record with an already taken PSM will fail, and
// not register any of the other records in the set of records.
TEST_F(ServerTest, RegisterServiceMultipleRecordsSamePsm) {
  ServiceRecord target_browse_record;
  target_browse_record.SetServiceClassUUIDs({profile::kAVRemoteControlTarget});
  target_browse_record.AddProtocolDescriptor(
      ServiceRecord::kPrimaryProtocolList,
      protocol::kL2CAP,
      DataElement(uint16_t{25}));
  target_browse_record.AddProtocolDescriptor(
      1, protocol::kL2CAP, DataElement(uint16_t{27}));

  ServiceRecord controller_record;
  controller_record.SetServiceClassUUIDs({profile::kAVRemoteControlController});
  controller_record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                          protocol::kL2CAP,
                                          DataElement(uint16_t{25}));

  std::vector<ServiceRecord> records;
  records.emplace_back(std::move(target_browse_record));
  records.emplace_back(std::move(controller_record));

  std::vector<uint16_t> protocols_discovered;
  auto cb = [&](auto /*channel*/, const DataElement& protocol_list) {
    EXPECT_EQ(DataElement::Type::kSequence, protocol_list.type());
    auto* psm = protocol_list.At(0);
    EXPECT_EQ(DataElement::Type::kSequence, psm->type());
    psm = psm->At(1);
    EXPECT_EQ(DataElement::Type::kUnsignedInt, psm->type());
    protocols_discovered.emplace_back(*psm->template Get<uint16_t>());
  };

  auto handle = server()->RegisterService(
      std::move(records), kChannelParams, std::move(cb));
  EXPECT_TRUE(handle);

  EXPECT_TRUE(
      l2cap()->TriggerInboundL2capChannel(kTestHandle1, 25, 0x40, 0x41));
  EXPECT_TRUE(
      l2cap()->TriggerInboundL2capChannel(kTestHandle1, 27, 0x44, 0x44));
  EXPECT_TRUE(
      l2cap()->TriggerInboundL2capChannel(kTestHandle1, 25, 0x42, 0x43));
  RunUntilIdle();

  // We expect two calls to the callback for psm=25, and one for psm=27.
  ASSERT_EQ(
      2u,
      std::count(protocols_discovered.begin(), protocols_discovered.end(), 25));
  ASSERT_EQ(
      1u,
      std::count(protocols_discovered.begin(), protocols_discovered.end(), 27));

  // Attempt to register existing PSM.
  ServiceRecord duplicate_psm;
  duplicate_psm.SetServiceClassUUIDs({profile::kAVRemoteControlTarget});
  duplicate_psm.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                      protocol::kL2CAP,
                                      DataElement(uint16_t{25}));

  ServiceRecord valid_psm;
  valid_psm.SetServiceClassUUIDs({profile::kAudioSource});
  valid_psm.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                  protocol::kL2CAP,
                                  DataElement(uint16_t{31}));

  std::vector<ServiceRecord> invalid_records;
  invalid_records.emplace_back(std::move(duplicate_psm));
  invalid_records.emplace_back(std::move(valid_psm));
  EXPECT_FALSE(server()->RegisterService(
      std::move(invalid_records), kChannelParams, NopConnectCallback));

  EXPECT_TRUE(server()->UnregisterService(handle));
}

TEST_F(ServerTest, RegisterObexService) {
  ServiceRecord record;
  record.SetServiceClassUUIDs({profile::kAVRemoteControlTarget});
  // The AVRCP Target primary protocol has format: L2CAP: uint16_t, AVCTP:
  // uint16_t.
  record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                               protocol::kL2CAP,
                               DataElement(uint16_t{l2cap::kAVCTP}));
  record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                               protocol::kAVCTP,
                               DataElement(uint16_t{0x0104}));
  // It also has 2 additional L2CAP protocols for Browsing & OBEX.
  record.AddProtocolDescriptor(
      1, protocol::kL2CAP, DataElement(uint16_t{l2cap::kAVCTP_Browse}));
  record.AddProtocolDescriptor(
      1, protocol::kAVCTP, DataElement(uint16_t{0x0104}));
  record.AddProtocolDescriptor(
      2, protocol::kL2CAP, DataElement(uint16_t{Server::kDynamicPsm}));
  record.AddProtocolDescriptor(2, protocol::kOBEX, DataElement());

  std::vector<ServiceRecord> records;
  records.emplace_back(std::move(record));

  size_t cb_count = 0;
  auto cb = [&](auto /*channel*/, auto& /*protocol_list*/) { cb_count++; };

  auto handle = server()->RegisterService(
      std::move(records), kChannelParams, std::move(cb));
  EXPECT_TRUE(handle);

  // Should be able to get the registered OBEX service record.
  auto registered_records = server()->GetRegisteredServices(handle);
  EXPECT_EQ(1u, registered_records.size());
  // The updated OBEX protocol should be in the additional protocol descriptors.
  auto registered_addl_protocols =
      registered_records[0]
          .GetAttribute(kAdditionalProtocolDescriptorList)
          .Get<std::vector<DataElement>>();
  EXPECT_EQ((*registered_addl_protocols).size(), 2u);
  // There are two entries - Browsing and OBEX.
  // The OBEX protocol sequence looks like: [[L2CAP UUID, Dynamic PSM], [OBEX
  // UUID].
  auto obex_protocol_seq =
      (*registered_addl_protocols)[0].Get<std::vector<DataElement>>();
  EXPECT_EQ((*obex_protocol_seq).size(), 2u);
  // PSM should be the second element in the first sequence.
  auto updated_psm =
      (*(*obex_protocol_seq).data()->Get<std::vector<DataElement>>())[1]
          .Get<uint16_t>();
  // Since the PSM is randomly assigned, we verify that it's in the valid range.
  EXPECT_TRUE(isValidDynamicPsm(*updated_psm));

  // We expect 3 PSMs to be allocated for this service.
  // The dynamic PSM is generated randomly and is not known in advance -
  // therefore we trigger L2CAP channels for all allocated PSMs.
  TriggerInboundL2capChannelsForAllocated(/*expected_psm_count=*/3);
  // Expect the 3 service-specific PSMs to be connectable.
  EXPECT_EQ(3u, cb_count);

  EXPECT_TRUE(server()->UnregisterService(handle));
  EXPECT_EQ(0u, server()->GetRegisteredServices(handle).size());
}

TEST_F(ServerTest, RegisterObexServiceWithAttribute) {
  ServiceRecord record;
  record.SetServiceClassUUIDs({profile::kMessageNotificationServer});
  // The MNS primary protocol has format: L2CAP, RFCOMM: uint8_t, OBEX
  record.AddProtocolDescriptor(
      ServiceRecord::kPrimaryProtocolList, protocol::kL2CAP, DataElement());
  record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                               protocol::kRFCOMM,
                               DataElement(uint8_t{8}));
  record.AddProtocolDescriptor(
      ServiceRecord::kPrimaryProtocolList, protocol::kOBEX, DataElement());
  // The MNS protocol also specifies an L2CAP protocol in the attributes
  // section.
  record.SetAttribute(kGoepL2capPsm,
                      DataElement(uint16_t(Server::kDynamicPsm)));

  std::vector<ServiceRecord> records;
  records.emplace_back(std::move(record));

  size_t cb_count = 0;
  auto cb = [&](auto /*channel*/, auto& /*protocol_list*/) { cb_count++; };

  auto handle = server()->RegisterService(
      std::move(records), kChannelParams, std::move(cb));
  EXPECT_TRUE(handle);

  // Should be able to get the registered OBEX service record.
  auto registered_records = server()->GetRegisteredServices(handle);
  EXPECT_EQ(1u, registered_records.size());
  // The GoepL2capProtocol should be in the additional attributes. The entry is
  // the updated PSM.
  auto updated_psm =
      registered_records[0].GetAttribute(kGoepL2capPsm).Get<uint16_t>();
  EXPECT_TRUE(isValidDynamicPsm(*updated_psm));

  // We expect 2 PSMs to be allocated and connectable for this service (RFCOMM &
  // Dynamic).
  TriggerInboundL2capChannelsForAllocated(/*expected_psm_count=*/2);
  EXPECT_EQ(2u, cb_count);
  EXPECT_TRUE(server()->UnregisterService(handle));
}

TEST_F(ServerTest, RegisterServiceWithMultipleDynamicPsms) {
  // This service is not defined in any Bluetooth specification. It requests 3
  // dynamic PSMs.
  ServiceRecord record;
  record.SetServiceClassUUIDs({profile::kMessageNotificationServer});
  // Fictional MNS service with a primary protocol with dynamic PSM.
  record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                               protocol::kL2CAP,
                               DataElement(uint16_t{Server::kDynamicPsm}));
  record.AddProtocolDescriptor(
      ServiceRecord::kPrimaryProtocolList, protocol::kOBEX, DataElement());
  // Also contains a dynamic PSM in the GoepL2capAttribute.
  record.SetAttribute(kGoepL2capPsm,
                      DataElement(uint16_t(Server::kDynamicPsm)));
  // Additional protocol has dynamic PSM as well.
  record.AddProtocolDescriptor(
      1, protocol::kL2CAP, DataElement(uint16_t{Server::kDynamicPsm}));
  record.AddProtocolDescriptor(1, protocol::kOBEX, DataElement());

  std::vector<ServiceRecord> records;
  records.emplace_back(std::move(record));

  size_t cb_count = 0;
  auto cb = [&](auto /*channel*/, auto& /*protocol_list*/) { cb_count++; };

  auto handle = server()->RegisterService(
      std::move(records), kChannelParams, std::move(cb));
  EXPECT_TRUE(handle);

  // Should be able to get the registered service record.
  auto registered_records = server()->GetRegisteredServices(handle);
  EXPECT_EQ(1u, registered_records.size());
  // Primary protocol list: [[L2CAP UUID, Dynamic PSM], [OBEX UUID]].
  auto registered_primary_protocol = registered_records[0]
                                         .GetAttribute(kProtocolDescriptorList)
                                         .Get<std::vector<DataElement>>();
  EXPECT_EQ((*registered_primary_protocol).size(), 2u);
  auto updated_psm1 = (*(*registered_primary_protocol)
                            .data()
                            ->Get<std::vector<DataElement>>())[1]
                          .Get<uint16_t>();
  EXPECT_TRUE(isValidDynamicPsm(*updated_psm1));

  // Additional protocol list has one OBEX entry for a dynamic PSM.
  auto registered_addl_protocols =
      registered_records[0]
          .GetAttribute(kAdditionalProtocolDescriptorList)
          .Get<std::vector<DataElement>>();
  ASSERT_EQ((*registered_addl_protocols).size(), 1u);
  // OBEX protocol: [[L2CAP UUID, Dynamic PSM], [OBEX UUID].
  auto obex_protocol_seq =
      (*registered_addl_protocols)[0].Get<std::vector<DataElement>>();
  auto updated_psm2 =
      (*(*obex_protocol_seq).data()->Get<std::vector<DataElement>>())[1]
          .Get<uint16_t>();
  EXPECT_TRUE(isValidDynamicPsm(*updated_psm2));

  // The GoepL2cap attribute should have the updated PSM.
  auto updated_psm3 =
      registered_records[0].GetAttribute(kGoepL2capPsm).Get<uint16_t>();
  // Since the PSM is randomly assigned, we verify that it's in the valid range.
  EXPECT_TRUE(isValidDynamicPsm(*updated_psm3));

  // The dynamic PSMs should all be unique and allocated. Can connect.
  TriggerInboundL2capChannelsForAllocated(/*expected_psm_count=*/3);
  EXPECT_EQ(3u, cb_count);
  EXPECT_TRUE(server()->UnregisterService(handle));
}

TEST_F(ServerTest, RegisterNonObexServiceWithAttributeIsIgnored) {
  ServiceRecord record;
  // The AVRCP Controller service does not require OBEX.
  record.SetServiceClassUUIDs({profile::kAVRemoteControlController});
  record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                               protocol::kL2CAP,
                               DataElement(uint16_t{l2cap::kAVCTP}));
  // The OBEX attribute should be ignored during registration.
  record.SetAttribute(kGoepL2capPsm, DataElement(uint16_t(55)));

  std::vector<ServiceRecord> records;
  records.emplace_back(std::move(record));

  size_t cb_count = 0;
  auto cb = [&](auto /*channel*/, auto& /*protocol_list*/) { cb_count++; };

  auto handle = server()->RegisterService(
      std::move(records), kChannelParams, std::move(cb));
  EXPECT_TRUE(handle);

  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kAVCTP, 0x40, 0x41));
  EXPECT_FALSE(
      l2cap()->TriggerInboundL2capChannel(kTestHandle1, 55, 0x42, 0x43));
  RunUntilIdle();
  EXPECT_EQ(1u, cb_count);
  EXPECT_TRUE(server()->UnregisterService(handle));
}

// Test ServiceSearchRequest:
//  - returns services with the UUID included
//  - doesn't return services that don't have the UUID
//  - fails when there are no items or too many items in the search
//  - doesn't return more than the max requested
TEST_F(ServerTest, ServiceSearchRequest) {
  RegistrationHandle spp_handle = AddSPP();
  RegistrationHandle a2dp_handle = AddA2DPSink();

  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad));
  RunUntilIdle();

  const StaticByteBuffer kL2capSearch(
      0x02,  // SDP_ServiceSearchRequest
      0x10,
      0x01,  // Transaction ID (0x1001)
      0x00,
      0x08,  // Parameter length (8 bytes)
      // ServiceSearchPattern
      0x35,
      0x03,  // Sequence uint8 3 bytes
      0x19,
      0x01,
      0x00,  // UUID: Protocol: L2CAP
      0xFF,
      0xFF,  // MaximumServiceRecordCount: (none)
      0x00   // Continuation State: none
  );

  ServiceSearchRequest search_req;
  EXPECT_FALSE(search_req.valid());
  EXPECT_EQ(nullptr, search_req.GetPDU(0x1001));

  search_req.set_search_pattern({protocol::kL2CAP});

  auto pdu = search_req.GetPDU(0x1001);
  EXPECT_NE(nullptr, pdu);

  EXPECT_TRUE(ContainersEqual(kL2capSearch, *pdu));

  const StaticByteBuffer kL2capSearchResponse(
      0x03,  // SDP_ServicesearchResponse
      0x10,
      0x01,  // Transaction ID (0x1001)
      0x00,
      0x0D,  // Parameter length (13 bytes)
      0x00,
      0x02,  // Total service record count: 2
      0x00,
      0x02,                            // Current service record count: 2
      UINT32_AS_BE_BYTES(spp_handle),  // This list isn't specifically ordered
      UINT32_AS_BE_BYTES(a2dp_handle),
      0x00  // No continuation state
  );

  bool recv = false;
  std::vector<ServiceHandle> handles;
  TransactionId tid;
  auto cb = [&recv, &handles, &tid](auto cb_packet) {
    EXPECT_LE(sizeof(Header), cb_packet->size());
    PacketView<Header> packet(cb_packet.get());
    EXPECT_EQ(kServiceSearchResponse, packet.header().pdu_id);
    tid = pw::bytes::ConvertOrderFrom(cpp20::endian::big, packet.header().tid);
    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    bt_log(TRACE, "unittest", "resize packet to %d", len);
    packet.Resize(len);
    ServiceSearchResponse resp;
    auto status = resp.Parse(packet.payload_data());
    EXPECT_EQ(fit::ok(), status);
    handles = resp.service_record_handle_list();
    recv = true;
  };

  fake_chan()->SetSendCallback(cb, dispatcher());
  fake_chan()->Receive(kL2capSearch);
  RunUntilIdle();

  EXPECT_TRUE(recv);
  EXPECT_EQ(0x1001, tid);
  EXPECT_EQ(2u, handles.size());
  EXPECT_NE(handles.end(),
            std::find(handles.begin(), handles.end(), spp_handle));
  EXPECT_NE(handles.end(),
            std::find(handles.begin(), handles.end(), a2dp_handle));

  const StaticByteBuffer kInvalidNoItems(
      0x02,  // SDP_ServiceSearchRequest
      0x10,
      0xA1,  // Transaction ID (0x10A1)
      0x00,
      0x05,  // Parameter length (5 bytes)
      // ServiceSearchPattern
      0x35,
      0x00,  // Sequence uint8 0 bytes
      0xFF,
      0xFF,  // MaximumServiceRecordCount: (none)
      0x00   // Continuation State: none
  );

  const auto kRspErrSyntax =
      SDP_ERROR_RSP(0x10A1, ErrorCode::kInvalidRequestSyntax);

  EXPECT_TRUE(ReceiveAndExpect(kInvalidNoItems, kRspErrSyntax));

  const StaticByteBuffer kInvalidTooManyItems(
      0x02,  // SDP_ServiceSearchRequest
      0x10,
      0xA1,  // Transaction ID (0x10B1)
      0x00,
      0x2C,  // Parameter length (44 bytes)
      // ServiceSearchPattern
      0x35,
      0x27,  // Sequence uint8 27 bytes
      0x19,
      0x30,
      0x01,  // 13 UUIDs in the search
      0x19,
      0x30,
      0x02,
      0x19,
      0x30,
      0x03,
      0x19,
      0x30,
      0x04,
      0x19,
      0x30,
      0x05,
      0x19,
      0x30,
      0x06,
      0x19,
      0x30,
      0x07,
      0x19,
      0x30,
      0x08,
      0x19,
      0x30,
      0x09,
      0x19,
      0x30,
      0x10,
      0x19,
      0x30,
      0x11,
      0x19,
      0x30,
      0x12,
      0x19,
      0x30,
      0x13,
      0xFF,
      0xFF,  // MaximumServiceRecordCount: (none)
      0x00   // Continuation State: none
  );

  EXPECT_TRUE(ReceiveAndExpect(kInvalidTooManyItems, kRspErrSyntax));
}

// Test ServiceSearchRequest:
//  - doesn't return more than the max requested
TEST_F(ServerTest, ServiceSearchRequestOneOfMany) {
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad));
  RunUntilIdle();

  RegistrationHandle spp_handle = AddSPP();
  RegistrationHandle a2dp_handle = AddA2DPSink();

  bool recv = false;
  std::vector<ServiceHandle> handles;
  TransactionId tid;
  auto cb = [&recv, &handles, &tid](auto cb_packet) {
    EXPECT_LE(sizeof(Header), cb_packet->size());
    PacketView<Header> packet(cb_packet.get());
    EXPECT_EQ(kServiceSearchResponse, packet.header().pdu_id);
    tid = pw::bytes::ConvertOrderFrom(cpp20::endian::big, packet.header().tid);
    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    bt_log(TRACE, "unittests", "resizing packet to %d", len);
    packet.Resize(len);
    ServiceSearchResponse resp;
    auto status = resp.Parse(packet.payload_data());
    EXPECT_EQ(fit::ok(), status);
    handles = resp.service_record_handle_list();
    recv = true;
  };

  const StaticByteBuffer kL2capSearchOne(0x02,  // SDP_ServiceSearchRequest
                                         0x10,
                                         0xC1,  // Transaction ID (0x10C1)
                                         0x00,
                                         0x08,  // Parameter length (8 bytes)
                                         // ServiceSearchPattern
                                         0x35,
                                         0x03,  // Sequence uint8 3 bytes
                                         0x19,
                                         0x01,
                                         0x00,  // UUID: Protocol: L2CAP
                                         0x00,
                                         0x01,  // MaximumServiceRecordCount: 1
                                         0x00   // Continuation State: none
  );

  handles.clear();
  recv = false;

  fake_chan()->SetSendCallback(cb, dispatcher());
  fake_chan()->Receive(kL2capSearchOne);
  RunUntilIdle();

  EXPECT_TRUE(recv);
  EXPECT_EQ(0x10C1, tid);
  EXPECT_EQ(1u, handles.size());
  bool found_spp =
      std::find(handles.begin(), handles.end(), spp_handle) != handles.end();
  bool found_a2dp =
      std::find(handles.begin(), handles.end(), a2dp_handle) != handles.end();
  EXPECT_TRUE(found_spp || found_a2dp);
}

// Test ServiceSearchRequest:
//  - returns continuation state if too many services match
//  - continuation state in request works correctly
TEST_F(ServerTest, ServiceSearchContinuationState) {
  // Set the TX MTU to the lowest that's allowed (48)
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad, 48 /* tx_mtu */));
  RunUntilIdle();

  // Add enough services to generate a continuation state.
  AddL2capService(0x1001);
  AddL2capService(0x1003);
  AddL2capService(0x1005);
  AddL2capService(0x1007);
  AddL2capService(0x1009);
  AddL2capService(0x100B);
  AddL2capService(0x100D);
  AddL2capService(0x100F);
  AddL2capService(0x1011);
  AddL2capService(0x1013);
  AddL2capService(0x1015);

  size_t received = 0;
  ServiceSearchResponse rsp;

  auto send_cb = [this, &rsp, &received](auto cb_packet) {
    EXPECT_LE(sizeof(Header), cb_packet->size());
    PacketView<sdp::Header> packet(cb_packet.get());
    ASSERT_EQ(0x03, packet.header().pdu_id);
    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    EXPECT_LE(len,
              0x2F);  // 10 records (4 * 10) + 2 (total count) + 2 (current
                      // count) + 3 (cont state)
    packet.Resize(len);
    fit::result<Error<>> result = rsp.Parse(packet.payload_data());
    if (received == 0) {
      // Server should have split this into more than one response.
      EXPECT_EQ(ToResult(HostError::kInProgress), result);
      EXPECT_FALSE(rsp.complete());
    }
    received++;
    if (result.is_error() && !result.error_value().is(HostError::kInProgress)) {
      // This isn't a valid packet and we shouldn't try to get
      // a continuation.
      return;
    }
    if (!rsp.complete()) {
      // Repeat the request with the continuation state if it was returned.
      auto continuation = rsp.ContinuationState();
      uint8_t cont_size = static_cast<uint8_t>(continuation.size());
      EXPECT_NE(0u, cont_size);
      // Make another request with the continuation data.
      uint16_t param_size = 8 + cont_size;
      StaticByteBuffer kContinuedRequestStart(
          0x02,  // SDP_ServiceSearchRequest
          0x10,
          0xC1,  // Transaction ID (0x10C1)
          UpperBits(param_size),
          LowerBits(param_size),  // Parameter length
          // ServiceSearchPattern
          0x35,
          0x03,  // Sequence uint8 3 bytes
          0x19,
          0x01,
          0x00,  // UUID: Protocol: L2CAP
          0x00,
          0xFF  // MaximumServiceRecordCount: 256
      );

      DynamicByteBuffer req(kContinuedRequestStart.size() + sizeof(uint8_t) +
                            cont_size);

      kContinuedRequestStart.Copy(&req);
      req.Write(&cont_size, sizeof(uint8_t), kContinuedRequestStart.size());
      req.Write(continuation, kContinuedRequestStart.size() + sizeof(uint8_t));

      fake_chan()->Receive(req);
    }
  };

  const StaticByteBuffer kL2capSearch(0x02,  // SDP_ServiceSearchRequest
                                      0x10,
                                      0xC1,  // Transaction ID (0x10C1)
                                      0x00,
                                      0x08,  // Parameter length (8 bytes)
                                      // ServiceSearchPattern
                                      0x35,
                                      0x03,  // Sequence uint8 3 bytes
                                      0x19,
                                      0x01,
                                      0x00,  // UUID: Protocol: L2CAP
                                      0x00,
                                      0xFF,  // MaximumServiceRecordCount: 256
                                      0x00   // Continuation State: none
  );

  fake_chan()->SetSendCallback(send_cb, dispatcher());
  fake_chan()->Receive(kL2capSearch);
  RunUntilIdle();

  EXPECT_GE(received, 1u);
  EXPECT_EQ(11u, rsp.service_record_handle_list().size());
}

// Test:
//  - Answers ServiceAttributeRequest correctly
//  - Continuation state is generated correctly re:
//    MaximumAttributeListByteCount
//  - Valid Continuation state continues response
TEST_F(ServerTest, ServiceAttributeRequest) {
  ServiceRecord record;
  record.SetServiceClassUUIDs({profile::kAVRemoteControl});
  record.SetAttribute(0xf00d, DataElement(uint32_t{0xfeedbeef}));
  record.SetAttribute(0xf000, DataElement(uint32_t{0x01234567}));

  std::vector<ServiceRecord> records;
  records.emplace_back(std::move(record));
  RegistrationHandle handle =
      server()->RegisterService(std::move(records), kChannelParams, {});

  EXPECT_TRUE(handle);

  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad));
  RunUntilIdle();

  const auto kRequestAttr =
      StaticByteBuffer(0x04,  // SDP_ServiceAttributeRequest
                       0x10,
                       0x01,  // Transaction ID (0x1001)
                       0x00,
                       0x11,  // Parameter length (17 bytes)
                       UINT32_AS_BE_BYTES(handle),  // ServiceRecordHandle
                       0x00,
                       0x0A,  // MaximumAttributeByteCount (10 bytes max)
                       // AttributeIDList
                       0x35,
                       0x08,  // Sequence uint8 8 bytes
                       0x09,  // uint16_t, single attribute
                       0x00,
                       0x01,  // ServiceClassIDList
                       0x0A,  // uint32_t, which is a range (0x3000 - 0xf000)
                       0x30,
                       0x00,  // low end of range
                       0xf0,
                       0x00,  // high end of range
                       0x00   // Continuation State: none
      );

  size_t received = 0;

  ServiceAttributeResponse rsp;

  auto send_cb = [this, handle, &rsp, &received](auto cb_packet) {
    EXPECT_LE(sizeof(Header), cb_packet->size());
    PacketView<sdp::Header> packet(cb_packet.get());
    ASSERT_EQ(0x05, packet.header().pdu_id);
    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    EXPECT_LE(len, 0x11);  // 10 + 2 (byte count) + 5 (cont state)
    packet.Resize(len);
    fit::result<Error<>> result = rsp.Parse(packet.payload_data());
    if (received == 0) {
      // Server should have split this into more than one response.
      EXPECT_EQ(ToResult(HostError::kInProgress), result);
      EXPECT_FALSE(rsp.complete());
    }
    received++;
    if (result.is_error() && !result.error_value().is(HostError::kInProgress)) {
      // This isn't a valid packet and we shouldn't try to get
      // a continuation.
      return;
    }
    if (!rsp.complete()) {
      // Repeat the request with the continuation state if it was returned.
      auto continuation = rsp.ContinuationState();
      uint8_t cont_size = static_cast<uint8_t>(continuation.size());
      EXPECT_NE(0u, cont_size);
      // Make another request with the continuation data.
      uint16_t param_size = 17 + cont_size;
      auto kContinuedRequestAttrStart = StaticByteBuffer(
          0x04,  // SDP_ServiceAttributeRequest
          0x10,
          0x01,  // Transaction ID (reused)
          UpperBits(param_size),
          LowerBits(param_size),       // Parameter length
          UINT32_AS_BE_BYTES(handle),  // ServiceRecordHandle
          0x00,
          0x0A,  // MaximumAttributeByteCount (10 bytes max)
          // AttributeIDList
          0x35,
          0x08,  // Sequence uint8 8 bytes
          0x09,  // uint16_t, single attribute
          0x00,
          0x01,  // ServiceClassIDList
          0x0A,  // uint32_t, which is a range (0x3000 - 0xf000)
          0x30,
          0x00,  // low end of range
          0xf0,
          0x00  // high end of range
      );
      DynamicByteBuffer req(kContinuedRequestAttrStart.size() +
                            sizeof(uint8_t) + cont_size);

      kContinuedRequestAttrStart.Copy(&req);
      req.Write(&cont_size, sizeof(uint8_t), kContinuedRequestAttrStart.size());
      req.Write(continuation,
                kContinuedRequestAttrStart.size() + sizeof(uint8_t));

      fake_chan()->Receive(req);
    }
  };

  fake_chan()->SetSendCallback(send_cb, dispatcher());
  fake_chan()->Receive(kRequestAttr);
  RunUntilIdle();

  EXPECT_GE(received, 1u);
  const auto& attrs = rsp.attributes();
  EXPECT_EQ(2u, attrs.size());
  EXPECT_NE(attrs.end(), attrs.find(kServiceClassIdList));
  EXPECT_NE(attrs.end(), attrs.find(0xf000));

  const auto kInvalidRangeOrder =
      StaticByteBuffer(0x04,  // SDP_ServiceAttributeRequest
                       0xE0,
                       0x01,  // Transaction ID (0xE001)
                       0x00,
                       0x11,  // Parameter length (17 bytes)
                       UINT32_AS_BE_BYTES(handle),  // ServiceRecordHandle
                       0x00,
                       0x0A,  // MaximumAttributeByteCount (10 bytes max)
                       // AttributeIDList
                       0x35,
                       0x08,  // Sequence uint8 8 bytes
                       0x09,  // uint16_t, single attribute
                       0x00,
                       0x01,  // ServiceClassIDList
                       0x0A,  // uint32_t, which is a range (0x3000 - 0xf000)
                       0xf0,
                       0x00,  // low end of range
                       0x30,
                       0x00,  // high end of range
                       0x00   // Continuation State: none
      );

  const auto kRspErrSyntax =
      SDP_ERROR_RSP(0xE001, ErrorCode::kInvalidRequestSyntax);

  EXPECT_TRUE(ReceiveAndExpect(kInvalidRangeOrder, kRspErrSyntax));

  const auto kInvalidMaxBytes =
      StaticByteBuffer(0x04,  // SDP_ServiceAttributeRequest
                       0xE0,
                       0x02,  // Transaction ID (0xE001)
                       0x00,
                       0x0C,  // Parameter length (12 bytes)
                       UINT32_AS_BE_BYTES(handle),  // ServiceRecordHandle
                       0x00,
                       0x05,  // MaximumAttributeByteCount (5 bytes max)
                       // AttributeIDList
                       0x35,
                       0x03,  // Sequence uint8 3 bytes
                       0x09,  // uint16_t, single attribute
                       0x00,
                       0x01,  // ServiceClassIDList
                       0x00   // Continuation State: none
      );

  const auto kRspErrSyntax2 =
      SDP_ERROR_RSP(0xE002, ErrorCode::kInvalidRequestSyntax);

  EXPECT_TRUE(ReceiveAndExpect(kInvalidMaxBytes, kRspErrSyntax2));
}

// Test:
//  - Answers ServiceSearchAttributeRequest correctly
//  - Continuation state is generated correctly re:
//    MaximumAttributeListsByteCount
//  - Valid Continuation state continues response
TEST_F(ServerTest, SearchAttributeRequest) {
  ServiceRecord record1;
  record1.SetServiceClassUUIDs({profile::kAVRemoteControl});
  record1.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                protocol::kL2CAP,
                                DataElement(uint16_t{500}));
  record1.SetAttribute(0xf00d, DataElement(uint32_t{0xfeedbeef}));
  record1.SetAttribute(0xf000, DataElement(uint32_t{0x01234567}));

  std::vector<ServiceRecord> records1;
  records1.emplace_back(std::move(record1));
  RegistrationHandle handle1 = server()->RegisterService(
      std::move(records1), kChannelParams, NopConnectCallback);

  EXPECT_TRUE(handle1);

  ServiceRecord record2;
  record2.SetServiceClassUUIDs({profile::kAVRemoteControl});
  record2.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                protocol::kL2CAP,
                                DataElement(uint16_t{501}));

  std::vector<ServiceRecord> records2;
  records2.emplace_back(std::move(record2));
  RegistrationHandle handle2 = server()->RegisterService(
      std::move(records2), kChannelParams, NopConnectCallback);

  EXPECT_TRUE(handle2);

  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad));
  RunUntilIdle();

  const auto kRequestAttr =
      StaticByteBuffer(0x06,  // SDP_ServiceAttributeRequest
                       0x10,
                       0x01,  // Transaction ID (0x1001)
                       0x00,
                       0x12,  // Parameter length (18 bytes)
                       // ServiceSearchPattern
                       0x35,
                       0x03,  // Sequence uint8 3 bytes
                       0x19,
                       0x01,
                       0x00,  // UUID: Protocol: L2CAP
                       0x00,
                       0x0A,  // MaximumAttributeByteCount (10 bytes max)
                       // AttributeIDList
                       0x35,
                       0x08,  // Sequence uint8 8 bytes
                       0x09,  // uint16_t, single attribute
                       0x00,
                       0x00,  // ServiceRecordHandle
                       0x0A,  // uint32_t, which is a range (0x3000 - 0xf000)
                       0x30,
                       0x00,  // low end of range
                       0xf0,
                       0x00,  // high end of range
                       0x00   // Continuation State: none
      );

  size_t received = 0;

  ServiceSearchAttributeResponse rsp;

  auto send_cb = [this, &rsp, &received](auto cb_packet) {
    EXPECT_LE(sizeof(Header), cb_packet->size());
    PacketView<sdp::Header> packet(cb_packet.get());
    ASSERT_EQ(0x07, packet.header().pdu_id);
    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    EXPECT_LE(len, 0x11);  // 2 (byte count) + 10 (max len) + 5 (cont state)
    packet.Resize(len);
    fit::result<Error<>> result = rsp.Parse(packet.payload_data());
    if (received == 0) {
      // Server should have split this into more than one response.
      EXPECT_EQ(ToResult(HostError::kInProgress), result);
      EXPECT_FALSE(rsp.complete());
    }
    received++;
    if (result.is_error() && !result.error_value().is(HostError::kInProgress)) {
      // This isn't a valid packet and we shouldn't try to get
      // a continuation.
      return;
    }
    if (!rsp.complete()) {
      // Repeat the request with the continuation state if it was returned.
      auto continuation = rsp.ContinuationState();
      uint8_t cont_size = static_cast<uint8_t>(continuation.size());
      EXPECT_NE(0u, cont_size);
      // Make another request with the continuation data.
      uint16_t param_size = 18 + cont_size;
      auto kContinuedRequestAttrStart = StaticByteBuffer(
          0x06,  // SDP_ServiceAttributeRequest
          0x10,
          static_cast<uint8_t>(received + 1),  // Transaction ID
          UpperBits(param_size),
          LowerBits(param_size),  // Parameter length
          0x35,
          0x03,  // Sequence uint8 3 bytes
          0x19,
          0x01,
          0x00,  // SearchPattern: L2CAP
          0x00,
          0x0A,  // MaximumAttributeByteCount (10 bytes max)
          // AttributeIDList
          0x35,
          0x08,  // Sequence uint8 8 bytes
          0x09,  // uint16_t, single attribute
          0x00,
          0x00,  // ServiceRecordHandle
          0x0A,  // uint32_t, which is a range (0x3000 - 0xf000)
          0x30,
          0x00,  // low end of range
          0xf0,
          0x00  // high end of range
      );
      DynamicByteBuffer req(kContinuedRequestAttrStart.size() +
                            sizeof(uint8_t) + cont_size);

      kContinuedRequestAttrStart.Copy(&req);
      req.Write(&cont_size, sizeof(uint8_t), kContinuedRequestAttrStart.size());
      req.Write(continuation,
                kContinuedRequestAttrStart.size() + sizeof(uint8_t));

      fake_chan()->Receive(req);
    }
  };

  fake_chan()->SetSendCallback(send_cb, dispatcher());
  fake_chan()->Receive(kRequestAttr);
  RunUntilIdle();

  EXPECT_GE(received, 1u);
  // We should receive both of our entered records.
  EXPECT_EQ(2u, rsp.num_attribute_lists());
  for (uint32_t i = 0; i < rsp.num_attribute_lists(); i++) {
    const auto& attrs = rsp.attributes(i);
    // Every service has a record handle
    auto handle_it = attrs.find(kServiceRecordHandle);
    EXPECT_NE(attrs.end(), handle_it);
    ServiceHandle received_handle = *handle_it->second.Get<uint32_t>();
    if (received_handle == handle1) {
      // The first service also has another attribute we should find.
      EXPECT_EQ(2u, attrs.size());
      EXPECT_NE(attrs.end(), attrs.find(0xf000));
    }
  }

  const auto kInvalidRangeOrder =
      StaticByteBuffer(0x06,  // SDP_ServiceAttributeRequest
                       0xE0,
                       0x01,  // Transaction ID (0xE001)
                       0x00,
                       0x12,  // Parameter length (18 bytes)
                       0x35,
                       0x03,
                       0x19,
                       0x01,
                       0x00,  // SearchPattern: L2CAP
                       0x00,
                       0x0A,  // MaximumAttributeByteCount (10 bytes max)
                       // AttributeIDList
                       0x35,
                       0x08,  // Sequence uint8 8 bytes
                       0x09,  // uint16_t, single attribute
                       0x00,
                       0x01,  // ServiceClassIDList
                       0x0A,  // uint32_t, which is a range (0x3000 - 0xf000)
                       0xf0,
                       0x00,  // low end of range
                       0x30,
                       0x00,  // high end of range
                       0x00   // Continuation State: none
      );

  const auto kRspErrSyntax =
      SDP_ERROR_RSP(0xE001, ErrorCode::kInvalidRequestSyntax);

  EXPECT_TRUE(ReceiveAndExpect(kInvalidRangeOrder, kRspErrSyntax));

  const auto kInvalidMaxBytes =
      StaticByteBuffer(0x04,  // SDP_ServiceAttributeRequest
                       0xE0,
                       0x02,  // Transaction ID (0xE002)
                       0x00,
                       0x0D,  // Parameter length (13 bytes)
                       0x35,
                       0x03,
                       0x19,
                       0x01,
                       0x00,  // SearchPattern: L2CAP
                       0x00,
                       0x05,  // MaximumAttributeByteCount (5 bytes max)
                       // AttributeIDList
                       0x35,
                       0x03,  // Sequence uint8 3 bytes
                       0x09,  // uint16_t, single attribute
                       0x00,
                       0x01,  // ServiceClassIDList
                       0x00   // Continuation State: none
      );

  const auto kRspErrSyntax2 =
      SDP_ERROR_RSP(0xE002, ErrorCode::kInvalidRequestSyntax);

  EXPECT_TRUE(ReceiveAndExpect(kInvalidMaxBytes, kRspErrSyntax2));
}

TEST_F(ServerTest, ConnectionCallbacks) {
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad));
  RunUntilIdle();

  std::vector<l2cap::Channel::WeakPtr> channels;
  hci_spec::ConnectionHandle latest_handle;

  // Register a service
  AddA2DPSink([&channels, &latest_handle](l2cap::Channel::WeakPtr chan,
                                          const auto& /*protocol*/) {
    bt_log(TRACE, "test", "Got channel for the a2dp sink");
    latest_handle = chan->link_handle();
    channels.emplace_back(std::move(chan));
  });

  // Connect to the service
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kAVDTP, kSdpChannel + 1, 0x0b00));
  RunUntilIdle();

  // It should get a callback with a channel
  EXPECT_EQ(1u, channels.size());
  EXPECT_EQ(kTestHandle1, latest_handle);

  // Connect to the same service again with the same PSM (on a different
  // connection, it should still work)
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle2, l2cap::kAVDTP, kSdpChannel + 2, 0x0b01));
  RunUntilIdle();

  ASSERT_EQ(2u, channels.size());
  EXPECT_EQ(kTestHandle2, latest_handle);
  EXPECT_NE(&channels.front().get(), &channels.back().get());

  // Connect to the service again, on the first connection.
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kAVDTP, kSdpChannel + 3, 0x0b00));
  RunUntilIdle();

  // It should get a callback with a channel
  EXPECT_EQ(3u, channels.size());
  EXPECT_EQ(kTestHandle1, latest_handle);
}

// Browse Group gets set correctly
TEST_F(ServerTest, BrowseGroup) {
  AddA2DPSink();

  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad));
  RunUntilIdle();

  const auto kRequestAttr =
      StaticByteBuffer(0x06,  // SDP_ServiceAttributeRequest
                       0x10,
                       0x01,  // Transaction ID (0x1001)
                       0x00,
                       0x0D,  // Parameter length (12 bytes)
                       // ServiceSearchPattern
                       0x35,
                       0x03,  // Sequence uint8 3 bytes
                       0x19,
                       0x01,
                       0x00,  // UUID: Protocol: L2CAP
                       0xFF,
                       0xFF,  // MaximumAttributeByteCount (no max)
                       // AttributeIDList
                       0x35,
                       0x03,  // Sequence uint8 3 bytes
                       0x09,  // uint16_t, single attribute
                       0x00,
                       0x05,  // BrowseGroupList
                       0x00   // Continuation State: none
      );

  ServiceSearchAttributeResponse rsp;
  auto send_cb = [&rsp](auto cb_packet) {
    EXPECT_LE(sizeof(Header), cb_packet->size());
    PacketView<sdp::Header> packet(cb_packet.get());
    ASSERT_EQ(0x07, packet.header().pdu_id);
    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    packet.Resize(len);
    auto status = rsp.Parse(packet.payload_data());
    EXPECT_EQ(fit::ok(), status);
  };

  fake_chan()->SetSendCallback(send_cb, dispatcher());
  fake_chan()->Receive(kRequestAttr);
  RunUntilIdle();

  EXPECT_EQ(1u, rsp.num_attribute_lists());
  auto& attributes = rsp.attributes(0);
  auto group_attr_it = attributes.find(kBrowseGroupList);
  ASSERT_EQ(DataElement::Type::kSequence, group_attr_it->second.type());
  ASSERT_EQ(DataElement::Type::kUuid, group_attr_it->second.At(0)->type());
  EXPECT_NE(attributes.end(), group_attr_it);
  EXPECT_EQ(kPublicBrowseRootUuid, *group_attr_it->second.At(0)->Get<UUID>());
}

// Channels created for a service registered with channel parameters should be
// configured with that service's channel parameters.
TEST_F(ServerTest, RegisterServiceWithChannelParameters) {
  l2cap::Psm kPsm = l2cap::kAVDTP;

  l2cap::ChannelParameters preferred_params;
  preferred_params.mode =
      l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission;
  preferred_params.max_rx_sdu_size = l2cap::kMinACLMTU;

  std::optional<l2cap::ChannelInfo> params;
  size_t chan_cb_count = 0;
  ASSERT_TRUE(AddL2capService(
      kPsm, preferred_params, [&](auto chan, auto& /*protocol*/) {
        chan_cb_count++;
        params = chan->info();
      }));

  EXPECT_TRUE(
      l2cap()->TriggerInboundL2capChannel(kTestHandle1, kPsm, 0x40, 0x41));
  RunUntilIdle();
  EXPECT_EQ(1u, chan_cb_count);
  ASSERT_TRUE(params);
  EXPECT_EQ(*preferred_params.mode, params->mode);
  EXPECT_EQ(*preferred_params.max_rx_sdu_size, params->max_rx_sdu_size);
}

// Test:
// - Creation of ServiceDiscoveryService is valid.
TEST_F(ServerTest, MakeServiceDiscoveryServiceIsValid) {
  auto sdp_def = server()->MakeServiceDiscoveryService();

  const DataElement& version_number_list =
      sdp_def.GetAttribute(kSDP_VersionNumberList);
  EXPECT_EQ(DataElement::Type::kSequence, version_number_list.type());

  auto* it = version_number_list.At(0);
  EXPECT_EQ(DataElement::Type::kUnsignedInt, it->type());
  EXPECT_EQ(0x0100u, it->Get<uint16_t>());
}

#ifndef NINSPECT
// Test:
// - The inspect hierarchy for the initial server is valid. It should
// only contain the registered PSM for SDP.
TEST_F(ServerTest, InspectHierarchy) {
  inspect::Inspector inspector;
  server()->AttachInspect(inspector.GetRoot());

  auto psm_matcher = AllOf(NodeMatches(
      AllOf(NameMatches("psm0x1"),
            PropertyList(UnorderedElementsAre(StringIs("psm", "SDP"))))));
  auto reg_psm_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("registered_psms"))),
            ChildrenMatch(UnorderedElementsAre(psm_matcher)));

  auto record_matcher = AllOf(
      NodeMatches(AllOf(NameMatches("record0x0"),
                        PropertyList(UnorderedElementsAre(StringIs(
                            "record",
                            "Service Class Id List: Sequence { "
                            "UUID(00001000-0000-1000-8000-00805f9b34fb) }"))))),
      ChildrenMatch(UnorderedElementsAre(reg_psm_matcher)));

  auto sdp_matcher = AllOf(NodeMatches(AllOf(NameMatches("sdp_server"))),
                           ChildrenMatch(UnorderedElementsAre(record_matcher)));

  auto hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo());
  ASSERT_TRUE(hierarchy);
  EXPECT_THAT(hierarchy.take_value(),
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(sdp_matcher))));
}
#endif  // NINSPECT

#ifndef NINSPECT
// Test:
// - The inspect hierarchy is updated correctly after registering another
// service.
// - The inspect hierarchy is updated correctly after unregistering a service.
// Note: None of the matchers test the name of the node. This is because the
// ordering of the std::unordered_map of PSMs is not guaranteed. Asserting on
// the name of the node is not feasible due to the usage of inspect::UniqueName,
// which assigns a new name to a node in every call. The contents of the node
// are verified.
TEST_F(ServerTest, InspectHierarchyAfterUnregisterService) {
  inspect::Inspector inspector;
  server()->AttachInspect(inspector.GetRoot());

  auto handle = AddA2DPSink();

  auto sdp_psm_matcher = AllOf(NodeMatches(
      AllOf(PropertyList(UnorderedElementsAre(StringIs("psm", "SDP"))))));
  auto sdp_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("registered_psms"))),
            ChildrenMatch(UnorderedElementsAre(sdp_psm_matcher)));

  auto a2dp_psm_matcher = AllOf(NodeMatches(
      AllOf(PropertyList(UnorderedElementsAre(StringIs("psm", "AVDTP"))))));
  auto a2dp_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("registered_psms"))),
            ChildrenMatch(UnorderedElementsAre(a2dp_psm_matcher)));

  auto sdp_record_matcher =
      AllOf(NodeMatches(AllOf(PropertyList(UnorderedElementsAre(
                StringIs("record",
                         "Service Class Id List: Sequence { "
                         "UUID(00001000-0000-1000-8000-00805f9b34fb) }"))))),
            ChildrenMatch(UnorderedElementsAre(sdp_matcher)));
  auto a2dp_record_matcher =
      AllOf(NodeMatches(AllOf(PropertyList(UnorderedElementsAre(
                StringIs("record",
                         "Profile Descriptor: Sequence { Sequence { "
                         "UUID(0000110d-0000-1000-8000-00805f9b34fb) "
                         "UnsignedInt:2(259) } }\nService Class "
                         "Id List: Sequence { "
                         "UUID(0000110b-0000-1000-8000-00805f9b34fb) }"))))),
            ChildrenMatch(UnorderedElementsAre(a2dp_matcher)));

  auto sdp_server_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("sdp_server"))),
            ChildrenMatch(
                UnorderedElementsAre(sdp_record_matcher, a2dp_record_matcher)));

  // Hierarchy should contain ServiceRecords and PSMs for SDP and A2DP Sink.
  auto hierarchy = inspect::ReadFromVmo(inspector.DuplicateVmo());
  ASSERT_TRUE(hierarchy);
  EXPECT_THAT(hierarchy.take_value(),
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(sdp_server_matcher))));

  // Unregister the A2DP Sink service.
  EXPECT_TRUE(server()->UnregisterService(handle));

  auto sdp_server_matcher2 =
      AllOf(NodeMatches(AllOf(NameMatches("sdp_server"))),
            ChildrenMatch(UnorderedElementsAre(sdp_record_matcher)));

  // The ServiceRecords and PSMs associated with A2DP Sink should be removed
  // after the service has been registered. Only SDP's data should still exist.
  auto hierarchy2 = inspect::ReadFromVmo(inspector.DuplicateVmo());
  ASSERT_TRUE(hierarchy2);
  EXPECT_THAT(hierarchy2.take_value(),
              AllOf(NodeMatches(NameMatches("root")),
                    ChildrenMatch(UnorderedElementsAre(sdp_server_matcher2))));
}
#endif  // NINSPECT

// Test:
// Server::HandleRequest() provides expected responses when called without
// a corresponding l2cap::channel for both successful requests and errors.
TEST_F(ServerTest, HandleRequestWithoutChannel) {
  const auto kRspErrSize = SDP_ERROR_RSP(0x1001, ErrorCode::kInvalidSize);
  const StaticByteBuffer kTooSmall(0x01,  // SDP_ServiceSearchRequest
                                   0x10,
                                   0x01,  // Transaction ID (0x1001)
                                   0x00,
                                   0x09  // Parameter length (9 bytes)
  );
  const auto kRspTooSmall = SDP_ERROR_RSP(0x1001, ErrorCode::kInvalidSize);
  auto too_small_rsp = server()->HandleRequest(
      std::unique_ptr<ByteBuffer>(new StaticByteBuffer(kTooSmall)),
      l2cap::kDefaultMTU);
  EXPECT_TRUE(ContainersEqual(*too_small_rsp.value(), kRspTooSmall));

  RegistrationHandle spp_handle = AddSPP();
  RegistrationHandle a2dp_handle = AddA2DPSink();
  const StaticByteBuffer kL2capSearch(
      0x02,  // SDP_ServiceSearchRequest
      0x10,
      0x01,  // Transaction ID (0x1001)
      0x00,
      0x08,  // Parameter length (8 bytes)
      // ServiceSearchPattern
      0x35,
      0x03,  // Sequence uint8 3 bytes
      0x19,
      0x01,
      0x00,  // UUID: Protocol: L2CAP
      0xFF,
      0xFF,  // MaximumServiceRecordCount: (none)
      0x00   // Continuation State: none
  );
  const StaticByteBuffer kL2capSearchResponse(
      0x03,  // SDP_ServicesearchResponse
      0x10,
      0x01,  // Transaction ID (0x1001)
      0x00,
      0x0D,  // Parameter length (13 bytes)
      0x00,
      0x02,  // Total service record count: 2
      0x00,
      0x02,                             // Current service record count: 2
      UINT32_AS_BE_BYTES(a2dp_handle),  // This list isn't specifically ordered
      UINT32_AS_BE_BYTES(spp_handle),
      0x00  // No continuation state
  );
  auto search_rsp = server()->HandleRequest(
      std::unique_ptr<ByteBuffer>(new StaticByteBuffer(kL2capSearch)),
      l2cap::kDefaultMTU);
  EXPECT_TRUE(ContainersEqual(*search_rsp.value(), kL2capSearchResponse));
}

// Verify that continuation requests hit the cache, and the cache is shared
// across channels and cleared when services are modified.
TEST_F(ServerTest, ContinuationCacheLifecycle) {
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad, 48 /* tx_mtu */));
  RunUntilIdle();

  ServiceRecord record;
  record.SetServiceClassUUIDs({profile::kSerialPort});
  record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                               protocol::kL2CAP,
                               DataElement(uint16_t{0x1001}));
  record.SetAttribute(
      0xf00d,
      DataElement(std::string(
          "A very long string attribute that will exceed the MTU size when "
          "serialized together with other attributes to force continuation")));
  RegistrationHandle handle = server()->RegisterService(
      {std::move(record)}, kChannelParams, NopConnectCallback);
  ASSERT_NE(0u, handle);

  size_t received = 0;
  std::vector<uint8_t> continuation_state;

  auto send_cb = [&](auto cb_packet) {
    received++;
    PacketView<sdp::Header> packet(cb_packet.get());

    ASSERT_EQ(0x07, packet.header().pdu_id);

    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    packet.Resize(len);

    uint16_t attr_list_byte_count =
        (packet.payload_data()[0] << 8) | packet.payload_data()[1];

    uint8_t continuation_state_length =
        packet.payload_data()[2 + attr_list_byte_count];

    if (received == 1) {
      ASSERT_EQ(4u, continuation_state_length);
      const uint8_t* continuation_src =
          packet.payload_data().data() + 2 + attr_list_byte_count + 1;
      continuation_state = std::vector<uint8_t>(
          continuation_src, continuation_src + continuation_state_length);

      // clang-format off
      StaticByteBuffer req_start(
          0x06,                         // ServiceSearchAttributeRequest
          0x10, 0xC1,                   // TID
          0x00, 0x00,                   // Parameter length (fill later)
          0x35, 0x03, 0x19, 0x01, 0x00, // Search pattern (L2CAP)
          0x00, 0x10,                   // Max attribute byte count
          0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF); // Attribute ID list (All)
      // clang-format on

      uint16_t param_size =
          req_start.size() - sizeof(Header) + 1 + continuation_state_length;

      DynamicByteBuffer req(req_start.size() + 1 + continuation_state_length);
      req_start.Copy(&req);
      req.mutable_data()[3] = static_cast<uint8_t>(param_size >> 8);
      req.mutable_data()[4] = static_cast<uint8_t>(param_size & 0xff);
      req.Write(&continuation_state_length, sizeof(uint8_t), req_start.size());
      req.Write(continuation_state.data(),
                continuation_state.size(),
                req_start.size() + 1);

      fake_chan()->Receive(req);
    } else if (received == 2) {
      EXPECT_GT(attr_list_byte_count, 0u);

      // clang-format off
      StaticByteBuffer new_req(
          0x06,                         // ServiceSearchAttributeRequest
          0x10, 0xC2,                   // Different TID
          0x00, 0x0F,                   // Parameter length (15)
          0x35, 0x03, 0x19, 0x01, 0x00, // Search pattern (L2CAP)
          0x00, 0xFF,                   // Max attribute byte count
          0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, // Attribute ID list (All)
          0x00                          // No continuation
      );
      // clang-format on
      fake_chan()->Receive(new_req);
    } else if (received == 3) {
      EXPECT_GT(attr_list_byte_count, 0u);
    }
  };

  fake_chan()->SetSendCallback(send_cb, dispatcher());

  // clang-format off
  StaticByteBuffer req(
      0x06,                         // ServiceSearchAttributeRequest
      0x10, 0xC1,                   // TID
      0x00, 0x0F,                   // Parameter length (15)
      0x35, 0x03, 0x19, 0x01, 0x00, // Search pattern (L2CAP)
      0x00, 0x10,                   // Max attribute byte count
      0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, // Attribute ID list (All)
      0x00                          // No continuation
  );
  // clang-format on

  fake_chan()->Receive(req);
  RunUntilIdle();

  EXPECT_EQ(3u, received);

  fake_chan()->Close();
  RunUntilIdle();

  // clang-format off
  StaticByteBuffer cont_req_direct(
      0x06,                         // ServiceSearchAttributeRequest
      0x10, 0xC1,                   // TID
      0x00, 0x13,                   // Parameter length (19)
      0x35, 0x03, 0x19, 0x01, 0x00, // Search pattern (L2CAP)
      0x00, 0x10,                   // Max attribute byte count
      0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, // Attribute ID list (All)
      0x04,                         // Continuation length
      continuation_state[0], continuation_state[1],
      continuation_state[2], continuation_state[3]);
  // clang-format on

  auto direct_rsp = server()->HandleRequest(
      std::make_unique<DynamicByteBuffer>(cont_req_direct), l2cap::kDefaultMTU);

  ASSERT_TRUE(direct_rsp.has_value());
  PacketView<sdp::Header> succ_packet(direct_rsp.value().get());
  EXPECT_EQ(0x07, succ_packet.header().pdu_id);

  // Modifying service records clears the cached responses.
  EXPECT_TRUE(server()->UnregisterService(handle));
  auto err_rsp = server()->HandleRequest(
      std::make_unique<DynamicByteBuffer>(cont_req_direct), l2cap::kDefaultMTU);
  ASSERT_TRUE(err_rsp.has_value());
  PacketView<sdp::Header> err_packet(err_rsp.value().get());
  EXPECT_EQ(0x01, err_packet.header().pdu_id);
}

// Verify that a new transaction (empty continuation state) with identical
// query parameters on an open channel bypasses any stale cached response.
TEST_F(ServerTest, ContinuationCacheNewTransactionBypassesCache) {
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad, l2cap::kDefaultMTU));
  RunUntilIdle();

  ServiceRecord record;
  record.SetServiceClassUUIDs({profile::kSerialPort});
  record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                               protocol::kL2CAP,
                               DataElement(uint16_t{0x1001}));
  record.SetAttribute(0xf00d, DataElement(std::string("Initial Value")));
  RegistrationHandle handle1 = server()->RegisterService(
      {std::move(record)}, kChannelParams, NopConnectCallback);
  ASSERT_NE(0u, handle1);

  size_t received = 0;
  auto send_cb = [&](auto cb_packet) {
    received++;
    PacketView<sdp::Header> packet(cb_packet.get());
    ASSERT_EQ(kServiceSearchAttributeResponse, packet.header().pdu_id);

    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    packet.Resize(len);

    ServiceSearchAttributeResponse resp;
    EXPECT_EQ(fit::ok(), resp.Parse(packet.payload_data()));
    ASSERT_EQ(1u, resp.num_attribute_lists());
    auto& attributes = resp.attributes(0);
    auto attr_it = attributes.find(0xf00d);
    ASSERT_NE(attributes.end(), attr_it);
    ASSERT_TRUE(attr_it->second.Get<std::string>());

    if (received == 1) {
      EXPECT_EQ("Initial Value", *attr_it->second.Get<std::string>());
    } else if (received == 2) {
      EXPECT_EQ("Updated Value", *attr_it->second.Get<std::string>());
    }
  };

  fake_chan()->SetSendCallback(send_cb, dispatcher());

  // clang-format off
  StaticByteBuffer req(
      0x06,                         // ServiceSearchAttributeRequest
      0x10, 0xC1,                   // TID
      0x00, 0x0F,                   // Parameter length (15)
      0x35, 0x03, 0x19, 0x01, 0x00, // Search pattern (L2CAP)
      0x00, 0xFF,                   // Max attribute byte count
      0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, // Attribute ID list (All)
      0x00                          // No continuation
  );
  // clang-format on

  fake_chan()->Receive(req);
  RunUntilIdle();
  EXPECT_EQ(1u, received);

  EXPECT_TRUE(server()->UnregisterService(handle1));

  ServiceRecord record2;
  record2.SetServiceClassUUIDs({profile::kSerialPort});
  record2.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                                protocol::kL2CAP,
                                DataElement(uint16_t{0x1001}));
  record2.SetAttribute(0xf00d, DataElement(std::string("Updated Value")));
  RegistrationHandle handle2 = server()->RegisterService(
      {std::move(record2)}, kChannelParams, NopConnectCallback);
  ASSERT_NE(0u, handle2);

  // Re-send exact same request without continuation state. It must bypass the
  // stale cache and serve the updated service attributes.
  fake_chan()->Receive(req);
  RunUntilIdle();
  EXPECT_EQ(2u, received);
}

// Verify that sending a request with a continuation state when no prior
// transaction exists on the open channel returns kInvalidContinuationState.
TEST_F(ServerTest, ContinuationRequestWithoutPriorTransactionReturnsError) {
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad, l2cap::kDefaultMTU));
  RunUntilIdle();

  ServiceRecord record;
  record.SetServiceClassUUIDs({profile::kSerialPort});
  record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                               protocol::kL2CAP,
                               DataElement(uint16_t{0x1001}));
  RegistrationHandle handle = server()->RegisterService(
      {std::move(record)}, kChannelParams, NopConnectCallback);
  ASSERT_NE(0u, handle);

  bool received = false;
  auto send_cb = [&received](auto cb_packet) {
    received = true;
    PacketView<sdp::Header> packet(cb_packet.get());
    ASSERT_EQ(kErrorResponse, packet.header().pdu_id);
    uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                               packet.header().param_length);
    packet.Resize(len);
    ErrorResponse rsp;
    EXPECT_EQ(fit::ok(), rsp.Parse(packet.payload_data()));
    EXPECT_EQ(ErrorCode::kInvalidContinuationState, rsp.error_code());
  };

  fake_chan()->SetSendCallback(send_cb, dispatcher());

  // clang-format off
  // Request with a placeholder continuation state of length 2
  StaticByteBuffer req(
      0x06,                         // ServiceSearchAttributeRequest
      0x10, 0xC1,                   // TID
      0x00, 0x11,                   // Parameter length (17)
      0x35, 0x03, 0x19, 0x01, 0x00, // Search pattern (L2CAP)
      0x00, 0xFF,                   // Max attribute byte count
      0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, // Attribute ID list (All)
      0x02,                         // Continuation state length
      0x00, 0x05                    // Continuation state data
  );
  // clang-format on

  fake_chan()->Receive(req);
  RunUntilIdle();
  EXPECT_TRUE(received);
}

// Verify that altering query parameters mid-transaction on a valid continuation
// state is rejected with kInvalidContinuationState.
TEST_F(ServerTest, ContinuationRequestWithModifiedParametersReturnsError) {
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      kTestHandle1, l2cap::kSDP, kSdpChannel, 0x0bad, 48 /* tx_mtu */));
  RunUntilIdle();

  ServiceRecord record;
  record.SetServiceClassUUIDs({profile::kSerialPort});
  record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                               protocol::kL2CAP,
                               DataElement(uint16_t{0x1001}));
  record.SetAttribute(
      0xf00d,
      DataElement(std::string(
          "A very long string attribute that will exceed the MTU size when "
          "serialized together with other attributes to force continuation")));
  RegistrationHandle handle = server()->RegisterService(
      {std::move(record)}, kChannelParams, NopConnectCallback);
  ASSERT_NE(0u, handle);

  size_t received = 0;
  auto send_cb = [&](auto cb_packet) {
    received++;
    PacketView<sdp::Header> packet(cb_packet.get());

    if (received == 1) {
      ASSERT_EQ(kServiceSearchAttributeResponse, packet.header().pdu_id);
      uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                                 packet.header().param_length);
      packet.Resize(len);
      uint16_t attr_list_byte_count =
          (packet.payload_data()[0] << 8) | packet.payload_data()[1];
      uint8_t cont_len = packet.payload_data()[2 + attr_list_byte_count];
      ASSERT_GT(cont_len, 0u);
      const uint8_t* cont_src =
          packet.payload_data().data() + 2 + attr_list_byte_count + 1;

      // clang-format off
      // Construct continuation request, but modify MaxAttributeByteCount from 0x0010 to 0x0020
      StaticByteBuffer req_start(
          0x06,                         // ServiceSearchAttributeRequest
          0x10, 0xC2,                   // TID
          0x00, 0x00,                   // Parameter length (fill later)
          0x35, 0x03, 0x19, 0x01, 0x00, // Search pattern (L2CAP)
          0x00, 0x20,                   // Max attribute byte count
          0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF); // Attribute ID list (All)
      // clang-format on
      uint16_t param_size = req_start.size() - sizeof(Header) + 1 + cont_len;
      DynamicByteBuffer req(req_start.size() + 1 + cont_len);
      req_start.Copy(&req);
      req.mutable_data()[3] = static_cast<uint8_t>(param_size >> 8);
      req.mutable_data()[4] = static_cast<uint8_t>(param_size & 0xff);
      req.Write(&cont_len, sizeof(uint8_t), req_start.size());
      req.Write(cont_src, cont_len, req_start.size() + 1);

      fake_chan()->Receive(req);
    } else if (received == 2) {
      ASSERT_EQ(kErrorResponse, packet.header().pdu_id);
      uint16_t len = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                                 packet.header().param_length);
      packet.Resize(len);
      ErrorResponse rsp;
      EXPECT_EQ(fit::ok(), rsp.Parse(packet.payload_data()));
      EXPECT_EQ(ErrorCode::kInvalidContinuationState, rsp.error_code());
    }
  };

  fake_chan()->SetSendCallback(send_cb, dispatcher());

  // clang-format off
  StaticByteBuffer req(
      0x06,                         // ServiceSearchAttributeRequest
      0x10, 0xC1,                   // TID
      0x00, 0x0F,                   // Parameter length (15)
      0x35, 0x03, 0x19, 0x01, 0x00, // Search pattern (L2CAP)
      0x00, 0x10,                   // Max attribute byte count
      0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, // Attribute ID list (All)
      0x00                          // No continuation
  );
  // clang-format on

  fake_chan()->Receive(req);
  RunUntilIdle();
  EXPECT_EQ(2u, received);
}

// Verify that the response cache holds up to kMaxCachedResponses entries and
// evicts the least recently used response when full.
TEST_F(ServerTest, ContinuationCacheLruEviction) {
  ServiceRecord record;
  record.SetServiceClassUUIDs({profile::kSerialPort});
  record.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                               protocol::kL2CAP,
                               DataElement(uint16_t{0x1001}));
  record.SetAttribute(
      0xf00d,
      DataElement(std::string(
          "A very long string attribute that will exceed the MTU size when "
          "serialized together with other attributes to force continuation")));
  RegistrationHandle handle = server()->RegisterService(
      {std::move(record)}, kChannelParams, NopConnectCallback);
  ASSERT_NE(0u, handle);

  // Send an initial request that generates continuation state.
  // clang-format off
  StaticByteBuffer req_start(
      0x06,                         // ServiceSearchAttributeRequest
      0x10, 0x01,                   // TID
      0x00, 0x0F,                   // Parameter length (15)
      0x35, 0x03, 0x19, 0x01, 0x00, // Search pattern (L2CAP)
      0x00, 0x10,                   // Max attribute byte count: 16 (0x0010)
      0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, // Attribute ID list (All)
      0x00                          // No continuation
  );
  // clang-format on

  auto rsp1 = server()->HandleRequest(
      std::make_unique<DynamicByteBuffer>(req_start), l2cap::kDefaultMTU);
  ASSERT_TRUE(rsp1.has_value());
  PacketView<sdp::Header> packet1(rsp1.value().get());
  ASSERT_EQ(kServiceSearchAttributeResponse, packet1.header().pdu_id);

  // Extract continuation state from rsp1.
  uint16_t len1 = pw::bytes::ConvertOrderFrom(cpp20::endian::big,
                                              packet1.header().param_length);
  packet1.Resize(len1);
  uint16_t attr_list_byte_count =
      (packet1.payload_data()[0] << 8) | packet1.payload_data()[1];
  uint8_t cont_len = packet1.payload_data()[2 + attr_list_byte_count];
  ASSERT_GT(cont_len, 0u);
  std::vector<uint8_t> cont_state(
      packet1.payload_data().data() + 2 + attr_list_byte_count + 1,
      packet1.payload_data().data() + 2 + attr_list_byte_count + 1 + cont_len);

  // Now perform kMaxCachedResponses distinct initial requests (by varying max
  // attribute byte count) to push out the original response from the LRU cache.
  for (uint16_t i = 0; i < 10; ++i) {
    DynamicByteBuffer req_i(req_start.size());
    req_start.Copy(&req_i);
    // Vary MaxAttributeByteCount so each request has a unique RequestKey.
    req_i.mutable_data()[10] = static_cast<uint8_t>(0x11 + i);
    auto rsp_i = server()->HandleRequest(
        std::make_unique<DynamicByteBuffer>(req_i), l2cap::kDefaultMTU);
    ASSERT_TRUE(rsp_i.has_value());
  }

  // Now attempt to use the continuation state from the very first request.
  // Because it was evicted from the LRU cache, it should fail with
  // kInvalidContinuationState.
  uint16_t param_size = req_start.size() - sizeof(Header) + cont_len;
  DynamicByteBuffer cont_req(req_start.size() + cont_len);
  req_start.Copy(&cont_req);
  cont_req.mutable_data()[3] = static_cast<uint8_t>(param_size >> 8);
  cont_req.mutable_data()[4] = static_cast<uint8_t>(param_size & 0xff);
  cont_req.Write(&cont_len, sizeof(uint8_t), req_start.size() - 1);
  cont_req.Write(cont_state.data(), cont_state.size(), req_start.size());

  auto cont_rsp = server()->HandleRequest(
      std::make_unique<DynamicByteBuffer>(cont_req), l2cap::kDefaultMTU);
  ASSERT_TRUE(cont_rsp.has_value());
  PacketView<sdp::Header> err_packet(cont_rsp.value().get());
  EXPECT_EQ(kErrorResponse, err_packet.header().pdu_id);
}

#undef SDP_ERROR_RSP
#undef UINT32_AS_LE_BYTES

}  // namespace
}  // namespace bt::sdp
