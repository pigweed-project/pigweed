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

#include "pw_bluetooth_sapphire/internal/host/sdp/client.h"

#include <pw_async/dispatcher.h>

#include <chrono>
#include <ratio>

#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel_test.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/service_record.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::sdp {
namespace {

using TestingBase = bt::l2cap::testing::FakeChannelTest;
constexpr l2cap::ChannelId kTestChannelId = 0x0041;
constexpr uint16_t kResponseMaxSize = 672;

class ClientTest : public TestingBase {
 public:
  ClientTest() = default;

 protected:
  void SetUp() override {
    ChannelOptions options(kTestChannelId);
    options.link_type = bt::LinkType::kACL;
    channel_ = CreateFakeChannel(options);
  }

  void TearDown() override { channel_ = nullptr; }

 private:
  std::unique_ptr<l2cap::testing::FakeChannel> channel_;
};

// Flower Path Test:
//  - sends correctly formatted request
//  - receives response in the callback
//  - receives kNotFound at the end of the callbacks
//  - closes SDP channel when client is deallocated
TEST_F(ClientTest, ConnectAndQuery) {
  {
    auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());

    EXPECT_TRUE(fake_chan()->activated());

    size_t cb_count = 0;
    auto result_cb =
        [&](fit::result<
            Error<>,
            std::reference_wrapper<const std::map<AttributeId, DataElement>>>
                attrs_result) {
          cb_count++;
          if (cb_count == 3) {
            EXPECT_EQ(Error(HostError::kNotFound), attrs_result);
            return true;
          }
          const std::map<AttributeId, DataElement>& attrs =
              attrs_result.value();
          // All results should have the ServiceClassIdList.
          EXPECT_EQ(1u, attrs.count(kServiceClassIdList));
          // The first result has a kProtocolDescriptorList and the second has a
          // kBluetoothProfileDescriptorList
          if (cb_count == 1) {
            EXPECT_EQ(1u, attrs.count(kProtocolDescriptorList));
            EXPECT_EQ(0u, attrs.count(kBluetoothProfileDescriptorList));
          } else if (cb_count == 2) {
            EXPECT_EQ(0u, attrs.count(kProtocolDescriptorList));
            EXPECT_EQ(1u, attrs.count(kBluetoothProfileDescriptorList));
          }
          return true;
        };

    const StaticByteBuffer kSearchExpectedParams(
        // ServiceSearchPattern
        0x35,
        0x03,  // Sequence uint8 3 bytes
        0x19,
        0x11,
        0x0B,  // UUID (kAudioSink)
        0xFF,
        0xFF,  // MaxAttributeByteCount (no max)
        // Attribute ID list
        0x35,
        0x09,  // Sequence uint8 9 bytes
        0x09,
        0x00,
        0x01,  // uint16_t (kServiceClassIdList)
        0x09,
        0x00,
        0x04,  // uint16_t (kProtocolDescriptorList)
        0x09,
        0x00,
        0x09,  // uint16_t (kBluetoothProfileDescriptorList)
        0x00   // No continuation state
    );

    uint16_t request_tid;
    bool success = false;

    fake_chan()->SetSendCallback(
        [&request_tid, &success, &kSearchExpectedParams](auto packet) {
          // First byte should be type.
          ASSERT_LE(3u, packet->size());
          ASSERT_EQ(kServiceSearchAttributeRequest, (*packet)[0]);
          ASSERT_EQ(kSearchExpectedParams, packet->view(5));
          request_tid = ((*packet)[1] << 8) != 0 || (*packet)[2];
          success = true;
        },
        dispatcher());

    // Search for all A2DP sinks, get the:
    //  - Service Class ID list
    //  - Descriptor List
    //  - Bluetooth Profile Descriptor List
    client->ServiceSearchAttributes({profile::kAudioSink},
                                    {kServiceClassIdList,
                                     kProtocolDescriptorList,
                                     kBluetoothProfileDescriptorList},
                                    result_cb);
    RunUntilIdle();
    EXPECT_TRUE(success);

    // Receive the response
    // Record makes building the response easier.
    ServiceRecord rec;
    rec.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                              protocol::kL2CAP,
                              DataElement(l2cap::kAVDTP));
    // The second element here indicates version 1.3 (specified in A2DP spec)
    rec.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                              protocol::kAVDTP,
                              DataElement(uint16_t{0x0103}));
    rec.AddProfile(profile::kAudioSink, 1, 3);
    ServiceSearchAttributeResponse rsp;
    rsp.SetAttribute(0,
                     kServiceClassIdList,
                     DataElement({DataElement(profile::kAudioSink)}));
    rsp.SetAttribute(0,
                     kProtocolDescriptorList,
                     rec.GetAttribute(kProtocolDescriptorList).Clone());

    rsp.SetAttribute(1,
                     kServiceClassIdList,
                     DataElement({DataElement(profile::kAudioSink)}));
    rsp.SetAttribute(1,
                     kBluetoothProfileDescriptorList,
                     rec.GetAttribute(kBluetoothProfileDescriptorList).Clone());

    auto rsp_ptr = rsp.GetPDU(0xFFFF /* Max attribute bytes */,
                              request_tid,
                              kResponseMaxSize,
                              BufferView());
    fake_chan()->Receive(*rsp_ptr);

    RunUntilIdle();

    EXPECT_EQ(3u, cb_count);
  }
  EXPECT_FALSE(fake_chan()->activated());
}

