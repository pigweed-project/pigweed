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

#include "pw_bluetooth_sapphire/internal/host/gatt/client.h"

#include "pw_bluetooth_sapphire/internal/host/att/att.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/mock_channel_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"

namespace bt::gatt {
namespace {

constexpr UUID kTestUuid1(uint16_t{0xDEAD});
constexpr UUID kTestUuid2(uint16_t{0xBEEF});
constexpr UUID kTestUuid3(
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});

att::Result<uint16_t> MtuResultFromErrCode(att::ErrorCode ecode) {
  return fit::error(att::Error(ecode));
}

att::Result<uint16_t> MtuResultFromHostErrCode(HostError ecode) {
  return fit::error(att::Error(ecode));
}

// clang-format off
const StaticByteBuffer kDiscoverPrimaryRequest(
    0x10,        // opcode: read by group type request
    0x01, 0x00,  // start handle: 0x0001
    0xFF, 0xFF,  // end handle: 0xFFFF
    0x00, 0x28   // type: primary service (0x2800)
);

const StaticByteBuffer kDiscoverPrimary16ByUUID(
    0x06,        // opcode: find by type value request
    0x01, 0x00,  // start handle: 0x0001
    0xFF, 0xFF,  // end handle: 0xFFFF
    0x00, 0x28,  // type: primary service (0x2800)
    0xAD, 0xDE  // UUID
);

const StaticByteBuffer kDiscoverPrimary128ByUUID(
    0x06,        // opcode: find by type value request
    0x01, 0x00,  // start handle: 0x0001
    0xFF, 0xFF,  // end handle: 0xFFFF
    0x00, 0x28,  // type: primary service (0x2800)
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 // UUID
);
// clang-format on

auto MakeFindInformation(att::Handle range_start = 0x0001,
                         att::Handle range_end = 0xFFFF) {
  return StaticByteBuffer(att::kFindInformationRequest,  // opcode
                          LowerBits(range_start),
                          UpperBits(range_start),  // start handle
                          LowerBits(range_end),
                          UpperBits(range_end)  // end handle
  );
}

void NopSvcCallback(const gatt::ServiceData&) {}
void NopChrcCallback(const gatt::CharacteristicData&) {}
void NopDescCallback(const gatt::DescriptorData&) {}

class ClientTest : public l2cap::testing::MockChannelTest {
 public:
  ClientTest() = default;
  ~ClientTest() override = default;

 protected:
  void SetUp() override {
    ChannelOptions options(l2cap::kATTChannelId);
    auto fake_chan = CreateFakeChannel(options);
    att_ = att::Bearer::Create(fake_chan->GetWeakPtr(), dispatcher());
    client_ = Client::Create(att_->GetWeakPtr());
  }

  void TearDown() override {
    client_ = nullptr;
    att_ = nullptr;
  }

  // |out_status| must remain valid.
  void SendDiscoverDescriptors(att::Result<>* out_status,
                               Client::DescriptorCallback desc_callback,
                               att::Handle range_start = 0x0001,
                               att::Handle range_end = 0xFFFF) {
    client()->DiscoverDescriptors(
        range_start,
        range_end,
        std::move(desc_callback),
        [out_status](att::Result<> val) { *out_status = val; });
  }

  att::Bearer* att() const { return att_.get(); }
  Client* client() const { return client_.get(); }
  void DestroyClient() { client_ = nullptr; }

 private:
  std::unique_ptr<att::Bearer> att_;
  std::unique_ptr<Client> client_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ClientTest);
};

TEST_F(ClientTest, ExchangeMTUMalformedResponse) {
  constexpr uint16_t kPreferredMTU = 100;
  const StaticByteBuffer kExpectedRequest(0x02,  // opcode: exchange MTU
                                          kPreferredMTU,
                                          0x00  // client rx mtu: kPreferredMTU
  );

  std::optional<att::Result<uint16_t>> result;
  auto mtu_cb = [&](att::Result<uint16_t> cb_result) { result = cb_result; };

  att()->set_preferred_mtu(kPreferredMTU);

  EXPECT_PACKET_OUT(kExpectedRequest);
  client()->ExchangeMTU(mtu_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());
  ASSERT_FALSE(fake_chan()->link_error());

  // Respond back with a malformed PDU. This should cause a link error and the
  // MTU request should fail.
  fake_chan()->Receive(StaticByteBuffer(
      0x03,  // opcode: exchange MTU response
      30     // server rx mtu is one octet too short
      ));

  RunUntilIdle();

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(MtuResultFromHostErrCode(HostError::kPacketMalformed), *result);
  EXPECT_TRUE(fake_chan()->link_error());
}

// Tests that the ATT "Request Not Supported" error results in the default MTU.
TEST_F(ClientTest, ExchangeMTUErrorNotSupported) {
  constexpr uint16_t kPreferredMTU = 100;
  constexpr uint16_t kInitialMTU = 50;
  const StaticByteBuffer kExpectedRequest(0x02,  // opcode: exchange MTU
                                          kPreferredMTU,
                                          0x00  // client rx mtu: kPreferredMTU
  );

  std::optional<att::Result<uint16_t>> result;
  auto mtu_cb = [&](att::Result<uint16_t> cb_result) { result = cb_result; };

  // Set the initial MTU to something other than the default LE MTU since we
  // want to confirm that the MTU changes to the default.
  att()->set_mtu(kInitialMTU);
  att()->set_preferred_mtu(kPreferredMTU);

  EXPECT_PACKET_OUT(kExpectedRequest);
  client()->ExchangeMTU(mtu_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  // Respond with "Request Not Supported". This will cause us to switch to the
  // default MTU.
  fake_chan()->Receive(
      StaticByteBuffer(att::kErrorResponse,       // opcode
                       att::kExchangeMTURequest,  // request opcode
                       0x00,
                       0x00,  // handle: 0
                       att::ErrorCode::kRequestNotSupported));

  RunUntilIdle();

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(MtuResultFromErrCode(att::ErrorCode::kRequestNotSupported),
            *result);
  EXPECT_EQ(att::kLEMinMTU, att()->mtu());
}

TEST_F(ClientTest, ExchangeMTUErrorOther) {
  constexpr uint16_t kPreferredMTU = 100;
  const auto kExpectedRequest =
      StaticByteBuffer(0x02,  // opcode: exchange MTU
                       kPreferredMTU,
                       0x00  // client rx mtu: kPreferredMTU
      );

  std::optional<att::Result<uint16_t>> result;
  auto mtu_cb = [&](att::Result<uint16_t> cb_result) { result = cb_result; };

  att()->set_preferred_mtu(kPreferredMTU);
  EXPECT_EQ(att::kLEMinMTU, att()->mtu());

  EXPECT_PACKET_OUT(kExpectedRequest);
  client()->ExchangeMTU(mtu_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  // Respond with an error. The MTU should remain unchanged.
  fake_chan()->Receive(
      StaticByteBuffer(att::kErrorResponse,       // opcode
                       att::kExchangeMTURequest,  // request opcode
                       0x00,
                       0x00,  // handle: 0
                       att::ErrorCode::kUnlikelyError));

  RunUntilIdle();

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(MtuResultFromErrCode(att::ErrorCode::kUnlikelyError), *result);
  EXPECT_EQ(att::kLEMinMTU, att()->mtu());
}

// Tests that the client rx MTU is selected when smaller.
TEST_F(ClientTest, ExchangeMTUSelectLocal) {
  constexpr uint16_t kPreferredMTU = 100;
  constexpr uint16_t kServerRxMTU = kPreferredMTU + 1;

  const auto kExpectedRequest =
      StaticByteBuffer(0x02,  // opcode: exchange MTU
                       kPreferredMTU,
                       0x00  // client rx mtu: kPreferredMTU
      );

  std::optional<att::Result<uint16_t>> result;
  auto mtu_cb = [&](att::Result<uint16_t> cb_result) { result = cb_result; };

  att()->set_preferred_mtu(kPreferredMTU);

  EXPECT_PACKET_OUT(kExpectedRequest);
  client()->ExchangeMTU(mtu_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());
  ASSERT_EQ(att::kLEMinMTU, att()->mtu());

  // Respond with an error. The MTU should remain unchanged.
  fake_chan()->Receive(StaticByteBuffer(0x03,  // opcode: exchange MTU response
                                        kServerRxMTU,
                                        0x00  // server rx mtu
                                        ));

  RunUntilIdle();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(att::Result<uint16_t>(fit::ok(kPreferredMTU)), *result);
  EXPECT_EQ(kPreferredMTU, att()->mtu());
}

// Tests that the server rx MTU is selected when smaller.
TEST_F(ClientTest, ExchangeMTUSelectRemote) {
  constexpr uint16_t kPreferredMTU = 100;
  constexpr uint16_t kServerRxMTU = kPreferredMTU - 1;

  const auto kExpectedRequest =
      StaticByteBuffer(0x02,  // opcode: exchange MTU
                       kPreferredMTU,
                       0x00  // client rx mtu: kPreferredMTU
      );

  std::optional<att::Result<uint16_t>> result;
  auto mtu_cb = [&](att::Result<uint16_t> cb_result) { result = cb_result; };

  att()->set_preferred_mtu(kPreferredMTU);

  EXPECT_PACKET_OUT(kExpectedRequest);
  client()->ExchangeMTU(mtu_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());
  ASSERT_EQ(att::kLEMinMTU, att()->mtu());

  // Respond with an error. The MTU should remain unchanged.
  fake_chan()->Receive(StaticByteBuffer(0x03,  // opcode: exchange MTU response
                                        kServerRxMTU,
                                        0x00  // server rx mtu
                                        ));

  RunUntilIdle();

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(att::Result<uint16_t>(fit::ok(kServerRxMTU)), *result);
  EXPECT_EQ(kServerRxMTU, att()->mtu());
}

// Tests that the default MTU is selected when one of the MTUs is too small.
TEST_F(ClientTest, ExchangeMTUSelectDefault) {
  constexpr uint16_t kPreferredMTU = 100;
  constexpr uint16_t kServerRxMTU = 5;  // Smaller than the LE default MTU

  const auto kExpectedRequest =
      StaticByteBuffer(0x02,  // opcode: exchange MTU
                       kPreferredMTU,
                       0x00  // client rx mtu: kPreferredMTU
      );

  std::optional<att::Result<uint16_t>> result;
  auto mtu_cb = [&](att::Result<uint16_t> cb_result) { result = cb_result; };

  att()->set_preferred_mtu(kPreferredMTU);

  EXPECT_PACKET_OUT(kExpectedRequest);
  client()->ExchangeMTU(mtu_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());
  ASSERT_EQ(att::kLEMinMTU, att()->mtu());

  // Respond with an error. The MTU should remain unchanged.
  fake_chan()->Receive(StaticByteBuffer(0x03,  // opcode: exchange MTU response
                                        kServerRxMTU,
                                        0x00  // server rx mtu
                                        ));

  RunUntilIdle();

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(att::Result<uint16_t>(fit::ok(att::kLEMinMTU)), *result);
  EXPECT_EQ(att::kLEMinMTU, att()->mtu());
}

TEST_F(ClientTest, DiscoverPrimaryResponseTooShort) {
  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kDiscoverPrimaryRequest);
  client()->DiscoverServices(ServiceKind::PRIMARY, NopSvcCallback, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  // Respond back with a malformed payload.
  fake_chan()->Receive(StaticByteBuffer(0x11));

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, DiscoverPrimaryMalformedDataLength) {
  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kDiscoverPrimaryRequest);
  client()->DiscoverServices(ServiceKind::PRIMARY, NopSvcCallback, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  // Respond back with an unexpected data length. This is 6 for services with a
  // 16-bit UUID (start (2) + end (2) + uuid (2)) and 20 for 128-bit
  // (start (2) + end (2) + uuid (16)).
  fake_chan()->Receive(StaticByteBuffer(
      0x11,  // opcode: read by group type response
      7,     // data length: 7 (not 6 or 20)
      0,
      1,
      2,
      3,
      4,
      5,
      6  // one entry of length 7, which will be ignored
      ));

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, DiscoverPrimaryMalformedAttrDataList) {
  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kDiscoverPrimaryRequest);
  client()->DiscoverServices(ServiceKind::PRIMARY, NopSvcCallback, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(StaticByteBuffer(
      0x11,  // opcode: read by group type response
      6,     // data length: 6 (16-bit UUIDs)
      0,
      1,
      2,
      3,
      4,
      5,  // entry 1: correct size
      0,
      1,
      2,
      3,
      4  // entry 2: incorrect size
      ));

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, DiscoverPrimaryResultsOutOfOrder) {
  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kDiscoverPrimaryRequest);
  client()->DiscoverServices(ServiceKind::PRIMARY, NopSvcCallback, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(StaticByteBuffer(
      0x11,  // opcode: read by group type response
      6,     // data length: 6 (16-bit UUIDs)
      0x12,
      0x00,  // svc 0 start: 0x0012
      0x13,
      0x00,  // svc 0 end: 0x0013
      0xEF,
      0xBE,  // svc 0 uuid: 0xBEEF
      0x10,
      0x00,  // svc 1 start: 0x0010
      0x11,
      0x00,  // svc 1 end: 0x0011
      0xAD,
      0xDE  // svc 1 uuid: 0xDEAD
      ));

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

// Tests that we handle an empty attribute data list. In practice, the
// server would send an "Attribute Not Found" error instead but our stack treats
// an empty data list as not an error.
TEST_F(ClientTest, DiscoverPrimaryEmptyDataList) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kDiscoverPrimaryRequest);
  client()->DiscoverServices(ServiceKind::PRIMARY, NopSvcCallback, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(StaticByteBuffer(
      0x11,  // opcode: read by group type response
      6      // data length: 6 (16-bit UUIDs)
             // data list is empty
      ));

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
}

// The first request results in "Attribute Not Found".
TEST_F(ClientTest, DiscoverPrimaryAttributeNotFound) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kDiscoverPrimaryRequest);
  client()->DiscoverServices(ServiceKind::PRIMARY, NopSvcCallback, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(StaticByteBuffer(0x01,  // opcode: error response
                                        0x10,  // request: read by group type
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x0A   // error: Attribute Not Found
                                        ));

  RunUntilIdle();

  // The procedure succeeds with no services.
  EXPECT_EQ(fit::ok(), status);
}

// The first request results in an error.
TEST_F(ClientTest, DiscoverPrimaryError) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kDiscoverPrimaryRequest);
  client()->DiscoverServices(ServiceKind::PRIMARY, NopSvcCallback, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(StaticByteBuffer(0x01,  // opcode: error response
                                        0x10,  // request: read by group type
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x06   // error: Request Not Supported
                                        ));

  RunUntilIdle();

  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), status);
}

