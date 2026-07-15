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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/connected_isochronous_group_server.h"

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/fake_adapter_test_fixture.h"
#include "pw_bluetooth_sapphire/internal/host/iso/fake_cig_stream_creator.h"
#include "pw_bluetooth_sapphire/internal/host/iso/fake_iso_group.h"

namespace bthost {
namespace {

namespace fble = fuchsia::bluetooth::le;

class ConnectedIsochronousGroupServerTest
    : public bt::fidl::testing::FakeAdapterTestFixture {
 public:
  ConnectedIsochronousGroupServerTest() = default;
  ~ConnectedIsochronousGroupServerTest() override = default;

  void SetUp() override {
    bt::fidl::testing::FakeAdapterTestFixture::SetUp();
    stream_creator_ =
        std::make_unique<bt::iso::testing::FakeCigStreamCreator>();
  }

  void TearDown() override {
    RunLoopUntilIdle();
    server_ = nullptr;
    stream_creator_ = nullptr;
    fake_cig_ = nullptr;
    fake_streams_.clear();
    bt::fidl::testing::FakeAdapterTestFixture::TearDown();
  }

 protected:
  std::unique_ptr<ConnectedIsochronousGroupServer>& server() { return server_; }
  std::unique_ptr<bt::iso::testing::FakeCigStreamCreator>& stream_creator() {
    return stream_creator_;
  }
  std::unique_ptr<bt::iso::testing::FakeIsoGroup>& fake_cig() {
    return fake_cig_;
  }

  void CreateServer(
      fidl::InterfaceRequest<fble::ConnectedIsochronousGroup> request,
      bt::iso::IsoGroup::WeakPtr iso_group = {},
      std::unordered_map<bt::hci_spec::CisIdentifier,
                         std::unique_ptr<IsoStreamServer>> stream_servers =
          {}) {
    server_ = std::make_unique<ConnectedIsochronousGroupServer>(
        adapter()->AsWeakPtr(),
        std::move(request),
        iso_group,
        std::move(stream_servers),
        [this](ConnectedIsochronousGroupServer&, zx_status_t status) {
          on_close_called_times_++;
          epitaph_ = status;
        });
  }

  bt::gap::Peer* SetupPeer() {
    auto* peer = adapter()->peer_cache()->NewPeer(
        bt::DeviceAddress(bt::DeviceAddress::Type::kLEPublic,
                          {1, 0, 0, 0, 0, 0}),
        /*connectable=*/true);
    // Peer supports CIS by default in tests
    peer->MutLe().SetFeatures(bt::hci_spec::LESupportedFeatures{
        static_cast<uint64_t>(bt::hci_spec::LESupportedFeature::
                                  kConnectedIsochronousStreamPeripheral)});
    return peer;
  }

  void SetupFakeCig() {
    fake_cig() = std::make_unique<bt::iso::testing::FakeIsoGroup>(
        /*id=*/1,
        bt::hci::Transport::WeakPtr(),
        stream_creator()->GetWeakPtr(),
        [](bt::iso::IsoGroup&) {});
  }

  void SetupStreamServers(
      std::unordered_map<bt::hci_spec::CisIdentifier,
                         std::unique_ptr<IsoStreamServer>>& out_servers,
      fidl::InterfaceHandle<fble::IsochronousStream>& out_handle,
      bt::hci_spec::CisIdentifier cis_id = 1) {
    auto stream_server =
        std::make_unique<IsoStreamServer>(out_handle.NewRequest(), [] {});

    auto stream_server_weak = stream_server->GetWeakPtr();
    bt::iso::CigCisParams cis_param;
    cis_param.config.cis_id = cis_id;
    cis_param.on_established_cb =
        [stream_server_weak](auto status, auto stream, const auto& params) {
          if (stream_server_weak.is_alive()) {
            if (status == pw::bluetooth::emboss::StatusCode::SUCCESS &&
                stream.has_value() && params.has_value()) {
              stream_server_weak->OnStreamEstablished(std::move(*stream),
                                                      std::move(*params));
            } else {
              stream_server_weak->OnStreamEstablishmentFailed(status);
            }
          }
        };
    fake_cig()->last_cis_params().push_back(std::move(cis_param));
    out_servers.emplace(cis_id, std::move(stream_server));
  }

  void TriggerFakeCisEstablished(bt::hci_spec::CisIdentifier cis_id,
                                 pw::bluetooth::emboss::StatusCode status) {
    bt::iso::CisEstablishedCallback cis_established_cb;
    for (auto& cis : fake_cig()->last_cis_params()) {
      if (cis.config.cis_id == cis_id) {
        cis_established_cb = std::move(cis.on_established_cb);
        break;
      }
    }
    ASSERT_TRUE(cis_established_cb);

    if (status == pw::bluetooth::emboss::StatusCode::SUCCESS) {
      auto fake_stream = std::make_unique<bt::iso::testing::FakeIsoStream>();
      auto weak_stream = fake_stream->GetWeakPtr();
      fake_streams_.emplace(cis_id, std::move(fake_stream));

      bt::iso::CisEstablishedParameters params;
      cis_established_cb(status, std::move(weak_stream), params);
    } else {
      cis_established_cb(status, std::nullopt, std::nullopt);
    }
  }

  uint32_t on_close_called_times_ = 0;
  std::optional<zx_status_t> epitaph_;

 private:
  std::unique_ptr<ConnectedIsochronousGroupServer> server_;
  std::unique_ptr<bt::iso::testing::FakeCigStreamCreator> stream_creator_;
  std::unique_ptr<bt::iso::testing::FakeIsoGroup> fake_cig_;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<bt::iso::testing::FakeIsoStream>>
      fake_streams_;
};

TEST_F(ConnectedIsochronousGroupServerTest, ClosedServerSide) {
  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(handle.NewRequest());

  server()->Close(ZX_ERR_WRONG_TYPE);
  RunLoopUntilIdle();

  EXPECT_EQ(on_close_called_times_, 1u);
  ASSERT_TRUE(epitaph_.has_value());
  EXPECT_EQ(epitaph_.value(), ZX_ERR_WRONG_TYPE);
}

TEST_F(ConnectedIsochronousGroupServerTest, RemoveClosedServerSide) {
  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(handle.NewRequest());

  auto client = handle.Bind();
  client->Remove();
  RunLoopUntilIdle();

  EXPECT_EQ(on_close_called_times_, 1u);
  ASSERT_TRUE(epitaph_.has_value());
  EXPECT_EQ(epitaph_.value(), ZX_OK);
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsSuccess) {
  SetupFakeCig();

  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;
  SetupStreamServers(stream_servers, stream_handle);

  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(
      handle.NewRequest(), fake_cig()->GetWeakPtr(), std::move(stream_servers));

  // Connect a fake peer to allow AddConnectionRef to succeed
  auto* peer = SetupPeer();
  bool connect_called = false;
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection_handle;
  adapter()->fake_le()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_TRUE(result.is_ok());
        connection_handle = std::move(result).value();
        connect_called = true;
      },
      bt::gap::LowEnergyConnectionOptions{});
  RunLoopUntilIdle();
  ASSERT_TRUE(connect_called);