TEST_F(ClientTest, TwoQueriesSubsequent) {
  {
    auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());

    EXPECT_TRUE(fake_chan()->activated());

    size_t cb_count = 0;
    auto result_cb =
        [&](fit::result<
            Error<>,
            std::reference_wrapper<const std::map<AttributeId, DataElement>>>
                attrs_result) {
          cb_count++;
          // We return no results for both queries.
          EXPECT_EQ(Error(HostError::kNotFound), attrs_result);
          return true;
        };

    const StaticByteBuffer kSearchExpectedParams(
        // ServiceSearchPattern
        0x35,
        0x03,  // Sequence uint8 3 bytes
        0x19,
        0x11,
        0x0B,  // UUID (kAudioSink)
        0xFF,
        0xFF,  // MaxAttributeByteCount (no max)
        // Attribute ID list
        0x35,
        0x03,  // Sequence uint8 3 bytes
        0x09,
        0x00,
        0x01,  // uint16_t (kServiceClassIdList)
        0x00   // No continuation state
    );

    uint16_t request_tid;
    bool success = false;

    fake_chan()->SetSendCallback(
        [&request_tid, &success, &kSearchExpectedParams](auto packet) {
          // First byte should be type.
          ASSERT_LE(3u, packet->size());
          ASSERT_EQ(kServiceSearchAttributeRequest, (*packet)[0]);
          ASSERT_EQ(kSearchExpectedParams, packet->view(5));
          request_tid = ((*packet)[1] << 8) != 0 || (*packet)[2];
          success = true;
        },
        dispatcher());

    // Search for all A2DP sinks, get the:
    //  - Service Class ID list
    client->ServiceSearchAttributes(
        {profile::kAudioSink}, {kServiceClassIdList}, result_cb);
    RunUntilIdle();
    EXPECT_TRUE(success);

    // Receive the response (empty response)
    // Record makes building the response easier.
    ServiceSearchAttributeResponse rsp;
    auto rsp_ptr = rsp.GetPDU(0xFFFF /* Max attribute bytes */,
                              request_tid,
                              kResponseMaxSize,
                              BufferView());
    fake_chan()->Receive(*rsp_ptr);

    RunUntilIdle();

    EXPECT_EQ(1u, cb_count);

    // Twice
    success = false;
    client->ServiceSearchAttributes(
        {profile::kAudioSink}, {kServiceClassIdList}, result_cb);
    RunUntilIdle();
    EXPECT_TRUE(success);

    rsp_ptr = rsp.GetPDU(0xFFFF /* Max attribute bytes */,
                         request_tid,
                         kResponseMaxSize,
                         BufferView());
    fake_chan()->Receive(*rsp_ptr);

    RunUntilIdle();

    EXPECT_EQ(2u, cb_count);
  }
  EXPECT_FALSE(fake_chan()->activated());
}