TEST_F(ClientTest, DiscoverPrimaryMalformedServiceRange) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kDiscoverPrimaryRequest);
  client()->DiscoverServices(ServiceKind::PRIMARY, NopSvcCallback, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  // Return a service where start > end.
  fake_chan()->Receive(StaticByteBuffer(
      0x11,  // opcode: read by group type response
      0x06,  // data length: 6 (16-bit UUIDs)
      0x02,
      0x00,  // svc 1 start: 0x0002
      0x01,
      0x00  // svc 1 end: 0x0001
      ));

  RunUntilIdle();

  // The procedure should be over since the last service in the payload has
  // end handle 0xFFFF.
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, DiscoverPrimary16BitResultsSingleRequest) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  EXPECT_PACKET_OUT(kDiscoverPrimaryRequest);
  client()->DiscoverServices(ServiceKind::PRIMARY, svc_cb, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(StaticByteBuffer(
      0x11,  // opcode: read by group type response
      0x06,  // data length: 6 (16-bit UUIDs)
      0x01,
      0x00,  // svc 1 start: 0x0001
      0x05,
      0x00,  // svc 1 end: 0x0005
      0xAD,
      0xDE,  // svc 1 uuid: 0xDEAD
      0x06,
      0x00,  // svc 2 start: 0x0006
      0xFF,
      0xFF,  // svc 2 end: 0xFFFF
      0xEF,
      0xBE  // svc 2 uuid: 0xBEEF
      ));

  RunUntilIdle();

  // The procedure should be over since the last service in the payload has
  // end handle 0xFFFF.
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(2u, services.size());
  EXPECT_EQ(0x0001, services[0].range_start);
  EXPECT_EQ(0x0005, services[0].range_end);
  EXPECT_EQ(kTestUuid1, services[0].type);
  EXPECT_EQ(0x0006, services[1].range_start);
  EXPECT_EQ(0xFFFF, services[1].range_end);
  EXPECT_EQ(kTestUuid2, services[1].type);
}

TEST_F(ClientTest, DiscoverPrimary128BitResultSingleRequest) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  EXPECT_PACKET_OUT(kDiscoverPrimaryRequest);
  client()->DiscoverServices(ServiceKind::PRIMARY, svc_cb, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(
      StaticByteBuffer(0x11,  // opcode: read by group type response
                       0x14,  // data length: 20 (128-bit UUIDs)
                       0x01,
                       0x00,  // svc 1 start: 0x0008
                       0xFF,
                       0xFF,  // svc 1 end: 0xFFFF

                       // UUID matches |kTestUuid3| declared above.
                       0,
                       1,
                       2,
                       3,
                       4,
                       5,
                       6,
                       7,
                       8,
                       9,
                       10,
                       11,
                       12,
                       13,
                       14,
                       15));

  RunUntilIdle();

  // The procedure should be over since the last service in the payload has
  // end handle 0xFFFF.
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1u, services.size());
  EXPECT_EQ(0x0001, services[0].range_start);
  EXPECT_EQ(0xFFFF, services[0].range_end);
  EXPECT_EQ(kTestUuid3, services[0].type);
}

TEST_F(ClientTest, DiscoverAllPrimaryMultipleRequests) {
  const auto kExpectedRequest0 =
      StaticByteBuffer(0x10,  // opcode: read by group type request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x00,
                       0x28  // type: primary service (0x2800)
      );
  const StaticByteBuffer kResponse0(
      0x11,  // opcode: read by group type response
      0x06,  // data length: 6 (16-bit UUIDs)
      0x01,
      0x00,  // svc 1 start: 0x0001
      0x05,
      0x00,  // svc 1 end: 0x0005
      0xAD,
      0xDE,  // svc 1 uuid: 0xDEAD
      0x06,
      0x00,  // svc 2 start: 0x0006
      0x07,
      0x00,  // svc 2 end: 0x0007
      0xEF,
      0xBE  // svc 2 uuid: 0xBEEF
  );
  const auto kExpectedRequest1 =
      StaticByteBuffer(0x10,  // opcode: read by group type request
                       0x08,
                       0x00,  // start handle: 0x0008
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x00,
                       0x28  // type: primary service (0x2800)
      );
  // Respond with one 128-bit service UUID.
  const StaticByteBuffer kResponse1(
      0x11,  // opcode: read by group type response
      0x14,  // data length: 20 (128-bit UUIDs)
      0x08,
      0x00,  // svc 1 start: 0x0008
      0x09,
      0x00,  // svc 1 end: 0x0009

      // UUID matches |kTestUuid3| declared above.
      0,
      1,
      2,
      3,
      4,
      5,
      6,
      7,
      8,
      9,
      10,
      11,
      12,
      13,
      14,
      15);
  const auto kExpectedRequest2 =
      StaticByteBuffer(0x10,  // opcode: read by group type request
                       0x0A,
                       0x00,  // start handle: 0x000A
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x00,
                       0x28  // type: primary service (0x2800)
      );
  // Terminate the procedure with an error response.
  const StaticByteBuffer kResponse2(0x01,  // opcode: error response
                                    0x10,  // request: read by group type
                                    0x0A,
                                    0x00,  // handle: 0x000A
                                    0x0A   // error: Attribute Not Found
  );

  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest0, &kResponse0);
  EXPECT_PACKET_OUT(kExpectedRequest1, &kResponse1);
  EXPECT_PACKET_OUT(kExpectedRequest2, &kResponse2);
  client()->DiscoverServices(ServiceKind::PRIMARY, svc_cb, res_cb);
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  // The procedure should be over since the last service in the payload has
  // end handle 0xFFFF.
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(3u, services.size());

  EXPECT_EQ(0x0001, services[0].range_start);
  EXPECT_EQ(0x0005, services[0].range_end);
  EXPECT_EQ(kTestUuid1, services[0].type);

  EXPECT_EQ(0x0006, services[1].range_start);
  EXPECT_EQ(0x0007, services[1].range_end);
  EXPECT_EQ(kTestUuid2, services[1].type);

  EXPECT_EQ(0x0008, services[2].range_start);
  EXPECT_EQ(0x0009, services[2].range_end);
  EXPECT_EQ(kTestUuid3, services[2].type);
}

TEST_F(ClientTest, DiscoverServicesInRangeMultipleRequests) {
  const att::Handle kRangeStart = 0x0010;
  const att::Handle kRangeEnd = 0x0020;

  const StaticByteBuffer kExpectedRequest0(
      0x10,  // opcode: read by group type request
      LowerBits(kRangeStart),
      UpperBits(kRangeStart),  // start handle
      LowerBits(kRangeEnd),
      UpperBits(kRangeEnd),  // end handle
      0x00,
      0x28  // type: primary service (0x2800)
  );

  const StaticByteBuffer kResponse0(
      0x11,  // opcode: read by group type response
      0x06,  // data length: 6 (16-bit UUIDs)
      0x10,
      0x00,  // svc 0 start: 0x0010
      0x11,
      0x00,  // svc 0 end: 0x0011
      0xAD,
      0xDE,  // svc 0 uuid: 0xDEAD
      0x12,
      0x00,  // svc 1 start: 0x0012
      0x13,
      0x00,  // svc 1 end: 0x0013
      0xEF,
      0xBE  // svc 1 uuid: 0xBEEF
  );
  const auto kExpectedRequest1 =
      StaticByteBuffer(0x10,  // opcode: read by group type request
                       0x14,
                       0x00,  // start handle: 0x0014
                       LowerBits(kRangeEnd),
                       UpperBits(kRangeEnd),  // end handle
                       0x00,
                       0x28  // type: primary service (0x2800)
      );
  // Respond with one 128-bit service UUID.
  const auto kResponse1 =
      StaticByteBuffer(0x11,  // opcode: read by group type response
                       0x14,  // data length: 20 (128-bit UUIDs)
                       0x14,
                       0x00,  // svc 2 start: 0x0014
                       0x15,
                       0x00,  // svc 2 end: 0x0015

                       // UUID matches |kTestUuid3| declared above.
                       0,
                       1,
                       2,
                       3,
                       4,
                       5,
                       6,
                       7,
                       8,
                       9,
                       10,
                       11,
                       12,
                       13,
                       14,
                       15);
  const auto kExpectedRequest2 =
      StaticByteBuffer(0x10,  // opcode: read by group type request
                       0x16,
                       0x00,  // start handle: 0x0016
                       LowerBits(kRangeEnd),
                       UpperBits(kRangeEnd),  // end handle
                       0x00,
                       0x28  // type: primary service (0x2800)
      );
  // Terminate the procedure with an error response.
  const auto kNotFoundResponse2 =
      StaticByteBuffer(0x01,  // opcode: error response
                       0x10,  // request: read by group type
                       0x16,
                       0x00,  // start handle: 0x0016
                       0x0A   // error: Attribute Not Found
      );

  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest0, &kResponse0);
  EXPECT_PACKET_OUT(kExpectedRequest1, &kResponse1);
  EXPECT_PACKET_OUT(kExpectedRequest2, &kNotFoundResponse2);
  client()->DiscoverServicesInRange(
      ServiceKind::PRIMARY, kRangeStart, kRangeEnd, svc_cb, res_cb);
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(3u, services.size());

  EXPECT_EQ(0x0010, services[0].range_start);
  EXPECT_EQ(0x0011, services[0].range_end);
  EXPECT_EQ(kTestUuid1, services[0].type);

  EXPECT_EQ(0x0012, services[1].range_start);
  EXPECT_EQ(0x0013, services[1].range_end);
  EXPECT_EQ(kTestUuid2, services[1].type);

  EXPECT_EQ(0x0014, services[2].range_start);
  EXPECT_EQ(0x0015, services[2].range_end);
  EXPECT_EQ(kTestUuid3, services[2].type);
}

TEST_F(ClientTest, DiscoverServicesInRangeFailsIfServiceResultIsOutOfRange) {
  const att::Handle kRangeStart = 0x0010;
  const att::Handle kRangeEnd = 0x0020;
  const att::Handle kServiceStart = 0x0001;
  const att::Handle kServiceEnd = 0x0011;

  const auto kExpectedRequest =
      StaticByteBuffer(0x10,  // opcode: read by group type request
                       LowerBits(kRangeStart),
                       UpperBits(kRangeStart),  // start handle
                       LowerBits(kRangeEnd),
                       UpperBits(kRangeEnd),  // end handle
                       0x00,
                       0x28  // type: primary service (0x2800)
      );

  const auto kResponse =
      StaticByteBuffer(0x11,  // opcode: read by group type response
                       0x06,  // data length: 6 (16-bit UUIDs)
                       LowerBits(kServiceStart),
                       UpperBits(kServiceStart),  // svc start
                       LowerBits(kServiceEnd),
                       UpperBits(kServiceEnd),  // svc end
                       0xAD,
                       0xDE  // svc uuid: 0xDEAD
      );

  std::optional<att::Result<>> status;
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest);
  client()->DiscoverServicesInRange(
      ServiceKind::PRIMARY, kRangeStart, kRangeEnd, svc_cb, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), *status);
  EXPECT_EQ(0u, services.size());
}

TEST_F(ClientTest, DiscoverServicesSucceedsWithServiceEndPastRangeEnd) {
  const att::Handle kRangeStart = 0x0010;
  const att::Handle kRangeEnd = 0x0020;
  const att::Handle kServiceStart = 0x00012;
  const att::Handle kServiceEnd = 0x0022;

  const auto kExpectedRequest =
      StaticByteBuffer(0x10,  // opcode: read by group type request
                       LowerBits(kRangeStart),
                       UpperBits(kRangeStart),  // start handle
                       LowerBits(kRangeEnd),
                       UpperBits(kRangeEnd),  // end handle
                       0x00,
                       0x28  // type: primary service (0x2800)
      );

  const auto kResponse =
      StaticByteBuffer(0x11,  // opcode: read by group type response
                       0x06,  // data length: 6 (16-bit UUIDs)
                       LowerBits(kServiceStart),
                       UpperBits(kServiceStart),  // svc start
                       LowerBits(kServiceEnd),
                       UpperBits(kServiceEnd),  // svc end
                       0xAD,
                       0xDE  // svc uuid: 0xDEAD
      );

  std::optional<att::Result<>> status;
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest);
  client()->DiscoverServicesInRange(
      ServiceKind::PRIMARY, kRangeStart, kRangeEnd, svc_cb, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());

  fake_chan()->Receive(kResponse);
  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  ASSERT_EQ(fit::ok(), status);
  EXPECT_EQ(1u, services.size());
}

TEST_F(ClientTest, DiscoverPrimaryWithUuidsByResponseTooShort) {
  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  // Respond back with a malformed payload.
  const StaticByteBuffer kResponse(0x7, 0x0);
  EXPECT_PACKET_OUT(kDiscoverPrimary16ByUUID, &kResponse);
  client()->DiscoverServicesWithUuids(
      ServiceKind::PRIMARY, NopSvcCallback, res_cb, {kTestUuid1});
  EXPECT_TRUE(AllExpectedPacketsSent());
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

// Tests that we handle an empty handle information list properly. In practice,
// the server would send an "Attribute Not Found" error instead.  A handle list
// that is empty is an error.
TEST_F(ClientTest, DiscoverPrimaryWithUuidsEmptyDataList) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  const StaticByteBuffer kResponse(0x07  // opcode: find by value type response
                                         // data list is empty
  );
  EXPECT_PACKET_OUT(kDiscoverPrimary16ByUUID, &kResponse);
  client()->DiscoverServicesWithUuids(
      ServiceKind::PRIMARY, NopSvcCallback, res_cb, {kTestUuid1});
  EXPECT_TRUE(AllExpectedPacketsSent());
  RunUntilIdle();
  EXPECT_TRUE(status.is_error());
}

// The first request results in "Attribute Not Found".
TEST_F(ClientTest, DiscoverPrimaryWithUuidsAttributeNotFound) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  const StaticByteBuffer kResponse(0x01,  // opcode: error response
                                   0x06,  // request: find by type value
                                   0x01,
                                   0x00,  // handle: 0x0001
                                   0x0A   // error: Attribute Not Found
  );

  EXPECT_PACKET_OUT(kDiscoverPrimary16ByUUID, &kResponse);
  client()->DiscoverServicesWithUuids(
      ServiceKind::PRIMARY, NopSvcCallback, res_cb, {kTestUuid1});
  EXPECT_TRUE(AllExpectedPacketsSent());
  RunUntilIdle();
  // The procedure succeeds with no services.
  EXPECT_EQ(fit::ok(), status);
}

// The first request results in an error.
TEST_F(ClientTest, DiscoverPrimaryWithUuidsError) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  const StaticByteBuffer kResponse(0x01,  // opcode: error response
                                   0x06,  // request: find by type value
                                   0x01,
                                   0x00,  // handle: 0x0001
                                   0x06   // error: Request Not Supported
  );

  EXPECT_PACKET_OUT(kDiscoverPrimary16ByUUID, &kResponse);
  client()->DiscoverServicesWithUuids(
      ServiceKind::PRIMARY, NopSvcCallback, res_cb, {kTestUuid1});
  EXPECT_TRUE(AllExpectedPacketsSent());
  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), status);
}

TEST_F(ClientTest, DiscoverPrimaryWithUuidsMalformedServiceRange) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  // Return a service where start > end.
  const StaticByteBuffer kResponse(0x07,  // opcode: find by type value response
                                   0x02,
                                   0x00,  // svc 1 start: 0x0002
                                   0x01,
                                   0x00  // svc 1 end: 0x0001
  );
  EXPECT_PACKET_OUT(kDiscoverPrimary16ByUUID, &kResponse);
  client()->DiscoverServicesWithUuids(
      ServiceKind::PRIMARY, NopSvcCallback, res_cb, {kTestUuid1});
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  // The procedure should be over since the last service in the payload has
  // end handle 0xFFFF.
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, DiscoverPrimaryWithUuidsServicesOutOfOrder) {
  std::optional<att::Result<>> status;
  auto res_cb = [&status](att::Result<> val) { status = val; };

  // Return services out of order.
  const StaticByteBuffer kResponse(0x07,  // opcode: find by type value response
                                   0x05,
                                   0x00,  // svc 0 start: 0x0005
                                   0x06,
                                   0x00,  // svc 0 end: 0x0006
                                   0x01,
                                   0x00,  // svc 1 start: 0x0001
                                   0x02,
                                   0x00  // svc 1 end: 0x0002
  );
  EXPECT_PACKET_OUT(kDiscoverPrimary16ByUUID, &kResponse);
  client()->DiscoverServicesWithUuids(
      ServiceKind::PRIMARY, NopSvcCallback, res_cb, {kTestUuid1});
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), *status);
}

