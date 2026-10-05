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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/low_energy_connection_server.h"

#include <pw_assert/check.h>

#include "pw_bluetooth_sapphire/fake_lease_provider.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/adapter_test_fixture.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/types.h"

namespace bthost {
namespace {

namespace fbt = fuchsia::bluetooth;
namespace fble = fuchsia::bluetooth::le;
namespace fbg = fuchsia::bluetooth::gatt2;

const bt::DeviceAddress kTestAddr(bt::DeviceAddress::Type::kLEPublic,
                                  {0x01, 0, 0, 0, 0, 0});

// Used for test cases that require all of the test infrastructure, but don't
// want to auto- configure the client and server at startup.
class LowEnergyConnectionServerTest
    : public bthost::testing::AdapterTestFixture {
 public:
  LowEnergyConnectionServerTest() = default;
  ~LowEnergyConnectionServerTest() override = default;

  void SetUp() override {
    bt::testing::FakeController::Settings settings;
    settings.ApplyDualModeDefaults();
    settings.le_features |=
        static_cast<uint64_t>(bt::hci_spec::LESupportedFeature::
                                  kConnectedIsochronousStreamPeripheral);
    bthost::testing::AdapterTestFixture::SetUp(settings);
  }

  fble::Connection* client() { return client_.get(); }

  void UnbindClient() { client_.Unbind(); }

  bt::PeerId peer_id() const { return peer_id_; }

  bool server_closed_cb_called() const { return server_closed_cb_called_; }

  bt::hci_spec::ConnectionHandle connection_handle() {
    return connection_handle_;
  }

 protected:
  void EstablishConnectionAndStartServer() {
    // Since LowEnergyConnectionHandle must be created by
    // LowEnergyConnectionManager, we discover and connect to a fake peer to get
    // a LowEnergyConnectionHandle.
    std::unique_ptr<bt::testing::FakePeer> fake_peer =
        std::make_unique<bt::testing::FakePeer>(
            kTestAddr, pw_dispatcher(), /*connectable=*/true);
    test_device()->AddPeer(std::move(fake_peer));

    std::optional<bt::PeerId> peer_id;
    bt::gap::LowEnergyDiscoverySessionPtr session;
    adapter()->le()->StartDiscovery(
        /*active=*/true,
        {},
        [&](bt::gap::LowEnergyDiscoverySessionPtr cb_session) {
          session = std::move(cb_session);
          session->SetResultCallback(
              [&](const bt::gap::Peer& peer) { peer_id = peer.identifier(); });
        });
    RunLoopUntilIdle();
    PW_CHECK(peer_id);
    peer_id_ = *peer_id;

    std::optional<bt::gap::LowEnergyConnectionManager::ConnectionResult>
        conn_result;
    adapter()->le()->Connect(
        peer_id_,
        [&](bt::gap::LowEnergyConnectionManager::ConnectionResult result) {
          conn_result = std::move(result);
        },
        bt::gap::LowEnergyConnectionOptions());
    RunLoopUntilIdle();
    PW_CHECK(conn_result);
    PW_CHECK(conn_result->is_ok());
    std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection =
        std::move(*conn_result).value();

    connection_handle_ = connection->handle();

    // Start our FIDL connection server.
    fidl::InterfaceHandle<fuchsia::bluetooth::le::Connection> handle;
    server_ = std::make_unique<LowEnergyConnectionServer>(
        adapter()->AsWeakPtr(),
        gatt()->GetWeakPtr(),
        lease_provider_,
        std::move(connection),
        handle.NewRequest().TakeChannel(),
        /*closed_cb=*/[this] {
          server_closed_cb_called_ = true;
          server_.reset();
        });
    client_ = handle.Bind();
  }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<LowEnergyConnectionServer> server_;
  fble::ConnectionPtr client_;
  bool server_closed_cb_called_ = false;
  bt::PeerId peer_id_;
  bt::hci_spec::ConnectionHandle connection_handle_;
};

// Tests that want to automatically allocate and start the client and server
// before entering the test body.
class LowEnergyConnectionServerAutoStartTest
    : public LowEnergyConnectionServerTest {
 public:
  LowEnergyConnectionServerAutoStartTest() = default;
  ~LowEnergyConnectionServerAutoStartTest() override = default;

  void SetUp() override {
    LowEnergyConnectionServerTest::SetUp();
    EstablishConnectionAndStartServer();
  }

 protected:
  void RunGetCodecDelayRangeTest(
      fuchsia::bluetooth::le::CodecDelayGetCodecLocalDelayRangeRequest&& params,
      std::optional<zx_status_t> err);
};

TEST_F(LowEnergyConnectionServerAutoStartTest, RequestGattClientTwice) {
  fidl::InterfaceHandle<fuchsia::bluetooth::gatt2::Client> handle_0;
  client()->RequestGattClient(handle_0.NewRequest());
  fbg::ClientPtr client_0 = handle_0.Bind();
  std::optional<zx_status_t> client_epitaph_0;
  client_0.set_error_handler(
      [&](zx_status_t epitaph) { client_epitaph_0 = epitaph; });
  RunLoopUntilIdle();
  EXPECT_FALSE(client_epitaph_0);

  fidl::InterfaceHandle<fuchsia::bluetooth::gatt2::Client> handle_1;
  client()->RequestGattClient(handle_1.NewRequest());
  fbg::ClientPtr client_1 = handle_1.Bind();
  std::optional<zx_status_t> client_epitaph_1;
  client_1.set_error_handler(
      [&](zx_status_t epitaph) { client_epitaph_1 = epitaph; });
  RunLoopUntilIdle();
  EXPECT_FALSE(client_epitaph_0);
  ASSERT_TRUE(client_epitaph_1);
  EXPECT_EQ(client_epitaph_1.value(), ZX_ERR_ALREADY_BOUND);
}

TEST_F(LowEnergyConnectionServerAutoStartTest, GattClientServerError) {
  fidl::InterfaceHandle<fuchsia::bluetooth::gatt2::Client> handle_0;
  client()->RequestGattClient(handle_0.NewRequest());
  fbg::ClientPtr client_0 = handle_0.Bind();
  std::optional<zx_status_t> client_epitaph_0;
  client_0.set_error_handler(
      [&](zx_status_t epitaph) { client_epitaph_0 = epitaph; });
  RunLoopUntilIdle();
  EXPECT_FALSE(client_epitaph_0);

  // Calling WatchServices twice should cause a server error.
  client_0->WatchServices({}, [](auto, auto) {});
  client_0->WatchServices({}, [](auto, auto) {});
  RunLoopUntilIdle();
  EXPECT_TRUE(client_epitaph_0);

  // Requesting a new GATT client should succeed.
  fidl::InterfaceHandle<fuchsia::bluetooth::gatt2::Client> handle_1;
  client()->RequestGattClient(handle_1.NewRequest());
  fbg::ClientPtr client_1 = handle_1.Bind();
  std::optional<zx_status_t> client_epitaph_1;
  client_1.set_error_handler(
      [&](zx_status_t epitaph) { client_epitaph_1 = epitaph; });
  RunLoopUntilIdle();
  EXPECT_FALSE(client_epitaph_1);
}

TEST_F(LowEnergyConnectionServerAutoStartTest,
       RequestGattClientThenUnbindThenRequestAgainShouldSucceed) {
  fidl::InterfaceHandle<fuchsia::bluetooth::gatt2::Client> handle_0;
  client()->RequestGattClient(handle_0.NewRequest());
  fbg::ClientPtr client_0 = handle_0.Bind();
  std::optional<zx_status_t> client_epitaph_0;
  client_0.set_error_handler(
      [&](zx_status_t epitaph) { client_epitaph_0 = epitaph; });
  RunLoopUntilIdle();
  EXPECT_FALSE(client_epitaph_0);
  client_0.Unbind();
  RunLoopUntilIdle();

  // Requesting a new GATT client should succeed.
  fidl::InterfaceHandle<fuchsia::bluetooth::gatt2::Client> handle_1;
  client()->RequestGattClient(handle_1.NewRequest());
  fbg::ClientPtr client_1 = handle_1.Bind();
  std::optional<zx_status_t> client_epitaph_1;
  client_1.set_error_handler(
      [&](zx_status_t epitaph) { client_epitaph_1 = epitaph; });
  RunLoopUntilIdle();
  EXPECT_FALSE(client_epitaph_1);
}

static ::fuchsia::bluetooth::le::CodecDelayGetCodecLocalDelayRangeRequest
CreateDelayRangeRequestParams(bool has_vendor_config) {
  ::fuchsia::bluetooth::le::CodecDelayGetCodecLocalDelayRangeRequest params;

  // params.logical_transport_type
  params.set_logical_transport_type(
      fuchsia::bluetooth::LogicalTransportType::LE_CIS);

  // params.data_direction
  params.set_data_direction(fuchsia::bluetooth::DataDirection::INPUT);

  // params.codec_attributes
  if (has_vendor_config) {
    uint16_t kCompanyId = 0x1234;
    uint16_t kVendorId = 0xfedc;
    fuchsia::bluetooth::VendorCodingFormat vendor_coding_format;
    vendor_coding_format.set_company_id(kCompanyId);
    vendor_coding_format.set_vendor_id(kVendorId);
    fuchsia::bluetooth::CodecAttributes codec_attributes;
    codec_attributes.mutable_codec_id()->set_vendor_format(
        std::move(vendor_coding_format));
    std::vector<uint8_t> codec_configuration{0x4f, 0x77, 0x65, 0x6e};
    codec_attributes.set_codec_configuration(codec_configuration);
    params.set_codec_attributes(std::move(codec_attributes));
  } else {
    fuchsia::bluetooth::CodecAttributes codec_attributes;
    codec_attributes.mutable_codec_id()->set_assigned_format(
        fuchsia::bluetooth::AssignedCodingFormat::LINEAR_PCM);
    params.set_codec_attributes(std::move(codec_attributes));
  }

  return params;
}

void LowEnergyConnectionServerAutoStartTest::RunGetCodecDelayRangeTest(
    ::fuchsia::bluetooth::le::CodecDelayGetCodecLocalDelayRangeRequest&& params,
    std::optional<zx_status_t> err = std::nullopt) {
  fuchsia::bluetooth::le::CodecDelay_GetCodecLocalDelayRange_Result result;
  LowEnergyConnectionServer::GetCodecLocalDelayRangeCallback callback =
      [&](fuchsia::bluetooth::le::CodecDelay_GetCodecLocalDelayRange_Result
              cb_result) { result = std::move(cb_result); };
  client()->GetCodecLocalDelayRange(std::move(params), std::move(callback));
  RunLoopUntilIdle();

  if (err.has_value()) {
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.err(), err.value());
  } else {
    ASSERT_TRUE(result.is_response());
    auto& response = result.response();
    // These are the default values returned by the FakeController
    EXPECT_EQ(response.min_controller_delay(), zx::sec(0).get());
    EXPECT_EQ(response.max_controller_delay(), zx::sec(4).get());
  }
}

// Invoking GetCodecLocalDelay with a spec-defined coding format
TEST_F(LowEnergyConnectionServerAutoStartTest,
       GetCodecLocalDelaySpecCodingFormat) {
  ::fuchsia::bluetooth::le::CodecDelayGetCodecLocalDelayRangeRequest params =
      CreateDelayRangeRequestParams(/*has_vendor_config=*/false);
  RunGetCodecDelayRangeTest(std::move(params));
}

// Invoking GetCodecLocalDelay with a vendor-defined coding format
TEST_F(LowEnergyConnectionServerAutoStartTest,
       GetCodecLocalDelayVendorCodingFormat) {
  ::fuchsia::bluetooth::le::CodecDelayGetCodecLocalDelayRangeRequest params =
      CreateDelayRangeRequestParams(/*has_vendor_config=*/true);
  RunGetCodecDelayRangeTest(std::move(params));
}

// Invoking GetCodecLocalDelay with missing parameters
TEST_F(LowEnergyConnectionServerAutoStartTest,
       GetCodecLocalDelayMissingParams) {
  // Logical transport type missing
  ::fuchsia::bluetooth::le::CodecDelayGetCodecLocalDelayRangeRequest params =
      CreateDelayRangeRequestParams(/*has_vendor_config=*/false);
  params.clear_logical_transport_type();
  RunGetCodecDelayRangeTest(std::move(params), ZX_ERR_INVALID_ARGS);

  // Data direction is missing
  params = CreateDelayRangeRequestParams(/*has_vendor_config=*/false);
  params.clear_data_direction();
  RunGetCodecDelayRangeTest(std::move(params), ZX_ERR_INVALID_ARGS);

  // Codec attributes is missing
  params = CreateDelayRangeRequestParams(/*has_vendor_config=*/true);
  params.clear_codec_attributes();
  RunGetCodecDelayRangeTest(std::move(params), ZX_ERR_INVALID_ARGS);

  // codec_attributes.codec_id is missing
  params = CreateDelayRangeRequestParams(/*has_vendor_config=*/true);
  params.mutable_codec_attributes()->clear_codec_id();
  RunGetCodecDelayRangeTest(std::move(params), ZX_ERR_INVALID_ARGS);
}

// Calling GetCodecLocalDelay when the controller doesn't support it
TEST_F(LowEnergyConnectionServerAutoStartTest,
       GetCodecLocalDelayCommandNotSupported) {
  // Disable the Read Local Supported Controller Delay instruction
  bt::testing::FakeController::Settings settings;
  pw::bluetooth::emboss::MakeSupportedCommandsView(
      settings.supported_commands, sizeof(settings.supported_commands))
      .read_local_supported_controller_delay()
      .Write(false);
  test_device()->set_settings(settings);

  ::fuchsia::bluetooth::le::CodecDelayGetCodecLocalDelayRangeRequest params =
      CreateDelayRangeRequestParams(/*has_vendor_config=*/false);
  RunGetCodecDelayRangeTest(std::move(params), ZX_ERR_INTERNAL);
}

// Class that creates and manages an AcceptCis request and associated objects
class AcceptCisRequest {
 public:
  AcceptCisRequest(fble::Connection* connection_client,
                   bt::iso::CigCisIdentifier id) {
    fuchsia::bluetooth::le::ConnectionAcceptCisRequest params;
    params.set_cig_id(id.cig_id());
    params.set_cis_id(id.cis_id());
    params.set_connection_stream(stream_handle_.NewRequest());
    client_stream_ptr_ = stream_handle_.Bind();
    client_stream_ptr_.set_error_handler(
        [this](zx_status_t epitaph) { epitaph_ = epitaph; });
    connection_client->AcceptCis(std::move(params));
  }
  std::optional<zx_status_t> epitaph() { return epitaph_; }