  // Establish streams
  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters establish_param;
  establish_param.set_cis_id(1);
  establish_param.set_id(
      ::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params.push_back(std::move(establish_param));
  request.set_cis_params(std::move(establish_params));

  bool establish_called = false;
  fble::ConnectedIsochronousGroup_EstablishStreams_Result establish_result;
  auto client = handle.Bind();
  client->EstablishStreams(
      std::move(request),
      [&](fble::ConnectedIsochronousGroup_EstablishStreams_Result res) {
        establish_called = true;
        establish_result = std::move(res);
      });

  RunLoopUntilIdle();
  EXPECT_FALSE(establish_called);  // Waiting on asynchronous establishment!

  TriggerFakeCisEstablished(1, pw::bluetooth::emboss::StatusCode::SUCCESS);
  RunLoopUntilIdle();
  EXPECT_TRUE(establish_called);
  EXPECT_TRUE(establish_result.is_response());

  // Verify FakeIsoGroup received CreateCises call
  ASSERT_EQ(fake_cig()->last_create_cises_data().size(), 1u);
  EXPECT_EQ(fake_cig()->last_create_cises_data()[0].peer_id,
            peer->identifier());
  EXPECT_EQ(fake_cig()->last_create_cises_data()[0].cis_id, 1);
  EXPECT_EQ(fake_cig()->last_create_cises_data()[0].acl_handle, 1);
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsFailNoPeer) {
  SetupFakeCig();

  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;
  SetupStreamServers(stream_servers, stream_handle);

  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(
      handle.NewRequest(), fake_cig()->GetWeakPtr(), std::move(stream_servers));

  // Use a peer ID that is not in the cache
  bt::PeerId non_existent_peer_id(0x123456789);

  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters establish_param;
  establish_param.set_cis_id(1);
  establish_param.set_id(
      ::fuchsia::bluetooth::PeerId{non_existent_peer_id.value()});
  establish_params.push_back(std::move(establish_param));
  request.set_cis_params(std::move(establish_params));

  bool establish_called = false;
  fble::ConnectedIsochronousGroup_EstablishStreams_Result establish_result;
  auto client = handle.Bind();
  client->EstablishStreams(
      std::move(request),
      [&](fble::ConnectedIsochronousGroup_EstablishStreams_Result res) {
        establish_called = true;
        establish_result = std::move(res);
      });

  RunLoopUntilIdle();
  EXPECT_TRUE(establish_called);
  ASSERT_TRUE(establish_result.is_err());
  EXPECT_EQ(establish_result.err(),
            fble::EstablishStreamsError::PEER_NOT_CONNECTED);
}

TEST_F(ConnectedIsochronousGroupServerTest,
       EstablishStreamsFailNoLeConnection) {
  SetupFakeCig();

  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;
  SetupStreamServers(stream_servers, stream_handle);

  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(
      handle.NewRequest(), fake_cig()->GetWeakPtr(), std::move(stream_servers));

  // Setup peer but do NOT connect (so no LE connection reference can be
  // obtained)
  auto* peer = SetupPeer();

  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters establish_param;
  establish_param.set_cis_id(1);
  establish_param.set_id(
      ::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params.push_back(std::move(establish_param));
  request.set_cis_params(std::move(establish_params));

  bool establish_called = false;
  fble::ConnectedIsochronousGroup_EstablishStreams_Result establish_result;
  auto client = handle.Bind();
  client->EstablishStreams(
      std::move(request),
      [&](fble::ConnectedIsochronousGroup_EstablishStreams_Result res) {
        establish_called = true;
        establish_result = std::move(res);
      });

  RunLoopUntilIdle();
  EXPECT_TRUE(establish_called);
  ASSERT_TRUE(establish_result.is_err());
  EXPECT_EQ(establish_result.err(),
            fble::EstablishStreamsError::PEER_NOT_CONNECTED);
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsFailCreateCises) {
  SetupFakeCig();

  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;
  SetupStreamServers(stream_servers, stream_handle);

  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(
      handle.NewRequest(), fake_cig()->GetWeakPtr(), std::move(stream_servers));

  // Configure FakeIsoGroup to fail CreateCises
  fake_cig()->set_create_cises_result(pw::Status::Internal());

  // Connect a fake peer to allow AddConnectionRef to succeed
  auto* peer = SetupPeer();
  bool connect_called = false;
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection_handle;
  adapter()->fake_le()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_TRUE(result.is_ok());
        connection_handle = std::move(result).value();
        connect_called = true;
      },
      bt::gap::LowEnergyConnectionOptions{});
  RunLoopUntilIdle();
  ASSERT_TRUE(connect_called);