TEST_F(ClientTest, DiscoverPrimaryWithUuids16BitResultsSingleRequest) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  const StaticByteBuffer kResponse(0x07,  // opcode: find by type value response
                                   0x01,
                                   0x00,  // svc 1 start: 0x0001
                                   0x05,
                                   0x00,  // svc 1 end: 0x0005
                                   0x06,
                                   0x00,  // svc 2 start: 0x0006
                                   0xFF,
                                   0xFF  // svc 2 end: 0xFFFF
  );

  EXPECT_PACKET_OUT(kDiscoverPrimary16ByUUID, &kResponse);
  client()->DiscoverServicesWithUuids(
      ServiceKind::PRIMARY, svc_cb, res_cb, {kTestUuid1});
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  // The procedure should be over since the last service in the payload has
  // end handle 0xFFFF.
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(2u, services.size());
  EXPECT_EQ(0x0001, services[0].range_start);
  EXPECT_EQ(0x0005, services[0].range_end);
  EXPECT_EQ(kTestUuid1, services[0].type);
  EXPECT_EQ(0x0006, services[1].range_start);
  EXPECT_EQ(0xFFFF, services[1].range_end);
  EXPECT_EQ(kTestUuid1, services[1].type);
}

TEST_F(ClientTest, DiscoverPrimaryWithUuids128BitResultSingleRequest) {
  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  const StaticByteBuffer kResponse(0x07,  // opcode: find by type value response
                                   0x01,
                                   0x00,  // svc 1 start: 0x0008
                                   0xFF,
                                   0xFF  // svc 1 end: 0xFFFF
  );
  EXPECT_PACKET_OUT(kDiscoverPrimary128ByUUID, &kResponse);
  client()->DiscoverServicesWithUuids(
      ServiceKind::PRIMARY, svc_cb, res_cb, {kTestUuid3});
  EXPECT_TRUE(AllExpectedPacketsSent());
  RunUntilIdle();

  // The procedure should be over since the last service in the payload has
  // end handle 0xFFFF.
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1u, services.size());
  EXPECT_EQ(0x0001, services[0].range_start);
  EXPECT_EQ(0xFFFF, services[0].range_end);
  EXPECT_EQ(kTestUuid3, services[0].type);
}

TEST_F(ClientTest, DiscoverAllPrimaryWithUuidsMultipleRequests) {
  const auto kExpectedRequest0 =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xAD,
                       0xDE  // svc 1 uuid: 0xDEAD
      );
  const auto kResponse0 =
      StaticByteBuffer(0x07,  // opcode: find by type value response
                       0x01,
                       0x00,  // svc 1 start: 0x0001
                       0x05,
                       0x00,  // svc 1 end: 0x0005
                       0x06,
                       0x00,  // svc 2 start: 0x0006
                       0x07,
                       0x00  // svc 2 end: 0x0007
      );
  const auto kExpectedRequest1 =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       0x08,
                       0x00,  // start handle: 0x0008
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xAD,
                       0xDE  // svc 1 uuid: 0xDEAD
      );
  // Respond with one 128-bit service UUID.
  const auto kResponse1 =
      StaticByteBuffer(0x07,  // opcode: find by type value response
                       0x08,
                       0x00,  // svc 1 start: 0x0008
                       0x09,
                       0x00  // svc 1 end: 0x0009
      );
  const auto kExpectedRequest2 =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       0x0A,
                       0x00,  // start handle: 0x000A
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xAD,
                       0xDE  // svc 1 uuid: 0xDEAD
      );
  // Terminate the procedure with an error response.
  const StaticByteBuffer kResponse2(0x01,  // opcode: error response
                                    0x06,  // request: find by type value
                                    0x0A,
                                    0x00,  // handle: 0x000A
                                    0x0A   // error: Attribute Not Found
  );

  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest0, &kResponse0);
  EXPECT_PACKET_OUT(kExpectedRequest1, &kResponse1);
  EXPECT_PACKET_OUT(kExpectedRequest2, &kResponse2);
  client()->DiscoverServicesWithUuids(
      ServiceKind::PRIMARY, svc_cb, res_cb, {kTestUuid1});
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  // The procedure should be over since the last service in the payload has end
  // handle 0xFFFF.
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(3u, services.size());

  EXPECT_EQ(0x0001, services[0].range_start);
  EXPECT_EQ(0x0005, services[0].range_end);
  EXPECT_EQ(kTestUuid1, services[0].type);

  EXPECT_EQ(0x0006, services[1].range_start);
  EXPECT_EQ(0x0007, services[1].range_end);
  EXPECT_EQ(kTestUuid1, services[1].type);

  EXPECT_EQ(0x0008, services[2].range_start);
  EXPECT_EQ(0x0009, services[2].range_end);
  EXPECT_EQ(kTestUuid1, services[2].type);
}

TEST_F(ClientTest, DiscoverPrimaryWithUuidsMultipleUuids) {
  const auto kExpectedRequest0 =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xAD,
                       0xDE  // kTestUuid1
      );
  const auto kResponse0 =
      StaticByteBuffer(0x07,  // opcode: find by type value response
                       0x01,
                       0x00,  // svc 1 start: 0x0001
                       0x05,
                       0x00  // svc 1 end: 0x0005
      );
  const auto kExpectedRequest1 =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       0x06,
                       0x00,  // start handle: 0x0006
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xAD,
                       0xDE  // kTestUuid1
      );
  const auto kNotFoundResponse1 =
      StaticByteBuffer(0x01,  // opcode: error response
                       0x06,  // request: find by type value
                       0x06,
                       0x00,  // handle: 0x0006
                       0x0A   // error: Attribute Not Found
      );
  const auto kExpectedRequest2 =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xEF,
                       0xBE  // kTestUuid2
      );
  const auto kResponse2 =
      StaticByteBuffer(0x07,  // opcode: find by type value response
                       0x06,
                       0x00,  // svc 1 start: 0x0006
                       0x09,
                       0x00  // svc 1 end: 0x0009
      );
  const auto kExpectedRequest3 =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       0x0A,
                       0x00,  // start handle: 0x000A
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xEF,
                       0xBE  // kTestUuid2
      );
  const auto kNotFoundResponse3 =
      StaticByteBuffer(0x01,  // opcode: error response
                       0x06,  // request: find by type value
                       0x0A,
                       0x00,  // handle: 0x000A
                       0x0A   // error: Attribute Not Found
      );

  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest0, &kResponse0);
  EXPECT_PACKET_OUT(kExpectedRequest1, &kNotFoundResponse1);
  EXPECT_PACKET_OUT(kExpectedRequest2, &kResponse2);
  EXPECT_PACKET_OUT(kExpectedRequest3, &kNotFoundResponse3);
  client()->DiscoverServicesWithUuids(
      ServiceKind::PRIMARY, svc_cb, res_cb, {kTestUuid2, kTestUuid1});
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(2u, services.size());

  EXPECT_EQ(0x0001, services[0].range_start);
  EXPECT_EQ(0x0005, services[0].range_end);
  EXPECT_EQ(kTestUuid1, services[0].type);

  EXPECT_EQ(0x0006, services[1].range_start);
  EXPECT_EQ(0x0009, services[1].range_end);
  EXPECT_EQ(kTestUuid2, services[1].type);
}

TEST_F(ClientTest, DiscoverServicesWithUuidsInRangeMultipleUuids) {
  const att::Handle kRangeStart = 0x0002;
  const att::Handle kRangeEnd = 0x0020;
  const auto kExpectedRequest0 =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       LowerBits(kRangeStart),
                       UpperBits(kRangeStart),  // start handle
                       LowerBits(kRangeEnd),
                       UpperBits(kRangeEnd),  // end handle
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xAD,
                       0xDE  // kTestUuid1
      );
  const auto kResponse0 =
      StaticByteBuffer(0x07,  // opcode: find by type value response
                       0x02,
                       0x00,  // svc 0 start: 0x0002
                       0x05,
                       0x00  // svc 0 end: 0x0005
      );
  const auto kExpectedRequest1 =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       0x06,
                       0x00,  // start handle: 0x0006
                       LowerBits(kRangeEnd),
                       UpperBits(kRangeEnd),  // end handle
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xAD,
                       0xDE  // kTestUuid1
      );
  const auto kNotFoundResponse1 =
      StaticByteBuffer(0x01,  // opcode: error response
                       0x06,  // request: find by type value
                       0x06,
                       0x00,  // handle: 0x0006
                       0x0A   // error: Attribute Not Found
      );
  const auto kExpectedRequest2 =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       LowerBits(kRangeStart),
                       UpperBits(kRangeStart),  // start handle
                       LowerBits(kRangeEnd),
                       UpperBits(kRangeEnd),  // end handle
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xEF,
                       0xBE  // kTestUuid2
      );
  const auto kResponse2 =
      StaticByteBuffer(0x07,  // opcode: find by type value response
                       0x06,
                       0x00,  // svc 1 start: 0x0006
                       0x09,
                       0x00  // svc 1 end: 0x0009
      );
  const auto kExpectedRequest3 =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       0x0A,
                       0x00,  // start handle: 0x000A
                       LowerBits(kRangeEnd),
                       UpperBits(kRangeEnd),  // end handle
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xEF,
                       0xBE  // kTestUuid2
      );
  const auto kNotFoundResponse3 =
      StaticByteBuffer(0x01,  // opcode: error response
                       0x06,  // request: find by type value
                       0x0A,
                       0x00,  // handle: 0x000A
                       0x0A   // error: Attribute Not Found
      );

  att::Result<> status = ToResult(HostError::kFailed);
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest0, &kResponse0);
  EXPECT_PACKET_OUT(kExpectedRequest1, &kNotFoundResponse1);
  EXPECT_PACKET_OUT(kExpectedRequest2, &kResponse2);
  EXPECT_PACKET_OUT(kExpectedRequest3, &kNotFoundResponse3);
  client()->DiscoverServicesWithUuidsInRange(ServiceKind::PRIMARY,
                                             kRangeStart,
                                             kRangeEnd,
                                             svc_cb,
                                             res_cb,
                                             {kTestUuid2, kTestUuid1});
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(2u, services.size());

  EXPECT_EQ(0x0002, services[0].range_start);
  EXPECT_EQ(0x0005, services[0].range_end);
  EXPECT_EQ(kTestUuid1, services[0].type);

  EXPECT_EQ(0x0006, services[1].range_start);
  EXPECT_EQ(0x0009, services[1].range_end);
  EXPECT_EQ(kTestUuid2, services[1].type);
}

TEST_F(ClientTest,
       DiscoverServicesWithUuidsInRangeFailsOnResultNotInRequestedRange) {
  const att::Handle kRangeStart = 0x0010;
  const att::Handle kRangeEnd = 0x0020;
  const att::Handle kServiceStart = 0x0002;
  const att::Handle kServiceEnd = 0x0011;

  const auto kExpectedRequest =
      StaticByteBuffer(0x06,  // opcode: find by type value request
                       LowerBits(kRangeStart),
                       UpperBits(kRangeStart),  // start handle
                       LowerBits(kRangeEnd),
                       UpperBits(kRangeEnd),  // end handle
                       0x00,
                       0x28,  // type: primary service (0x2800)
                       0xAD,
                       0xDE  // kTestUuid1
      );
  const auto kResponse =
      StaticByteBuffer(0x07,  // opcode: find by type value response
                       LowerBits(kServiceStart),
                       UpperBits(kServiceStart),  // svc start
                       LowerBits(kServiceEnd),
                       UpperBits(kServiceEnd)  // svc end
      );

  std::optional<att::Result<>> status;
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<ServiceData> services;
  auto svc_cb = [&services](const ServiceData& svc) {
    services.push_back(svc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverServicesWithUuidsInRange(ServiceKind::PRIMARY,
                                             kRangeStart,
                                             kRangeEnd,
                                             svc_cb,
                                             res_cb,
                                             {kTestUuid1});
  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), *status);
  EXPECT_EQ(0u, services.size());
}

TEST_F(ClientTest, CharacteristicDiscoveryHandlesEqual) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0x0001;

  att::Result<> status = ToResult(HostError::kFailed);  // Initialize as error
  auto res_cb = [&status](att::Result<> val) { status = val; };

  // Should succeed immediately.
  client()->DiscoverCharacteristics(kStart, kEnd, NopChrcCallback, res_cb);
  EXPECT_EQ(fit::ok(), status);
}

TEST_F(ClientTest, CharacteristicDiscoveryResponseTooShort) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0xFFFF;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  const StaticByteBuffer kMalformedResponse(0x09);

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kExpectedRequest, &kMalformedResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, NopChrcCallback, res_cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, CharacteristicDiscoveryMalformedDataLength) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0xFFFF;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  // Respond back with an unexpected data length. This is 7 for characteristics
  // with a 16-bit UUID (handle (2) + props (1) + value handle (2) + uuid (2))
  // and 21 for 128-bit (handle (2) + props (1) + value handle (2) + uuid (16)).
  const StaticByteBuffer kResponse(
      0x09,  // opcode: read by type response
      8,     // data length: 8 (not 7 or 21)
      0,
      1,
      2,
      3,
      4,
      5,
      6,
      7  // one entry of length 8, which will be ignored
  );

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, NopChrcCallback, res_cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, CharacteristicDiscoveryMalformedAttrDataList) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0xFFFF;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );

  // Respond back with an unexpected data length. This is 7 for characteristics
  // with a 16-bit UUID (handle (2) + props (1) + value handle (2) + uuid (2))
  // and 21 for 128-bit (handle (2) + props (1) + value handle (2) + uuid (16)).
  const StaticByteBuffer kResponse(0x09,  // opcode: read by type response
                                   7,     // data length: 7 (16-bit UUIDs)
                                   0,
                                   1,
                                   2,
                                   3,
                                   4,
                                   5,
                                   6,  // entry 1: correct size
                                   0,
                                   1,
                                   2,
                                   3,
                                   4,
                                   5  // entry 2: incorrect size
  );

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, NopChrcCallback, res_cb);
  EXPECT_TRUE(AllExpectedPacketsSent());
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, CharacteristicDiscoveryEmptyDataList) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0xFFFF;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  const StaticByteBuffer kResponse(0x09,  // opcode: read by type response
                                   7      // data length: 7 (16-bit UUIDs)
                                          // data list empty
  );

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, NopChrcCallback, res_cb);
  RunUntilIdle();
  EXPECT_EQ(status, ToResult(HostError::kPacketMalformed));
}

TEST_F(ClientTest, CharacteristicDiscoveryAttributeNotFound) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0xFFFF;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  const StaticByteBuffer kResponse(0x01,  // opcode: error response
                                   0x08,  // request: read by type
                                   0x01,
                                   0x00,  // handle: 0x0001
                                   0x0A   // error: Attribute Not Found
  );

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, NopChrcCallback, res_cb);
  RunUntilIdle();
  // Attribute Not Found error means the procedure is over.
  EXPECT_EQ(fit::ok(), status);
}