TEST_F(ClientTest, TwoQueriesQueued) {
  {
    auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());

    EXPECT_TRUE(fake_chan()->activated());

    size_t cb_count = 0;
    auto result_cb =
        [&](fit::result<
            Error<>,
            std::reference_wrapper<const std::map<AttributeId, DataElement>>>
                attrs_result) {
          cb_count++;
          // We return no results for both queries.
          EXPECT_EQ(Error(HostError::kNotFound), attrs_result);
          return true;
        };

    const StaticByteBuffer kSearchExpectedParams(
        // ServiceSearchPattern
        0x35,
        0x03,  // Sequence uint8 3 bytes
        0x19,
        0x11,
        0x0B,  // UUID (kAudioSink)
        0xFF,
        0xFF,  // MaxAttributeByteCount (no max)
        // Attribute ID list
        0x35,
        0x03,  // Sequence uint8 3 bytes
        0x09,
        0x00,
        0x01,  // uint16_t (kServiceClassIdList)
        0x00   // No continuation state
    );

    uint16_t request_tid;
    size_t sent_packets = 0;

    fake_chan()->SetSendCallback(
        [&request_tid, &sent_packets, &kSearchExpectedParams](auto packet) {
          // First byte should be type.
          ASSERT_LE(3u, packet->size());
          ASSERT_EQ(kServiceSearchAttributeRequest, (*packet)[0]);
          ASSERT_EQ(kSearchExpectedParams, packet->view(5));
          request_tid = ((*packet)[1] << 8) != 0 || (*packet)[2];
          sent_packets++;
        },
        dispatcher());

    // Search for all A2DP sinks, get the:
    //  - Service Class ID list
    client->ServiceSearchAttributes(
        {profile::kAudioSink}, {kServiceClassIdList}, result_cb);
    // Twice (without waiting)
    client->ServiceSearchAttributes(
        {profile::kAudioSink}, {kServiceClassIdList}, result_cb);
    RunUntilIdle();
    // Only one request should have been sent.
    EXPECT_EQ(1u, sent_packets);

    // Receive the response (empty response)
    // Record makes building the response easier.
    ServiceSearchAttributeResponse rsp;
    auto rsp_ptr = rsp.GetPDU(0xFFFF /* Max attribute bytes */,
                              request_tid,
                              kResponseMaxSize,
                              BufferView());
    fake_chan()->Receive(*rsp_ptr);

    RunUntilIdle();

    EXPECT_EQ(1u, cb_count);
    // The second request should have been sent when the first completed.
    EXPECT_EQ(2u, sent_packets);

    // Respond to the second request.
    rsp_ptr = rsp.GetPDU(0xFFFF /* Max attribute bytes */,
                         request_tid,
                         kResponseMaxSize,
                         BufferView());
    fake_chan()->Receive(*rsp_ptr);

    RunUntilIdle();

    EXPECT_EQ(2u, cb_count);
    EXPECT_EQ(2u, sent_packets);
  }
  EXPECT_FALSE(fake_chan()->activated());
}