  // Establish streams
  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters establish_param;
  establish_param.set_cis_id(1);
  establish_param.set_id(
      ::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params.push_back(std::move(establish_param));
  request.set_cis_params(std::move(establish_params));

  bool error_handler_called = false;
  auto client = handle.Bind();
  client.set_error_handler([&](zx_status_t status) {
    error_handler_called = true;
    EXPECT_EQ(status, ZX_ERR_INTERNAL);
  });
  client->EstablishStreams(
      std::move(request),
      [&](fble::ConnectedIsochronousGroup_EstablishStreams_Result) {
        FAIL() << "EstablishStreams callback should not be called";
      });

  RunLoopUntilIdle();
  EXPECT_TRUE(error_handler_called);
  EXPECT_EQ(on_close_called_times_, 1u);
  ASSERT_TRUE(epitaph_.has_value());
  EXPECT_EQ(epitaph_.value(), ZX_ERR_INTERNAL);
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsPassesPeerSca) {
  SetupFakeCig();

  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;
  SetupStreamServers(stream_servers, stream_handle);

  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(
      handle.NewRequest(), fake_cig()->GetWeakPtr(), std::move(stream_servers));

  // Connect a fake peer
  auto* peer = SetupPeer();

  // Set peer SCA
  peer->MutLe().set_sleep_clock_accuracy(
      pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_31_TO_50);

  bool connect_called = false;
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection_handle;
  adapter()->fake_le()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_TRUE(result.is_ok());
        connection_handle = std::move(result).value();
        connect_called = true;
      },
      bt::gap::LowEnergyConnectionOptions{});
  RunLoopUntilIdle();
  ASSERT_TRUE(connect_called);

  // Establish streams
  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters establish_param;
  establish_param.set_cis_id(1);
  establish_param.set_id(
      ::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params.push_back(std::move(establish_param));
  request.set_cis_params(std::move(establish_params));

  bool establish_called = false;
  fble::ConnectedIsochronousGroup_EstablishStreams_Result establish_result;
  auto client = handle.Bind();
  client->EstablishStreams(
      std::move(request),
      [&](fble::ConnectedIsochronousGroup_EstablishStreams_Result res) {
        establish_called = true;
        establish_result = std::move(res);
      });

  RunLoopUntilIdle();
  EXPECT_FALSE(establish_called);

  TriggerFakeCisEstablished(1, pw::bluetooth::emboss::StatusCode::SUCCESS);
  RunLoopUntilIdle();
  EXPECT_TRUE(establish_called);
  EXPECT_TRUE(establish_result.is_response());

  // Verify FakeIsoGroup received the correct SCA
  ASSERT_EQ(fake_cig()->last_create_cises_data().size(), 1u);
  ASSERT_TRUE(fake_cig()->last_create_cises_data()[0].sca.has_value());
  EXPECT_EQ(fake_cig()->last_create_cises_data()[0].sca.value(),
            pw::bluetooth::emboss::LESleepClockAccuracyRange::PPM_31_TO_50);
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsFailCisCallback) {
  SetupFakeCig();

  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;
  SetupStreamServers(stream_servers, stream_handle);

  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(
      handle.NewRequest(), fake_cig()->GetWeakPtr(), std::move(stream_servers));

  // Connect a fake peer to allow AddConnectionRef to succeed
  auto* peer = SetupPeer();
  bool connect_called = false;
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection_handle;
  adapter()->fake_le()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_TRUE(result.is_ok());
        connection_handle = std::move(result).value();
        connect_called = true;
      },
      bt::gap::LowEnergyConnectionOptions{});
  RunLoopUntilIdle();
  ASSERT_TRUE(connect_called);

  // Bind the stream client and set its error handler to verify it gets closed
  auto stream_client = stream_handle.Bind();
  std::optional<zx_status_t> stream_epitaph;
  stream_client.set_error_handler(
      [&](zx_status_t status) { stream_epitaph = status; });

  // Establish streams
  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters establish_param;
  establish_param.set_cis_id(1);
  establish_param.set_id(
      ::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params.push_back(std::move(establish_param));
  request.set_cis_params(std::move(establish_params));

  bool establish_called = false;
  fble::ConnectedIsochronousGroup_EstablishStreams_Result establish_result;
  auto client = handle.Bind();
  client->EstablishStreams(
      std::move(request),
      [&](fble::ConnectedIsochronousGroup_EstablishStreams_Result res) {
        establish_called = true;
        establish_result = std::move(res);
      });

  RunLoopUntilIdle();
  EXPECT_FALSE(establish_called);

  // Verify connection has 2 handles (one in test, one in server)
  {
    auto conn_iter =
        adapter()->fake_le()->connections().find(peer->identifier());
    ASSERT_NE(conn_iter, adapter()->fake_le()->connections().end());
    EXPECT_EQ(conn_iter->second.handles.size(), 2u);
  }

  // Simulate CIS establishment failure callback by invoking the callback with
  // UNSPECIFIED_ERROR (which is unmapped)
  TriggerFakeCisEstablished(
      1, pw::bluetooth::emboss::StatusCode::UNSPECIFIED_ERROR);
  RunLoopUntilIdle();

  // Verify EstablishStreams failed with NOT_SUPPORTED
  EXPECT_TRUE(establish_called);
  ASSERT_TRUE(establish_result.is_err());
  EXPECT_EQ(establish_result.err(), fble::EstablishStreamsError::NOT_SUPPORTED);
  EXPECT_EQ(on_close_called_times_, 0u);

  // Verify stream channel was closed with ZX_ERR_INTERNAL
  ASSERT_TRUE(stream_epitaph.has_value());
  EXPECT_EQ(*stream_epitaph, ZX_ERR_INTERNAL);
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsDuplicateCisError) {
  SetupFakeCig();

  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;
  SetupStreamServers(stream_servers, stream_handle);

  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(
      handle.NewRequest(), fake_cig()->GetWeakPtr(), std::move(stream_servers));

  auto* peer = SetupPeer();
  bool connect_called = false;
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection_handle;
  adapter()->fake_le()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_TRUE(result.is_ok());
        connection_handle = std::move(result).value();
        connect_called = true;
      },
      bt::gap::LowEnergyConnectionOptions{});
  RunLoopUntilIdle();
  ASSERT_TRUE(connect_called);

  // 1. Send request with duplicate CIS ID (1) in the same call
  {
    fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
    std::vector<fble::CisParameters> establish_params;
    fble::CisParameters param1;
    param1.set_cis_id(1);
    param1.set_id(::fuchsia::bluetooth::PeerId{peer->identifier().value()});
    establish_params.push_back(std::move(param1));

    fble::CisParameters param2;
    param2.set_cis_id(1);
    param2.set_id(::fuchsia::bluetooth::PeerId{peer->identifier().value()});
    establish_params.push_back(std::move(param2));

    request.set_cis_params(std::move(establish_params));

    bool cb_called = false;
    fble::ConnectedIsochronousGroup_EstablishStreams_Result res;
    auto client = handle.Bind();
    client->EstablishStreams(std::move(request), [&](auto r) {
      cb_called = true;
      res = std::move(r);
    });
    RunLoopUntilIdle();
    ASSERT_TRUE(cb_called);
    ASSERT_TRUE(res.is_err());
    EXPECT_EQ(res.err(), fble::EstablishStreamsError::DUPLICATE_CIS);
    handle = client.Unbind();
  }

  // 2. Setup first call succeeding, and second call sending duplicate
  {
    fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
    std::vector<fble::CisParameters> establish_params;
    fble::CisParameters param1;
    param1.set_cis_id(1);
    param1.set_id(::fuchsia::bluetooth::PeerId{peer->identifier().value()});
    establish_params.push_back(std::move(param1));
    request.set_cis_params(std::move(establish_params));

    bool cb_called = false;
    fble::ConnectedIsochronousGroup_EstablishStreams_Result res;
    auto client = handle.Bind();
    client->EstablishStreams(std::move(request), [&](auto r) {
      cb_called = true;
      res = std::move(r);
    });
    RunLoopUntilIdle();
    EXPECT_FALSE(cb_called);

    TriggerFakeCisEstablished(1, pw::bluetooth::emboss::StatusCode::SUCCESS);
    RunLoopUntilIdle();
    ASSERT_TRUE(cb_called);
    ASSERT_TRUE(res.is_response());

    // Send another request with the same CIS ID (1), which is now already
    // established
    fble::ConnectedIsochronousGroupEstablishStreamsRequest dup_request;
    std::vector<fble::CisParameters> dup_params;
    fble::CisParameters dup_param;
    dup_param.set_cis_id(1);
    dup_param.set_id(::fuchsia::bluetooth::PeerId{peer->identifier().value()});
    dup_params.push_back(std::move(dup_param));
    dup_request.set_cis_params(std::move(dup_params));

    bool dup_cb_called = false;
    fble::ConnectedIsochronousGroup_EstablishStreams_Result dup_res;
    client->EstablishStreams(std::move(dup_request), [&](auto r) {
      dup_cb_called = true;
      dup_res = std::move(r);
    });
    RunLoopUntilIdle();
    ASSERT_TRUE(dup_cb_called);
    ASSERT_TRUE(dup_res.is_err());
    EXPECT_EQ(dup_res.err(),
              fble::EstablishStreamsError::CIS_ALREADY_ESTABLISHED);
  }
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsFailNotSupported) {
  SetupFakeCig();

  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;
  SetupStreamServers(stream_servers, stream_handle);

  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(
      handle.NewRequest(), fake_cig()->GetWeakPtr(), std::move(stream_servers));

  // Connect a peer
  auto* peer = SetupPeer();

  // Set features to NOT support ConnectedIsochronousStreamPeripheral
  peer->MutLe().SetFeatures(bt::hci_spec::LESupportedFeatures{0});

  bool connect_called = false;
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection_handle;
  adapter()->fake_le()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_TRUE(result.is_ok());
        connection_handle = std::move(result).value();
        connect_called = true;
      },
      bt::gap::LowEnergyConnectionOptions{});
  RunLoopUntilIdle();
  ASSERT_TRUE(connect_called);

  // Establish streams
  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters establish_param;
  establish_param.set_cis_id(1);
  establish_param.set_id(
      ::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params.push_back(std::move(establish_param));
  request.set_cis_params(std::move(establish_params));

  bool establish_called = false;
  fble::ConnectedIsochronousGroup_EstablishStreams_Result establish_result;
  auto client = handle.Bind();
  client->EstablishStreams(
      std::move(request),
      [&](fble::ConnectedIsochronousGroup_EstablishStreams_Result res) {
        establish_called = true;
        establish_result = std::move(res);
      });

  RunLoopUntilIdle();
  EXPECT_TRUE(establish_called);
  ASSERT_TRUE(establish_result.is_err());
  EXPECT_EQ(establish_result.err(), fble::EstablishStreamsError::NOT_SUPPORTED);
}