TEST_F(ClientTest, CharacteristicDiscoveryError) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0xFFFF;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  const StaticByteBuffer kResponse(0x01,  // opcode: error response
                                   0x08,  // request: read by type
                                   0x01,
                                   0x00,  // handle: 0x0001
                                   0x06   // error: Request Not Supported
  );

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, NopChrcCallback, res_cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), status);
}

TEST_F(ClientTest, CharacteristicDiscovery16BitResultsSingleRequest) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0x0005;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0x05,
                       0x00,  // end handle: 0x0005
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  const StaticByteBuffer kResponse(
      0x09,  // opcode: read by type response
      0x07,  // data length: 7 (16-bit UUIDs)
      0x03,
      0x00,  // chrc 1 handle
      0x00,  // chrc 1 properties
      0x04,
      0x00,  // chrc 1 value handle
      0xAD,
      0xDE,  // chrc 1 uuid: 0xDEAD
      0x05,
      0x00,  // chrc 2 handle (0x0005 is the end of the requested range)
      0x01,  // chrc 2 properties
      0x06,
      0x00,  // chrc 2 value handle
      0xEF,
      0xBE  // chrc 2 uuid: 0xBEEF
  );

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<CharacteristicData> chrcs;
  auto chrc_cb = [&chrcs](const CharacteristicData& chrc) {
    chrcs.push_back(chrc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, chrc_cb, res_cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  ASSERT_EQ(2u, chrcs.size());
  EXPECT_EQ(0x0003, chrcs[0].handle);
  EXPECT_EQ(0, chrcs[0].properties);
  EXPECT_EQ(0x0004, chrcs[0].value_handle);
  EXPECT_EQ(kTestUuid1, chrcs[0].type);
  EXPECT_EQ(0x0005, chrcs[1].handle);
  EXPECT_EQ(1, chrcs[1].properties);
  EXPECT_EQ(0x0006, chrcs[1].value_handle);
  EXPECT_EQ(kTestUuid2, chrcs[1].type);
}

TEST_F(ClientTest, CharacteristicDiscovery128BitResultsSingleRequest) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0x0005;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0x05,
                       0x00,  // end handle: 0x0005
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  const StaticByteBuffer kResponse(0x09,  // opcode: read by type response
                                   0x15,  // data length: 21 (128-bit UUIDs)
                                   0x05,
                                   0x00,  // chrc handle
                                   0x00,  // chrc properties
                                   0x06,
                                   0x00,  // chrc value handle

                                   // UUID matches |kTestUuid3| declared above.
                                   0,
                                   1,
                                   2,
                                   3,
                                   4,
                                   5,
                                   6,
                                   7,
                                   8,
                                   9,
                                   10,
                                   11,
                                   12,
                                   13,
                                   14,
                                   15);

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<CharacteristicData> chrcs;
  auto chrc_cb = [&chrcs](const CharacteristicData& chrc) {
    chrcs.push_back(chrc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, chrc_cb, res_cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(1u, chrcs.size());
  EXPECT_EQ(0x0005, chrcs[0].handle);
  EXPECT_EQ(0, chrcs[0].properties);
  EXPECT_EQ(0x0006, chrcs[0].value_handle);
  EXPECT_EQ(kTestUuid3, chrcs[0].type);
}

TEST_F(ClientTest, CharacteristicDiscoveryMultipleRequests) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0xFFFF;

  const auto kExpectedRequest0 =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x01,
                       0x00,  // start handle: 0x0001
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  const auto kResponse0 =
      StaticByteBuffer(0x09,  // opcode: read by type response
                       0x07,  // data length: 7 (16-bit UUIDs)
                       0x03,
                       0x00,  // chrc 1 handle
                       0x00,  // chrc 1 properties
                       0x04,
                       0x00,  // chrc 1 value handle
                       0xAD,
                       0xDE,  // chrc 1 uuid: 0xDEAD
                       0x05,
                       0x00,  // chrc 2 handle
                       0x01,  // chrc 2 properties
                       0x06,
                       0x00,  // chrc 2 value handle
                       0xEF,
                       0xBE  // chrc 2 uuid: 0xBEEF
      );
  const auto kExpectedRequest1 =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x06,
                       0x00,  // start handle: 0x0006
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  // Respond with one characteristic with a 128-bit UUID
  const auto kResponse1 =
      StaticByteBuffer(0x09,  // opcode: read by type response
                       0x15,  // data length: 21 (128-bit UUIDs)
                       0x07,
                       0x00,  // chrc handle
                       0x00,  // chrc properties
                       0x08,
                       0x00,  // chrc value handle
                       // UUID matches |kTestUuid3| declared above.
                       0,
                       1,
                       2,
                       3,
                       4,
                       5,
                       6,
                       7,
                       8,
                       9,
                       10,
                       11,
                       12,
                       13,
                       14,
                       15);
  const auto kExpectedRequest2 =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x08,
                       0x00,  // start handle: 0x0008
                       0xFF,
                       0xFF,  // end handle: 0xFFFF
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  // Terminate the procedure with an error response.
  const StaticByteBuffer kResponse2(0x01,  // opcode: error response
                                    0x08,  // request: read by type
                                    0x0A,
                                    0x00,  // handle: 0x000A
                                    0x0A   // error: Attribute Not Found
  );

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<CharacteristicData> chrcs;
  auto chrc_cb = [&chrcs](const CharacteristicData& chrc) {
    chrcs.push_back(chrc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest0, &kResponse0);
  EXPECT_PACKET_OUT(kExpectedRequest1, &kResponse1);
  EXPECT_PACKET_OUT(kExpectedRequest2, &kResponse2);
  client()->DiscoverCharacteristics(kStart, kEnd, chrc_cb, res_cb);
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());

  EXPECT_EQ(fit::ok(), status);
  EXPECT_EQ(3u, chrcs.size());

  EXPECT_EQ(0x0003, chrcs[0].handle);
  EXPECT_EQ(0, chrcs[0].properties);
  EXPECT_EQ(0x0004, chrcs[0].value_handle);
  EXPECT_EQ(kTestUuid1, chrcs[0].type);

  EXPECT_EQ(0x0005, chrcs[1].handle);
  EXPECT_EQ(1, chrcs[1].properties);
  EXPECT_EQ(0x0006, chrcs[1].value_handle);
  EXPECT_EQ(kTestUuid2, chrcs[1].type);

  EXPECT_EQ(0x0007, chrcs[2].handle);
  EXPECT_EQ(0, chrcs[2].properties);
  EXPECT_EQ(0x0008, chrcs[2].value_handle);
  EXPECT_EQ(kTestUuid3, chrcs[2].type);
}

// Expects the discovery procedure to end with an error if a batch contains
// results that are from before requested range.
TEST_F(ClientTest, CharacteristicDiscoveryResultsBeforeRange) {
  constexpr att::Handle kStart = 0x0002;
  constexpr att::Handle kEnd = 0x0005;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x02,
                       0x00,  // start handle: 0x0002
                       0x05,
                       0x00,  // end handle: 0x0005
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  const StaticByteBuffer kResponse(
      0x09,  // opcode: read by type response
      0x07,  // data length: 7 (16-bit UUIDs)
      0x01,
      0x00,  // chrc 1 handle (handle is before the range)
      0x00,  // chrc 1 properties
      0x02,
      0x00,  // chrc 1 value handle
      0xAD,
      0xDE  // chrc 1 uuid: 0xDEAD
  );

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<CharacteristicData> chrcs;
  auto chrc_cb = [&chrcs](const CharacteristicData& chrc) {
    chrcs.push_back(chrc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, chrc_cb, res_cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
  EXPECT_TRUE(chrcs.empty());
}

// Expects the discovery procedure to end with an error if a batch contains
// results that are from beyond the requested range.
TEST_F(ClientTest, CharacteristicDiscoveryResultsBeyondRange) {
  constexpr att::Handle kStart = 0x0002;
  constexpr att::Handle kEnd = 0x0005;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x02,
                       0x00,  // start handle: 0x0002
                       0x05,
                       0x00,  // end handle: 0x0005
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  const StaticByteBuffer kResponse(
      0x09,  // opcode: read by type response
      0x07,  // data length: 7 (16-bit UUIDs)
      0x06,
      0x00,  // chrc 1 handle (handle is beyond the range)
      0x00,  // chrc 1 properties
      0x07,
      0x00,  // chrc 1 value handle
      0xAD,
      0xDE  // chrc 1 uuid: 0xDEAD
  );

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<CharacteristicData> chrcs;
  auto chrc_cb = [&chrcs](const CharacteristicData& chrc) {
    chrcs.push_back(chrc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, chrc_cb, res_cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
  EXPECT_TRUE(chrcs.empty());
}

// Expects the characteristic value handle to immediately follow the
// declaration as specified in Vol 3, Part G, 3.3.
TEST_F(ClientTest, CharacteristicDiscoveryValueNotContiguous) {
  constexpr att::Handle kStart = 0x0002;
  constexpr att::Handle kEnd = 0x0005;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x02,
                       0x00,  // start handle: 0x0002
                       0x05,
                       0x00,  // end handle: 0x0005
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  const StaticByteBuffer kResponse(0x09,  // opcode: read by type response
                                   0x07,  // data length: 7 (16-bit UUIDs)
                                   0x02,
                                   0x00,  // chrc 1 handle
                                   0x00,  // chrc 1 properties
                                   0x04,
                                   0x00,  // chrc 1 value handle (not immediate)
                                   0xAD,
                                   0xDE  // chrc 1 uuid: 0xDEAD
  );

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<CharacteristicData> chrcs;
  auto chrc_cb = [&chrcs](const CharacteristicData& chrc) {
    chrcs.push_back(chrc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, chrc_cb, res_cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
  EXPECT_TRUE(chrcs.empty());
}

TEST_F(ClientTest, CharacteristicDiscoveryHandlesNotIncreasing) {
  constexpr att::Handle kStart = 0x0002;
  constexpr att::Handle kEnd = 0x0005;

  const auto kExpectedRequest =
      StaticByteBuffer(0x08,  // opcode: read by type request
                       0x02,
                       0x00,  // start handle: 0x0002
                       0x05,
                       0x00,  // end handle: 0x0005
                       0x03,
                       0x28  // type: characteristic decl. (0x2803)
      );
  const StaticByteBuffer kResponse(0x09,  // opcode: read by type response
                                   0x07,  // data length: 7 (16-bit UUIDs)
                                   0x02,
                                   0x00,  // chrc 1 handle
                                   0x00,  // chrc 1 properties
                                   0x03,
                                   0x00,  // chrc 1 value handle
                                   0xAD,
                                   0xDE,  // chrc 1 uuid: 0xDEAD
                                   0x02,
                                   0x00,  // chrc 1 handle (repeated)
                                   0x00,  // chrc 1 properties
                                   0x03,
                                   0x00,  // chrc 1 value handle
                                   0xEF,
                                   0xBE  // chrc 1 uuid: 0xBEEF
  );

  att::Result<> status = fit::ok();
  auto res_cb = [&status](att::Result<> val) { status = val; };

  std::vector<CharacteristicData> chrcs;
  auto chrc_cb = [&chrcs](const CharacteristicData& chrc) {
    chrcs.push_back(chrc);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->DiscoverCharacteristics(kStart, kEnd, chrc_cb, res_cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
  // No Characteristics should be reported.
  EXPECT_EQ(0u, chrcs.size());
}

// Equal handles should not short-circuit and should result in a request.
TEST_F(ClientTest, DescriptorDiscoveryHandlesEqual) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0x0001;

  att::Result<> status = ToResult(HostError::kFailed);  // Initialize as error
  EXPECT_PACKET_OUT(MakeFindInformation(kStart, kEnd));
  SendDiscoverDescriptors(&status, NopDescCallback, kStart, kEnd);
  EXPECT_TRUE(AllExpectedPacketsSent());
}

TEST_F(ClientTest, DescriptorDiscoveryResponseTooShort) {
  att::Result<> status = fit::ok();
  // Respond back with a malformed payload.
  const StaticByteBuffer kResponse(0x05);
  EXPECT_PACKET_OUT(MakeFindInformation(), &kResponse);
  SendDiscoverDescriptors(&status, NopDescCallback);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, DescriptorDiscoveryMalformedDataLength) {
  att::Result<> status = fit::ok();
  const StaticByteBuffer kResponse(0x05,  // opcode: find information response
                                   0x03   // format (must be 1 or 2)
  );
  EXPECT_PACKET_OUT(MakeFindInformation(), &kResponse);
  SendDiscoverDescriptors(&status, NopDescCallback);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, DescriptorDiscoveryMalformedAttrDataList16) {
  att::Result<> status = fit::ok();
  const StaticByteBuffer kResponse(
      0x05,  // opcode: find information response
      0x01,  // format: 16-bit. Data length must be 4
      1,
      2,
      3,
      4,
      5);
  EXPECT_PACKET_OUT(MakeFindInformation(), &kResponse);
  SendDiscoverDescriptors(&status, NopDescCallback);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, DescriptorDiscoveryMalformedAttrDataList128) {
  att::Result<> status = fit::ok();
  const StaticByteBuffer kResponse(
      0x05,  // opcode: find information response
      0x02,  // format: 128-bit. Data length must be 18
      1,
      2,
      3,
      4,
      5,
      6,
      7,
      8,
      9,
      10,
      11,
      12,
      13,
      14,
      15,
      16,
      17);
  EXPECT_PACKET_OUT(MakeFindInformation(), &kResponse);
  SendDiscoverDescriptors(&status, NopDescCallback);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, DescriptorDiscoveryEmptyDataList) {
  att::Result<> status = ToResult(HostError::kFailed);
  const StaticByteBuffer kResponse(0x05,  // opcode: find information response
                                   0x01   // format: 16-bit.
                                          // data list empty
  );
  EXPECT_PACKET_OUT(MakeFindInformation(), &kResponse);
  SendDiscoverDescriptors(&status, NopDescCallback);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
}

TEST_F(ClientTest, DescriptorDiscoveryAttributeNotFound) {
  att::Result<> status = ToResult(HostError::kFailed);
  const StaticByteBuffer kResponse(0x01,  // opcode: error response
                                   0x04,  // request: find information
                                   0x01,
                                   0x00,  // handle: 0x0001
                                   0x0A   // error: Attribute Not Found
  );
  EXPECT_PACKET_OUT(MakeFindInformation(), &kResponse);
  SendDiscoverDescriptors(&status, NopDescCallback);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
}

TEST_F(ClientTest, DescriptorDiscoveryError) {
  att::Result<> status = ToResult(HostError::kFailed);
  const StaticByteBuffer kResponse(0x01,  // opcode: error response
                                   0x04,  // request: find information
                                   0x01,
                                   0x00,  // handle: 0x0001
                                   0x06   // error: Request Not Supported
  );
  EXPECT_PACKET_OUT(MakeFindInformation(), &kResponse);
  SendDiscoverDescriptors(&status, NopDescCallback);
  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), status);
}

TEST_F(ClientTest, DescriptorDiscovery16BitResultsSingleRequest) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0x0003;

  const StaticByteBuffer kResponse(
      0x05,  // opcode: find information response
      0x01,  // format: 16-bit. Data length must be 4
      0x01,
      0x00,  // desc 1 handle
      0xEF,
      0xBE,  // desc 1 uuid
      0x02,
      0x00,  // desc 2 handle
      0xAD,
      0xDE,  // desc 2 uuid
      0x03,
      0x00,  // desc 3 handle
      0xFE,
      0xFE  // desc 3 uuid
  );

  std::vector<DescriptorData> descrs;
  auto desc_cb = [&descrs](const DescriptorData& desc) {
    descrs.push_back(desc);
  };

  att::Result<> status = ToResult(HostError::kFailed);
  EXPECT_PACKET_OUT(MakeFindInformation(kStart, kEnd), &kResponse);
  SendDiscoverDescriptors(&status, std::move(desc_cb), kStart, kEnd);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  ASSERT_EQ(3u, descrs.size());
  EXPECT_EQ(0x0001, descrs[0].handle);
  EXPECT_EQ(0x0002, descrs[1].handle);
  EXPECT_EQ(0x0003, descrs[2].handle);
  EXPECT_EQ(uint16_t{0xBEEF}, descrs[0].type);
  EXPECT_EQ(uint16_t{0xDEAD}, descrs[1].type);
  EXPECT_EQ(uint16_t{0xFEFE}, descrs[2].type);
}

TEST_F(ClientTest, DescriptorDiscovery128BitResultsSingleRequest) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0x0002;

  const StaticByteBuffer kResponse(
      0x05,  // opcode: find information response
      0x02,  // format: 128-bit. Data length must be 18
      0x01,
      0x00,  // desc 1 handle
      0xFB,
      0x34,
      0x9B,
      0x5F,
      0x80,
      0x00,
      0x00,
      0x80,
      0x00,
      0x10,
      0x00,
      0x00,
      0xEF,
      0xBE,
      0x00,
      0x00,  // desc 1 uuid
      0x02,
      0x00,  // desc 2 handle
      0xFB,
      0x34,
      0x9B,
      0x5F,
      0x80,
      0x00,
      0x00,
      0x80,
      0x00,
      0x10,
      0x00,
      0x00,
      0xAD,
      0xDE,
      0x00,
      0x00  // desc 2 uuid
  );

  std::vector<DescriptorData> descrs;
  auto desc_cb = [&descrs](const DescriptorData& desc) {
    descrs.push_back(desc);
  };
  att::Result<> status = ToResult(HostError::kFailed);
  att()->set_mtu(512);

  EXPECT_PACKET_OUT(MakeFindInformation(kStart, kEnd), &kResponse);
  SendDiscoverDescriptors(&status, std::move(desc_cb), kStart, kEnd);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  ASSERT_EQ(2u, descrs.size());
  EXPECT_EQ(0x0001, descrs[0].handle);
  EXPECT_EQ(0x0002, descrs[1].handle);
  EXPECT_EQ(uint16_t{0xBEEF}, descrs[0].type);
  EXPECT_EQ(uint16_t{0xDEAD}, descrs[1].type);
}

TEST_F(ClientTest, DescriptorDiscoveryMultipleRequests) {
  constexpr att::Handle kEnd = 0x0005;
  constexpr att::Handle kStart0 = 0x0001;
  const StaticByteBuffer kRequest0 = MakeFindInformation(kStart0, kEnd);
  const StaticByteBuffer kResponse0(
      att::kFindInformationResponse,  // opcode: find information response
      0x01,                           // format: 16-bit. Data length must be 4
      0x01,
      0x00,  // desc 1 handle
      0xEF,
      0xBE,  // desc 1 uuid
      0x02,
      0x00,  // desc 2 handle
      0xAD,
      0xDE  // desc 2 uuid
  );
  constexpr att::Handle kStart1 = 0x0003;
  const StaticByteBuffer kRequest1 = MakeFindInformation(kStart1, kEnd);
  const StaticByteBuffer kResponse1(
      att::kFindInformationResponse,  // opcode: find information response
      0x02,                           // format: 128-bit. Data length must be 18
      0x03,
      0x00,  // desc 3 handle
      0xFB,
      0x34,
      0x9B,
      0x5F,
      0x80,
      0x00,
      0x00,
      0x80,
      0x00,
      0x10,
      0x00,
      0x00,
      0xFE,
      0xFE,
      0x00,
      0x00  // desc 3 uuid
  );
  constexpr att::Handle kStart2 = 0x0004;
  const StaticByteBuffer kRequest2 = MakeFindInformation(kStart2, kEnd);
  const StaticByteBuffer kResponse2(
      att::kErrorResponse,           // response opcode
      att::kFindInformationRequest,  // request opcode that generated error
      0x04,
      0x00,  // handle: kStart2
      0x0A   // error: Attribute Not Found
  );

  std::vector<DescriptorData> descrs;
  auto desc_cb = [&descrs](const DescriptorData& desc) {
    descrs.push_back(desc);
  };
  att::Result<> status = ToResult(HostError::kFailed);

  EXPECT_PACKET_OUT(kRequest0, &kResponse0);
  EXPECT_PACKET_OUT(kRequest1, &kResponse1);
  EXPECT_PACKET_OUT(kRequest2, &kResponse2);
  SendDiscoverDescriptors(&status, std::move(desc_cb), kStart0, kEnd);
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_EQ(fit::ok(), status);
  ASSERT_EQ(3u, descrs.size());
  EXPECT_EQ(0x0001, descrs[0].handle);
  EXPECT_EQ(0x0002, descrs[1].handle);
  EXPECT_EQ(0x0003, descrs[2].handle);
  EXPECT_EQ(uint16_t{0xBEEF}, descrs[0].type);
  EXPECT_EQ(uint16_t{0xDEAD}, descrs[1].type);
  EXPECT_EQ(uint16_t{0xFEFE}, descrs[2].type);
}

TEST_F(ClientTest, DescriptorDiscoveryResultsBeforeRange) {
  constexpr att::Handle kStart = 0x0002;
  const StaticByteBuffer kResponse(0x05,  // opcode: find information response
                                   0x01,  // format: 16-bit.
                                   0x01,
                                   0x00,  // handle is before kStart
                                   0xEF,
                                   0xBE  // uuid
  );
  att::Result<> status = fit::ok();
  EXPECT_PACKET_OUT(MakeFindInformation(kStart), &kResponse);
  SendDiscoverDescriptors(&status, NopDescCallback, kStart);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, DescriptorDiscoveryResultsBeyondRange) {
  constexpr att::Handle kStart = 0x0001;
  constexpr att::Handle kEnd = 0x0002;
  const StaticByteBuffer kResponse(0x05,  // opcode: find information response
                                   0x01,  // format: 16-bit.
                                   0x03,
                                   0x00,  // handle is beyond kEnd
                                   0xEF,
                                   0xBE  // uuid
  );
  att::Result<> status = fit::ok();
  EXPECT_PACKET_OUT(MakeFindInformation(kStart, kEnd), &kResponse);
  SendDiscoverDescriptors(&status, NopDescCallback, kStart, kEnd);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, DescriptorDiscoveryHandlesNotIncreasing) {
  att::Result<> status = fit::ok();
  const StaticByteBuffer kResponse(0x05,  // opcode: find information response
                                   0x01,  // format: 16-bit.
                                   0x01,
                                   0x00,  // handle: 0x0001
                                   0xEF,
                                   0xBE,  // uuid
                                   0x01,
                                   0x00,  // handle: 0x0001 (repeats)
                                   0xAD,
                                   0xDE  // uuid
  );
  EXPECT_PACKET_OUT(MakeFindInformation(), &kResponse);
  SendDiscoverDescriptors(&status, NopDescCallback);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, WriteRequestMalformedResponse) {
  const StaticByteBuffer kValue('f', 'o', 'o');
  const auto kHandle = 0x0001;
  const StaticByteBuffer kExpectedRequest(0x12,  // opcode: write request
                                          0x01,
                                          0x00,  // handle: 0x0001
                                          'f',
                                          'o',
                                          'o'  // value: "foo"
  );
  // Respond back with a malformed PDU. This should result in a link error.
  const StaticByteBuffer kResponse(
      0x013,  // opcode: write response
      0       // One byte payload. The write request has no parameters.
  );

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  ASSERT_FALSE(fake_chan()->link_error());
  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->WriteRequest(kHandle, kValue, cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
  EXPECT_TRUE(fake_chan()->link_error());
}

TEST_F(ClientTest, WriteRequestExceedsMtu) {
  const StaticByteBuffer kValue('f', 'o', 'o');
  constexpr att::Handle kHandle = 0x0001;
  constexpr size_t kMtu = 5;
  const StaticByteBuffer kExpectedRequest(0x12,  // opcode: write request
                                          0x01,
                                          0x00,  // handle: 0x0001
                                          'f',
                                          'o',
                                          'o'  // value: "foo"
  );
  ASSERT_EQ(kMtu + 1, kExpectedRequest.size());

  att()->set_mtu(kMtu);

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  client()->WriteRequest(kHandle, kValue, cb);

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, WriteRequestError) {
  const StaticByteBuffer kValue('f', 'o', 'o');
  const auto kHandle = 0x0001;
  const StaticByteBuffer kExpectedRequest(0x12,  // opcode: write request
                                          0x01,
                                          0x00,  // handle: 0x0001
                                          'f',
                                          'o',
                                          'o'  // value: "foo"
  );
  const StaticByteBuffer kResponse(0x01,  // opcode: error response
                                   0x12,  // request: write request
                                   0x01,
                                   0x00,  // handle: 0x0001
                                   0x06   // error: Request Not Supported
  );

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->WriteRequest(kHandle, kValue, cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, WriteRequestSuccess) {
  const StaticByteBuffer kValue('f', 'o', 'o');
  const auto kHandle = 0x0001;
  const StaticByteBuffer kExpectedRequest(0x12,  // opcode: write request
                                          0x01,
                                          0x00,  // handle: 0x0001
                                          'f',
                                          'o',
                                          'o'  // value: "foo"
  );
  const StaticByteBuffer kResponse(0x13  // opcode: write response
  );
  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };
  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->WriteRequest(kHandle, kValue, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, PrepareWriteRequestExceedsMtu) {
  const StaticByteBuffer kValue('f', 'o', 'o');
  constexpr att::Handle kHandle = 0x0001;
  constexpr auto kOffset = 0;
  constexpr size_t kMtu = 7;
  const StaticByteBuffer kExpectedRequest(
      0x16,  // opcode: prepare write request
      0x01,
      0x00,  // handle: 0x0001
      0x00,
      0x00,  // offset: 0x0000
      'f',
      'o',
      'o'  // value: "foo"
  );
  ASSERT_EQ(kMtu + 1, kExpectedRequest.size());

  att()->set_mtu(kMtu);

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status, const ByteBuffer&) {
    status = cb_status;
  };

  client()->PrepareWriteRequest(kHandle, kOffset, kValue, cb);

  RunUntilIdle();

  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

TEST_F(ClientTest, PrepareWriteRequestError) {
  const StaticByteBuffer kValue('f', 'o', 'o');
  const auto kHandle = 0x0001;
  const auto kOffset = 5;
  const StaticByteBuffer kExpectedRequest(
      0x16,  // opcode: prepare write request
      0x01,
      0x00,  // handle: 0x0001
      0x05,
      0x00,  // offset: 0x0005
      'f',
      'o',
      'o'  // value: "foo"
  );
  const StaticByteBuffer kResponse(0x01,  // opcode: error response
                                   0x16,  // request: prepare write request
                                   0x01,
                                   0x00,  // handle: 0x0001
                                   0x06   // error: Request Not Supported
  );
  std::optional<att::Result<>> status;
  auto cb = [&status](att::Result<> cb_status, const ByteBuffer&) {
    status = cb_status;
  };
  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->PrepareWriteRequest(kHandle, kOffset, kValue, cb);
  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), status.value());
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, PrepareWriteRequestSuccess) {
  const StaticByteBuffer kValue('f', 'o', 'o');
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const StaticByteBuffer kExpectedRequest(
      0x16,  // opcode: prepare write request
      0x01,
      0x00,  // handle: 0x0001
      0x00,
      0x00,  // offset: 0x0000
      'f',
      'o',
      'o'  // value: "foo"
  );
  const StaticByteBuffer kResponse(0x17,  // opcode: prepare write response
                                   0x01,
                                   0x00,  // handle: 0x0001
                                   0x00,
                                   0x00,  // offset: 0x0000
                                   'f',
                                   'o',
                                   'o'  // value: "foo"
  );
  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status, const ByteBuffer&) {
    status = cb_status;
  };
  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->PrepareWriteRequest(kHandle, kOffset, kValue, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ExecuteWriteRequestPendingSuccess) {
  const auto kFlag = att::ExecuteWriteFlag::kWritePending;
  const StaticByteBuffer kExpectedRequest(
      0x18,  // opcode: execute write request
      0x01   // flag: write pending
  );
  const StaticByteBuffer kResponse(0x19);  // opcode: execute write response
  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };
  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->ExecuteWriteRequest(kFlag, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ExecuteWriteRequestCancelSuccess) {
  const auto kFlag = att::ExecuteWriteFlag::kCancelAll;
  const StaticByteBuffer kExpectedRequest(
      0x18,  // opcode: execute write request
      0x00   // flag: cancel all
  );
  const StaticByteBuffer kResponse(0x19);  // opcode: execute write response
  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };
  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->ExecuteWriteRequest(kFlag, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

// ExecutePrepareWrites should send each QueuedWrite request in the
// PrepareWriteQueue as a PrepareWriteRequest then finally send an ExecuteWrite.
TEST_F(ClientTest, ExecutePrepareWritesSuccess) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const StaticByteBuffer kValue1('f', 'o', 'o');
  const StaticByteBuffer kValue2('b', 'a', 'r');

  const StaticByteBuffer kExpectedPrep1(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x00,
                                        0x00,  // offset: 0x0000
                                        'f',
                                        'o',
                                        'o'  // value: "foo"
  );
  const auto kResponse1 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x00,
                       0x00,  // offset: 0x0000
                       'f',
                       'o',
                       'o'  // value: "foo"
      );
  const StaticByteBuffer kExpectedPrep2(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x03,
                                        0x00,  // offset: 0x0003
                                        'b',
                                        'a',
                                        'r'  // value: "bar"
  );
  const auto kResponse2 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x03,
                       0x00,  // offset: 0x0003
                       'b',
                       'a',
                       'r'  // value: "bar"
      );
  const StaticByteBuffer kExpectedExec(0x18,  // opcode: execute write request
                                       0x01   // flag: write pending
  );
  const StaticByteBuffer kExecResponse(0x19);  // opcode: execute write response

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));
  prep_write_queue.push(att::QueuedWrite(
      kHandle, static_cast<uint16_t>(kOffset + kValue1.size()), kValue2));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  EXPECT_PACKET_OUT(kExpectedPrep2, &kResponse2);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kDisabled, cb);
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