// Continuing response test:
//  - send correctly formatted request
//  - receives a response with a continuing response
//  - sends a second request to get the rest of the response
//  - receives the continued response
//  - responds with the results
//  - gives up when callback returns false
TEST_F(ClientTest, ContinuingResponseRequested) {
  auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());

  size_t cb_count = 0;
  auto result_cb =
      [&](fit::result<
          Error<>,
          std::reference_wrapper<const std::map<AttributeId, DataElement>>>
              attrs_result) {
        cb_count++;
        if (cb_count == 3) {
          EXPECT_EQ(Error(HostError::kNotFound), attrs_result);
          return true;
        }
        const std::map<AttributeId, DataElement>& attrs = attrs_result.value();
        // All results should have the ServiceClassIdList.
        EXPECT_EQ(1u, attrs.count(kServiceClassIdList));
        EXPECT_EQ(1u, attrs.count(kProtocolDescriptorList));
        return true;
      };

  const StaticByteBuffer kSearchExpectedParams(
      // ServiceSearchPattern
      0x35,
      0x03,  // Sequence uint8 3 bytes
      0x19,
      0x11,
      0x0B,  // UUID (kAudioSink)
      0xFF,
      0xFF,  // MaxAttributeByteCount (no max)
      // Attribute ID list
      0x35,
      0x06,  // Sequence uint8 6 bytes
      0x09,
      0x00,
      0x01,  // uint16_t (0x0001 = kServiceClassIdList)
      0x09,
      0x00,
      0x04  // uint16_t (0x0004 = kProtocolDescriptorList)
  );

  size_t requests_made = 0;

  // Record makes building the response easier.
  ServiceRecord rec;
  rec.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                            protocol::kL2CAP,
                            DataElement(l2cap::kAVDTP));
  // The second element here indicates version 1.3 (specified in A2DP spec)
  rec.AddProtocolDescriptor(ServiceRecord::kPrimaryProtocolList,
                            protocol::kAVDTP,
                            DataElement(uint16_t{0x0103}));
  rec.AddProfile(profile::kAudioSink, 1, 3);
  ServiceSearchAttributeResponse rsp;
  rsp.SetAttribute(
      0, kServiceClassIdList, DataElement({DataElement(profile::kAudioSink)}));
  rsp.SetAttribute(0,
                   kProtocolDescriptorList,
                   rec.GetAttribute(kProtocolDescriptorList).Clone());
  rsp.SetAttribute(
      1, kServiceClassIdList, DataElement({DataElement(profile::kAudioSink)}));
  rsp.SetAttribute(1,
                   kProtocolDescriptorList,
                   rec.GetAttribute(kProtocolDescriptorList).Clone());

  fake_chan()->SetSendCallback(
      [&](auto packet) {
        requests_made++;
        // First byte should be type.
        ASSERT_LE(5u, packet->size());
        ASSERT_EQ(kServiceSearchAttributeRequest, (*packet)[0]);
        uint16_t request_tid = ((*packet)[1] << 8) != 0 || (*packet)[2];
        ASSERT_EQ(kSearchExpectedParams,
                  packet->view(5, kSearchExpectedParams.size()));
        // The stuff after the params is the continuation state.
        auto rsp_ptr =
            rsp.GetPDU(16 /* Max attribute bytes */,
                       request_tid,
                       kResponseMaxSize,
                       packet->view(5 + kSearchExpectedParams.size() + 1));
        fake_chan()->Receive(*rsp_ptr);
      },
      dispatcher());

  // Search for all A2DP sinks, get the:
  //  - Service Class ID list
  //  - Descriptor List
  //  - Bluetooth Profile Descriptor List
  client->ServiceSearchAttributes(
      {profile::kAudioSink},
      {kServiceClassIdList, kProtocolDescriptorList},
      result_cb);
  RunUntilIdle();
  EXPECT_EQ(3u, cb_count);
  EXPECT_EQ(4u, requests_made);
}

// No results test:
//  - send correctly formatted request
//  - receives response with no results
//  - callback with no results (kNotFound right away)
TEST_F(ClientTest, NoResults) {
  auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());

  size_t cb_count = 0;
  auto result_cb =
      [&](fit::result<
          Error<>,
          std::reference_wrapper<const std::map<AttributeId, DataElement>>>
              attrs_result) {
        cb_count++;
        EXPECT_EQ(Error(HostError::kNotFound), attrs_result);
        return true;
      };

  const StaticByteBuffer kSearchExpectedParams(
      // ServiceSearchPattern
      0x35,
      0x03,  // Sequence uint8 3 bytes
      0x19,
      0x11,
      0x0B,  // UUID (kAudioSink)
      0xFF,
      0xFF,  // MaxAttributeByteCount (no max)
      // Attribute ID list
      0x35,
      0x06,  // Sequence uint8 6 bytes
      0x09,
      0x00,
      0x01,  // uint16_t (0x0001 = kServiceClassIdList)
      0x09,
      0x00,
      0x04,  // uint16_t (0x0004 = kProtocolDescriptorList)
      0x00   // No continuation state
  );

  uint16_t request_tid;
  bool success = false;

  fake_chan()->SetSendCallback(
      [&request_tid, &success, &kSearchExpectedParams](auto packet) {
        // First byte should be type.
        ASSERT_LE(3u, packet->size());
        ASSERT_EQ(kServiceSearchAttributeRequest, (*packet)[0]);
        ASSERT_EQ(kSearchExpectedParams, packet->view(5));
        request_tid = ((*packet)[1] << 8) != 0 || (*packet)[2];
        success = true;
      },
      dispatcher());

  // Search for all A2DP sinks, get the:
  //  - Service Class ID list
  //  - Descriptor List
  //  - Bluetooth Profile Descriptor List
  client->ServiceSearchAttributes(
      {profile::kAudioSink},
      {kServiceClassIdList, kProtocolDescriptorList},
      result_cb);
  RunUntilIdle();
  EXPECT_TRUE(success);

  // Receive an empty response
  ServiceSearchAttributeResponse rsp;
  auto rsp_ptr = rsp.GetPDU(0xFFFF /* Max attribute bytes */,
                            request_tid,
                            kResponseMaxSize,
                            BufferView());
  fake_chan()->Receive(*rsp_ptr);

  RunUntilIdle();

  EXPECT_EQ(1u, cb_count);
}