TEST_F(ConnectedIsochronousGroupServerTest,
       EstablishStreamsFailPeerParametersOutOfRange) {
  SetupFakeCig();

  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;
  SetupStreamServers(stream_servers, stream_handle);

  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(
      handle.NewRequest(), fake_cig()->GetWeakPtr(), std::move(stream_servers));

  // Configure FakeIsoGroup to fail CreateCises with OutOfRange
  fake_cig()->set_create_cises_result(pw::Status::OutOfRange());

  auto* peer = SetupPeer();
  bool connect_called = false;
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection_handle;
  adapter()->fake_le()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_TRUE(result.is_ok());
        connection_handle = std::move(result).value();
        connect_called = true;
      },
      bt::gap::LowEnergyConnectionOptions{});
  RunLoopUntilIdle();
  ASSERT_TRUE(connect_called);

  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters establish_param;
  establish_param.set_cis_id(1);
  establish_param.set_id(
      ::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params.push_back(std::move(establish_param));
  request.set_cis_params(std::move(establish_params));

  bool establish_called = false;
  fble::ConnectedIsochronousGroup_EstablishStreams_Result establish_result;
  auto client = handle.Bind();
  client->EstablishStreams(
      std::move(request),
      [&](fble::ConnectedIsochronousGroup_EstablishStreams_Result res) {
        establish_called = true;
        establish_result = std::move(res);
      });

  RunLoopUntilIdle();
  EXPECT_TRUE(establish_called);
  ASSERT_TRUE(establish_result.is_err());
  EXPECT_EQ(establish_result.err(),
            fble::EstablishStreamsError::PEER_PARAMETERS_OUT_OF_RANGE);
}