// When the PreparedWrite request exceeds the mtu, the client should
// automatically send a kCancellAll request.
TEST_F(ClientTest, ExecutePrepareWritesMalformedFailure) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  constexpr size_t kMtu = 7;
  const StaticByteBuffer kValue1('f', 'o');
  const StaticByteBuffer kValue2('b', 'a', 'r');

  const StaticByteBuffer kExpectedPrep1(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x00,
                                        0x00,  // offset: 0x0000
                                        'f',
                                        'o'  // value: "fo"
  );
  const auto kResponse1 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x00,
                       0x00,  // offset: 0x0000
                       'f',
                       'o'  // value: "fo"
      );
  // The second request is malformed, the client should send an ExecuteWrite
  // instead of the malformed PrepareWrite.
  const StaticByteBuffer kExpectedPrep2(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x02,
                                        0x00,  // offset: 0x0002
                                        'b',
                                        'a',
                                        'r'  // value: "bar"
  );
  const StaticByteBuffer kExpectedExec(0x18,  // opcode: execute write request
                                       0x00   // flag: kCancelAll
  );
  const StaticByteBuffer kExecResponse(0x19);  // opcode: execute write response

  ASSERT_EQ(kMtu, kExpectedPrep1.size());
  ASSERT_EQ(kMtu + 1, kExpectedPrep2.size());

  att()->set_mtu(kMtu);

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));
  prep_write_queue.push(att::QueuedWrite(
      kHandle, static_cast<uint16_t>(kOffset + kValue1.size()), kValue2));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kDisabled, cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kPacketMalformed), status);
}