// Disconnect early test:
//  - send request
//  - remote end disconnects
//  - result should be called with kLinkDisconnected
TEST_F(ClientTest, Disconnected) {
  auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());

  size_t cb_count = 0;
  auto result_cb =
      [&](fit::result<
          Error<>,
          std::reference_wrapper<const std::map<AttributeId, DataElement>>>
              attrs_result) {
        cb_count++;
        EXPECT_EQ(Error(HostError::kLinkDisconnected), attrs_result);
        return true;
      };

  const StaticByteBuffer kSearchExpectedParams(
      // ServiceSearchPattern
      0x35,
      0x03,  // Sequence uint8 3 bytes
      0x19,
      0x11,
      0x0B,  // UUID (kAudioSink)
      0xFF,
      0xFF,  // MaxAttributeByteCount (no max)
      // Attribute ID list
      0x35,
      0x06,  // Sequence uint8 6 bytes
      0x09,
      0x00,
      0x01,  // uint16_t (0x0001 = kServiceClassIdList)
      0x09,
      0x00,
      0x04,  // uint16_t (0x0004 = kProtocolDescriptorList)
      0x00   // No continuation state
  );

  bool requested = false;

  fake_chan()->SetSendCallback(
      [&](auto packet) {
        // First byte should be type.
        ASSERT_LE(3u, packet->size());
        ASSERT_EQ(kServiceSearchAttributeRequest, (*packet)[0]);
        ASSERT_EQ(kSearchExpectedParams, packet->view(5));
        requested = true;
      },
      dispatcher());

  // Search for all A2DP sinks, get the:
  //  - Service Class ID list
  //  - Descriptor List
  //  - Bluetooth Profile Descriptor List
  client->ServiceSearchAttributes(
      {profile::kAudioSink},
      {kServiceClassIdList, kProtocolDescriptorList},
      result_cb);
  RunUntilIdle();
  EXPECT_TRUE(requested);
  EXPECT_EQ(0u, cb_count);

  // Remote end closes the channel.
  fake_chan()->Close();

  RunUntilIdle();

  EXPECT_EQ(1u, cb_count);
}

// Malformed reply test:
//  - remote end sends wrong packet type in response (dropped)
//  - remote end sends invalid response
//  - callback receives no response with a malformed packet error
TEST_F(ClientTest, InvalidResponse) {
  auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());

  size_t cb_count = 0;
  auto result_cb =
      [&](fit::result<
          Error<>,
          std::reference_wrapper<const std::map<AttributeId, DataElement>>>
              attrs_result) {
        cb_count++;
        EXPECT_EQ(Error(HostError::kPacketMalformed), attrs_result);
        return true;
      };

  const StaticByteBuffer kSearchExpectedParams(
      // ServiceSearchPattern
      0x35,
      0x03,  // Sequence uint8 3 bytes
      0x19,
      0x11,
      0x0B,  // UUID (kAudioSink)
      0xFF,
      0xFF,  // MaxAttributeByteCount (no max)
      // Attribute ID list
      0x35,
      0x06,  // Sequence uint8 6 bytes
      0x09,
      0x00,
      0x01,  // uint16_t (0x0001 = kServiceClassIdList)
      0x09,
      0x00,
      0x04,  // uint16_t (0x0004 = kProtocolDescriptorList)
      0x00   // No continuation state
  );

  uint16_t request_tid;
  bool requested = false;

  fake_chan()->SetSendCallback(
      [&](auto packet) {
        // First byte should be type.
        ASSERT_LE(3u, packet->size());
        ASSERT_EQ(kServiceSearchAttributeRequest, (*packet)[0]);
        ASSERT_EQ(kSearchExpectedParams, packet->view(5));
        request_tid = ((*packet)[1] << 8) != 0 || (*packet)[2];
        requested = true;
      },
      dispatcher());

  // Search for all A2DP sinks, get the:
  //  - Service Class ID list
  //  - Descriptor List
  //  - Bluetooth Profile Descriptor List
  client->ServiceSearchAttributes(
      {profile::kAudioSink},
      {kServiceClassIdList, kProtocolDescriptorList},
      result_cb);
  RunUntilIdle();
  EXPECT_TRUE(requested);
  EXPECT_EQ(0u, cb_count);

  // Remote end sends some unparsable stuff for the packet.
  fake_chan()->Receive(StaticByteBuffer(0x07,
                                        UpperBits(request_tid),
                                        LowerBits(request_tid),
                                        0x00,
                                        0x03,
                                        0x05,
                                        0x06,
                                        0x07));

  RunUntilIdle();

  EXPECT_EQ(1u, cb_count);
}