TEST_F(ConnectedIsochronousGroupServerTest,
       EstablishStreamsMultiStreamFailClosesAll) {
  SetupFakeCig();

  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle1;
  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle2;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;
  SetupStreamServers(stream_servers, stream_handle1, /*cis_id=*/1);
  SetupStreamServers(stream_servers, stream_handle2, /*cis_id=*/2);

  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(
      handle.NewRequest(), fake_cig()->GetWeakPtr(), std::move(stream_servers));

  auto* peer = SetupPeer();
  bool connect_called = false;
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection_handle;
  adapter()->fake_le()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_TRUE(result.is_ok());
        connection_handle = std::move(result).value();
        connect_called = true;
      },
      bt::gap::LowEnergyConnectionOptions{});
  RunLoopUntilIdle();
  ASSERT_TRUE(connect_called);

  // Bind both stream clients and record their closures
  auto stream_client1 = stream_handle1.Bind();
  std::optional<zx_status_t> stream_epitaph1;
  stream_client1.set_error_handler(
      [&](zx_status_t status) { stream_epitaph1 = status; });

  auto stream_client2 = stream_handle2.Bind();
  std::optional<zx_status_t> stream_epitaph2;
  stream_client2.set_error_handler(
      [&](zx_status_t status) { stream_epitaph2 = status; });

  // Request to establish both CIS 1 and CIS 2
  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;

  fble::CisParameters param1;
  param1.set_cis_id(1);
  param1.set_id(::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params.push_back(std::move(param1));

  fble::CisParameters param2;
  param2.set_cis_id(2);
  param2.set_id(::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params.push_back(std::move(param2));

  request.set_cis_params(std::move(establish_params));

  bool establish_called = false;
  fble::ConnectedIsochronousGroup_EstablishStreams_Result establish_result;
  auto client = handle.Bind();
  client->EstablishStreams(
      std::move(request),
      [&](fble::ConnectedIsochronousGroup_EstablishStreams_Result res) {
        establish_called = true;
        establish_result = std::move(res);
      });

  RunLoopUntilIdle();
  EXPECT_FALSE(establish_called);

  // Simulate CIS 1 failing to establish (e.g. UNSPECIFIED_ERROR)
  TriggerFakeCisEstablished(
      1, pw::bluetooth::emboss::StatusCode::UNSPECIFIED_ERROR);
  RunLoopUntilIdle();

  // Verify EstablishStreams fails with NOT_SUPPORTED
  EXPECT_TRUE(establish_called);
  ASSERT_TRUE(establish_result.is_err());
  EXPECT_EQ(establish_result.err(), fble::EstablishStreamsError::NOT_SUPPORTED);

  // Both stream channels should have been closed with ZX_ERR_INTERNAL
  ASSERT_TRUE(stream_epitaph1.has_value());
  EXPECT_EQ(*stream_epitaph1, ZX_ERR_INTERNAL);

  ASSERT_TRUE(stream_epitaph2.has_value());
  EXPECT_EQ(*stream_epitaph2, ZX_ERR_INTERNAL);

  // The CIG channel itself should remain open
  EXPECT_EQ(on_close_called_times_, 0u);
}

TEST_F(ConnectedIsochronousGroupServerTest,
       EstablishStreamsConcurrentCallsError) {
  SetupFakeCig();

  fidl::InterfaceHandle<fble::IsochronousStream> stream_handle;
  std::unordered_map<bt::hci_spec::CisIdentifier,
                     std::unique_ptr<IsoStreamServer>>
      stream_servers;
  SetupStreamServers(stream_servers, stream_handle);

  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(
      handle.NewRequest(), fake_cig()->GetWeakPtr(), std::move(stream_servers));

  auto* peer = SetupPeer();
  bool connect_called = false;
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection_handle;
  adapter()->fake_le()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_TRUE(result.is_ok());
        connection_handle = std::move(result).value();
        connect_called = true;
      },
      bt::gap::LowEnergyConnectionOptions{});
  RunLoopUntilIdle();
  ASSERT_TRUE(connect_called);

  fble::ConnectedIsochronousGroupEstablishStreamsRequest request1;
  std::vector<fble::CisParameters> establish_params1;
  fble::CisParameters param1;
  param1.set_cis_id(1);
  param1.set_id(::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params1.push_back(std::move(param1));
  request1.set_cis_params(std::move(establish_params1));

  auto client = handle.Bind();
  bool first_called = false;
  client->EstablishStreams(std::move(request1),
                           [&](auto) { first_called = true; });

  // Call EstablishStreams again concurrently
  fble::ConnectedIsochronousGroupEstablishStreamsRequest request2;
  std::vector<fble::CisParameters> establish_params2;
  fble::CisParameters param2;
  param2.set_cis_id(1);
  param2.set_id(::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params2.push_back(std::move(param2));
  request2.set_cis_params(std::move(establish_params2));

  bool error_handler_called = false;
  client.set_error_handler([&](zx_status_t status) {
    error_handler_called = true;
    EXPECT_EQ(status, ZX_ERR_BAD_STATE);
  });

  client->EstablishStreams(std::move(request2), [&](auto) {
    FAIL() << "Second EstablishStreams should fail and close channel";
  });

  RunLoopUntilIdle();

  EXPECT_TRUE(error_handler_called);
  EXPECT_FALSE(first_called);
  EXPECT_EQ(on_close_called_times_, 1u);
  ASSERT_TRUE(epitaph_.has_value());
  EXPECT_EQ(epitaph_.value(), ZX_ERR_BAD_STATE);
}

TEST_F(ConnectedIsochronousGroupServerTest, CigIdGetter) {
  // Initially no group, should be nullopt
  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(handle.NewRequest());
  EXPECT_FALSE(server()->cig_id().has_value());

  // Set up server with a live group
  SetupFakeCig();
  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle2;
  CreateServer(handle2.NewRequest(), fake_cig()->GetWeakPtr());
  ASSERT_TRUE(server()->cig_id().has_value());
  EXPECT_EQ(server()->cig_id().value(), 1);
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsIsoGroupDead) {
  // Create server without a group
  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(handle.NewRequest());

  auto client = handle.Bind();
  bool error_handler_called = false;
  client.set_error_handler([&](zx_status_t status) {
    error_handler_called = true;
    EXPECT_EQ(status, ZX_ERR_INTERNAL);
  });

  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters param;
  param.set_cis_id(1);
  param.set_id(::fuchsia::bluetooth::PeerId{1});
  establish_params.push_back(std::move(param));
  request.set_cis_params(std::move(establish_params));

  client->EstablishStreams(std::move(request), [](auto) {});
  RunLoopUntilIdle();

  EXPECT_TRUE(error_handler_called);
  EXPECT_EQ(on_close_called_times_, 1u);
  EXPECT_EQ(epitaph_, ZX_ERR_INTERNAL);
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsEmptyCisParams) {
  SetupFakeCig();
  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(handle.NewRequest(), fake_cig()->GetWeakPtr());

  auto client = handle.Bind();
  bool error_handler_called = false;
  client.set_error_handler([&](zx_status_t status) {
    error_handler_called = true;
    EXPECT_EQ(status, ZX_ERR_INVALID_ARGS);
  });

  // Empty request (no cis_params set)
  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  client->EstablishStreams(std::move(request), [](auto) {});
  RunLoopUntilIdle();

  EXPECT_TRUE(error_handler_called);
  EXPECT_EQ(on_close_called_times_, 1u);
  EXPECT_EQ(epitaph_, ZX_ERR_INVALID_ARGS);

  // Request with empty vector
  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle2;
  CreateServer(handle2.NewRequest(), fake_cig()->GetWeakPtr());
  auto client2 = handle2.Bind();
  bool error_handler_called2 = false;
  client2.set_error_handler([&](zx_status_t status) {
    error_handler_called2 = true;
    EXPECT_EQ(status, ZX_ERR_INVALID_ARGS);
  });

  fble::ConnectedIsochronousGroupEstablishStreamsRequest request2;
  request2.set_cis_params({});
  client2->EstablishStreams(std::move(request2), [](auto) {});
  RunLoopUntilIdle();

  EXPECT_TRUE(error_handler_called2);
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsMissingCisId) {
  SetupFakeCig();
  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(handle.NewRequest(), fake_cig()->GetWeakPtr());

  auto client = handle.Bind();
  bool error_handler_called = false;
  client.set_error_handler([&](zx_status_t status) {
    error_handler_called = true;
    EXPECT_EQ(status, ZX_ERR_INVALID_ARGS);
  });

  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters param;
  // Only set peer ID, missing CIS ID
  param.set_id(::fuchsia::bluetooth::PeerId{1});
  establish_params.push_back(std::move(param));
  request.set_cis_params(std::move(establish_params));

  client->EstablishStreams(std::move(request), [](auto) {});
  RunLoopUntilIdle();

  EXPECT_TRUE(error_handler_called);
  EXPECT_EQ(on_close_called_times_, 1u);
  EXPECT_EQ(epitaph_, ZX_ERR_INVALID_ARGS);
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsMissingPeerId) {
  SetupFakeCig();
  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(handle.NewRequest(), fake_cig()->GetWeakPtr());

  auto client = handle.Bind();
  bool error_handler_called = false;
  client.set_error_handler([&](zx_status_t status) {
    error_handler_called = true;
    EXPECT_EQ(status, ZX_ERR_INVALID_ARGS);
  });

  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters param;
  // Only set CIS ID, missing peer ID
  param.set_cis_id(1);
  establish_params.push_back(std::move(param));
  request.set_cis_params(std::move(establish_params));

  client->EstablishStreams(std::move(request), [](auto) {});
  RunLoopUntilIdle();

  EXPECT_TRUE(error_handler_called);
  EXPECT_EQ(on_close_called_times_, 1u);
  EXPECT_EQ(epitaph_, ZX_ERR_INVALID_ARGS);
}

TEST_F(ConnectedIsochronousGroupServerTest, EstablishStreamsUntrackedCisId) {
  SetupFakeCig();
  // Server set up with NO stream servers registered
  fidl::InterfaceHandle<fble::ConnectedIsochronousGroup> handle;
  CreateServer(handle.NewRequest(), fake_cig()->GetWeakPtr(), {});

  auto* peer = SetupPeer();
  bool connect_called = false;
  std::unique_ptr<bt::gap::LowEnergyConnectionHandle> connection_handle;
  adapter()->fake_le()->Connect(
      peer->identifier(),
      [&](auto result) {
        ASSERT_TRUE(result.is_ok());
        connection_handle = std::move(result).value();
        connect_called = true;
      },
      bt::gap::LowEnergyConnectionOptions{});
  RunLoopUntilIdle();
  ASSERT_TRUE(connect_called);

  auto client = handle.Bind();
  bool error_handler_called = false;
  client.set_error_handler([&](zx_status_t status) {
    error_handler_called = true;
    EXPECT_EQ(status, ZX_ERR_INVALID_ARGS);
  });

  fble::ConnectedIsochronousGroupEstablishStreamsRequest request;
  std::vector<fble::CisParameters> establish_params;
  fble::CisParameters param;
  param.set_cis_id(1);  // Untracked
  param.set_id(::fuchsia::bluetooth::PeerId{peer->identifier().value()});
  establish_params.push_back(std::move(param));
  request.set_cis_params(std::move(establish_params));

  client->EstablishStreams(std::move(request), [](auto) {});
  RunLoopUntilIdle();

  EXPECT_TRUE(error_handler_called);
  EXPECT_EQ(on_close_called_times_, 1u);
  EXPECT_EQ(epitaph_, ZX_ERR_INVALID_ARGS);
}

}  // namespace
}  // namespace bthost