// When the PreparedWrite receives an error response, the client should
// automatically send a kCancellAll request.
TEST_F(ClientTest, ExecutePrepareWritesErrorFailure) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const StaticByteBuffer kValue1('f', 'o', 'o');
  const StaticByteBuffer kValue2('b', 'a', 'r');

  const StaticByteBuffer kExpectedPrep1(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x00,
                                        0x00,  // offset: 0x0000
                                        'f',
                                        'o',
                                        'o'  // value: "fo"
  );
  const auto kResponse1 =
      StaticByteBuffer(0x01,  // opcode: error response
                       0x16,  // request: prepare write request
                       0x01,
                       0x00,  // handle: 0x0001
                       0x06   // error: Request Not Supported
      );
  // The first request returned an error, the client should send an ExecuteWrite
  // instead of the second PrepareWrite.
  const StaticByteBuffer kExpectedExec(0x18,  // opcode: execute write request
                                       0x00   // flag: kCancelAll
  );
  const StaticByteBuffer kExecResponse(0x19);  // opcode: execute write response

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  // Create the PrepareWriteQueue of requests to pass to the client
  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));
  prep_write_queue.push(att::QueuedWrite(
      kHandle, static_cast<uint16_t>(kOffset + kValue1.size()), kValue2));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kDisabled, cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

// ExecutePrepareWrites should enqueue immediately and send both long writes,
// one after the other.
TEST_F(ClientTest, ExecutePrepareWritesEnqueueRequestSuccess) {
  const auto kHandle1 = 0x0001;
  const auto kHandle2 = 0x0002;
  const auto kOffset = 0;
  const StaticByteBuffer kValue1('f', 'o', 'o');
  const StaticByteBuffer kValue2('b', 'a', 'r');

  const StaticByteBuffer kExpectedPrep1(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x00,
                                        0x00,  // offset: 0x0000
                                        'f',
                                        'o',
                                        'o'  // value: "foo"
  );
  const auto kResponse1 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x00,
                       0x00,  // offset: 0x0000
                       'f',
                       'o',
                       'o'  // value: "foo"
      );
  const StaticByteBuffer kExpectedPrep2(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x03,
                                        0x00,  // offset: 0x0003
                                        'b',
                                        'a',
                                        'r'  // value: "bar"
  );
  const auto kResponse2 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x03,
                       0x00,  // offset: 0x0003
                       'b',
                       'a',
                       'r'  // value: "bar"
      );
  // Start second write:
  const StaticByteBuffer kExpectedPrep3(0x16,  // opcode: prepare write request
                                        0x02,
                                        0x00,  // handle: 0x0002
                                        0x00,
                                        0x00,  // offset: 0x0000
                                        'f',
                                        'o',
                                        'o'  // value: "foo"
  );
  const auto kResponse3 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x02,
                       0x00,  // handle: 0x0002
                       0x00,
                       0x00,  // offset: 0x0000
                       'f',
                       'o',
                       'o'  // value: "foo"
      );
  const StaticByteBuffer kExpectedPrep4(0x16,  // opcode: prepare write request
                                        0x02,
                                        0x00,  // handle: 0x0002
                                        0x03,
                                        0x00,  // offset: 0x0003
                                        'b',
                                        'a',
                                        'r'  // value: "bar"
  );
  const auto kResponse4 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x02,
                       0x00,  // handle: 0x0002
                       0x03,
                       0x00,  // offset: 0x0003
                       'b',
                       'a',
                       'r'  // value: "bar"
      );
  const StaticByteBuffer kExpectedExec(0x18,  // opcode: execute write request
                                       0x01   // flag: write pending
  );
  const auto kExecuteWriteResponse =
      StaticByteBuffer(0x19);  // opcode: execute write response

  att::Result<> status1 = fit::ok();
  auto cb1 = [&status1](att::Result<> cb_status) { status1 = cb_status; };
  att::PrepareWriteQueue prep_write_queue1;
  prep_write_queue1.push(att::QueuedWrite(kHandle1, kOffset, kValue1));
  prep_write_queue1.push(att::QueuedWrite(
      kHandle1, static_cast<uint16_t>(kOffset + kValue1.size()), kValue2));
  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue1), ReliableMode::kDisabled, cb1);
  EXPECT_TRUE(AllExpectedPacketsSent());

  att::Result<> status2 = fit::ok();
  auto cb2 = [&status2](att::Result<> cb_status) { status2 = cb_status; };
  att::PrepareWriteQueue prep_write_queue2;
  prep_write_queue2.push(att::QueuedWrite(kHandle2, kOffset, kValue1));
  prep_write_queue2.push(att::QueuedWrite(
      kHandle2, static_cast<uint16_t>(kOffset + kValue1.size()), kValue2));
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue2), ReliableMode::kDisabled, cb2);

  EXPECT_PACKET_OUT(kExpectedPrep2, &kResponse2);
  EXPECT_PACKET_OUT(
      kExpectedExec);  // Delay sending response so the status can be checked
  RunUntilIdle();
  EXPECT_TRUE(AllExpectedPacketsSent());
  // The first request should be fully complete now, and should trigger the
  // second.
  EXPECT_EQ(fit::ok(), status1);

  EXPECT_PACKET_OUT(kExpectedPrep3, &kResponse3);
  EXPECT_PACKET_OUT(kExpectedPrep4, &kResponse4);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecuteWriteResponse);
  // Responding to the first execute request should start the second write
  // request.
  fake_chan()->Receive(kExecuteWriteResponse);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status2);
  EXPECT_FALSE(fake_chan()->link_error());
}

// ExecutePrepareWrites should send each QueuedWrite request in the
// PrepareWriteQueue as a PrepareWriteRequest and then send an ExecuteWrite.
// Test that a WriteRequest succeeds if ReliableMode is disabled even when the
// echoed response is different.
TEST_F(ClientTest, ExecutePrepareWritesDifferingResponseSuccess) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const StaticByteBuffer kValue1('f', 'o', 'o');
  const StaticByteBuffer kValue2('b', 'a', 'r');

  const StaticByteBuffer kExpectedPrep1(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x00,
                                        0x00,  // offset: 0x0000
                                        'f',
                                        'o',
                                        'o'  // value: "foo"
  );
  const auto kResponse1 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x00,
                       0x00,  // offset: 0x0000
                       'f',
                       'l'  // value: "fl" -> different, but OK.
      );
  const StaticByteBuffer kExpectedPrep2(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x03,
                                        0x00,  // offset: 0x0003
                                        'b',
                                        'a',
                                        'r'  // value: "bar"
  );
  const auto kResponse2 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x03,
                       0x00,  // offset: 0x0003
                       'b',
                       'a',
                       'r'  // value: "bar"
      );
  const StaticByteBuffer kExpectedExec(0x18,  // opcode: execute write request
                                       0x01   // flag: write pending
  );
  const StaticByteBuffer kExecResponse(0x19);  // opcode: execute write response

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  // Create the PrepareWriteQueue of requests to pass to the client
  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));
  prep_write_queue.push(att::QueuedWrite(
      kHandle, static_cast<uint16_t>(kOffset + kValue1.size()), kValue2));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kDisabled, cb);
  EXPECT_PACKET_OUT(kExpectedPrep2, &kResponse2);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

// ExecutePrepareWrites should send each QueuedWrite request in the
// PrepareWriteQueue as a PrepareWriteRequest, validate the responses,
// then finally send an ExecuteWrite.
TEST_F(ClientTest, ExecutePrepareWritesReliableWriteSuccess) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const StaticByteBuffer kValue1('f', 'o', 'o');
  const StaticByteBuffer kValue2('b', 'a', 'r');

  const StaticByteBuffer kExpectedPrep1(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x00,
                                        0x00,  // offset: 0x0000
                                        'f',
                                        'o',
                                        'o'  // value: "foo"
  );
  const auto kResponse1 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x00,
                       0x00,  // offset: 0x0000
                       'f',
                       'o',
                       'o'  // value: "foo"
      );
  const StaticByteBuffer kExpectedPrep2(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x03,
                                        0x00,  // offset: 0x0003
                                        'b',
                                        'a',
                                        'r'  // value: "bar"
  );
  const auto kResponse2 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x03,
                       0x00,  // offset: 0x0003
                       'b',
                       'a',
                       'r'  // value: "bar"
      );
  const StaticByteBuffer kExpectedExec(0x18,  // opcode: execute write request
                                       0x01   // flag: write pending
  );
  const StaticByteBuffer kExecResponse(0x19);  // opcode: execute write response

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  // Create the PrepareWriteQueue of requests to pass to the client
  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));
  prep_write_queue.push(att::QueuedWrite(
      kHandle, static_cast<uint16_t>(kOffset + kValue1.size()), kValue2));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kEnabled, cb);
  EXPECT_PACKET_OUT(kExpectedPrep2, &kResponse2);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

// If ReliableMode is enabled:
// When the requested buffer is empty, the reliability check should
// succeed when vailidating the echoed response.
TEST_F(ClientTest, ExecutePrepareWritesReliableEmptyBufSuccess) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const auto kValue1 = BufferView();

  const StaticByteBuffer kExpectedPrep1(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x00,
                                        0x00  // offset: 0x0000
  );
  const auto kResponse1 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x00,
                       0x00  // offset: 0x0000
      );
  const StaticByteBuffer kExpectedExec(0x18,  // opcode: execute write request
                                       0x01   // flag: write pending
  );
  const StaticByteBuffer kExecResponse(0x19);  // opcode: execute write response

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kEnabled, cb);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

// If ReliableMode is enabled:
// When the PreparedWrite response differs from the PreparedWrite request,
// the client should automatically send a kCancellAll request.
TEST_F(ClientTest, ExecutePrepareWritesReliableDifferingResponseError) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const StaticByteBuffer kValue1('f', 'o', 'o');
  const StaticByteBuffer kValue2('b', 'a', 'r');

  const StaticByteBuffer kExpectedPrep1(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x00,
                                        0x00,  // offset: 0x0000
                                        'f',
                                        'o',
                                        'o'  // value: "foo"
  );
  const auto kResponse1 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x00,
                       0x00,  // offset: 0x0000
                       'f',
                       'o',
                       'b',
                       '1'  // value: "fob1" -> invalid
      );
  // The first response was invalid, the client should send an ExecuteWrite
  // instead of the second PrepareWrite.
  const StaticByteBuffer kExpectedExec(0x18,  // opcode: execute write request
                                       0x00   // flag: kCancelAll
  );
  const StaticByteBuffer kExecResponse(0x19);  // opcode: execute write response

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));
  prep_write_queue.push(att::QueuedWrite(
      kHandle, static_cast<uint16_t>(kOffset + kValue1.size()), kValue2));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kEnabled, cb);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kNotReliable), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

// If ReliableMode is enabled:
// When the PreparedWrite response is malformed, the client should
// automatically send a kCancellAll request.
TEST_F(ClientTest, ExecutePrepareWritesReliableMalformedResponseError) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const StaticByteBuffer kValue1('f', 'o', 'o');
  const StaticByteBuffer kValue2('b', 'a', 'r');

  const StaticByteBuffer kExpectedPrep1(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x00,
                                        0x00,  // offset: 0x0000
                                        'f',
                                        'o',
                                        'o'  // value: "foo"
  );
  const auto kResponse1 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x00   // offset: malformed
      );
  // The first response was malformed, the client should send an ExecuteWrite
  // instead of the second PrepareWrite.
  const StaticByteBuffer kExpectedExec(0x18,  // opcode: execute write request
                                       0x00   // flag: kCancelAll
  );
  const StaticByteBuffer kExecResponse(0x19);  // opcode: execute write response

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));
  prep_write_queue.push(att::QueuedWrite(
      kHandle, static_cast<uint16_t>(kOffset + kValue1.size()), kValue2));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kEnabled, cb);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kNotReliable), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

// If ReliableMode is enabled:
// When the PreparedWrite response contains an incorrect offset, but correct
// value, the client should automatically send a kCancellAll request.
TEST_F(ClientTest, ExecutePrepareWritesReliableOffsetMismatchError) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const StaticByteBuffer kValue1('f', 'o', 'o');

  const StaticByteBuffer kExpectedPrep1(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x00,
                                        0x00,  // offset: 0x0000
                                        'f',
                                        'o',
                                        'o'  // value: "foo"
  );
  const auto kResponse1 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x01,
                       0x00,  // offset: incorrect
                       'f',
                       'o',
                       'o'  // value: 'foo'
      );
  // The first response was malformed, the client should send an ExecuteWrite
  // instead of the second PrepareWrite.
  const StaticByteBuffer kExpectedExec(0x18,  // opcode: execute write request
                                       0x00   // flag: kCancelAll
  );
  const StaticByteBuffer kExecResponse(0x19);  // opcode: execute write response

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kEnabled, cb);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kNotReliable), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ExecutePrepareWritesDestroyClientInCallback) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const StaticByteBuffer kValue1('f', 'o', 'o');

  const StaticByteBuffer kExpectedPrep1(0x16,  // prepare write request
                                        0x01,
                                        0x00,  // handle
                                        0x00,
                                        0x00,  // offset
                                        'f',
                                        'o',
                                        'o');
  const auto kResponse1 = StaticByteBuffer(0x17,  // prepare write response
                                           0x01,
                                           0x00,  // handle
                                           0x00,
                                           0x00,  // offset
                                           'f',
                                           'o',
                                           'o');
  const StaticByteBuffer kExpectedExec(0x18,  // execute write request
                                       0x01   // flag: write pending
  );
  const StaticByteBuffer kExecResponse(0x19);  // execute write response

  bool cb_called = false;
  auto cb = [&](att::Result<> cb_status) {
    cb_called = true;
    EXPECT_EQ(fit::ok(), cb_status);
    DestroyClient();
  };

  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);

  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kDisabled, std::move(cb));

  RunUntilIdle();
  EXPECT_TRUE(cb_called);
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ExecutePrepareWritesErrorFailureDestroyClientInCallback) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const StaticByteBuffer kValue1('f', 'o', 'o');

  const StaticByteBuffer kExpectedPrep1(0x16,  // prepare write request
                                        0x01,
                                        0x00,  // handle
                                        0x00,
                                        0x00,  // offset
                                        'f',
                                        'o',
                                        'o');
  const auto kResponse1 = StaticByteBuffer(0x01,  // error response
                                           0x16,  // request: prepare write
                                           0x01,
                                           0x00,  // handle
                                           0x06  // error: Request Not Supported
  );
  const StaticByteBuffer kExpectedExec(0x18,  // execute write request
                                       0x00   // flag: kCancelAll
  );
  const StaticByteBuffer kExecResponse(0x19);  // execute write response

  bool cb_called = false;
  auto cb = [&](att::Result<> cb_status) {
    cb_called = true;
    EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), cb_status);
    DestroyClient();
  };

  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);

  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kDisabled, std::move(cb));

  RunUntilIdle();
  EXPECT_TRUE(cb_called);
  EXPECT_TRUE(AllExpectedPacketsSent());
  EXPECT_FALSE(fake_chan()->link_error());
}