TEST_F(ClientTest, ShortSduResponse) {
  size_t cb_count = 0;
  auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());
  auto result_cb =
      [&](fit::result<
          Error<>,
          std::reference_wrapper<const std::map<AttributeId, DataElement>>>
              attrs_result) {
        cb_count++;
        EXPECT_EQ(Error(HostError::kCanceled), attrs_result);
        return true;
      };

  uint16_t request_tid = 0;
  bool request_sent = false;

  fake_chan()->SetSendCallback(
      [&](auto packet) {
        ASSERT_GE(packet->size(), 3u);
        request_tid = (static_cast<uint16_t>((*packet)[1]) << 8) | (*packet)[2];
        request_sent = true;
      },
      dispatcher());

  client->ServiceSearchAttributes(
      {profile::kAudioSink},
      {kServiceClassIdList, kProtocolDescriptorList},
      result_cb);

  RunUntilIdle();
  EXPECT_TRUE(request_sent);
  EXPECT_EQ(0u, cb_count);

  // Send a packet that is shorter than the SDP header (5 bytes)
  // to fail the request.
  fake_chan()->Receive(StaticByteBuffer(
      0x07, UpperBits(request_tid), LowerBits(request_tid), 0x00));

  RunUntilIdle();

  // The packet should be dropped
  EXPECT_EQ(0u, cb_count);
}

// Time out (or possibly dropped packets that were malformed)
TEST_F(ClientTest, Timeout) {
  constexpr uint32_t kTimeoutMs = 10000;
  auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());

  size_t cb_count = 0;
  auto result_cb =
      [&](fit::result<
          Error<>,
          std::reference_wrapper<const std::map<AttributeId, DataElement>>>
              attrs_result) {
        cb_count++;
        EXPECT_EQ(Error(HostError::kTimedOut), attrs_result);
        return true;
      };

  const StaticByteBuffer kSearchExpectedParams(
      // ServiceSearchPattern
      0x35,
      0x03,  // Sequence uint8 3 bytes
      0x19,
      0x11,
      0x0B,  // UUID (kAudioSink)
      0xFF,
      0xFF,  // MaxAttributeByteCount (no max)
      // Attribute ID list
      0x35,
      0x06,  // Sequence uint8 6 bytes
      0x09,
      0x00,
      0x01,  // uint16_t (0x0001 = kServiceClassIdList)
      0x09,
      0x00,
      0x04,  // uint16_t (0x0004 = kProtocolDescriptorList)
      0x00   // No continuation state
  );

  bool requested = false;

  fake_chan()->SetSendCallback(
      [&](auto packet) {
        // First byte should be type.
        ASSERT_LE(3u, packet->size());
        ASSERT_EQ(kServiceSearchAttributeRequest, (*packet)[0]);
        ASSERT_EQ(kSearchExpectedParams, packet->view(5));
        requested = true;
      },
      dispatcher());

  // Search for all A2DP sinks, get the:
  //  - Service Class ID list
  //  - Descriptor List
  //  - Bluetooth Profile Descriptor List
  client->ServiceSearchAttributes(
      {profile::kAudioSink},
      {kServiceClassIdList, kProtocolDescriptorList},
      result_cb);
  RunUntilIdle();
  EXPECT_TRUE(requested);
  EXPECT_EQ(0u, cb_count);

  // Wait until the timeout happens
  RunFor(std::chrono::milliseconds(kTimeoutMs + 1));

  EXPECT_EQ(1u, cb_count);
}