 private:
  fidl::InterfaceHandle<fuchsia::bluetooth::le::IsochronousStream>
      stream_handle_;
  fuchsia::bluetooth::le::IsochronousStreamPtr client_stream_ptr_;
  std::optional<zx_status_t> epitaph_;
};

// Verify that all calls to AcceptCis() with unique CIG/CIS pairs are accepted
// and duplicate calls are rejected and the IsochronousStream handle is closed
// with an INVALID_ARGS epitaph.
TEST_F(LowEnergyConnectionServerTest, MultipleAcceptCisCalls) {
  // AcceptCis() should only be called on a connection where we are acting as
  // the peripheral.
  bt::testing::FakeController::Settings settings;
  settings.le_connection_role =
      pw::bluetooth::emboss::ConnectionRole::PERIPHERAL;
  test_device()->set_settings(settings);
  EstablishConnectionAndStartServer();

  AcceptCisRequest request1(client(), {/*cig_id=*/0x10, /*cis_id=*/0x08});
  AcceptCisRequest request2(client(), {/*cig_id=*/0x11, /*cis_id=*/0x08});
  AcceptCisRequest request3(client(), {/*cig_id=*/0x10, /*cis_id=*/0x07});
  AcceptCisRequest request1_dup(client(), {/*cig_id=*/0x10, /*cis_id=*/0x08});
  RunLoopUntilIdle();

  // All unique requests are pending
  EXPECT_FALSE(request1.epitaph());
  EXPECT_FALSE(request2.epitaph());
  EXPECT_FALSE(request3.epitaph());

  // Duplicate request is rejected
  ASSERT_TRUE(request1_dup.epitaph());
  EXPECT_EQ(*(request1_dup.epitaph()), ZX_ERR_INVALID_ARGS);
}

// Calling AcceptCis when we are the central should fail with
// ZX_ERR_NOT_SUPPORTED
TEST_F(LowEnergyConnectionServerTest, AcceptCisCalledFromCentral) {
  bt::testing::FakeController::Settings settings;
  settings.le_connection_role = pw::bluetooth::emboss::ConnectionRole::CENTRAL;
  test_device()->set_settings(settings);
  EstablishConnectionAndStartServer();

  AcceptCisRequest request(client(), {/*cig_id=*/0x10, /*cis_id=*/0x08});
  RunLoopUntilIdle();
  ASSERT_TRUE(request.epitaph());
  EXPECT_EQ(*(request.epitaph()), ZX_ERR_NOT_SUPPORTED);
}

// Test fixture where the controller does not support CIS.
class LowEnergyConnectionServerCisUnsupportedTest
    : public LowEnergyConnectionServerTest {
 public:
  void SetUp() override {
    bt::testing::FakeController::Settings settings;
    settings.ApplyDualModeDefaults();
    // Do not set kConnectedIsochronousStreamPeripheral feature bit.
    bthost::testing::AdapterTestFixture::SetUp(settings);
  }
};

// Calling AcceptCis when the controller does not support CIS should fail with
// ZX_ERR_NOT_SUPPORTED
TEST_F(LowEnergyConnectionServerCisUnsupportedTest,
       AcceptCisFailsWhenCisNotSupported) {
  bt::testing::FakeController::Settings settings;
  settings.le_connection_role =
      pw::bluetooth::emboss::ConnectionRole::PERIPHERAL;
  test_device()->set_settings(settings);
  EstablishConnectionAndStartServer();

  AcceptCisRequest request(client(), {/*cig_id=*/0x10, /*cis_id=*/0x08});
  RunLoopUntilIdle();
  ASSERT_TRUE(request.epitaph());
  EXPECT_EQ(*(request.epitaph()), ZX_ERR_NOT_SUPPORTED);
}

TEST_F(LowEnergyConnectionServerAutoStartTest, ServerClosedOnConnectionClosed) {
  adapter()->le()->Disconnect(peer_id());
  RunLoopUntilIdle();
  EXPECT_TRUE(server_closed_cb_called());
}

TEST_F(LowEnergyConnectionServerAutoStartTest,
       ServerClosedWhenFIDLClientClosesConnection) {
  UnbindClient();
  RunLoopUntilIdle();
  EXPECT_TRUE(server_closed_cb_called());
}

TEST_F(LowEnergyConnectionServerAutoStartTest, OpenL2capHappyDefault) {
  constexpr bt::l2cap::Psm kPsm = 15;
  constexpr bt::l2cap::ChannelParameters kExpectedChannelParameters{
      .mode = bt::l2cap::CreditBasedFlowControlMode::kLeCreditBasedFlowControl,
      .max_rx_sdu_size = std::nullopt,
      .flush_timeout = std::nullopt,
  };

  l2cap()->ExpectOutboundL2capChannel(
      connection_handle(), kPsm, 0x40, 0x41, kExpectedChannelParameters);

  std::optional<zx_status_t> error;
  fbt::ChannelPtr channel_client;
  channel_client.set_error_handler([&](auto status) { error = status; });
  auto request = std::move(fble::ConnectionConnectL2capRequest()
                               .set_parameters(fbt::ChannelParameters())
                               .set_psm(kPsm)
                               .set_channel(channel_client.NewRequest()));

  client()->ConnectL2cap(std::move(request));
  RunLoopUntilIdle();
  EXPECT_FALSE(error);
  EXPECT_TRUE(channel_client);
}

TEST_F(LowEnergyConnectionServerAutoStartTest, OpenL2capHappyParams) {
  constexpr bt::l2cap::Psm kPsm = 15;
  constexpr bt::l2cap::ChannelParameters kChannelParameters{
      .mode = bt::l2cap::CreditBasedFlowControlMode::kLeCreditBasedFlowControl,
      .max_rx_sdu_size = 32,
      .flush_timeout = std::nullopt,
  };

  l2cap()->ExpectOutboundL2capChannel(
      connection_handle(), kPsm, 0x40, 0x41, kChannelParameters);

  auto params = std::move(
      fbt::ChannelParameters()
          .set_channel_mode(fbt::ChannelMode::LE_CREDIT_BASED_FLOW_CONTROL)
          .set_max_rx_packet_size(*kChannelParameters.max_rx_sdu_size));

  std::optional<zx_status_t> error;
  fbt::ChannelPtr channel_client;
  channel_client.set_error_handler([&](auto status) { error = status; });
  auto request = std::move(fble::ConnectionConnectL2capRequest()
                               .set_parameters(std::move(params))
                               .set_psm(kPsm)
                               .set_channel(channel_client.NewRequest()));

  client()->ConnectL2cap(std::move(request));
  RunLoopUntilIdle();
  EXPECT_FALSE(error);
  EXPECT_TRUE(channel_client);
}

TEST_F(LowEnergyConnectionServerAutoStartTest, OpenL2capBadMode) {
  constexpr bt::l2cap::Psm kPsm = 15;
  auto params = std::move(
      fbt::ChannelParameters().set_channel_mode(fbt::ChannelMode::BASIC));

  std::optional<zx_status_t> error;
  fbt::ChannelPtr channel_client;
  channel_client.set_error_handler([&](auto status) { error = status; });
  auto request = std::move(fble::ConnectionConnectL2capRequest()
                               .set_parameters(std::move(params))
                               .set_psm(kPsm)
                               .set_channel(channel_client.NewRequest()));

  client()->ConnectL2cap(std::move(request));
  RunLoopUntilIdle();
  ASSERT_TRUE(error);
  EXPECT_EQ(*error, ZX_ERR_INVALID_ARGS);
  EXPECT_FALSE(channel_client);
}

TEST_F(LowEnergyConnectionServerAutoStartTest, OpenL2capFailFlushTimeout) {
  constexpr bt::l2cap::Psm kPsm = 15;
  auto params = std::move(fbt::ChannelParameters().set_flush_timeout(150));

  std::optional<zx_status_t> error;
  fbt::ChannelPtr channel_client;
  channel_client.set_error_handler([&](auto status) { error = status; });
  auto request = std::move(fble::ConnectionConnectL2capRequest()
                               .set_parameters(std::move(params))
                               .set_psm(kPsm)
                               .set_channel(channel_client.NewRequest()));

  client()->ConnectL2cap(std::move(request));
  RunLoopUntilIdle();
  ASSERT_TRUE(error);
  EXPECT_EQ(*error, ZX_ERR_INVALID_ARGS);
  EXPECT_FALSE(channel_client);
}

}  // namespace
}  // namespace bthost