// If ReliableMode is enabled:
// When the PreparedWrite response contains an incorrect empty value,
// the client should automatically send a kCancellAll request.
TEST_F(ClientTest, ExecutePrepareWritesReliableEmptyValueError) {
  const auto kHandle = 0x0001;
  const auto kOffset = 0;
  const StaticByteBuffer kValue1('f', 'o', 'o');

  const StaticByteBuffer kExpectedPrep1(0x16,  // opcode: prepare write request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x00,
                                        0x00,  // offset: 0x0000
                                        'f',
                                        'o',
                                        'o'  // value: "foo"
  );
  const auto kResponse1 =
      StaticByteBuffer(0x17,  // opcode: prepare write response
                       0x01,
                       0x00,  // handle: 0x0001
                       0x00,
                       0x00  // offset: 0x0000
      );
  // The first response was malformed (empty value), the client should send an
  // ExecuteWrite instead of the second PrepareWrite.
  const StaticByteBuffer kExpectedExec(0x18,  // opcode: execute write request
                                       0x00   // flag: kCancelAll
  );
  const StaticByteBuffer kExecResponse(0x19);  // opcode: execute write response

  att::Result<> status = fit::ok();
  auto cb = [&status](att::Result<> cb_status) { status = cb_status; };

  att::PrepareWriteQueue prep_write_queue;
  prep_write_queue.push(att::QueuedWrite(kHandle, kOffset, kValue1));

  EXPECT_PACKET_OUT(kExpectedPrep1, &kResponse1);
  client()->ExecutePrepareWrites(
      std::move(prep_write_queue), ReliableMode::kEnabled, cb);
  EXPECT_PACKET_OUT(kExpectedExec, &kExecResponse);
  RunUntilIdle();
  EXPECT_EQ(ToResult(HostError::kNotReliable), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, WriteWithoutResponseExceedsMtu) {
  const StaticByteBuffer kValue('f', 'o', 'o');
  constexpr att::Handle kHandle = 0x0001;
  constexpr size_t kMtu = 5;
  att()->set_mtu(kMtu);
  const StaticByteBuffer kExpectedRequest(0x52,  // opcode: write command
                                          0x01,
                                          0x00,  // handle: 0x0001
                                          'f',
                                          'o',
                                          'o'  // value: "foo"
  );
  ASSERT_EQ(kMtu + 1, kExpectedRequest.size());

  std::optional<att::Result<>> status;
  // No packet should be sent.
  client()->WriteWithoutResponse(
      kHandle, kValue, [&](att::Result<> cb_status) { status = cb_status; });
  RunUntilIdle();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(ToResult(HostError::kFailed), *status);
}

TEST_F(ClientTest, WriteWithoutResponseSuccess) {
  const StaticByteBuffer kValue('f', 'o', 'o');
  const auto kHandle = 0x0001;
  const StaticByteBuffer kExpectedRequest(0x52,  // opcode: write request
                                          0x01,
                                          0x00,  // handle: 0x0001
                                          'f',
                                          'o',
                                          'o'  // value: "foo"
  );
  std::optional<att::Result<>> status;
  EXPECT_PACKET_OUT(kExpectedRequest);
  client()->WriteWithoutResponse(
      kHandle, kValue, [&](att::Result<> cb_status) { status = cb_status; });
  ASSERT_TRUE(status.has_value());
  ASSERT_EQ(fit::ok(), *status);
}

TEST_F(ClientTest, ReadRequestEmptyResponse) {
  constexpr att::Handle kHandle = 0x0001;
  const StaticByteBuffer kExpectedRequest(0x0A,  // opcode: read request
                                          0x01,
                                          0x00  // handle: 0x0001
  );
  // ATT Read Response with no payload.
  const StaticByteBuffer kResponse(0x0B);

  att::Result<> status = ToResult(HostError::kFailed);
  auto cb = [&status](att::Result<> cb_status,
                      const ByteBuffer& value,
                      bool maybe_truncated) {
    status = cb_status;

    // We expect an empty value
    EXPECT_EQ(0u, value.size());
    EXPECT_FALSE(maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->ReadRequest(kHandle, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ReadRequestSuccess) {
  constexpr att::Handle kHandle = 0x0001;
  const StaticByteBuffer kExpectedRequest(0x0A,  // opcode: read request
                                          0x01,
                                          0x00  // handle: 0x0001
  );

  const StaticByteBuffer kExpectedResponse(0x0B,  // opcode: read response
                                           't',
                                           'e',
                                           's',
                                           't'  // value: "test"
  );

  att::Result<> status = ToResult(HostError::kFailed);
  auto cb = [&](att::Result<> cb_status,
                const ByteBuffer& value,
                bool maybe_truncated) {
    status = cb_status;
    EXPECT_TRUE(ContainersEqual(kExpectedResponse.view(1), value));
    EXPECT_FALSE(maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kExpectedResponse);
  client()->ReadRequest(kHandle, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ReadRequestSuccessMaybeTruncatedDueToMtu) {
  constexpr att::Handle kHandle = 0x0001;
  const auto kExpectedRequest = StaticByteBuffer(0x0A,  // opcode: read request
                                                 LowerBits(kHandle),
                                                 UpperBits(kHandle)  // handle
  );

  DynamicByteBuffer expected_response(att()->mtu());
  expected_response.Fill(0);
  expected_response.WriteObj(att::kReadResponse);  // opcode: read response

  att::Result<> status = ToResult(HostError::kFailed);
  auto cb = [&](att::Result<> cb_status,
                const ByteBuffer& value,
                bool maybe_truncated) {
    status = cb_status;
    EXPECT_TRUE(ContainersEqual(expected_response.view(1), value));
    EXPECT_TRUE(maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &expected_response);
  client()->ReadRequest(kHandle, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ReadRequestSuccessNotTruncatedWhenMtuAllowsMaxValueLength) {
  constexpr uint16_t kPreferredMTU =
      att::kMaxAttributeValueLength + sizeof(att::OpCode);
  att()->set_preferred_mtu(kPreferredMTU);
  constexpr uint16_t kServerRxMTU = kPreferredMTU;

  const auto kExpectedMtuRequest =
      StaticByteBuffer(0x02,  // opcode: exchange MTU
                       LowerBits(kPreferredMTU),
                       UpperBits(kPreferredMTU)  // client rx mtu
      );

  std::optional<att::Result<uint16_t>> result;
  auto mtu_cb = [&](att::Result<uint16_t> cb_result) { result = cb_result; };

  EXPECT_PACKET_OUT(kExpectedMtuRequest);
  client()->ExchangeMTU(mtu_cb);
  ASSERT_EQ(att::kLEMinMTU, att()->mtu());
  fake_chan()->Receive(StaticByteBuffer(
      0x03,  // opcode: exchange MTU response
      LowerBits(kServerRxMTU),
      UpperBits(kServerRxMTU)  // server rx mtu
      ));
  RunUntilIdle();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(att::Result<uint16_t>(fit::ok(kPreferredMTU)), *result);
  EXPECT_EQ(kPreferredMTU, att()->mtu());

  constexpr att::Handle kHandle = 0x0001;
  const auto kExpectedReadRequest =
      StaticByteBuffer(0x0A,  // opcode: read request
                       LowerBits(kHandle),
                       UpperBits(kHandle)  // handle
      );

  DynamicByteBuffer expected_response(att()->mtu());
  expected_response.Fill(0);
  expected_response.WriteObj(att::kReadResponse);  // opcode: read response

  att::Result<> status = ToResult(HostError::kFailed);
  auto cb = [&](att::Result<> cb_status,
                const ByteBuffer& value,
                bool maybe_truncated) {
    status = cb_status;
    EXPECT_TRUE(ContainersEqual(expected_response.view(1), value));
    EXPECT_FALSE(maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedReadRequest, &expected_response);
  client()->ReadRequest(kHandle, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ReadRequestError) {
  constexpr att::Handle kHandle = 0x0001;
  const StaticByteBuffer kExpectedRequest(0x0A,  // opcode: read request
                                          0x01,
                                          0x00  // handle: 0x0001
  );
  const StaticByteBuffer kErrorResponse(0x01,  // opcode: error response
                                        0x0A,  // request: read request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x06   // error: Request Not Supported
  );

  att::Result<> status = fit::ok();
  auto cb = [&](att::Result<> cb_status,
                const ByteBuffer& value,
                bool maybe_truncated) {
    status = cb_status;

    // Value should be empty due to the error.
    EXPECT_EQ(0u, value.size());
    EXPECT_FALSE(maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kErrorResponse);
  client()->ReadRequest(kHandle, cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kRequestNotSupported), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ReadByTypeRequestSuccess16BitUUID) {
  const UUID kUuid16(uint16_t{0xBEEF});
  constexpr att::Handle kStartHandle = 0x0001;
  constexpr att::Handle kEndHandle = 0xFFFF;
  const auto kExpectedRequest =
      StaticByteBuffer(att::kReadByTypeRequest,  // opcode
                       LowerBits(kStartHandle),
                       UpperBits(kStartHandle),  // start handle
                       LowerBits(kEndHandle),
                       UpperBits(kEndHandle),  // end handle
                       // UUID
                       0xEF,
                       0xBE);

  constexpr att::Handle kHandle0 = 0x0002;
  constexpr att::Handle kHandle1 = 0x0003;
  const auto kExpectedResponse = StaticByteBuffer(att::kReadByTypeResponse,
                                                  0x03,  // pair length
                                                  LowerBits(kHandle0),
                                                  UpperBits(kHandle0),
                                                  0x00,  // attribute pair 0
                                                  LowerBits(kHandle1),
                                                  UpperBits(kHandle1),
                                                  0x01  // attribute pair 1
  );

  bool cb_called = false;
  auto cb = [&](Client::ReadByTypeResult result) {
    cb_called = true;
    ASSERT_EQ(fit::ok(), result);
    const auto& values = result.value();
    ASSERT_EQ(2u, values.size());
    EXPECT_EQ(kHandle0, values[0].handle);
    EXPECT_TRUE(ContainersEqual(StaticByteBuffer(0x00), values[0].value));
    EXPECT_FALSE(values[0].maybe_truncated);
    EXPECT_EQ(kHandle1, values[1].handle);
    EXPECT_TRUE(ContainersEqual(StaticByteBuffer(0x01), values[1].value));
    EXPECT_FALSE(values[1].maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kExpectedResponse);
  client()->ReadByTypeRequest(kUuid16, kStartHandle, kEndHandle, cb);
  RunUntilIdle();
  EXPECT_TRUE(cb_called);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ReadByTypeRequestSuccess128BitUUID) {
  const UUID kUuid128({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
  constexpr att::Handle kStartHandle = 0x0001;
  constexpr att::Handle kEndHandle = 0xFFFF;
  const auto kExpectedRequest =
      StaticByteBuffer(att::kReadByTypeRequest,  // opcode
                       LowerBits(kStartHandle),
                       UpperBits(kStartHandle),  // start handle
                       LowerBits(kEndHandle),
                       UpperBits(kEndHandle),  // end handle
                       // UUID
                       0,
                       1,
                       2,
                       3,
                       4,
                       5,
                       6,
                       7,
                       8,
                       9,
                       10,
                       11,
                       12,
                       13,
                       14,
                       15);

  constexpr att::Handle kHandle0 = 0x0002;
  constexpr att::Handle kHandle1 = 0x0003;
  const auto kExpectedResponse = StaticByteBuffer(att::kReadByTypeResponse,
                                                  0x03,  // pair length
                                                  LowerBits(kHandle0),
                                                  UpperBits(kHandle0),
                                                  0x00,  // attribute pair 0
                                                  LowerBits(kHandle1),
                                                  UpperBits(kHandle1),
                                                  0x01  // attribute pair 1
  );

  bool cb_called = false;
  auto cb = [&](Client::ReadByTypeResult result) {
    cb_called = true;
    ASSERT_EQ(fit::ok(), result);
    const auto& values = result.value();
    ASSERT_EQ(2u, values.size());
    EXPECT_EQ(kHandle0, values[0].handle);
    EXPECT_TRUE(ContainersEqual(StaticByteBuffer(0x00), values[0].value));
    EXPECT_EQ(kHandle1, values[1].handle);
    EXPECT_TRUE(ContainersEqual(StaticByteBuffer(0x01), values[1].value));
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kExpectedResponse);
  client()->ReadByTypeRequest(kUuid128, kStartHandle, kEndHandle, cb);
  RunUntilIdle();
  EXPECT_TRUE(cb_called);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ReadByTypeRequestError) {
  constexpr att::Handle kStartHandle = 0x0001;
  constexpr att::Handle kEndHandle = 0xFFFF;
  const auto kExpectedRequest =
      StaticByteBuffer(att::kReadByTypeRequest,  // opcode
                       LowerBits(kStartHandle),
                       UpperBits(kStartHandle),  // start handle
                       LowerBits(kEndHandle),
                       UpperBits(kEndHandle),  // end handle
                       // UUID matches |kTestUuid3| declared above.
                       0,
                       1,
                       2,
                       3,
                       4,
                       5,
                       6,
                       7,
                       8,
                       9,
                       10,
                       11,
                       12,
                       13,
                       14,
                       15);

  const auto kErrorResponse = StaticByteBuffer(
      att::kErrorResponse,      // opcode
      att::kReadByTypeRequest,  // request opcode
      LowerBits(kStartHandle),
      UpperBits(kStartHandle),                                  // start handle
      static_cast<uint8_t>(att::ErrorCode::kAttributeNotFound)  // error code
  );

  std::optional<att::Error> error;
  std::optional<att::Handle> handle;
  auto cb = [&](Client::ReadByTypeResult result) {
    ASSERT_TRUE(result.is_error());
    error = result.error_value().error;
    handle = result.error_value().handle;
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kErrorResponse);
  client()->ReadByTypeRequest(kTestUuid3, kStartHandle, kEndHandle, cb);
  RunUntilIdle();
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(att::Error(att::ErrorCode::kAttributeNotFound), *error);
  ASSERT_TRUE(handle.has_value());
  EXPECT_EQ(kStartHandle, handle.value());
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ReadByTypeRequestInvalidResponses) {
  constexpr att::Handle kStartHandle = 0x0002;
  constexpr att::Handle kEndHandle = 0xFF00;
  constexpr att::Handle kHandle0 = 0x0005;
  constexpr att::Handle kHandle1 = 0x0006;

  const auto kResponseEmptyPayload = StaticByteBuffer(att::kReadByTypeResponse);
  const auto kResponseLengthGreaterThanListLength =
      StaticByteBuffer(att::kReadByTypeResponse,
                       0x02,   // length
                       0x01);  // invalid list (too small)
  const auto kResponseWithInvalidLength =
      StaticByteBuffer(att::kReadByTypeResponse,
                       0x00,  // invalid pair length (less than handle size)
                       LowerBits(kHandle0),
                       UpperBits(kHandle0),
                       0x00);  // attribute pair 0
  const auto kResponseWithEmptyList = StaticByteBuffer(att::kReadByTypeResponse,
                                                       0x03);  // pair length
  const auto kResponseWithInvalidList = StaticByteBuffer(
      att::kReadByTypeResponse,
      0x03,  // length
      LowerBits(kHandle0),
      UpperBits(kHandle0));  // invalid attribute pair 0 (invalid length)
  const auto kResponseWithInvalidAttributeHandleLessThanStart =
      StaticByteBuffer(att::kReadByTypeResponse,
                       0x02,  // length
                       // invalid attribute pair 0 (handle out of range)
                       LowerBits(kStartHandle - 1),
                       UpperBits(kStartHandle - 1));
  const auto kResponseWithInvalidAttributeHandleGreaterThanEnd =
      StaticByteBuffer(att::kReadByTypeResponse,
                       0x02,  // length
                       // invalid attribute pair 0 (handle out of range)
                       LowerBits(kEndHandle + 1),
                       UpperBits(kEndHandle + 1));
  const auto kResponseWithInvalidListWithDecreasingHandles =
      StaticByteBuffer(att::kReadByTypeResponse,
                       0x02,  // length
                       LowerBits(kHandle1),
                       UpperBits(kHandle1),  // attribute pair 0
                       LowerBits(kHandle0),
                       UpperBits(kHandle0));  // attribute pair 1
  const auto kResponseWithInvalidListWithDuplicateHandles =
      StaticByteBuffer(att::kReadByTypeResponse,
                       0x02,  // length
                       LowerBits(kHandle0),
                       UpperBits(kHandle0),  // attribute pair 0
                       LowerBits(kHandle0),
                       UpperBits(kHandle0));  // attribute pair 1

  const std::vector<std::pair<const char*, const ByteBuffer&>>
      kInvalidResponses = {
          {"kResponseEmptyPayload", kResponseEmptyPayload},
          {"kResponseLengthGreaterThanListLength",
           kResponseLengthGreaterThanListLength},
          {"kResponseWithInvalidLength", kResponseWithInvalidLength},
          {"kResponseWithEmptyList", kResponseWithEmptyList},
          {"kResponseWithInvalidList", kResponseWithInvalidList},
          {"kResponseWithInvalidAttributeHandleLessThanStart",
           kResponseWithInvalidAttributeHandleLessThanStart},
          {"kResponseWithInvalidAttributeHandleGreaterThanEnd",
           kResponseWithInvalidAttributeHandleGreaterThanEnd},
          {"kResponseWithInvalidListWithDecreasingHandles",
           kResponseWithInvalidListWithDecreasingHandles},
          {"kResponseWithInvalidListWithDuplicateHandles",
           kResponseWithInvalidListWithDuplicateHandles}};

  const auto kExpectedRequest =
      StaticByteBuffer(att::kReadByTypeRequest,  // opcode
                       LowerBits(kStartHandle),
                       UpperBits(kStartHandle),  // start handle
                       LowerBits(kEndHandle),
                       UpperBits(kEndHandle),  // end handle
                       // UUID matches |kTestUuid3| declared above.
                       0,
                       1,
                       2,
                       3,
                       4,
                       5,
                       6,
                       7,
                       8,
                       9,
                       10,
                       11,
                       12,
                       13,
                       14,
                       15);

  for (const auto& [name, invalid_rsp] : kInvalidResponses) {
    SCOPED_TRACE(bt_lib_cpp_string::StringPrintf("Invalid Response: %s", name));

    std::optional<att::Error> error;
    auto cb = [&](Client::ReadByTypeResult result) {
      ASSERT_TRUE(result.is_error());
      error = result.error_value().error;
      EXPECT_FALSE(result.error_value().handle.has_value());
    };

    EXPECT_PACKET_OUT(kExpectedRequest, &invalid_rsp);
    client()->ReadByTypeRequest(kTestUuid3, kStartHandle, kEndHandle, cb);
    RunUntilIdle();
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(Error(HostError::kPacketMalformed), *error);
    EXPECT_FALSE(fake_chan()->link_error());
  }
}

TEST_F(ClientTest, ReadBlobRequestEmptyResponse) {
  constexpr att::Handle kHandle = 1;
  constexpr uint16_t kOffset = 5;
  const StaticByteBuffer kExpectedRequest(0x0C,  // opcode: read blob request
                                          0x01,
                                          0x00,  // handle: 1
                                          0x05,
                                          0x00  // offset: 5
  );
  // ATT Read Blob Response with no payload.
  const StaticByteBuffer kResponse(0x0D);

  att::Result<> status = ToResult(HostError::kFailed);
  auto cb = [&](att::Result<> cb_status,
                const ByteBuffer& value,
                bool maybe_truncated) {
    status = cb_status;

    // We expect an empty value
    EXPECT_EQ(0u, value.size());
    EXPECT_FALSE(maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kResponse);
  client()->ReadBlobRequest(kHandle, kOffset, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ReadBlobRequestSuccess) {
  constexpr att::Handle kHandle = 1;
  constexpr uint16_t kOffset = 5;
  const StaticByteBuffer kExpectedRequest(0x0C,  // opcode: read blob request
                                          0x01,
                                          0x00,  // handle: 1
                                          0x05,
                                          0x00  // offset: 5
  );
  const StaticByteBuffer kExpectedResponse(0x0D,  // opcode: read blob response
                                           't',
                                           'e',
                                           's',
                                           't'  // value: "test"
  );

  att::Result<> status = ToResult(HostError::kFailed);
  auto cb = [&](att::Result<> cb_status,
                const ByteBuffer& value,
                bool maybe_truncated) {
    status = cb_status;

    // We expect an empty value
    EXPECT_TRUE(ContainersEqual(kExpectedResponse.view(1), value));
    EXPECT_FALSE(maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kExpectedResponse);
  client()->ReadBlobRequest(kHandle, kOffset, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ReadBlobRequestMaybeTruncated) {
  constexpr att::Handle kHandle = 0x0001;
  constexpr uint16_t kOffset = 5;
  const auto kExpectedRequest =
      StaticByteBuffer(0x0C,  // opcode: read blob request
                       LowerBits(kHandle),
                       UpperBits(kHandle),  // handle
                       LowerBits(kOffset),
                       UpperBits(kOffset)  // offset
      );

  DynamicByteBuffer expected_response(att()->mtu());
  expected_response.Fill(0);
  expected_response.WriteObj(
      att::kReadBlobResponse);  // opcode: read blob response

  att::Result<> status = ToResult(HostError::kFailed);
  auto cb = [&](att::Result<> cb_status,
                const ByteBuffer& value,
                bool maybe_truncated) {
    status = cb_status;
    EXPECT_TRUE(ContainersEqual(expected_response.view(1), value));
    EXPECT_TRUE(maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &expected_response);
  client()->ReadBlobRequest(kHandle, kOffset, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(
    ClientTest,
    ReadBlobRequestSuccessNotTruncatedWhenOffsetPlusMtuEqualsMaxValueLength) {
  constexpr att::Handle kHandle = 0x0001;
  const uint16_t kOffset =
      att::kMaxAttributeValueLength - (att()->mtu() - sizeof(att::OpCode));
  const auto kExpectedRequest =
      StaticByteBuffer(0x0C,  // opcode: read blob request
                       LowerBits(kHandle),
                       UpperBits(kHandle),  // handle
                       LowerBits(kOffset),
                       UpperBits(kOffset)  // offset
      );

  // The blob should both max out the MTU and max out the value length.
  DynamicByteBuffer expected_response(att()->mtu());
  expected_response.Fill(0);
  expected_response.WriteObj(
      att::kReadBlobResponse);  // opcode: read blob response

  att::Result<> status = ToResult(HostError::kFailed);
  auto cb = [&](att::Result<> cb_status,
                const ByteBuffer& value,
                bool maybe_truncated) {
    status = cb_status;
    EXPECT_TRUE(ContainersEqual(expected_response.view(1), value));
    EXPECT_FALSE(maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &expected_response);
  client()->ReadBlobRequest(kHandle, kOffset, cb);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, ReadBlobRequestError) {
  constexpr att::Handle kHandle = 1;
  constexpr uint16_t kOffset = 5;
  const StaticByteBuffer kExpectedRequest(0x0C,  // opcode: read blob request
                                          0x01,
                                          0x00,  // handle: 1
                                          0x05,
                                          0x00  // offset: 5
  );
  const StaticByteBuffer kErrorResponse(0x01,  // opcode: error response
                                        0x0C,  // request: read blob request
                                        0x01,
                                        0x00,  // handle: 0x0001
                                        0x07   // error: Invalid Offset
  );

  att::Result<> status = ToResult(HostError::kFailed);
  auto cb = [&](att::Result<> cb_status,
                const ByteBuffer& value,
                bool maybe_truncated) {
    status = cb_status;

    // We expect an empty value
    EXPECT_EQ(0u, value.size());
    EXPECT_FALSE(maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &kErrorResponse);
  client()->ReadBlobRequest(kHandle, kOffset, cb);
  RunUntilIdle();
  EXPECT_EQ(ToResult(att::ErrorCode::kInvalidOffset), status);
  EXPECT_FALSE(fake_chan()->link_error());
}

TEST_F(ClientTest, EmptyNotification) {
  constexpr att::Handle kHandle = 1;

  bool called = false;
  client()->SetNotificationHandler(
      [&](bool ind, auto handle, const auto& value, bool /*maybe_truncated*/) {
        called = true;
        EXPECT_FALSE(ind);
        EXPECT_EQ(kHandle, handle);
        EXPECT_EQ(0u, value.size());
      });

  // clang-format off
  fake_chan()->Receive(StaticByteBuffer(
      0x1B,       // opcode: notification
      0x01, 0x00  // handle: 1
      ));
  // clang-format on

  RunUntilIdle();
  EXPECT_TRUE(called);
}

TEST_F(ClientTest, Notification) {
  constexpr att::Handle kHandle = 1;

  bool called = false;
  client()->SetNotificationHandler(
      [&](bool ind, auto handle, const auto& value, bool maybe_truncated) {
        called = true;
        EXPECT_FALSE(ind);
        EXPECT_EQ(kHandle, handle);
        EXPECT_EQ("test", value.AsString());
        EXPECT_FALSE(maybe_truncated);
      });

  // clang-format off
  fake_chan()->Receive(StaticByteBuffer(
      0x1B,               // opcode: notification
      0x01, 0x00,         // handle: 1
      't', 'e', 's', 't'  // value: "test"
  ));
  // clang-format on

  RunUntilIdle();
  EXPECT_TRUE(called);
}

TEST_F(ClientTest, NotificationTruncated) {
  constexpr att::Handle kHandle = 1;
  StaticByteBuffer pdu_header(0x1B,  // opcode: notification
                              0x01,
                              0x00  // handle: 1

  );
  DynamicByteBuffer pdu(att()->mtu());
  pdu.Fill(0);
  pdu_header.Copy(&pdu);

  bool called = false;
  client()->SetNotificationHandler([&](bool ind,
                                       auto handle,
                                       const ByteBuffer& value,
                                       bool maybe_truncated) {
    called = true;
    EXPECT_FALSE(ind);
    EXPECT_EQ(kHandle, handle);
    EXPECT_EQ(value.size(), att()->mtu() - pdu_header.size());
    EXPECT_TRUE(maybe_truncated);
  });
  fake_chan()->Receive(pdu);

  RunUntilIdle();
  EXPECT_TRUE(called);
}

TEST_F(ClientTest, Indication) {
  constexpr att::Handle kHandle = 1;

  bool called = false;
  client()->SetNotificationHandler(
      [&](bool ind, auto handle, const auto& value, bool maybe_truncated) {
        called = true;
        EXPECT_TRUE(ind);
        EXPECT_EQ(kHandle, handle);
        EXPECT_EQ("test", value.AsString());
        EXPECT_FALSE(maybe_truncated);
      });

  const auto kIndication = StaticByteBuffer(0x1D,  // opcode: indication
                                            0x01,
                                            0x00,  // handle: 1
                                            't',
                                            'e',
                                            's',
                                            't'  // value: "test"
  );

  // Wait until a confirmation gets sent.
  const auto kConfirmation = StaticByteBuffer(0x1E);
  EXPECT_PACKET_OUT(kConfirmation);
  fake_chan()->Receive(kIndication);
  EXPECT_TRUE(called);
}

// Maxing out the length parameter of a Read By Type request is not possible
// with the current max preferred MTU. If the max MTU is increased, this test
// will need to be updated to test that ReadByTypeValue.maybe_truncated is true
// for read by type responses with values that max out the length parameter.
TEST_F(ClientTest, ReadByTypeRequestSuccessValueTruncatedByLengthParam) {
  const uint16_t kSizeOfReadByTypeResponseWithValueThatMaxesOutLengthParam =
      sizeof(att::kReadByTypeResponse) + sizeof(att::ReadByTypeResponseParams) +
      std::numeric_limits<
          decltype(att::ReadByTypeResponseParams::length)>::max();
  EXPECT_LT(att()->preferred_mtu(),
            kSizeOfReadByTypeResponseWithValueThatMaxesOutLengthParam);
  EXPECT_LT(att::kLEMaxMTU,
            kSizeOfReadByTypeResponseWithValueThatMaxesOutLengthParam);
}

TEST_F(ClientTest, ReadByTypeRequestSuccessValueTruncatedByMtu) {
  EXPECT_EQ(att()->mtu(), att::kLEMinMTU);

  const UUID kUuid16(uint16_t{0xBEEF});
  constexpr att::Handle kStartHandle = 0x0001;
  constexpr att::Handle kEndHandle = 0xFFFF;
  const auto kExpectedRequest =
      StaticByteBuffer(att::kReadByTypeRequest,  // opcode
                       LowerBits(kStartHandle),
                       UpperBits(kStartHandle),  // start handle
                       LowerBits(kEndHandle),
                       UpperBits(kEndHandle),  // end handle
                       // UUID
                       0xEF,
                       0xBE);

  constexpr att::Handle kHandle = 0x0002;
  const uint8_t kMaxReadByTypeValueLengthWithMinMtu = att::kLEMinMTU - 4;
  const auto kExpectedResponseHeader = StaticByteBuffer(
      att::kReadByTypeResponse,
      sizeof(kHandle) + kMaxReadByTypeValueLengthWithMinMtu,  // pair length
      LowerBits(kHandle),
      UpperBits(kHandle)  // attribute handle
  );
  DynamicByteBuffer expected_response(kExpectedResponseHeader.size() +
                                      kMaxReadByTypeValueLengthWithMinMtu);
  expected_response.Fill(0);
  kExpectedResponseHeader.Copy(&expected_response);

  bool cb_called = false;
  auto cb = [&](Client::ReadByTypeResult result) {
    cb_called = true;
    ASSERT_EQ(fit::ok(), result);
    const auto& values = result.value();
    ASSERT_EQ(1u, values.size());
    EXPECT_EQ(kHandle, values[0].handle);
    EXPECT_EQ(values[0].value.size(), kMaxReadByTypeValueLengthWithMinMtu);
    EXPECT_TRUE(values[0].maybe_truncated);
  };

  EXPECT_PACKET_OUT(kExpectedRequest, &expected_response);
  client()->ReadByTypeRequest(kUuid16, kStartHandle, kEndHandle, cb);
  RunUntilIdle();
  EXPECT_TRUE(cb_called);
  EXPECT_FALSE(fake_chan()->link_error());
}

}  // namespace
}  // namespace bt::gatt