TEST_F(ClientTest, DestroyClientInErrorResultCallbackDoesNotCrash) {
  constexpr uint32_t kTimeoutMs = 10000;
  auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());

  size_t cb_count = 0;
  auto result_cb =
      [&](fit::result<
          Error<>,
          std::reference_wrapper<const std::map<AttributeId, DataElement>>>
              attrs_result) {
        cb_count++;
        EXPECT_TRUE(attrs_result.is_error());
        client.reset();
        return true;
      };

  bool requested = false;
  fake_chan()->SetSendCallback([&](auto /*packet*/) { requested = true; },
                               dispatcher());

  client->ServiceSearchAttributes(
      {profile::kAudioSink},
      {kServiceClassIdList, kProtocolDescriptorList},
      result_cb);
  RunUntilIdle();
  EXPECT_TRUE(requested);
  EXPECT_EQ(0u, cb_count);

  // Wait until the timeout happens
  RunFor(std::chrono::milliseconds(kTimeoutMs + 1));

  EXPECT_EQ(1u, cb_count);
}

TEST_F(ClientTest, DestroyClientInDisconnectedResultCallback) {
  auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());

  size_t cb_count = 0;
  auto result_cb =
      [&](fit::result<
          Error<>,
          std::reference_wrapper<const std::map<AttributeId, DataElement>>>
              attrs_result) {
        cb_count++;
        EXPECT_EQ(Error(HostError::kLinkDisconnected), attrs_result);
        client.reset();
        return true;
      };

  bool requested = false;
  fake_chan()->SetSendCallback([&](auto /*packet*/) { requested = true; },
                               dispatcher());

  client->ServiceSearchAttributes(
      {profile::kAudioSink},
      {kServiceClassIdList, kProtocolDescriptorList},
      result_cb);
  RunUntilIdle();
  EXPECT_TRUE(requested);
  EXPECT_EQ(0u, cb_count);

  // Remote end closes the channel.
  fake_chan()->Close();

  RunUntilIdle();

  EXPECT_EQ(1u, cb_count);
}

// Tests that receiving a malformed SDP response cancels the current transaction
// and correctly unblocks subsequent queued searches.
TEST_F(ClientTest, MalformedResponseDoesNotWedgeQueuedSearches) {
  auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());
  ASSERT_TRUE(fake_chan()->activated());

  // Count how many PDUs the client sends on the L2CAP channel, and capture
  // the TID of each so we can echo it back.
  size_t sent_packets = 0;
  uint16_t last_transaction_id = 0;
  fake_chan()->SetSendCallback(
      [&](auto packet) {
        ASSERT_LE(5u, packet->size());
        ASSERT_EQ(kServiceSearchAttributeRequest, (*packet)[0]);
        last_transaction_id =
            static_cast<uint16_t>(((*packet)[1] << 8) | (*packet)[2]);
        sent_packets++;
      },
      dispatcher());

  size_t first_callback_count = 0;
  std::optional<HostError> first_callback_error;
  auto first_callback =
      [&](fit::result<Error<>,
                      std::reference_wrapper<
                          const std::map<AttributeId, DataElement>>> result) {
        first_callback_count++;
        if (result.is_error()) {
          first_callback_error = result.error_value().host_error();
        }
        return true;
      };

  size_t second_callback_count = 0;
  std::optional<HostError> second_callback_error;
  auto second_callback =
      [&](fit::result<Error<>,
                      std::reference_wrapper<
                          const std::map<AttributeId, DataElement>>> result) {
        second_callback_count++;
        if (result.is_error()) {
          second_callback_error = result.error_value().host_error();
        }
        return true;
      };

  // Queue two searches synchronously to mirror ServiceDiscoverer looping over
  // all registered searches in StartServiceDiscovery.
  client->ServiceSearchAttributes(
      {profile::kAudioSink}, {kServiceClassIdList}, std::move(first_callback));
  client->ServiceSearchAttributes({profile::kAudioSource},
                                  {kServiceClassIdList},
                                  std::move(second_callback));
  RunUntilIdle();

  ASSERT_EQ(1u, sent_packets);
  ASSERT_EQ(0u, first_callback_count);
  ASSERT_EQ(0u, second_callback_count);

  // Send a malformed ServiceSearchAttributeResponse (payload < 4 bytes) so that
  // Parse() returns kPacketMalformed and cancels whichever transaction was sent
  // first.
  fake_chan()->Receive(StaticByteBuffer(0x07,
                                        UpperBits(last_transaction_id),
                                        LowerBits(last_transaction_id),
                                        0x00,
                                        0x03,
                                        0x05,
                                        0x06,
                                        0x07));
  RunUntilIdle();

  // Exactly one transaction should have received kPacketMalformed.
  ASSERT_EQ(1u, first_callback_count + second_callback_count);
  if (first_callback_count == 1u) {
    ASSERT_TRUE(first_callback_error.has_value());
    EXPECT_EQ(HostError::kPacketMalformed, *first_callback_error);
  } else {
    ASSERT_TRUE(second_callback_error.has_value());
    EXPECT_EQ(HostError::kPacketMalformed, *second_callback_error);
  }

  // Verify that the second queued transaction is sent after the first is
  // cancelled, and that its timeout timer fires after 10 seconds of silence.
  EXPECT_EQ(2u, sent_packets);

  RunFor(std::chrono::seconds(60));

  EXPECT_EQ(2u, sent_packets);
  EXPECT_EQ(1u, first_callback_count);
  EXPECT_EQ(1u, second_callback_count);

  RunFor(std::chrono::hours(1));
  EXPECT_EQ(1u, first_callback_count);
  EXPECT_EQ(1u, second_callback_count);

  client.reset();
  EXPECT_EQ(1u, first_callback_count);
  EXPECT_EQ(1u, second_callback_count);
}

// Tests that when an SDP transaction times out, subsequent queued searches are
// unblocked and sent.
TEST_F(ClientTest, TimeoutDoesNotWedgeQueuedSearches) {
  auto client = Client::Create(fake_chan()->GetWeakPtr(), dispatcher());
  ASSERT_TRUE(fake_chan()->activated());

  size_t sent_packets = 0;
  fake_chan()->SetSendCallback(
      [&](auto packet) {
        ASSERT_LE(5u, packet->size());
        ASSERT_EQ(kServiceSearchAttributeRequest, (*packet)[0]);
        sent_packets++;
      },
      dispatcher());

  size_t first_callback_count = 0;
  std::optional<HostError> first_callback_error;
  auto first_callback =
      [&](fit::result<Error<>,
                      std::reference_wrapper<
                          const std::map<AttributeId, DataElement>>> result) {
        first_callback_count++;
        if (result.is_error()) {
          first_callback_error = result.error_value().host_error();
        }
        return true;
      };

  size_t second_callback_count = 0;
  std::optional<HostError> second_callback_error;
  auto second_callback =
      [&](fit::result<Error<>,
                      std::reference_wrapper<
                          const std::map<AttributeId, DataElement>>> result) {
        second_callback_count++;
        if (result.is_error()) {
          second_callback_error = result.error_value().host_error();
        }
        return true;
      };

  client->ServiceSearchAttributes(
      {profile::kAudioSink}, {kServiceClassIdList}, std::move(first_callback));
  client->ServiceSearchAttributes({profile::kAudioSource},
                                  {kServiceClassIdList},
                                  std::move(second_callback));
  RunUntilIdle();

  ASSERT_EQ(1u, sent_packets);

  // Advance time past the 10-second transaction timeout to cancel whichever
  // request was sent first.
  RunFor(std::chrono::seconds(11));

  ASSERT_EQ(1u, first_callback_count + second_callback_count);
  if (first_callback_count == 1u) {
    ASSERT_TRUE(first_callback_error.has_value());
    EXPECT_EQ(HostError::kTimedOut, *first_callback_error);
  } else {
    ASSERT_TRUE(second_callback_error.has_value());
    EXPECT_EQ(HostError::kTimedOut, *second_callback_error);
  }

  // Verify that the second queued request is sent after the timeout and
  // eventually times out itself.
  EXPECT_EQ(2u, sent_packets);

  RunFor(std::chrono::hours(1));
  EXPECT_EQ(2u, sent_packets);
  EXPECT_EQ(1u, first_callback_count);
  EXPECT_EQ(1u, second_callback_count);

  client.reset();
  EXPECT_EQ(1u, first_callback_count);
  EXPECT_EQ(1u, second_callback_count);
}

}  // namespace
}  // namespace bt::sdp
