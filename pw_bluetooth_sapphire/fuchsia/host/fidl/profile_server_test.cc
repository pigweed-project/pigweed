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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/profile_server.h"

#include <fuchsia/bluetooth/bredr/cpp/fidl_test_base.h>
#include <gmock/gmock.h>
#include <zircon/errors.h>

#include <memory>

#include "fuchsia/bluetooth/bredr/cpp/fidl.h"
#include "fuchsia/bluetooth/cpp/fidl.h"
#include "lib/fidl/cpp/vector.h"
#include "lib/zx/socket.h"
#include "pw_bluetooth_sapphire/fake_lease_provider.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/adapter_test_fixture.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/fake_adapter_test_fixture.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/helpers.h"
#include "pw_bluetooth_sapphire/internal/host/common/host_error.h"
#include "pw_bluetooth_sapphire/internal/host/gap/fake_pairing_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_l2cap.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/data_element.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/sdp.h"
#include "pw_bluetooth_sapphire/internal/host/testing/fake_peer.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_unit_test/framework.h"

namespace bthost {
namespace {

namespace fbt = fuchsia::bluetooth;
namespace fidlbredr = fuchsia::bluetooth::bredr;
namespace android_emb = pw::bluetooth::vendor::android_hci;

using bt::l2cap::testing::FakeChannel;
using pw::bluetooth::AclPriority;
using FeaturesBits = pw::bluetooth::Controller::FeaturesBits;

void NopAdvertiseCallback(fidlbredr::Profile_Advertise_Result) {}

const bt::DeviceAddress kTestDevAddr(bt::DeviceAddress::Type::kBREDR, {1});
constexpr bt::l2cap::Psm kPsm = bt::l2cap::kAVDTP;

constexpr uint16_t kSynchronousDataPacketLength = 64;
constexpr uint8_t kTotalNumSynchronousDataPackets = 1;

fidlbredr::ScoConnectionParameters CreateScoConnectionParameters(
    fidlbredr::HfpParameterSet param_set = fidlbredr::HfpParameterSet::T2) {
  fidlbredr::ScoConnectionParameters params;
  params.set_parameter_set(param_set);
  params.set_air_coding_format(fbt::AssignedCodingFormat::MSBC);
  params.set_air_frame_size(8u);
  params.set_io_bandwidth(32000);
  params.set_io_coding_format(fbt::AssignedCodingFormat::LINEAR_PCM);
  params.set_io_frame_size(16u);
  params.set_io_pcm_data_format(
      fuchsia::hardware::audio::SampleFormat::PCM_SIGNED);
  params.set_io_pcm_sample_payload_msb_position(3u);
  params.set_path(fidlbredr::DataPath::OFFLOAD);
  return params;
}

fidlbredr::ServiceDefinition MakeFIDLServiceDefinition() {
  fidlbredr::ServiceDefinition def;
  def.mutable_service_class_uuids()->emplace_back(
      fidl_helpers::UuidToFidl(bt::sdp::profile::kAudioSink));

  fidlbredr::ProtocolDescriptor l2cap_proto;
  l2cap_proto.set_protocol(fidlbredr::ProtocolIdentifier::L2CAP);
  fidlbredr::DataElement l2cap_data_el;
  l2cap_data_el.set_uint16(fidlbredr::PSM_AVDTP);
  std::vector<fidlbredr::DataElement> l2cap_params;
  l2cap_params.emplace_back(std::move(l2cap_data_el));
  l2cap_proto.set_params(std::move(l2cap_params));

  def.mutable_protocol_descriptor_list()->emplace_back(std::move(l2cap_proto));

  fidlbredr::ProtocolDescriptor avdtp_proto;
  avdtp_proto.set_protocol(fidlbredr::ProtocolIdentifier::AVDTP);
  fidlbredr::DataElement avdtp_data_el;
  avdtp_data_el.set_uint16(0x0103);  // Version 1.3
  std::vector<fidlbredr::DataElement> avdtp_params;
  avdtp_params.emplace_back(std::move(avdtp_data_el));
  avdtp_proto.set_params(std::move(avdtp_params));

  def.mutable_protocol_descriptor_list()->emplace_back(std::move(avdtp_proto));

  fidlbredr::ProfileDescriptor prof_desc;
  prof_desc.set_profile_id(
      fidlbredr::ServiceClassProfileIdentifier::ADVANCED_AUDIO_DISTRIBUTION);
  prof_desc.set_major_version(1);
  prof_desc.set_minor_version(3);
  def.mutable_profile_descriptors()->emplace_back(std::move(prof_desc));

  // Additional attributes are also OK.
  fidlbredr::Attribute addl_attr;
  addl_attr.set_id(0x000A);  // Documentation URL ID
  fidlbredr::DataElement doc_url_el;
  doc_url_el.set_url("fuchsia.dev");
  addl_attr.set_element(std::move(doc_url_el));
  def.mutable_additional_attributes()->emplace_back(std::move(addl_attr));

  return def;
}

fidlbredr::ServiceDefinition MakeMapMceServiceDefinition() {
  // MAP MCE service definition requires RFCOMM and OBEX.
  fidlbredr::ServiceDefinition def;
  def.mutable_service_class_uuids()->emplace_back(
      fidl_helpers::UuidToFidl(bt::sdp::profile::kMessageNotificationServer));

  // [[L2CAP], [RFCOMM, Channel#], [OBEX]]
  fidlbredr::ProtocolDescriptor l2cap_proto;
  l2cap_proto.set_protocol(fidlbredr::ProtocolIdentifier::L2CAP);
  std::vector<fidlbredr::DataElement> l2cap_params;
  l2cap_proto.set_params(std::move(l2cap_params));
  def.mutable_protocol_descriptor_list()->emplace_back(std::move(l2cap_proto));
  fidlbredr::ProtocolDescriptor rfcomm_proto;
  rfcomm_proto.set_protocol(fidlbredr::ProtocolIdentifier::RFCOMM);
  fidlbredr::DataElement rfcomm_data_el;
  rfcomm_data_el.set_uint8(5);  // Random RFCOMM channel
  std::vector<fidlbredr::DataElement> rfcomm_params;
  rfcomm_params.emplace_back(std::move(rfcomm_data_el));
  rfcomm_proto.set_params(std::move(rfcomm_params));
  def.mutable_protocol_descriptor_list()->emplace_back(std::move(rfcomm_proto));
  fidlbredr::ProtocolDescriptor obex_proto;
  obex_proto.set_protocol(fidlbredr::ProtocolIdentifier::OBEX);
  std::vector<fidlbredr::DataElement> obex_params;
  obex_proto.set_params(std::move(obex_params));
  def.mutable_protocol_descriptor_list()->emplace_back(std::move(obex_proto));

  // Additional protocols. NOTE: This is fictional and not part of a real MCE
  // definition.
  std::vector<fidlbredr::ProtocolDescriptor> additional_proto;
  fidlbredr::ProtocolDescriptor additional_l2cap_proto;
  additional_l2cap_proto.set_protocol(fidlbredr::ProtocolIdentifier::L2CAP);
  fidlbredr::DataElement additional_l2cap_data_el;
  additional_l2cap_data_el.set_uint16(fidlbredr::PSM_DYNAMIC);
  std::vector<fidlbredr::DataElement> additional_l2cap_params;
  additional_l2cap_params.emplace_back(std::move(additional_l2cap_data_el));
  additional_l2cap_proto.set_params(std::move(additional_l2cap_params));
  additional_proto.emplace_back(std::move(additional_l2cap_proto));
  fidlbredr::ProtocolDescriptor additional_obex_proto;
  additional_obex_proto.set_protocol(fidlbredr::ProtocolIdentifier::OBEX);
  std::vector<fidlbredr::DataElement> additional_obex_params;
  additional_obex_proto.set_params(std::move(additional_obex_params));
  additional_proto.emplace_back(std::move(additional_obex_proto));
  def.mutable_additional_protocol_descriptor_lists()->emplace_back(
      std::move(additional_proto));

  fidlbredr::Information info;
  info.set_language("en");
  info.set_name("foo_test");
  def.mutable_information()->emplace_back(std::move(info));

  fidlbredr::ProfileDescriptor prof_desc;
  prof_desc.set_profile_id(
      fidlbredr::ServiceClassProfileIdentifier::MESSAGE_ACCESS_PROFILE);
  prof_desc.set_major_version(1);
  prof_desc.set_minor_version(4);
  def.mutable_profile_descriptors()->emplace_back(std::move(prof_desc));

  // Additional attributes - one requests a dynamic PSM.
  fidlbredr::Attribute goep_attr;
  goep_attr.set_id(0x200);  // GoepL2capPsm
  fidlbredr::DataElement goep_el;
  goep_el.set_uint16(fidlbredr::PSM_DYNAMIC);
  goep_attr.set_element(std::move(goep_el));
  def.mutable_additional_attributes()->emplace_back(std::move(goep_attr));

  fidlbredr::Attribute addl_attr;
  addl_attr.set_id(0x317);  // MAP supported features
  fidlbredr::DataElement addl_el;
  addl_el.set_uint32(1);  // Random features
  addl_attr.set_element(std::move(addl_el));
  def.mutable_additional_attributes()->emplace_back(std::move(addl_attr));

  return def;
}

// Returns a basic protocol list element with a protocol descriptor list that
// only contains an L2CAP descriptor.
bt::sdp::DataElement MakeL2capProtocolListElement() {
  bt::sdp::DataElement l2cap_uuid_el;
  l2cap_uuid_el.Set(bt::UUID(bt::sdp::protocol::kL2CAP));
  std::vector<bt::sdp::DataElement> l2cap_descriptor_list;
  l2cap_descriptor_list.emplace_back(std::move(l2cap_uuid_el));
  std::vector<bt::sdp::DataElement> protocols;
  protocols.emplace_back(std::move(l2cap_descriptor_list));
  bt::sdp::DataElement protocol_list_el;
  protocol_list_el.Set(std::move(protocols));
  return protocol_list_el;
}

using TestingBase = bthost::testing::AdapterTestFixture;
class ProfileServerTest : public TestingBase {
 public:
  ProfileServerTest() = default;
  ~ProfileServerTest() override = default;

 protected:
  void SetUp(FeaturesBits features) {
    bt::testing::FakeController::Settings settings;
    settings.ApplyDualModeDefaults();
    settings.synchronous_data_packet_length = kSynchronousDataPacketLength;
    settings.total_num_synchronous_data_packets =
        kTotalNumSynchronousDataPackets;
    TestingBase::SetUp(settings, features);

    fidlbredr::ProfileHandle profile_handle;
    client_.Bind(std::move(profile_handle));
    uint8_t sco_offload_index = 6;
    server_ = std::make_unique<ProfileServer>(adapter()->AsWeakPtr(),
                                              lease_provider_,
                                              sco_offload_index,
                                              client_.NewRequest(dispatcher()));
  }
  void SetUp() override { SetUp(FeaturesBits{0}); }

  void TearDown() override {
    RunLoopUntilIdle();
    client_ = nullptr;
    server_ = nullptr;
    TestingBase::TearDown();
  }

  ProfileServer* server() const { return server_.get(); }

  fidlbredr::ProfilePtr& client() { return client_; }

  bt::gap::PeerCache* peer_cache() const { return adapter()->peer_cache(); }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<ProfileServer> server_;
  fidlbredr::ProfilePtr client_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ProfileServerTest);
};

class ProfileServerTestWithBoolParam
    : public ProfileServerTest,
      public ::testing::WithParamInterface<bool> {};

class FakeConnectionReceiver
    : public fidlbredr::testing::ConnectionReceiver_TestBase,
      public fidlbredr::testing::ConnectionReceiver2_TestBase {
 public:
  FakeConnectionReceiver(fidl::InterfaceRequest<ConnectionReceiver> request,
                         async_dispatcher_t* dispatcher)
      : binding_(std::in_place_type<fidl::Binding<ConnectionReceiver>>,
                 this,
                 std::move(request),
                 dispatcher),
        connected_count_(0),
        closed_(false) {
    std::get<fidl::Binding<ConnectionReceiver>>(binding_).set_error_handler(
        [this](zx_status_t /*status*/) { closed_ = true; });
  }

  FakeConnectionReceiver(fidl::InterfaceRequest<ConnectionReceiver2> request,
                         async_dispatcher_t* dispatcher)
      : binding_(std::in_place_type<fidl::Binding<ConnectionReceiver2>>,
                 this,
                 std::move(request),
                 dispatcher),
        connected_count_(0),
        closed_(false) {
    std::get<fidl::Binding<ConnectionReceiver2>>(binding_).set_error_handler(
        [this](zx_status_t /*status*/) { closed_ = true; });
  }

  void Connected(fuchsia::bluetooth::PeerId peer_id,
                 fidlbredr::Channel channel,
                 std::vector<fidlbredr::ProtocolDescriptor> protocol) override {
    peer_id_ = peer_id;
    channel_ = std::move(channel);
    protocol_ = std::move(protocol);
    connected_count_++;
  }

  void Connected(fidlbredr::ConnectionReceiver2ConnectedRequest request,
                 ConnectedCallback callback) override {
    peer_id_ = request.peer_id();
    channel_ = std::move(*request.mutable_channel());
    protocol_ = std::move(*request.mutable_protocol());
    connected_count_++;
    callback(fidlbredr::ConnectionReceiver2_Connected_Result::WithResponse({}));
  }

  void Revoke() {
    std::visit([](auto&& binding) { binding.events().OnRevoke(); }, binding_);
  }

  size_t connected_count() const { return connected_count_; }
  const std::optional<fuchsia::bluetooth::PeerId>& peer_id() const {
    return peer_id_;
  }
  const std::optional<fidlbredr::Channel>& channel() const { return channel_; }
  const std::optional<std::vector<fidlbredr::ProtocolDescriptor>>& protocol()
      const {
    return protocol_;
  }
  bool closed() { return closed_; }

  std::optional<fidlbredr::AudioDirectionExtPtr> bind_ext_direction() {
    if (!channel().has_value()) {
      return std::nullopt;
    }
    fidlbredr::AudioDirectionExtPtr client =
        channel_.value().mutable_ext_direction()->Bind();
    return client;
  }

  fidlbredr::Channel take_channel() {
    fidlbredr::Channel channel = std::move(channel_.value());
    channel_.reset();
    return channel;
  }

  void Close() {
    std::visit([](auto&& binding) { binding.Close(ZX_ERR_PEER_CLOSED); },
               binding_);
  }

 private:
  std::variant<fidl::Binding<ConnectionReceiver>,
               fidl::Binding<ConnectionReceiver2>>
      binding_;
  size_t connected_count_;
  std::optional<fuchsia::bluetooth::PeerId> peer_id_;
  std::optional<fidlbredr::Channel> channel_;
  std::optional<std::vector<fidlbredr::ProtocolDescriptor>> protocol_;
  bool closed_;

  void NotImplemented_(const std::string& name) override {
    FAIL() << name << " is not implemented";
  }
};

class FakeSearchResults : public fidlbredr::testing::SearchResults_TestBase {
 public:
  FakeSearchResults(fidl::InterfaceRequest<SearchResults> request,
                    async_dispatcher_t* dispatcher)
      : binding_(this, std::move(request), dispatcher),
        service_found_count_(0) {
    binding_.set_error_handler([this](zx_status_t) { closed_ = true; });
  }

  void ServiceFound(fuchsia::bluetooth::PeerId peer_id,
                    fidl::VectorPtr<fidlbredr::ProtocolDescriptor> protocol,
                    std::vector<fidlbredr::Attribute> attributes,
                    ServiceFoundCallback callback) override {
    ++service_found_count_;
    peer_id_ = peer_id;
    attributes_ = std::move(attributes);
    if (result_cb_) {
      result_cb_(std::move(callback));
    }
  }

  bool closed() const { return closed_; }
  size_t service_found_count() const { return service_found_count_; }
  const std::optional<fuchsia::bluetooth::PeerId>& peer_id() const {
    return peer_id_;
  }
  const std::optional<std::vector<fidlbredr::Attribute>>& attributes() const {
    return attributes_;
  }

  using ResultCallback = fit::function<void(ServiceFoundCallback)>;
  void set_result_cb(ResultCallback cb) { result_cb_ = std::move(cb); }
  void set_result_cb(fit::function<void()> cb) {
    result_cb_ = [result_cb =
                      std::move(cb)](ServiceFoundCallback service_found_cb) {
      if (result_cb) {
        result_cb();
      }
      service_found_cb(
          fidlbredr::SearchResults_ServiceFound_Result::WithResponse(
              fidlbredr::SearchResults_ServiceFound_Response()));
      return;
    };
  }

 private:
  bool closed_ = false;
  fidl::Binding<SearchResults> binding_;
  std::optional<fuchsia::bluetooth::PeerId> peer_id_;
  std::optional<std::vector<fidlbredr::Attribute>> attributes_;
  size_t service_found_count_ = 0;
  ResultCallback result_cb_;

  void NotImplemented_(const std::string& name) override {
    FAIL() << name << " is not implemented";
  }
};

TEST_F(ProfileServerTest, ErrorOnInvalidDefinition) {
  fidlbredr::ConnectionReceiver2Handle receiver_handle;
  fidl::InterfaceRequest<fidlbredr::ConnectionReceiver2> request =
      receiver_handle.NewRequest();

  std::vector<fidlbredr::ServiceDefinition> services;
  fidlbredr::ServiceDefinition def;
  // Empty service definition is not allowed - it must contain at least a
  // service UUID.

  services.emplace_back(std::move(def));

  size_t cb_count = 0;
  auto cb = [&](fidlbredr::Profile_Advertise_Result result) {
    cb_count++;
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.err(), fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
  };

  fidlbredr::ProfileAdvertiseRequest adv_request;
  adv_request.set_services(std::move(services));
  adv_request.set_connection_receiver(std::move(receiver_handle));
  client()->Advertise(std::move(adv_request), std::move(cb));

  RunLoopUntilIdle();

  ASSERT_EQ(cb_count, 1u);
  // Server should close because it's an invalid definition.
  zx_signals_t signals;
  request.channel().wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time(0), &signals);
  EXPECT_TRUE(signals & ZX_CHANNEL_PEER_CLOSED);
}

TEST_F(ProfileServerTest, ErrorOnMultipleAdvertiseRequests) {
  fidlbredr::ConnectionReceiver2Handle receiver_handle1;
  fidl::InterfaceRequest<fidlbredr::ConnectionReceiver2> request1 =
      receiver_handle1.NewRequest();

  std::vector<fidlbredr::ServiceDefinition> services1;
  services1.emplace_back(MakeFIDLServiceDefinition());

  size_t cb1_count = 0;
  auto cb1 = [&](fidlbredr::Profile_Advertise_Result result) {
    cb1_count++;
    EXPECT_TRUE(result.is_response());
  };

  fidlbredr::ProfileAdvertiseRequest adv_request1;
  adv_request1.set_services(std::move(services1));
  adv_request1.set_connection_receiver(std::move(receiver_handle1));
  client()->Advertise(std::move(adv_request1), std::move(cb1));

  RunLoopUntilIdle();

  // First callback should be invoked with success since the advertisement is
  // valid.
  ASSERT_EQ(cb1_count, 1u);

  fidlbredr::ConnectionReceiver2Handle receiver_handle2;
  fidl::InterfaceRequest<fidlbredr::ConnectionReceiver2> request2 =
      receiver_handle2.NewRequest();

  std::vector<fidlbredr::ServiceDefinition> services2;
  services2.emplace_back(MakeFIDLServiceDefinition());

  // Second callback should error because the second advertisement is requesting
  // a taken PSM.
  size_t cb2_count = 0;
  auto cb2 = [&](fidlbredr::Profile_Advertise_Result response) {
    cb2_count++;
    EXPECT_TRUE(response.is_err());
    EXPECT_EQ(response.err(), fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
  };

  fidlbredr::ProfileAdvertiseRequest adv_request2;
  adv_request2.set_services(std::move(services2));
  adv_request2.set_connection_receiver(std::move(receiver_handle2));
  client()->Advertise(std::move(adv_request2), std::move(cb2));

  RunLoopUntilIdle();

  ASSERT_EQ(cb1_count, 1u);
  ASSERT_EQ(cb2_count, 1u);

  // Second channel should close.
  zx_signals_t signals;
  request2.channel().wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time(0), &signals);
  EXPECT_TRUE(signals & ZX_CHANNEL_PEER_CLOSED);
}

TEST_F(ProfileServerTest, ErrorOnInvalidConnectParametersNoPsm) {
  // Random peer, since we don't expect the connection.
  fuchsia::bluetooth::PeerId peer_id{123};

  // No PSM provided - this is invalid.
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  // Expect an error result.
  auto sock_cb = [](fidlbredr::Profile_Connect_Result result) {
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.err(), fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
  };

  client()->Connect(peer_id, std::move(conn_params), std::move(sock_cb));
  RunLoopUntilIdle();
}

TEST_F(ProfileServerTest, ErrorOnInvalidConnectParametersRfcomm) {
  // Random peer, since we don't expect the connection.
  fuchsia::bluetooth::PeerId peer_id{123};

  // RFCOMM Parameters are provided - this is not supported.
  fidlbredr::RfcommParameters rfcomm_params;
  fidlbredr::ConnectParameters conn_params;
  conn_params.set_rfcomm(std::move(rfcomm_params));

  // Expect an error result.
  auto sock_cb = [](fidlbredr::Profile_Connect_Result result) {
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.err(), fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
  };

  client()->Connect(peer_id, std::move(conn_params), std::move(sock_cb));
  RunLoopUntilIdle();
}

TEST_P(ProfileServerTestWithBoolParam, DynamicPsmAdvertisementIsUpdated) {
  fidlbredr::ProfileAdvertiseRequest adv_request;

  std::vector<fidlbredr::ServiceDefinition> services;
  services.emplace_back(MakeMapMceServiceDefinition());
  adv_request.set_services(std::move(services));

  std::optional<FakeConnectionReceiver> receiver;
  if (GetParam()) {
    fidlbredr::ConnectionReceiverHandle receiver_handle;
    receiver.emplace(receiver_handle.NewRequest(), dispatcher());
    adv_request.set_receiver(std::move(receiver_handle));
  } else {
    fidlbredr::ConnectionReceiver2Handle receiver_handle;
    receiver.emplace(receiver_handle.NewRequest(), dispatcher());
    adv_request.set_connection_receiver(std::move(receiver_handle));
  }

  size_t cb_count = 0;
  auto cb = [&](fidlbredr::Profile_Advertise_Result result) {
    cb_count++;
    EXPECT_TRUE(result.is_response());
    EXPECT_EQ(result.response().services().size(), 1u);
    const auto registered_def =
        std::move(result.response().mutable_services()->front());
    const auto original_def = MakeMapMceServiceDefinition();
    // The UUIDs, primary protocol list, & profile descriptors should be
    // unchanged.
    ASSERT_TRUE(::fidl::Equals(registered_def.service_class_uuids(),
                               original_def.service_class_uuids()));
    ASSERT_TRUE(::fidl::Equals(registered_def.protocol_descriptor_list(),
                               original_def.protocol_descriptor_list()));
    ASSERT_TRUE(::fidl::Equals(registered_def.profile_descriptors(),
                               original_def.profile_descriptors()));
    // The additional protocol list should be updated with a randomly assigned
    // dynamic PSM.
    EXPECT_EQ(registered_def.additional_protocol_descriptor_lists().size(), 1u);
    EXPECT_NE(registered_def.additional_protocol_descriptor_lists()
                  .front()
                  .front()
                  .params()
                  .front()
                  .uint16(),
              fidlbredr::PSM_DYNAMIC);
    // TODO(b/327758656): Verify information and additional attributes once
    // implemented.
  };

  client()->Advertise(std::move(adv_request), std::move(cb));

  RunLoopUntilIdle();
  EXPECT_EQ(cb_count, 1u);
}

TEST_P(ProfileServerTestWithBoolParam,
       RevokeConnectionReceiverUnregistersAdvertisement) {
  fidlbredr::ProfileAdvertiseRequest adv_request;

  std::optional<FakeConnectionReceiver> connect_receiver;
  if (GetParam()) {
    fidlbredr::ConnectionReceiverHandle receiver_handle;
    connect_receiver.emplace(receiver_handle.NewRequest(), dispatcher());
    adv_request.set_receiver(std::move(receiver_handle));
  } else {
    fidlbredr::ConnectionReceiver2Handle receiver_handle;
    connect_receiver.emplace(receiver_handle.NewRequest(), dispatcher());
    adv_request.set_connection_receiver(std::move(receiver_handle));
  }

  std::vector<fidlbredr::ServiceDefinition> services;
  services.emplace_back(MakeFIDLServiceDefinition());

  size_t cb_count = 0;
  auto cb = [&](fidlbredr::Profile_Advertise_Result result) {
    cb_count++;
    EXPECT_TRUE(result.is_response());
  };

  adv_request.set_services(std::move(services));
  client()->Advertise(std::move(adv_request), std::move(cb));
  RunLoopUntilIdle();

  // Advertisement should be registered. The callback should be invoked with the
  // advertised set of services, and the `ConnectionReceiver` should still be
  // open.
  ASSERT_EQ(cb_count, 1u);
  ASSERT_FALSE(connect_receiver->closed());

  // Server end of `ConnectionReceiver` revokes the advertisement.
  connect_receiver->Revoke();
  RunLoopUntilIdle();

  // Profile server should drop the advertisement - the `connect_receiver`
  // should be closed.
  ASSERT_TRUE(connect_receiver->closed());
}

class ProfileServerTestConnectedPeer : public ProfileServerTest {
 public:
  ProfileServerTestConnectedPeer() = default;
  ~ProfileServerTestConnectedPeer() override = default;

 protected:
  void SetUp(FeaturesBits features) {
    ProfileServerTest::SetUp(features);
    peer_ = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
    std::unique_ptr<bt::testing::FakePeer> fake_peer =
        std::make_unique<bt::testing::FakePeer>(kTestDevAddr, pw_dispatcher());
    test_device()->AddPeer(std::move(fake_peer));

    std::optional<bt::hci::Result<>> status;
    auto connect_cb = [this, &status](auto cb_status, auto cb_conn_ref) {
      ASSERT_TRUE(cb_conn_ref);
      status = cb_status;
      connection_ = std::move(cb_conn_ref);
    };

    EXPECT_TRUE(adapter()->bredr()->Connect(peer_->identifier(), connect_cb));
    EXPECT_EQ(bt::gap::Peer::ConnectionState::kInitializing,
              peer_->bredr()->connection_state());

    RunLoopUntilIdle();
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(fit::ok(), status.value());
    ASSERT_TRUE(connection_);
    EXPECT_EQ(peer_->identifier(), connection_->peer_id());
    EXPECT_NE(bt::gap::Peer::ConnectionState::kNotConnected,
              peer_->bredr()->connection_state());
  }

  void SetUp() override { SetUp(FeaturesBits::kHciSco); }

  void TearDown() override {
    connection_ = nullptr;
    peer_ = nullptr;
    ProfileServerTest::TearDown();
  }

  bt::gap::BrEdrConnection* connection() const { return connection_; }

  bt::gap::Peer* peer() const { return peer_; }

 private:
  bt::gap::BrEdrConnection* connection_;
  bt::gap::Peer* peer_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ProfileServerTestConnectedPeer);
};

class ProfileServerTestConnectedPeerWithBoolParam
    : public ProfileServerTestConnectedPeer,
      public ::testing::WithParamInterface<bool> {};

class ProfileServerTestScoConnected : public ProfileServerTestConnectedPeer {
 public:
  void SetUp() override {
    fidlbredr::ScoConnectionParameters params =
        CreateScoConnectionParameters(fidlbredr::HfpParameterSet::D0);
    params.set_path(fidlbredr::DataPath::HOST);
    SetUp(std::move(params));
  }

  void SetUp(fidlbredr::ScoConnectionParameters conn_params) {
    ProfileServerTestConnectedPeer::SetUp(FeaturesBits::kHciSco);

    test_device()->set_configure_sco_cb(
        [](auto, auto, auto, auto cb) { cb(PW_STATUS_OK); });
    test_device()->set_reset_sco_cb([](auto cb) { cb(PW_STATUS_OK); });

    std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
    sco_params_list.emplace_back(std::move(conn_params));

    fidlbredr::ProfileConnectScoRequest request;
    request.set_peer_id(
        fuchsia::bluetooth::PeerId{peer()->identifier().value()});
    request.set_initiator(false);
    request.set_params(std::move(sco_params_list));
    fidlbredr::ScoConnectionHandle connection_handle;
    request.set_connection(connection_handle.NewRequest());

    sco_connection_ = connection_handle.Bind();
    sco_connection_.set_error_handler([this](zx_status_t status) {
      sco_connection_ = nullptr;
      sco_conn_error_ = status;
    });

    std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
        connection_complete;
    sco_connection_.events().OnConnectionComplete =
        [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
          connection_complete = std::move(request);
        };

    client()->ConnectSco(std::move(request));
    RunLoopUntilIdle();
    test_device()->SendConnectionRequest(peer()->address(),
                                         pw::bluetooth::emboss::LinkType::SCO);
    RunLoopUntilIdle();
    ASSERT_TRUE(connection_complete.has_value());
    ASSERT_TRUE(connection_complete->is_connected_params());

    // OnConnectionComplete should never be called again.
    sco_connection_.events().OnConnectionComplete =
        [](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
          FAIL();
        };

    // Find the link handle used for the SCO connection.
    bt::testing::FakePeer* fake_peer =
        test_device()->FindPeer(peer()->address());
    ASSERT_TRUE(fake_peer);
    // There are 2 connections: BR/EDR, SCO
    ASSERT_EQ(fake_peer->logical_links().size(), 2u);
    bt::testing::FakePeer::HandleSet links = fake_peer->logical_links();
    // The link that is not the BR/EDR connection link must be the SCO link.
    links.erase(connection()->link().handle());
    sco_conn_handle_ = *links.begin();
  }

  void TearDown() override { ProfileServerTestConnectedPeer::TearDown(); }

  fidlbredr::ScoConnectionPtr& sco_connection() { return sco_connection_; }

  std::optional<zx_status_t> sco_conn_error() const { return sco_conn_error_; }

  bt::hci_spec::ConnectionHandle sco_handle() const { return sco_conn_handle_; }

 private:
  fidlbredr::ScoConnectionPtr sco_connection_;
  bt::hci_spec::ConnectionHandle sco_conn_handle_;
  std::optional<zx_status_t> sco_conn_error_;
};

class ProfileServerTestOffloadedScoConnected
    : public ProfileServerTestScoConnected {
 public:
  void SetUp() override {
    fidlbredr::ScoConnectionParameters params =
        CreateScoConnectionParameters(fidlbredr::HfpParameterSet::D0);
    params.set_path(fidlbredr::DataPath::OFFLOAD);
    ProfileServerTestScoConnected::SetUp(std::move(params));
  }
};

TEST_F(ProfileServerTestConnectedPeer, ConnectL2capChannelParametersUseSocket) {
  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());
  // Approve pairing requests.
  pairing_delegate->SetConfirmPairingCallback(
      [](bt::PeerId, auto confirm_cb) { confirm_cb(true); });
  pairing_delegate->SetCompletePairingCallback(
      [&](bt::PeerId, bt::sm::Result<> status) {
        EXPECT_EQ(fit::ok(), status);
      });

  bt::l2cap::ChannelParameters expected_params;
  expected_params.mode =
      bt::l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission;
  expected_params.max_rx_sdu_size = bt::l2cap::kMinACLMTU;
  l2cap()->ExpectOutboundL2capChannel(
      connection()->link().handle(), kPsm, 0x40, 0x41, expected_params);

  // Expect a non-empty channel result.
  std::optional<fidlbredr::Channel> channel;
  auto chan_cb = [&channel](fidlbredr::Profile_Connect_Result result) {
    EXPECT_TRUE(result.is_response());
    channel = std::move(result.response().channel);
  };
  // Initiates pairing

  fuchsia::bluetooth::PeerId peer_id{peer()->identifier().value()};

  // Set L2CAP channel parameters
  fbt::ChannelParameters chan_params;
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  chan_params.set_channel_mode(fbt::ChannelMode::ENHANCED_RETRANSMISSION);
  chan_params.set_max_rx_packet_size(bt::l2cap::kMinACLMTU);
  l2cap_params.set_psm(kPsm);
  l2cap_params.set_parameters(std::move(chan_params));
  conn_params.set_l2cap(std::move(l2cap_params));

  client()->Connect(peer_id, std::move(conn_params), std::move(chan_cb));
  RunLoopUntilIdle();

  ASSERT_TRUE(channel.has_value());
  EXPECT_TRUE(channel->has_socket());
  EXPECT_FALSE(channel->IsEmpty());
  EXPECT_EQ(channel->channel_mode(), chan_params.channel_mode());
  // FakeL2cap returns channels with max tx sdu size of kDefaultMTU.
  EXPECT_EQ(channel->max_tx_sdu_size(), bt::l2cap::kDefaultMTU);
  EXPECT_FALSE(channel->has_ext_direction());
  EXPECT_FALSE(channel->has_flush_timeout());
}

TEST_F(ProfileServerTestConnectedPeer,
       ConnectL2capChannelParametersUseConnection) {
  server()->set_use_sockets(false);

  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());
  // Approve pairing requests.
  pairing_delegate->SetConfirmPairingCallback(
      [](bt::PeerId, auto confirm_cb) { confirm_cb(true); });
  pairing_delegate->SetCompletePairingCallback(
      [&](bt::PeerId, bt::sm::Result<> status) {
        EXPECT_EQ(fit::ok(), status);
      });

  bt::l2cap::ChannelParameters expected_params;
  expected_params.mode =
      bt::l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission;
  expected_params.max_rx_sdu_size = bt::l2cap::kMinACLMTU;
  l2cap()->ExpectOutboundL2capChannel(
      connection()->link().handle(), kPsm, 0x40, 0x41, expected_params);

  // Expect a non-empty channel result.
  std::optional<fidlbredr::Channel> channel;
  auto chan_cb = [&channel](fidlbredr::Profile_Connect_Result result) {
    EXPECT_TRUE(result.is_response());
    channel = std::move(result.response().channel);
  };
  // Initiates pairing

  fuchsia::bluetooth::PeerId peer_id{peer()->identifier().value()};

  // Set L2CAP channel parameters
  fbt::ChannelParameters chan_params;
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  chan_params.set_channel_mode(fbt::ChannelMode::ENHANCED_RETRANSMISSION);
  chan_params.set_max_rx_packet_size(bt::l2cap::kMinACLMTU);
  l2cap_params.set_psm(kPsm);
  l2cap_params.set_parameters(std::move(chan_params));
  conn_params.set_l2cap(std::move(l2cap_params));

  client()->Connect(peer_id, std::move(conn_params), std::move(chan_cb));
  RunLoopUntilIdle();

  ASSERT_TRUE(channel.has_value());
  EXPECT_TRUE(channel->has_connection());
  EXPECT_FALSE(channel->IsEmpty());
  EXPECT_EQ(channel->channel_mode(), chan_params.channel_mode());
  // FakeL2cap returns channels with max tx sdu size of kDefaultMTU.
  EXPECT_EQ(channel->max_tx_sdu_size(), bt::l2cap::kDefaultMTU);
  EXPECT_FALSE(channel->has_ext_direction());
  EXPECT_FALSE(channel->has_flush_timeout());
}

TEST_F(ProfileServerTestConnectedPeer,
       ConnectWithAuthenticationRequiredButLinkKeyNotAuthenticatedFails) {
  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kNoInputNoOutput);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());
  pairing_delegate->SetCompletePairingCallback(
      [&](bt::PeerId, bt::sm::Result<> status) {
        EXPECT_EQ(fit::ok(), status);
      });

  fbt::SecurityRequirements security;
  security.set_authentication_required(true);

  // Set L2CAP channel parameters
  fbt::ChannelParameters chan_params;
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  chan_params.set_security_requirements(std::move(security));
  l2cap_params.set_psm(kPsm);
  l2cap_params.set_parameters(std::move(chan_params));
  conn_params.set_l2cap(std::move(l2cap_params));

  size_t sock_cb_count = 0;
  auto sock_cb = [&](fidlbredr::Profile_Connect_Result result) {
    sock_cb_count++;
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(fuchsia::bluetooth::ErrorCode::FAILED, result.err());
  };

  fuchsia::bluetooth::PeerId peer_id{peer()->identifier().value()};

  // Initiates pairing.
  // FakeController will create an unauthenticated key.
  client()->Connect(peer_id, std::move(conn_params), std::move(sock_cb));
  RunLoopUntilIdle();

  EXPECT_EQ(1u, sock_cb_count);
}

// Tests receiving an empty Channel results in an error propagated through the
// callback.
TEST_F(ProfileServerTestConnectedPeer, ConnectEmptyChannelResponse) {
  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());
  // Approve pairing requests.
  pairing_delegate->SetConfirmPairingCallback(
      [](bt::PeerId, auto confirm_cb) { confirm_cb(true); });
  pairing_delegate->SetCompletePairingCallback(
      [&](bt::PeerId, bt::sm::Result<> status) {
        EXPECT_EQ(fit::ok(), status);
      });

  // Make the l2cap channel creation fail.
  l2cap()->set_simulate_open_channel_failure(true);

  bt::l2cap::ChannelParameters expected_params;
  expected_params.mode =
      bt::l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission;
  expected_params.max_rx_sdu_size = bt::l2cap::kMinACLMTU;
  l2cap()->ExpectOutboundL2capChannel(
      connection()->link().handle(), kPsm, 0x40, 0x41, expected_params);

  fbt::ChannelParameters chan_params;
  chan_params.set_channel_mode(fbt::ChannelMode::ENHANCED_RETRANSMISSION);
  chan_params.set_max_rx_packet_size(bt::l2cap::kMinACLMTU);
  auto sock_cb = [](fidlbredr::Profile_Connect_Result result) {
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(fuchsia::bluetooth::ErrorCode::FAILED, result.err());
  };
  // Initiates pairing

  fuchsia::bluetooth::PeerId peer_id{peer()->identifier().value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(kPsm);
  l2cap_params.set_parameters(std::move(chan_params));
  conn_params.set_l2cap(std::move(l2cap_params));

  client()->Connect(peer_id, std::move(conn_params), std::move(sock_cb));
  RunLoopUntilIdle();
}

TEST_P(
    ProfileServerTestConnectedPeerWithBoolParam,
    AdvertiseChannelParametersReceivedInOnChannelConnectedCallbackUseSocket) {
  constexpr uint16_t kTxMtu = bt::l2cap::kMinACLMTU;

  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());

  std::vector<fidlbredr::ServiceDefinition> services;
  services.emplace_back(MakeFIDLServiceDefinition());
  fbt::ChannelParameters chan_params;
  chan_params.set_channel_mode(fbt::ChannelMode::ENHANCED_RETRANSMISSION);

  fidlbredr::ProfileAdvertiseRequest adv_request;
  adv_request.set_services(std::move(services));
  adv_request.set_parameters(std::move(chan_params));

  std::optional<FakeConnectionReceiver> connect_receiver;
  if (GetParam()) {
    fidlbredr::ConnectionReceiverHandle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_receiver(std::move(connect_receiver_handle));
  } else {
    fidlbredr::ConnectionReceiver2Handle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_connection_receiver(std::move(connect_receiver_handle));
  }

  client()->Advertise(std::move(adv_request), NopAdvertiseCallback);
  RunLoopUntilIdle();

  ASSERT_EQ(connect_receiver->connected_count(), 0u);
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      connection()->link().handle(), kPsm, 0x40, 0x41, kTxMtu));
  RunLoopUntilIdle();

  ASSERT_EQ(connect_receiver->connected_count(), 1u);
  ASSERT_EQ(connect_receiver->peer_id().value().value,
            peer()->identifier().value());
  ASSERT_TRUE(connect_receiver->channel().value().has_socket());
  EXPECT_EQ(connect_receiver->channel().value().channel_mode(),
            fbt::ChannelMode::ENHANCED_RETRANSMISSION);
  EXPECT_EQ(connect_receiver->channel().value().max_tx_sdu_size(), kTxMtu);
  EXPECT_FALSE(connect_receiver->channel().value().has_ext_direction());
  EXPECT_FALSE(connect_receiver->channel().value().has_flush_timeout());
}

TEST_P(
    ProfileServerTestConnectedPeerWithBoolParam,
    AdvertiseChannelParametersReceivedInOnChannelConnectedCallbackUseConnection) {
  server()->set_use_sockets(false);

  constexpr uint16_t kTxMtu = bt::l2cap::kMinACLMTU;

  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());

  std::vector<fidlbredr::ServiceDefinition> services;
  services.emplace_back(MakeFIDLServiceDefinition());
  fbt::ChannelParameters chan_params;
  chan_params.set_channel_mode(fbt::ChannelMode::ENHANCED_RETRANSMISSION);

  fidlbredr::ProfileAdvertiseRequest adv_request;
  adv_request.set_services(std::move(services));
  adv_request.set_parameters(std::move(chan_params));

  std::optional<FakeConnectionReceiver> connect_receiver;
  if (GetParam()) {
    fidlbredr::ConnectionReceiverHandle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_receiver(std::move(connect_receiver_handle));
  } else {
    fidlbredr::ConnectionReceiver2Handle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_connection_receiver(std::move(connect_receiver_handle));
  }

  client()->Advertise(std::move(adv_request), NopAdvertiseCallback);
  RunLoopUntilIdle();

  ASSERT_EQ(connect_receiver->connected_count(), 0u);
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      connection()->link().handle(), kPsm, 0x40, 0x41, kTxMtu));
  RunLoopUntilIdle();

  ASSERT_EQ(connect_receiver->connected_count(), 1u);
  ASSERT_EQ(connect_receiver->peer_id().value().value,
            peer()->identifier().value());
  ASSERT_TRUE(connect_receiver->channel().value().has_connection());
  EXPECT_EQ(connect_receiver->channel().value().channel_mode(),
            fbt::ChannelMode::ENHANCED_RETRANSMISSION);
  EXPECT_EQ(connect_receiver->channel().value().max_tx_sdu_size(), kTxMtu);
  EXPECT_FALSE(connect_receiver->channel().value().has_ext_direction());
  EXPECT_FALSE(connect_receiver->channel().value().has_flush_timeout());
}

class AclPrioritySupportedTest : public ProfileServerTestConnectedPeer {
 public:
  void SetUp() override {
    ProfileServerTestConnectedPeer::SetUp(FeaturesBits::kSetAclPriorityCommand);
  }
};

class PriorityTest : public AclPrioritySupportedTest,
                     public ::testing::WithParamInterface<
                         std::pair<fidlbredr::A2dpDirectionPriority, bool>> {};

class PriorityTestWithBoolParam : public AclPrioritySupportedTest,
                                  public ::testing::WithParamInterface<bool> {};

TEST_P(PriorityTest, OutboundConnectAndSetPriority) {
  const fidlbredr::A2dpDirectionPriority kPriority = GetParam().first;
  const bool kExpectSuccess = GetParam().second;

  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());
  // Approve pairing requests.
  pairing_delegate->SetConfirmPairingCallback(
      [](bt::PeerId, auto confirm_cb) { confirm_cb(true); });
  pairing_delegate->SetCompletePairingCallback(
      [&](bt::PeerId, bt::sm::Result<> status) {
        EXPECT_EQ(fit::ok(), status);
      });

  l2cap()->ExpectOutboundL2capChannel(connection()->link().handle(),
                                      kPsm,
                                      0x40,
                                      0x41,
                                      bt::l2cap::ChannelParameters());

  FakeChannel::WeakPtr fake_channel;
  l2cap()->set_channel_callback(
      [&](FakeChannel::WeakPtr chan) { fake_channel = std::move(chan); });

  // Expect a non-empty channel result.
  std::optional<fidlbredr::Channel> channel;
  auto chan_cb = [&channel](fidlbredr::Profile_Connect_Result result) {
    ASSERT_TRUE(result.is_response());
    channel = std::move(result.response().channel);
  };

  fuchsia::bluetooth::PeerId peer_id{peer()->identifier().value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(kPsm);
  conn_params.set_l2cap(std::move(l2cap_params));

  // Initiates pairing
  client()->Connect(peer_id, std::move(conn_params), std::move(chan_cb));
  RunLoopUntilIdle();
  ASSERT_TRUE(fake_channel.is_alive());
  ASSERT_TRUE(channel.has_value());
  ASSERT_TRUE(channel->has_ext_direction());
  fidlbredr::AudioDirectionExtPtr client =
      channel->mutable_ext_direction()->Bind();

  size_t priority_cb_count = 0;
  fake_channel->set_acl_priority_fails(!kExpectSuccess);
  client->SetPriority(
      kPriority, [&](fidlbredr::AudioDirectionExt_SetPriority_Result result) {
        EXPECT_EQ(result.is_response(), kExpectSuccess);
        priority_cb_count++;
      });

  RunLoopUntilIdle();
  EXPECT_EQ(priority_cb_count, 1u);
  client = nullptr;
  RunLoopUntilIdle();

  if (kExpectSuccess) {
    switch (kPriority) {
      case fidlbredr::A2dpDirectionPriority::SOURCE:
        EXPECT_EQ(fake_channel->requested_acl_priority(), AclPriority::kSource);
        break;
      case fidlbredr::A2dpDirectionPriority::SINK:
        EXPECT_EQ(fake_channel->requested_acl_priority(), AclPriority::kSink);
        break;
      default:
        EXPECT_EQ(fake_channel->requested_acl_priority(), AclPriority::kNormal);
    }
  } else {
    EXPECT_EQ(fake_channel->requested_acl_priority(), AclPriority::kNormal);
  }
}

const std::array<std::pair<fidlbredr::A2dpDirectionPriority, bool>, 4>
    kPriorityParams = {{{fidlbredr::A2dpDirectionPriority::SOURCE, false},
                        {fidlbredr::A2dpDirectionPriority::SOURCE, true},
                        {fidlbredr::A2dpDirectionPriority::SINK, true},
                        {fidlbredr::A2dpDirectionPriority::NORMAL, true}}};
INSTANTIATE_TEST_SUITE_P(ProfileServerTestConnectedPeer,
                         PriorityTest,
                         ::testing::ValuesIn(kPriorityParams));

TEST_P(PriorityTestWithBoolParam, InboundConnectAndSetPriority) {
  constexpr uint16_t kTxMtu = bt::l2cap::kMinACLMTU;

  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());

  FakeChannel::WeakPtr fake_channel;
  l2cap()->set_channel_callback(
      [&](FakeChannel::WeakPtr chan) { fake_channel = std::move(chan); });

  std::vector<fidlbredr::ServiceDefinition> services;
  services.emplace_back(MakeFIDLServiceDefinition());
  fidlbredr::ProfileAdvertiseRequest adv_request;
  adv_request.set_services(std::move(services));
  std::optional<FakeConnectionReceiver> connect_receiver;
  if (GetParam()) {
    fidlbredr::ConnectionReceiverHandle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_receiver(std::move(connect_receiver_handle));
  } else {
    fidlbredr::ConnectionReceiver2Handle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_connection_receiver(std::move(connect_receiver_handle));
  }
  client()->Advertise(std::move(adv_request), NopAdvertiseCallback);
  RunLoopUntilIdle();

  ASSERT_EQ(connect_receiver->connected_count(), 0u);
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      connection()->link().handle(), kPsm, 0x40, 0x41, kTxMtu));

  RunLoopUntilIdle();
  ASSERT_EQ(connect_receiver->connected_count(), 1u);
  ASSERT_TRUE(connect_receiver->channel().has_value());
  ASSERT_TRUE(connect_receiver->channel().value().has_ext_direction());
  // Taking value() is safe because of the has_ext_direction() check.
  fidlbredr::AudioDirectionExtPtr client =
      connect_receiver->bind_ext_direction().value();

  size_t priority_cb_count = 0;
  client->SetPriority(
      fidlbredr::A2dpDirectionPriority::SINK,
      [&](fidlbredr::AudioDirectionExt_SetPriority_Result result) {
        EXPECT_TRUE(result.is_response());
        priority_cb_count++;
      });

  RunLoopUntilIdle();
  EXPECT_EQ(priority_cb_count, 1u);
  ASSERT_TRUE(fake_channel.is_alive());
  EXPECT_EQ(fake_channel->requested_acl_priority(), AclPriority::kSink);
}

// Verifies that a socket channel relay is correctly set up such that bytes
// written to the socket are sent to the channel.
TEST_F(ProfileServerTestConnectedPeer, ConnectReturnsValidSocket) {
  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());
  // Approve pairing requests.
  pairing_delegate->SetConfirmPairingCallback(
      [](bt::PeerId, auto confirm_cb) { confirm_cb(true); });
  pairing_delegate->SetCompletePairingCallback(
      [&](bt::PeerId, bt::sm::Result<> status) {
        EXPECT_EQ(fit::ok(), status);
      });

  bt::l2cap::ChannelParameters expected_params;
  l2cap()->ExpectOutboundL2capChannel(
      connection()->link().handle(), kPsm, 0x40, 0x41, expected_params);

  std::optional<FakeChannel::WeakPtr> fake_chan;
  l2cap()->set_channel_callback(
      [&fake_chan](FakeChannel::WeakPtr chan) { fake_chan = std::move(chan); });

  // Expect a non-empty channel result.
  std::optional<fidlbredr::Channel> channel;
  auto result_cb = [&channel](fidlbredr::Profile_Connect_Result result) {
    EXPECT_TRUE(result.is_response());
    channel = std::move(result.response().channel);
  };

  fuchsia::bluetooth::PeerId peer_id{peer()->identifier().value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(kPsm);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  // Initiates pairing
  client()->Connect(peer_id, std::move(conn_params), std::move(result_cb));
  RunLoopUntilIdle();

  ASSERT_TRUE(channel.has_value());
  ASSERT_TRUE(channel->has_socket());
  auto& socket = channel->socket();

  ASSERT_TRUE(fake_chan.has_value());
  FakeChannel::WeakPtr fake_chan_ptr = fake_chan.value();
  size_t send_count = 0;
  fake_chan_ptr->SetSendCallback([&send_count](auto buffer) { send_count++; },
                                 pw_dispatcher());

  const char write_data[2] = "a";
  size_t bytes_written = 0;
  auto status =
      socket.write(0, write_data, sizeof(write_data) - 1, &bytes_written);
  EXPECT_EQ(ZX_OK, status);
  EXPECT_EQ(1u, bytes_written);
  RunLoopUntilIdle();
  EXPECT_EQ(1u, send_count);
}

// Verifies that a BrEdrConnectionServer is correctly set up such that bytes
// written to the Connection are sent to the channel.
TEST_F(ProfileServerTestConnectedPeer, ConnectReturnsValidConnection) {
  server()->set_use_sockets(false);

  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());
  // Approve pairing requests.
  pairing_delegate->SetConfirmPairingCallback(
      [](bt::PeerId, auto confirm_cb) { confirm_cb(true); });
  pairing_delegate->SetCompletePairingCallback(
      [&](bt::PeerId, bt::sm::Result<> status) {
        EXPECT_EQ(fit::ok(), status);
      });

  bt::l2cap::ChannelParameters expected_params;
  l2cap()->ExpectOutboundL2capChannel(
      connection()->link().handle(), kPsm, 0x40, 0x41, expected_params);

  std::optional<FakeChannel::WeakPtr> fake_chan;
  l2cap()->set_channel_callback(
      [&fake_chan](FakeChannel::WeakPtr chan) { fake_chan = std::move(chan); });

  // Expect a non-empty channel result.
  std::optional<fidlbredr::Channel> channel;
  auto result_cb = [&channel](fidlbredr::Profile_Connect_Result result) {
    EXPECT_TRUE(result.is_response());
    channel = std::move(result.response().channel);
  };

  fuchsia::bluetooth::PeerId peer_id{peer()->identifier().value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(kPsm);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  // Initiates pairing
  client()->Connect(peer_id, std::move(conn_params), std::move(result_cb));
  RunLoopUntilIdle();

  ASSERT_TRUE(channel.has_value());
  ASSERT_TRUE(channel->has_connection());

  ASSERT_TRUE(fake_chan.has_value());
  ASSERT_TRUE(fake_chan.value()->activated());
  FakeChannel::WeakPtr fake_chan_ptr = fake_chan.value();
  int send_count = 0;
  fake_chan_ptr->SetSendCallback([&send_count](auto buffer) { send_count++; },
                                 pw_dispatcher());

  fidl::InterfacePtr<fbt::Channel> conn = channel->mutable_connection()->Bind();
  int send_cb_count = 0;
  std::vector<fbt::Packet> packets{fbt::Packet{std::vector<uint8_t>{0x02}}};
  conn->Send(std::move(packets), [&](fbt::Channel_Send_Result result) {
    EXPECT_TRUE(result.is_response());
    send_cb_count++;
  });
  RunLoopUntilIdle();
  EXPECT_EQ(1, send_count);
  EXPECT_EQ(1, send_cb_count);
}

TEST_F(ProfileServerTestConnectedPeer,
       ConnectFailsDueToChannelActivationFailure) {
  server()->set_use_sockets(false);

  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());
  // Approve pairing requests.
  pairing_delegate->SetConfirmPairingCallback(
      [](bt::PeerId, auto confirm_cb) { confirm_cb(true); });
  pairing_delegate->SetCompletePairingCallback(
      [&](bt::PeerId, bt::sm::Result<> status) {
        EXPECT_EQ(fit::ok(), status);
      });

  bt::l2cap::ChannelParameters expected_params;
  l2cap()->ExpectOutboundL2capChannel(
      connection()->link().handle(), kPsm, 0x40, 0x41, expected_params);

  std::optional<FakeChannel::WeakPtr> fake_chan;
  l2cap()->set_channel_callback([&fake_chan](FakeChannel::WeakPtr chan) {
    chan->set_activate_fails(true);
    fake_chan = std::move(chan);
  });

  int connect_cb_count = 0;
  auto connect_cb = [&](fidlbredr::Profile_Connect_Result result) {
    EXPECT_TRUE(result.is_err());
    connect_cb_count++;
  };

  fuchsia::bluetooth::PeerId peer_id{peer()->identifier().value()};

  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(kPsm);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  client()->Connect(peer_id, std::move(conn_params), std::move(connect_cb));
  RunLoopUntilIdle();
  EXPECT_EQ(connect_cb_count, 1);
  EXPECT_FALSE(fake_chan.value()->activated());
}

// Verifies that a socket channel relay is correctly set up such that bytes
// written to the socket are sent to the channel.
TEST_P(ProfileServerTestConnectedPeerWithBoolParam,
       ConnectionReceiverReturnsValidSocket) {
  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());

  fidlbredr::ProfileAdvertiseRequest adv_request;

  std::optional<FakeConnectionReceiver> connect_receiver;
  if (GetParam()) {
    fidlbredr::ConnectionReceiverHandle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_receiver(std::move(connect_receiver_handle));
  } else {
    fidlbredr::ConnectionReceiver2Handle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_connection_receiver(std::move(connect_receiver_handle));
  }

  std::optional<FakeChannel::WeakPtr> fake_chan;
  l2cap()->set_channel_callback(
      [&fake_chan](FakeChannel::WeakPtr chan) { fake_chan = std::move(chan); });

  std::vector<fidlbredr::ServiceDefinition> services;
  services.emplace_back(MakeFIDLServiceDefinition());

  adv_request.set_services(std::move(services));
  client()->Advertise(std::move(adv_request), NopAdvertiseCallback);
  RunLoopUntilIdle();

  ASSERT_EQ(connect_receiver->connected_count(), 0u);
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      connection()->link().handle(), kPsm, 0x40, 0x41));
  RunLoopUntilIdle();

  ASSERT_EQ(connect_receiver->connected_count(), 1u);
  ASSERT_EQ(connect_receiver->peer_id().value().value,
            peer()->identifier().value());
  ASSERT_TRUE(connect_receiver->channel().has_value());
  ASSERT_TRUE(connect_receiver->channel().value().has_socket());
  // Taking channel is safe because of the previous checks.
  fidlbredr::Channel channel = connect_receiver->take_channel();

  ASSERT_TRUE(fake_chan.has_value());
  FakeChannel::WeakPtr fake_chan_ptr = fake_chan.value();
  size_t send_count = 0;
  fake_chan_ptr->SetSendCallback([&send_count](auto buffer) { send_count++; },
                                 pw_dispatcher());

  const char write_data[2] = "a";
  size_t bytes_written = 0;
  int status = channel.socket().write(
      0, write_data, sizeof(write_data) - 1, &bytes_written);
  EXPECT_EQ(ZX_OK, status);
  EXPECT_EQ(1u, bytes_written);
  RunLoopUntilIdle();
  EXPECT_EQ(1u, send_count);
}

// Verifies that a BrEdrConnectionServer is correctly set up such that bytes
// written to the Connection are sent to the channel.
TEST_P(ProfileServerTestConnectedPeerWithBoolParam,
       ConnectionReceiverReturnsValidConnection) {
  server()->set_use_sockets(false);

  std::unique_ptr<bt::gap::FakePairingDelegate> pairing_delegate =
      std::make_unique<bt::gap::FakePairingDelegate>(
          bt::sm::IOCapability::kDisplayYesNo);
  adapter()->SetPairingDelegate(pairing_delegate->GetWeakPtr());

  fidlbredr::ProfileAdvertiseRequest adv_request;

  std::optional<FakeConnectionReceiver> connect_receiver;
  if (GetParam()) {
    fidlbredr::ConnectionReceiverHandle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_receiver(std::move(connect_receiver_handle));
  } else {
    fidlbredr::ConnectionReceiver2Handle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_connection_receiver(std::move(connect_receiver_handle));
  }

  std::optional<FakeChannel::WeakPtr> fake_chan;
  l2cap()->set_channel_callback(
      [&fake_chan](FakeChannel::WeakPtr chan) { fake_chan = std::move(chan); });

  std::vector<fidlbredr::ServiceDefinition> services;
  services.emplace_back(MakeFIDLServiceDefinition());

  adv_request.set_services(std::move(services));
  client()->Advertise(std::move(adv_request), NopAdvertiseCallback);
  RunLoopUntilIdle();

  ASSERT_EQ(connect_receiver->connected_count(), 0u);
  EXPECT_TRUE(l2cap()->TriggerInboundL2capChannel(
      connection()->link().handle(), kPsm, 0x40, 0x41));
  RunLoopUntilIdle();

  ASSERT_EQ(connect_receiver->connected_count(), 1u);
  ASSERT_EQ(connect_receiver->peer_id().value().value,
            peer()->identifier().value());
  ASSERT_TRUE(connect_receiver->channel().has_value());
  ASSERT_TRUE(connect_receiver->channel().value().has_connection());
  // Taking channel is safe because of the previous checks.
  fidlbredr::Channel channel = connect_receiver->take_channel();

  ASSERT_TRUE(fake_chan.has_value());
  FakeChannel::WeakPtr fake_chan_ptr = fake_chan.value();
  int send_count = 0;
  fake_chan_ptr->SetSendCallback([&send_count](auto buffer) { send_count++; },
                                 pw_dispatcher());

  fidl::InterfacePtr<fbt::Channel> conn = channel.mutable_connection()->Bind();
  int send_cb_count = 0;
  std::vector<fbt::Packet> packets{fbt::Packet{std::vector<uint8_t>{0x02}}};
  conn->Send(std::move(packets), [&](fbt::Channel_Send_Result result) {
    EXPECT_TRUE(result.is_response());
    send_cb_count++;
  });
  RunLoopUntilIdle();
  EXPECT_EQ(1, send_count);
  EXPECT_EQ(1, send_cb_count);
}

TEST_F(ProfileServerTest, ConnectScoWithInvalidParameters) {
  std::vector<fidlbredr::ScoConnectionParameters> bad_sco_params;
  bad_sco_params.emplace_back();
  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{1});
  request.set_initiator(true);
  request.set_params(std::move(bad_sco_params));
  fidlbredr::ScoConnectionHandle connection_handle;
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  RunLoopUntilIdle();
  ASSERT_TRUE(connection_complete);
  ASSERT_TRUE(connection_complete->is_error());
  EXPECT_EQ(connection_complete->error(),
            fidlbredr::ScoErrorCode::INVALID_ARGUMENTS);
  EXPECT_FALSE(sco_connection.is_bound());
}

TEST_F(ProfileServerTest, ConnectScoWithMissingPeerId) {
  fidlbredr::ScoConnectionParameters sco_params =
      CreateScoConnectionParameters();
  uint8_t sco_offload_index = 6;
  EXPECT_TRUE(
      fidl_helpers::FidlToScoParameters(sco_params, sco_offload_index).is_ok());
  std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
  sco_params_list.emplace_back(std::move(sco_params));
  fidlbredr::ProfileConnectScoRequest request;
  request.set_initiator(true);
  request.set_params(std::move(sco_params_list));
  fidlbredr::ScoConnectionHandle connection_handle;
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  RunLoopUntilIdle();
  ASSERT_TRUE(connection_complete);
  ASSERT_TRUE(connection_complete->is_error());
  EXPECT_EQ(connection_complete->error(),
            fidlbredr::ScoErrorCode::INVALID_ARGUMENTS);
  EXPECT_FALSE(sco_connection.is_bound());
}

TEST_F(ProfileServerTest, ConnectScoWithMissingConnectionDoesNotCrash) {
  fidlbredr::ScoConnectionParameters sco_params =
      CreateScoConnectionParameters();
  uint8_t sco_offload_index = 6;
  EXPECT_TRUE(
      fidl_helpers::FidlToScoParameters(sco_params, sco_offload_index).is_ok());
  std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
  sco_params_list.emplace_back(std::move(sco_params));
  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{1});
  request.set_initiator(true);
  request.set_params(std::move(sco_params_list));
  client()->ConnectSco(std::move(request));
  RunLoopUntilIdle();
}

TEST_F(ProfileServerTest, ConnectScoWithEmptyParameters) {
  fidlbredr::ScoConnectionHandle connection_handle;
  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{1});
  request.set_initiator(true);
  request.set_params({});
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  RunLoopUntilIdle();
  ASSERT_TRUE(connection_complete);
  ASSERT_TRUE(connection_complete->is_error());
  EXPECT_EQ(connection_complete->error(),
            fidlbredr::ScoErrorCode::INVALID_ARGUMENTS);
  EXPECT_FALSE(sco_connection.is_bound());
  ASSERT_TRUE(sco_connection_status);
  EXPECT_EQ(sco_connection_status.value(), ZX_ERR_PEER_CLOSED);
}

TEST_F(ProfileServerTest, ConnectScoInitiatorWithTooManyParameters) {
  std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
  sco_params_list.emplace_back(CreateScoConnectionParameters());
  sco_params_list.emplace_back(CreateScoConnectionParameters());

  fidlbredr::ScoConnectionHandle connection_handle;
  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{1});
  request.set_initiator(true);
  request.set_params(std::move(sco_params_list));
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  RunLoopUntilIdle();
  ASSERT_TRUE(connection_complete);
  ASSERT_TRUE(connection_complete->is_error());
  EXPECT_EQ(connection_complete->error(),
            fidlbredr::ScoErrorCode::INVALID_ARGUMENTS);
  EXPECT_FALSE(sco_connection.is_bound());
  ASSERT_TRUE(sco_connection_status);
  EXPECT_EQ(sco_connection_status.value(), ZX_ERR_PEER_CLOSED);
}

TEST_F(ProfileServerTest, ConnectScoWithUnconnectedPeerReturnsError) {
  fidlbredr::ScoConnectionParameters sco_params =
      CreateScoConnectionParameters();
  uint8_t sco_offload_index = 6;
  EXPECT_TRUE(
      fidl_helpers::FidlToScoParameters(sco_params, sco_offload_index).is_ok());
  std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
  sco_params_list.emplace_back(std::move(sco_params));

  fidlbredr::ScoConnectionHandle connection_handle;
  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{1});
  request.set_initiator(true);
  request.set_params(std::move(sco_params_list));
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  RunLoopUntilIdle();
  ASSERT_TRUE(connection_complete);
  ASSERT_TRUE(connection_complete->is_error());
  EXPECT_EQ(connection_complete->error(), fidlbredr::ScoErrorCode::FAILURE);
  EXPECT_FALSE(sco_connection.is_bound());
  ASSERT_TRUE(sco_connection_status);
  EXPECT_EQ(sco_connection_status.value(), ZX_ERR_PEER_CLOSED);
}

TEST_F(ProfileServerTestConnectedPeer, ConnectScoInitiatorSuccess) {
  fidlbredr::ScoConnectionParameters sco_params =
      CreateScoConnectionParameters(fidlbredr::HfpParameterSet::T1);
  uint8_t sco_offload_index = 6;
  EXPECT_TRUE(
      fidl_helpers::FidlToScoParameters(sco_params, sco_offload_index).is_ok());
  std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
  sco_params_list.emplace_back(std::move(sco_params));

  fidlbredr::ScoConnectionHandle connection_handle;
  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{peer()->identifier().value()});
  request.set_initiator(true);
  request.set_params(std::move(sco_params_list));
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  RunLoopUntilIdle();
  ASSERT_TRUE(connection_complete);
  ASSERT_TRUE(connection_complete->is_connected_params());
  EXPECT_TRUE(sco_connection.is_bound());
  ASSERT_TRUE(connection_complete->connected_params().has_parameter_set());
  EXPECT_EQ(connection_complete->connected_params().parameter_set(),
            fidlbredr::HfpParameterSet::T1);
  ASSERT_TRUE(connection_complete->connected_params().has_max_tx_data_size());
  EXPECT_EQ(connection_complete->connected_params().max_tx_data_size(),
            kSynchronousDataPacketLength);
}

TEST_F(ProfileServerTestConnectedPeer, ConnectScoResponderSuccess) {
  // Use 2 parameter sets to test that the profile server returns the second set
  // when a SCO connection request is received (T2 is ESCO only and D0 is SCO
  // only, so D0 will be used to accept the connection).
  std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
  sco_params_list.emplace_back(
      CreateScoConnectionParameters(fidlbredr::HfpParameterSet::T2));
  sco_params_list.emplace_back(
      CreateScoConnectionParameters(fidlbredr::HfpParameterSet::D0));

  fidlbredr::ScoConnectionHandle connection_handle;
  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{peer()->identifier().value()});
  request.set_initiator(false);
  request.set_params(std::move(sco_params_list));
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  RunLoopUntilIdle();
  // Receive a SCO connection request. The D0 parameters will be used to accept
  // the request.
  test_device()->SendConnectionRequest(peer()->address(),
                                       pw::bluetooth::emboss::LinkType::SCO);
  RunLoopUntilIdle();
  ASSERT_TRUE(connection_complete);
  ASSERT_TRUE(connection_complete->is_connected_params());
  EXPECT_TRUE(sco_connection.is_bound());
  ASSERT_TRUE(connection_complete->connected_params().has_parameter_set());
  EXPECT_EQ(connection_complete->connected_params().parameter_set(),
            fidlbredr::HfpParameterSet::D0);
}

TEST_F(ProfileServerTestConnectedPeer,
       ScoConnectionReadBeforeConnectionComplete) {
  std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
  sco_params_list.emplace_back(
      CreateScoConnectionParameters(fidlbredr::HfpParameterSet::D0));

  fidlbredr::ScoConnectionHandle connection_handle;
  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{peer()->identifier().value()});
  request.set_initiator(false);
  request.set_params(std::move(sco_params_list));
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  RunLoopUntilIdle();
  ASSERT_FALSE(connection_complete);

  sco_connection->Read(
      [](fidlbredr::ScoConnection_Read_Result result) { FAIL(); });
  RunLoopUntilIdle();
  ASSERT_TRUE(sco_connection_status);
  EXPECT_EQ(sco_connection_status.value(), ZX_ERR_IO_REFUSED);
}

TEST_F(ProfileServerTestConnectedPeer,
       ScoConnectionWriteBeforeConnectionComplete) {
  std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
  sco_params_list.emplace_back(
      CreateScoConnectionParameters(fidlbredr::HfpParameterSet::D0));

  fidlbredr::ScoConnectionHandle connection_handle;
  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{peer()->identifier().value()});
  request.set_initiator(false);
  request.set_params(std::move(sco_params_list));
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  RunLoopUntilIdle();
  ASSERT_FALSE(connection_complete);

  ::fuchsia::bluetooth::bredr::ScoConnectionWriteRequest write;
  write.set_data({0x00});
  sco_connection->Write(
      std::move(write),
      [](::fuchsia::bluetooth::bredr::ScoConnection_Write_Result result) {
        FAIL();
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(sco_connection_status);
  EXPECT_EQ(sco_connection_status.value(), ZX_ERR_IO_REFUSED);
}

TEST_F(ProfileServerTestConnectedPeer,
       ConnectScoResponderUnconnectedPeerReturnsError) {
  std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
  sco_params_list.emplace_back(CreateScoConnectionParameters());

  fidlbredr::ScoConnectionHandle connection_handle;
  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{1});
  request.set_initiator(false);
  request.set_params(std::move(sco_params_list));
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  RunLoopUntilIdle();
  ASSERT_TRUE(connection_complete);
  ASSERT_TRUE(connection_complete->is_error());
  EXPECT_EQ(connection_complete->error(), fidlbredr::ScoErrorCode::FAILURE);
  EXPECT_FALSE(sco_connection.is_bound());
  ASSERT_TRUE(sco_connection_status);
  EXPECT_EQ(sco_connection_status.value(), ZX_ERR_PEER_CLOSED);
}

TEST_F(ProfileServerTestConnectedPeer, ConnectScoInitiatorAndCloseProtocol) {
  fidlbredr::ScoConnectionParameters sco_params =
      CreateScoConnectionParameters();
  uint8_t sco_offload_index = 6;
  EXPECT_TRUE(
      fidl_helpers::FidlToScoParameters(sco_params, sco_offload_index).is_ok());
  std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
  sco_params_list.emplace_back(std::move(sco_params));

  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{peer()->identifier().value()});
  request.set_initiator(true);
  request.set_params(std::move(sco_params_list));
  fidlbredr::ScoConnectionHandle connection_handle;
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  sco_connection.Unbind();
  RunLoopUntilIdle();
  ASSERT_FALSE(connection_complete);
}

// Verifies that the profile server gracefully ignores connection results after
// the receiver has closed.
TEST_F(ProfileServerTestConnectedPeer,
       ConnectScoInitiatorAndCloseReceiverBeforeCompleteEvent) {
  fidlbredr::ScoConnectionParameters sco_params =
      CreateScoConnectionParameters();
  uint8_t sco_offload_index = 6;
  EXPECT_TRUE(
      fidl_helpers::FidlToScoParameters(sco_params, sco_offload_index).is_ok());
  std::vector<fidlbredr::ScoConnectionParameters> sco_params_list;
  sco_params_list.emplace_back(std::move(sco_params));

  test_device()->SetDefaultCommandStatus(
      bt::hci_spec::kEnhancedSetupSynchronousConnection,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  fidlbredr::ProfileConnectScoRequest request;
  request.set_peer_id(fuchsia::bluetooth::PeerId{peer()->identifier().value()});
  request.set_initiator(true);
  request.set_params(std::move(sco_params_list));
  fidlbredr::ScoConnectionHandle connection_handle;
  request.set_connection(connection_handle.NewRequest());

  fidlbredr::ScoConnectionPtr sco_connection = connection_handle.Bind();
  std::optional<zx_status_t> sco_connection_status;
  sco_connection.set_error_handler(
      [&](zx_status_t status) { sco_connection_status = status; });
  std::optional<fidlbredr::ScoConnectionOnConnectionCompleteRequest>
      connection_complete;
  sco_connection.events().OnConnectionComplete =
      [&](fidlbredr::ScoConnectionOnConnectionCompleteRequest request) {
        connection_complete = std::move(request);
      };

  client()->ConnectSco(std::move(request));
  sco_connection.Unbind();
  RunLoopUntilIdle();
  ASSERT_FALSE(connection_complete);
  test_device()->SendCommandChannelPacket(
      bt::testing::SynchronousConnectionCompletePacket(
          0x00,
          peer()->address(),
          bt::hci_spec::LinkType::kSCO,
          pw::bluetooth::emboss::StatusCode::CONNECTION_TIMEOUT));
  RunLoopUntilIdle();
  ASSERT_FALSE(connection_complete);
}

class ProfileServerTestFakeAdapter
    : public bt::fidl::testing::FakeAdapterTestFixture {
 public:
  ProfileServerTestFakeAdapter() = default;
  ~ProfileServerTestFakeAdapter() override = default;

  void SetUp() override {
    FakeAdapterTestFixture::SetUp();

    fidlbredr::ProfileHandle profile_handle;
    client_.Bind(std::move(profile_handle));
    uint8_t sco_offload_index = 6;
    server_ = std::make_unique<ProfileServer>(adapter()->AsWeakPtr(),
                                              lease_provider_,
                                              sco_offload_index,
                                              client_.NewRequest(dispatcher()));
  }

  void TearDown() override { FakeAdapterTestFixture::TearDown(); }

  fidlbredr::ProfilePtr& client() { return client_; }

  pw::bluetooth_sapphire::testing::FakeLeaseProvider& lease_provider() {
    return lease_provider_;
  }

 private:
  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<ProfileServer> server_;
  fidlbredr::ProfilePtr client_;
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ProfileServerTestFakeAdapter);
};

class ProfileServerTestFakeAdapterWithBoolParam
    : public ProfileServerTestFakeAdapter,
      public ::testing::WithParamInterface<bool> {};

TEST_F(ProfileServerTestFakeAdapter,
       ConnectChannelParametersContainsFlushTimeout) {
  const bt::PeerId kPeerId;
  const fuchsia::bluetooth::PeerId kFidlPeerId{kPeerId.value()};
  const pw::chrono::SystemClock::duration kFlushTimeout(
      std::chrono::milliseconds(100));

  FakeChannel::WeakPtr last_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { last_channel = std::move(chan); });

  // Set L2CAP channel parameters
  fbt::ChannelParameters chan_params;
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  chan_params.set_flush_timeout(kFlushTimeout.count());
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  l2cap_params.set_parameters(std::move(chan_params));
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(kFidlPeerId,
                    std::move(conn_params),
                    [&](fidlbredr::Profile_Connect_Result result) {
                      ASSERT_TRUE(result.is_response());
                      response_channel = std::move(result.response().channel);
                    });
  RunLoopUntilIdle();
  ASSERT_TRUE(last_channel.is_alive());
  EXPECT_EQ(last_channel->info().flush_timeout, std::optional(kFlushTimeout));
  ASSERT_TRUE(response_channel.has_value());
  ASSERT_TRUE(response_channel->has_flush_timeout());
  ASSERT_EQ(response_channel->flush_timeout(), kFlushTimeout.count());
}

TEST_P(ProfileServerTestFakeAdapterWithBoolParam,
       AdvertiseChannelParametersContainsFlushTimeout) {
  const pw::chrono::SystemClock::duration kFlushTimeout(
      std::chrono::milliseconds(100));
  const bt::hci_spec::ConnectionHandle kHandle(1);

  std::vector<fidlbredr::ServiceDefinition> services;
  services.emplace_back(MakeFIDLServiceDefinition());
  fbt::ChannelParameters chan_params;
  chan_params.set_flush_timeout(kFlushTimeout.count());

  fidlbredr::ProfileAdvertiseRequest adv_request;

  std::optional<FakeConnectionReceiver> connect_receiver;
  if (GetParam()) {
    fidlbredr::ConnectionReceiverHandle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_receiver(std::move(connect_receiver_handle));
  } else {
    fidlbredr::ConnectionReceiver2Handle connect_receiver_handle;
    connect_receiver.emplace(connect_receiver_handle.NewRequest(),
                             dispatcher());
    adv_request.set_connection_receiver(std::move(connect_receiver_handle));
  }

  adv_request.set_services(std::move(services));
  adv_request.set_parameters(std::move(chan_params));
  client()->Advertise(std::move(adv_request), NopAdvertiseCallback);
  RunLoopUntilIdle();

  ASSERT_EQ(adapter()->fake_bredr()->registered_services().size(), 1u);
  auto service_iter = adapter()->fake_bredr()->registered_services().begin();
  EXPECT_EQ(service_iter->second.channel_params.flush_timeout,
            std::optional(kFlushTimeout));

  bt::l2cap::ChannelInfo chan_info =
      bt::l2cap::ChannelInfo::MakeBasicMode(bt::l2cap::kDefaultMTU,
                                            bt::l2cap::kDefaultMTU,
                                            bt::l2cap::kAVDTP,
                                            kFlushTimeout);
  auto channel =
      std::make_unique<FakeChannel>(bt::l2cap::kFirstDynamicChannelId,
                                    bt::l2cap::kFirstDynamicChannelId,
                                    kHandle,
                                    bt::LinkType::kACL,
                                    chan_info);
  service_iter->second.connect_callback(channel->GetWeakPtr(),
                                        MakeL2capProtocolListElement());
  RunLoopUntilIdle();
  ASSERT_TRUE(connect_receiver->channel().has_value());
  fidlbredr::Channel fidl_channel = connect_receiver->take_channel();
  ASSERT_TRUE(fidl_channel.has_flush_timeout());
  EXPECT_EQ(fidl_channel.flush_timeout(), kFlushTimeout.count());

  channel->Close();
  RunLoopUntilIdle();
}

TEST_P(ProfileServerTestFakeAdapterWithBoolParam,
       ClientClosesAdvertisementWithConnectionReceiver) {
  fidlbredr::ProfileAdvertiseRequest adv_request;

  std::optional<FakeConnectionReceiver> connection_receiver;
  if (GetParam()) {
    fidlbredr::ConnectionReceiverHandle receiver_handle;
    connection_receiver.emplace(receiver_handle.NewRequest(), dispatcher());
    adv_request.set_receiver(std::move(receiver_handle));
  } else {
    fidlbredr::ConnectionReceiver2Handle receiver_handle;
    connection_receiver.emplace(receiver_handle.NewRequest(), dispatcher());
    adv_request.set_connection_receiver(std::move(receiver_handle));
  }

  std::vector<fidlbredr::ServiceDefinition> services;
  services.emplace_back(MakeFIDLServiceDefinition());

  size_t cb_count = 0;
  auto cb = [&](fidlbredr::Profile_Advertise_Result result) {
    cb_count++;
    EXPECT_TRUE(result.is_response());
  };

  adv_request.set_services(std::move(services));
  client()->Advertise(std::move(adv_request), std::move(cb));
  RunLoopUntilIdle();
  ASSERT_EQ(cb_count, 1u);
  ASSERT_EQ(adapter()->fake_bredr()->registered_services().size(), 1u);

  // Client closes Advertisement by dropping the `ConnectionReceiver`. This is
  // OK, and the profile server should handle this by unregistering the
  // advertisement.
  connection_receiver->Close();
  RunLoopUntilIdle();
  ASSERT_EQ(adapter()->fake_bredr()->registered_services().size(), 0u);
}

TEST_F(ProfileServerTestFakeAdapter, AdvertiseWithMissingFields) {
  size_t cb_ok_count = 0;
  auto adv_ok_cb = [&](fidlbredr::Profile_Advertise_Result result) {
    cb_ok_count++;
    EXPECT_TRUE(result.is_response());
  };
  size_t cb_err_count = 0;
  auto adv_err_cb = [&](fidlbredr::Profile_Advertise_Result result) {
    cb_err_count++;
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.err(), fuchsia::bluetooth::ErrorCode::INVALID_ARGUMENTS);
  };

  fidlbredr::ProfileAdvertiseRequest adv_request_missing_receiver;
  std::vector<fidlbredr::ServiceDefinition> services1;
  services1.emplace_back(MakeFIDLServiceDefinition());
  adv_request_missing_receiver.set_services(std::move(services1));
  adv_request_missing_receiver.set_parameters(
      ::fuchsia::bluetooth::ChannelParameters());
  client()->Advertise(std::move(adv_request_missing_receiver), adv_err_cb);
  RunLoopUntilIdle();
  ASSERT_EQ(cb_err_count, 1u);
  ASSERT_EQ(adapter()->fake_bredr()->registered_services().size(), 0u);

  fidlbredr::ConnectionReceiver2Handle connect_receiver_handle1;
  FakeConnectionReceiver connect_receiver1(
      connect_receiver_handle1.NewRequest(), dispatcher());

  fidlbredr::ProfileAdvertiseRequest adv_request_missing_services;
  adv_request_missing_services.set_connection_receiver(
      std::move(connect_receiver_handle1));
  adv_request_missing_services.set_parameters(
      ::fuchsia::bluetooth::ChannelParameters());
  client()->Advertise(std::move(adv_request_missing_services), adv_err_cb);
  RunLoopUntilIdle();
  ASSERT_EQ(cb_err_count, 2u);
  ASSERT_EQ(adapter()->fake_bredr()->registered_services().size(), 0u);

  // Missing parameters is allowed.
  fidlbredr::ProfileAdvertiseRequest adv_request_missing_parameters;
  std::vector<fidlbredr::ServiceDefinition> services2;
  services2.emplace_back(MakeFIDLServiceDefinition());
  adv_request_missing_parameters.set_services(std::move(services2));
  fidlbredr::ConnectionReceiver2Handle connect_receiver_handle2;
  FakeConnectionReceiver connect_receiver2(
      connect_receiver_handle2.NewRequest(), dispatcher());
  adv_request_missing_parameters.set_connection_receiver(
      std::move(connect_receiver_handle2));
  client()->Advertise(std::move(adv_request_missing_parameters), adv_ok_cb);
  RunLoopUntilIdle();
  ASSERT_EQ(cb_ok_count, 1u);
  ASSERT_EQ(adapter()->fake_bredr()->registered_services().size(), 1u);
}

TEST_F(ProfileServerTestFakeAdapter,
       L2capParametersExtRequestParametersSucceeds) {
  const bt::PeerId kPeerId;
  const fuchsia::bluetooth::PeerId kFidlPeerId{kPeerId.value()};
  const zx::duration kFlushTimeout(zx::msec(100));
  const uint16_t kMaxRxSduSize(200);

  FakeChannel::WeakPtr last_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { last_channel = chan; });

  // Set L2CAP channel parameters
  fbt::ChannelParameters chan_params;
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  chan_params.set_channel_mode(fbt::ChannelMode::BASIC);
  chan_params.set_max_rx_packet_size(kMaxRxSduSize);
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  l2cap_params.set_parameters(std::move(chan_params));
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(kFidlPeerId,
                    std::move(conn_params),
                    [&](fidlbredr::Profile_Connect_Result result) {
                      ASSERT_TRUE(result.is_response());
                      response_channel = std::move(result.response().channel);
                    });
  RunLoopUntilIdle();
  ASSERT_TRUE(last_channel.is_alive());
  EXPECT_FALSE(last_channel->info().flush_timeout.has_value());
  ASSERT_TRUE(response_channel.has_value());
  ASSERT_FALSE(response_channel->has_flush_timeout());
  ASSERT_TRUE(response_channel->has_ext_l2cap());

  fbt::ChannelParameters request_chan_params;
  request_chan_params.set_flush_timeout(kFlushTimeout.get());

  std::optional<fbt::ChannelParameters> result_chan_params;
  fidlbredr::L2capParametersExtPtr l2cap_client =
      response_channel->mutable_ext_l2cap()->Bind();
  l2cap_client->RequestParameters(
      std::move(request_chan_params),
      [&](fidlbredr::L2capParametersExt_RequestParameters_Result new_params) {
        result_chan_params = new_params.response().ResultValue_();
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(result_chan_params.has_value());
  ASSERT_TRUE(result_chan_params->has_channel_mode());
  ASSERT_TRUE(result_chan_params->has_max_rx_packet_size());
  // TODO(fxbug.dev/42152567): set current security requirements in returned
  // channel parameters
  ASSERT_FALSE(result_chan_params->has_security_requirements());
  ASSERT_TRUE(result_chan_params->has_flush_timeout());
  EXPECT_EQ(result_chan_params->channel_mode(), fbt::ChannelMode::BASIC);
  EXPECT_EQ(result_chan_params->max_rx_packet_size(), kMaxRxSduSize);
  EXPECT_EQ(result_chan_params->flush_timeout(), kFlushTimeout.get());
  l2cap_client.Unbind();
  RunLoopUntilIdle();
}

TEST_F(ProfileServerTestFakeAdapter, L2capParametersExtRequestParametersFails) {
  const bt::PeerId kPeerId;
  const fuchsia::bluetooth::PeerId kFidlPeerId{kPeerId.value()};
  const zx::duration kFlushTimeout(zx::msec(100));

  FakeChannel::WeakPtr last_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { last_channel = chan; });

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(kFidlPeerId,
                    std::move(conn_params),
                    [&](fidlbredr::Profile_Connect_Result result) {
                      ASSERT_TRUE(result.is_response());
                      response_channel = std::move(result.response().channel);
                    });
  RunLoopUntilIdle();
  ASSERT_TRUE(last_channel.is_alive());
  EXPECT_FALSE(last_channel->info().flush_timeout.has_value());
  ASSERT_TRUE(response_channel.has_value());
  ASSERT_FALSE(response_channel->has_flush_timeout());
  ASSERT_TRUE(response_channel->has_ext_l2cap());

  last_channel->set_flush_timeout_succeeds(false);

  fbt::ChannelParameters request_chan_params;
  request_chan_params.set_flush_timeout(kFlushTimeout.get());
  std::optional<fbt::ChannelParameters> result_chan_params;
  fidlbredr::L2capParametersExtPtr l2cap_client =
      response_channel->mutable_ext_l2cap()->Bind();
  l2cap_client->RequestParameters(
      std::move(request_chan_params),
      [&](fidlbredr::L2capParametersExt_RequestParameters_Result new_params) {
        result_chan_params = new_params.response().ResultValue_();
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(result_chan_params.has_value());
  EXPECT_FALSE(result_chan_params->has_flush_timeout());
  l2cap_client.Unbind();
  RunLoopUntilIdle();
}

TEST_F(ProfileServerTestFakeAdapter,
       L2capParametersExtRequestParametersClosedOnChannelClosed) {
  const bt::PeerId kPeerId;
  const fuchsia::bluetooth::PeerId kFidlPeerId{kPeerId.value()};

  FakeChannel::WeakPtr last_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { last_channel = chan; });

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(kFidlPeerId,
                    std::move(conn_params),
                    [&](fidlbredr::Profile_Connect_Result result) {
                      ASSERT_TRUE(result.is_response());
                      response_channel = std::move(result.response().channel);
                    });
  RunLoopUntilIdle();
  ASSERT_TRUE(last_channel.is_alive());
  ASSERT_TRUE(response_channel.has_value());

  fidlbredr::L2capParametersExtPtr l2cap_client =
      response_channel->mutable_ext_l2cap()->Bind();
  bool l2cap_client_closed = false;
  l2cap_client.set_error_handler(
      [&](zx_status_t /*status*/) { l2cap_client_closed = true; });

  // Closing the channel should close l2cap_client (after running the loop).
  last_channel->Close();
  // Destroy the channel (like the real LogicalLink would) to verify that
  // ProfileServer doesn't try to use channel pointers.
  EXPECT_TRUE(adapter()->fake_bredr()->DestroyChannel(last_channel->id()));

  // Any request for the closed channel should be ignored.
  fbt::ChannelParameters request_chan_params;
  std::optional<fbt::ChannelParameters> result_chan_params;
  l2cap_client->RequestParameters(
      std::move(request_chan_params),
      [&](fidlbredr::L2capParametersExt_RequestParameters_Result new_params) {
        result_chan_params = new_params.response().ResultValue_();
      });
  RunLoopUntilIdle();
  EXPECT_TRUE(l2cap_client_closed);
  EXPECT_FALSE(result_chan_params.has_value());
  l2cap_client.Unbind();
  RunLoopUntilIdle();
}

TEST_F(ProfileServerTestFakeAdapter,
       L2capParametersExtRequestParametersFlushTimeoutCallbackOnChannelClosed) {
  const bt::PeerId kPeerId;
  const fuchsia::bluetooth::PeerId kFidlPeerId{kPeerId.value()};

  FakeChannel::WeakPtr last_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { last_channel = chan; });

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(kFidlPeerId,
                    std::move(conn_params),
                    [&](fidlbredr::Profile_Connect_Result result) {
                      ASSERT_TRUE(result.is_response());
                      response_channel = std::move(result.response().channel);
                    });
  RunLoopUntilIdle();
  ASSERT_TRUE(last_channel.is_alive());
  ASSERT_TRUE(response_channel.has_value());

  fidlbredr::L2capParametersExtPtr l2cap_client =
      response_channel->mutable_ext_l2cap()->Bind();
  bool l2cap_client_closed = false;
  l2cap_client.set_error_handler(
      [&](zx_status_t /*status*/) { l2cap_client_closed = true; });

  bt::hci::ResultCallback<> pending_flush_timeout_cb;
  last_channel->set_flush_timeout_callback(
      [&](auto /*flush_timeout*/, bt::hci::ResultCallback<> callback) {
        pending_flush_timeout_cb = std::move(callback);
      });

  // Send a request with a flush timeout so that it executes the lambda callback
  // asynchronously.
  fbt::ChannelParameters request_chan_params;
  request_chan_params.set_flush_timeout(100);
  std::optional<fbt::ChannelParameters> result_chan_params;
  l2cap_client->RequestParameters(
      std::move(request_chan_params),
      [&](fidlbredr::L2capParametersExt_RequestParameters_Result new_params) {
        result_chan_params = new_params.response().ResultValue_();
      });

  // Process RequestParameters message
  RunLoopUntilIdle();
  ASSERT_TRUE(pending_flush_timeout_cb);

  // Closing the channel should close l2cap_client.
  last_channel->Close();
  // Destroy the channel (like the real LogicalLink would) while the async
  // SetBrEdrAutomaticFlushTimeout callback is still pending.
  EXPECT_TRUE(adapter()->fake_bredr()->DestroyChannel(last_channel->id()));

  // Execute the pending callback. It should gracefully handle the dead pointer
  // and return without crashing.
  pending_flush_timeout_cb(fit::ok());

  // Process the channel closure and the error handler logic asynchronously.
  RunLoopUntilIdle();

  EXPECT_TRUE(l2cap_client_closed);
  EXPECT_FALSE(result_chan_params.has_value());
  l2cap_client.Unbind();
  RunLoopUntilIdle();
}

TEST_F(ProfileServerTestFakeAdapter,
       AudioDirectionExtRequestParametersClosedOnChannelClosed) {
  const bt::PeerId kPeerId;
  const fuchsia::bluetooth::PeerId kFidlPeerId{kPeerId.value()};

  FakeChannel::WeakPtr last_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { last_channel = chan; });

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(kFidlPeerId,
                    std::move(conn_params),
                    [&](fidlbredr::Profile_Connect_Result result) {
                      ASSERT_TRUE(result.is_response());
                      response_channel = std::move(result.response().channel);
                    });
  RunLoopUntilIdle();
  ASSERT_TRUE(last_channel.is_alive());
  ASSERT_TRUE(response_channel.has_value());

  fidlbredr::AudioDirectionExtPtr audio_client =
      response_channel->mutable_ext_direction()->Bind();
  bool audio_client_closed = false;
  audio_client.set_error_handler(
      [&](zx_status_t /*status*/) { audio_client_closed = true; });

  // Closing the channel should close audio_client (after running the loop).
  last_channel->Close();
  // Destroy the channel (like the real LogicalLink would) to verify that
  // ProfileServer doesn't try to use channel pointers.
  EXPECT_TRUE(adapter()->fake_bredr()->DestroyChannel(last_channel->id()));

  // Any request for the closed channel should be ignored.
  size_t priority_cb_count = 0;
  audio_client->SetPriority(
      fidlbredr::A2dpDirectionPriority::NORMAL,
      [&](fidlbredr::AudioDirectionExt_SetPriority_Result result) {
        priority_cb_count++;
      });

  RunLoopUntilIdle();
  EXPECT_TRUE(audio_client_closed);
  EXPECT_EQ(priority_cb_count, 0u);
  audio_client.Unbind();
  RunLoopUntilIdle();
}

TEST_F(ProfileServerTestFakeAdapter,
       AudioOffloadExtRequestParametersClosedOnChannelClosed) {
  const bt::PeerId kPeerId;
  const fuchsia::bluetooth::PeerId kFidlPeerId{kPeerId.value()};

  FakeChannel::WeakPtr last_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { last_channel = std::move(chan); });

  // Support Android Vendor Extensions to enable Audio Offload Extension
  adapter()->mutable_state().controller_features |=
      FeaturesBits::kAndroidVendorExtensions;
  bt::StaticPacket<
      android_emb::LEGetVendorCapabilitiesCommandCompleteEventWriter>
      params;
  params.SetToZeros();
  params.view().status().Write(pw::bluetooth::emboss::StatusCode::SUCCESS);
  params.view().version_supported().major_number().Write(0);
  params.view().version_supported().minor_number().Write(98);
  params.view().a2dp_source_offload_capability_mask().aac().Write(true);
  adapter()->mutable_state().android_vendor_capabilities =
      bt::gap::AndroidVendorCapabilities::New(params.view(), 0);

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(kFidlPeerId,
                    std::move(conn_params),
                    [&](fidlbredr::Profile_Connect_Result result) {
                      ASSERT_TRUE(result.is_response());
                      response_channel = std::move(result.response().channel);
                    });
  RunLoopUntilIdle();
  ASSERT_TRUE(last_channel.is_alive());
  ASSERT_TRUE(response_channel.has_value());
  ASSERT_TRUE(response_channel->has_ext_audio_offload());

  fidlbredr::AudioOffloadExtPtr audio_client =
      response_channel->mutable_ext_audio_offload()->Bind();
  bool audio_client_closed = false;
  audio_client.set_error_handler(
      [&](zx_status_t /*status*/) { audio_client_closed = true; });

  // Closing the channel should close |audio_client| (after running the loop).
  last_channel->Close();
  // Destroy the channel (like the real LogicalLink would) to verify that
  // ProfileServer doesn't try to use channel pointers.
  EXPECT_TRUE(adapter()->fake_bredr()->DestroyChannel(last_channel->id()));

  // Any request for the closed channel should be ignored.
  std::optional<fidlbredr::AudioOffloadExt_GetSupportedFeatures_Response>
      result_features;
  audio_client->GetSupportedFeatures(
      [&result_features](
          fidlbredr::AudioOffloadExt_GetSupportedFeatures_Result features) {
        result_features = std::move(features.response());
      });

  RunLoopUntilIdle();
  EXPECT_TRUE(audio_client_closed);
  EXPECT_FALSE(result_features.has_value());
  audio_client.Unbind();
  RunLoopUntilIdle();
}

class ProfileServerInvalidSamplingFrequencyTest
    : public ProfileServerTestFakeAdapter,
      public ::testing::WithParamInterface<fidlbredr::AudioSamplingFrequency> {
};

const std::vector<fidlbredr::AudioSamplingFrequency>
    kInvalidSamplingFrequencies = {
        fidlbredr::AudioSamplingFrequency::HZ_88200,
        fidlbredr::AudioSamplingFrequency::HZ_96000,
};

INSTANTIATE_TEST_SUITE_P(ProfileServerTestFakeAdapter,
                         ProfileServerInvalidSamplingFrequencyTest,
                         ::testing::ValuesIn(kInvalidSamplingFrequencies));

TEST_P(ProfileServerInvalidSamplingFrequencyTest, SbcInvalidSamplingFrequency) {
  // enable a2dp offloading
  adapter()->mutable_state().controller_features |=
      FeaturesBits::kAndroidVendorExtensions;

  // enable offloaded sbc encoding
  bt::StaticPacket<
      android_emb::LEGetVendorCapabilitiesCommandCompleteEventWriter>
      params;
  params.view().status().Write(pw::bluetooth::emboss::StatusCode::SUCCESS);
  params.view().version_supported().major_number().Write(0);
  params.view().version_supported().minor_number().Write(98);
  params.view().a2dp_source_offload_capability_mask().sbc().Write(true);
  adapter()->mutable_state().android_vendor_capabilities =
      bt::gap::AndroidVendorCapabilities::New(params.view(), 0);

  // set up a fake channel and connection
  FakeChannel::WeakPtr fake_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](auto chan) { fake_channel = std::move(chan); });

  const bt::PeerId peer_id(1);
  const fuchsia::bluetooth::PeerId fidl_peer_id{peer_id.value()};

  fidlbredr::L2capParameters l2cap_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);

  fbt::ChannelParameters chan_params;
  l2cap_params.set_parameters(std::move(chan_params));

  fidlbredr::ConnectParameters conn_params;
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(
      fidl_peer_id,
      std::move(conn_params),
      [&response_channel](fidlbredr::Profile_Connect_Result result) {
        ASSERT_TRUE(result.is_response());
        response_channel = std::move(result.response().channel);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(response_channel.has_value());
  ASSERT_TRUE(response_channel->has_ext_audio_offload());

  // set up the bad configuration
  std::unique_ptr<fidlbredr::AudioOffloadFeatures> codec =
      fidlbredr::AudioOffloadFeatures::New();
  std::unique_ptr<fidlbredr::AudioSbcSupport> codec_value =
      fidlbredr::AudioSbcSupport::New();
  codec->set_sbc(std::move(*codec_value));

  std::unique_ptr<fidlbredr::AudioEncoderSettings> encoder_settings =
      std::make_unique<fidlbredr::AudioEncoderSettings>();
  std::unique_ptr<fuchsia::media::SbcEncoderSettings> encoder_settings_value =
      fuchsia::media::SbcEncoderSettings::New();
  encoder_settings->set_sbc(*encoder_settings_value);

  std::unique_ptr<fidlbredr::AudioOffloadConfiguration> config =
      std::make_unique<fidlbredr::AudioOffloadConfiguration>();
  config->set_codec(std::move(*codec));
  config->set_max_latency(10);
  config->set_scms_t_enable(true);
  config->set_sampling_frequency(GetParam());
  config->set_bits_per_sample(fidlbredr::AudioBitsPerSample::BPS_16);
  config->set_channel_mode(fidlbredr::AudioChannelMode::MONO);
  config->set_encoded_bit_rate(10);
  config->set_encoder_settings(std::move(*encoder_settings));

  // attempt to start the audio offload
  fidl::InterfaceHandle<fidlbredr::AudioOffloadController> controller_handle;
  fidl::InterfaceRequest<fidlbredr::AudioOffloadController> controller_request =
      controller_handle.NewRequest();
  fidl::InterfacePtr<fidlbredr::AudioOffloadExt> audio_offload_ext_client =
      response_channel->mutable_ext_audio_offload()->Bind();
  audio_offload_ext_client->StartAudioOffload(std::move(*config),
                                              std::move(controller_request));

  fidl::InterfacePtr<fidlbredr::AudioOffloadController>
      audio_offload_controller_client;
  audio_offload_controller_client.Bind(std::move(controller_handle));

  std::optional<zx_status_t> audio_offload_controller_epitaph;
  audio_offload_controller_client.set_error_handler(
      [&](zx_status_t status) { audio_offload_controller_epitaph = status; });

  RunLoopUntilIdle();

  // Verify that |audio_offload_controller_client| was closed with
  // |ZX_ERR_INTERNAL| epitaph
  ASSERT_TRUE(audio_offload_controller_epitaph.has_value());
  EXPECT_EQ(audio_offload_controller_epitaph.value(), ZX_ERR_NOT_SUPPORTED);
}

class AndroidSupportedFeaturesTest
    : public ProfileServerTestFakeAdapter,
      public ::testing::WithParamInterface<
          std::pair<bool /*android_vendor_support*/,
                    uint32_t /*android_vendor_capabilities*/>> {};

void AddAndroidVendorExtSupport(bt::gap::testing::FakeAdapter* adapter,
                                const uint32_t a2dp_offload_capabilities) {
  adapter->mutable_state().controller_features |=
      FeaturesBits::kAndroidVendorExtensions;

  bt::StaticPacket<
      android_emb::LEGetVendorCapabilitiesCommandCompleteEventWriter>
      params;
  params.SetToZeros();
  params.view().status().Write(pw::bluetooth::emboss::StatusCode::SUCCESS);
  params.view().version_supported().major_number().Write(0);
  params.view().version_supported().minor_number().Write(98);
  params.view()
      .a2dp_source_offload_capability_mask()
      .BackingStorage()
      .UncheckedWriteUInt(a2dp_offload_capabilities);
  adapter->mutable_state().android_vendor_capabilities =
      bt::gap::AndroidVendorCapabilities::New(params.view(), 0);
}

TEST_P(AndroidSupportedFeaturesTest, AudioOffloadExtGetSupportedFeatures) {
  const bool android_vendor_ext_support = GetParam().first;
  const uint32_t a2dp_offload_capabilities = GetParam().second;

  if (android_vendor_ext_support) {
    AddAndroidVendorExtSupport(adapter(), a2dp_offload_capabilities);
  }

  const bt::PeerId peer_id(1);
  const fuchsia::bluetooth::PeerId fidl_peer_id{peer_id.value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(
      fidl_peer_id,
      std::move(conn_params),
      [&response_channel](fidlbredr::Profile_Connect_Result result) {
        ASSERT_TRUE(result.is_response());
        response_channel = std::move(result.response().channel);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(response_channel.has_value());
  if (!android_vendor_ext_support || !a2dp_offload_capabilities) {
    EXPECT_FALSE(response_channel->has_ext_audio_offload());
    return;
  }
  ASSERT_TRUE(response_channel->has_ext_audio_offload());

  std::optional<fidlbredr::AudioOffloadExt_GetSupportedFeatures_Response>
      result_features;
  fidlbredr::AudioOffloadExtPtr audio_offload_ext_client =
      response_channel->mutable_ext_audio_offload()->Bind();
  audio_offload_ext_client->GetSupportedFeatures(
      [&result_features](
          fidlbredr::AudioOffloadExt_GetSupportedFeatures_Result features) {
        result_features = std::move(features.response());
      });
  RunLoopUntilIdle();

  EXPECT_TRUE(result_features->has_audio_offload_features());
  const std::vector<fidlbredr::AudioOffloadFeatures>& audio_offload_features =
      result_features->audio_offload_features();
  const uint32_t audio_offload_features_size =
      std::bitset<std::numeric_limits<uint32_t>::digits>(
          a2dp_offload_capabilities)
          .count();
  EXPECT_EQ(audio_offload_features_size, audio_offload_features.size());

  uint32_t capabilities = 0;
  const uint32_t sbc_capability =
      static_cast<uint32_t>(android_emb::A2dpCodecType::SBC);
  const uint32_t aac_capability =
      static_cast<uint32_t>(android_emb::A2dpCodecType::AAC);
  for (const fidlbredr::AudioOffloadFeatures& feature :
       audio_offload_features) {
    if (feature.is_sbc()) {
      capabilities |= sbc_capability;
    }
    if (feature.is_aac()) {
      capabilities |= aac_capability;
    }
  }
  EXPECT_EQ(capabilities, a2dp_offload_capabilities);
}

TEST_P(AndroidSupportedFeaturesTest, AudioOffloadExtStartAudioOffloadSuccess) {
  const bool android_vendor_ext_support = GetParam().first;
  const uint32_t a2dp_offload_capabilities = GetParam().second;

  if (android_vendor_ext_support) {
    AddAndroidVendorExtSupport(adapter(), a2dp_offload_capabilities);
  }

  const bt::PeerId peer_id(1);
  const fuchsia::bluetooth::PeerId fidl_peer_id{peer_id.value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(
      fidl_peer_id,
      std::move(conn_params),
      [&response_channel](fidlbredr::Profile_Connect_Result result) {
        ASSERT_TRUE(result.is_response());
        response_channel = std::move(result.response().channel);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(response_channel.has_value());
  if (!android_vendor_ext_support || !a2dp_offload_capabilities) {
    EXPECT_FALSE(response_channel->has_ext_audio_offload());
    return;
  }
  ASSERT_TRUE(response_channel->has_ext_audio_offload());

  // Set Audio Offload Configuration Values
  std::unique_ptr<fidlbredr::AudioOffloadFeatures> codec =
      fidlbredr::AudioOffloadFeatures::New();
  std::unique_ptr<fidlbredr::AudioSbcSupport> codec_value =
      fidlbredr::AudioSbcSupport::New();
  codec->set_sbc(std::move(*codec_value));

  std::unique_ptr<fidlbredr::AudioEncoderSettings> encoder_settings =
      std::make_unique<fidlbredr::AudioEncoderSettings>();
  std::unique_ptr<fuchsia::media::SbcEncoderSettings> encoder_settings_value =
      fuchsia::media::SbcEncoderSettings::New();
  encoder_settings->set_sbc(*encoder_settings_value);

  std::unique_ptr<fidlbredr::AudioOffloadConfiguration> config =
      std::make_unique<fidlbredr::AudioOffloadConfiguration>();
  config->set_codec(std::move(*codec));
  config->set_max_latency(10);
  config->set_scms_t_enable(true);
  config->set_sampling_frequency(fidlbredr::AudioSamplingFrequency::HZ_44100);
  config->set_bits_per_sample(fidlbredr::AudioBitsPerSample::BPS_16);
  config->set_channel_mode(fidlbredr::AudioChannelMode::MONO);
  config->set_encoded_bit_rate(10);
  config->set_encoder_settings(std::move(*encoder_settings));

  fidlbredr::AudioOffloadExtPtr audio_offload_ext_client =
      response_channel->mutable_ext_audio_offload()->Bind();
  fidlbredr::AudioOffloadControllerHandle controller_handle;
  fidl::InterfaceRequest<fidlbredr::AudioOffloadController> controller_request =
      controller_handle.NewRequest();
  audio_offload_ext_client->StartAudioOffload(std::move(*config),
                                              std::move(controller_request));

  fidlbredr::AudioOffloadControllerPtr audio_offload_controller_client;
  audio_offload_controller_client.Bind(std::move(controller_handle));

  std::optional<zx_status_t> audio_offload_controller_epitaph;
  audio_offload_controller_client.set_error_handler(
      [&](zx_status_t status) { audio_offload_controller_epitaph = status; });

  size_t on_started_count = 0;
  audio_offload_controller_client.events().OnStarted = [&]() {
    on_started_count++;
  };

  RunLoopUntilIdle();

  // Verify that OnStarted event was sent successfully
  EXPECT_EQ(on_started_count, 1u);

  // Verify that |audio_offload_controller_client| was not closed with an
  // epitaph
  ASSERT_FALSE(audio_offload_controller_epitaph.has_value());
}

TEST_P(AndroidSupportedFeaturesTest, AudioOffloadExtStartAudioOffloadFail) {
  FakeChannel::WeakPtr fake_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { fake_channel = std::move(chan); });

  const bool android_vendor_ext_support = GetParam().first;
  const uint32_t a2dp_offload_capabilities = GetParam().second;

  if (android_vendor_ext_support) {
    AddAndroidVendorExtSupport(adapter(), a2dp_offload_capabilities);
  }

  const bt::PeerId peer_id(1);
  const fuchsia::bluetooth::PeerId fidl_peer_id{peer_id.value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(
      fidl_peer_id,
      std::move(conn_params),
      [&response_channel](fidlbredr::Profile_Connect_Result result) {
        ASSERT_TRUE(result.is_response());
        response_channel = std::move(result.response().channel);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(response_channel.has_value());
  if (!android_vendor_ext_support || !a2dp_offload_capabilities) {
    EXPECT_FALSE(response_channel->has_ext_audio_offload());
    return;
  }
  ASSERT_TRUE(response_channel->has_ext_audio_offload());

  // Make A2DP offloading fail, resulting in |ZX_ERR_INTERNAL| epitaph
  ASSERT_TRUE(fake_channel.is_alive());
  fake_channel->set_a2dp_offload_fails(bt::HostError::kFailed);

  // Set Audio Offload Configuration Values
  std::unique_ptr<fidlbredr::AudioOffloadFeatures> codec =
      fidlbredr::AudioOffloadFeatures::New();
  std::unique_ptr<fidlbredr::AudioSbcSupport> codec_value =
      fidlbredr::AudioSbcSupport::New();
  codec->set_sbc(std::move(*codec_value));

  std::unique_ptr<fidlbredr::AudioEncoderSettings> encoder_settings =
      std::make_unique<fidlbredr::AudioEncoderSettings>();
  std::unique_ptr<fuchsia::media::SbcEncoderSettings> encoder_settings_value =
      fuchsia::media::SbcEncoderSettings::New();
  encoder_settings->set_sbc(*encoder_settings_value);

  std::unique_ptr<fidlbredr::AudioOffloadConfiguration> config =
      std::make_unique<fidlbredr::AudioOffloadConfiguration>();
  config->set_codec(std::move(*codec));
  config->set_max_latency(10);
  config->set_scms_t_enable(true);
  config->set_sampling_frequency(fidlbredr::AudioSamplingFrequency::HZ_44100);
  config->set_bits_per_sample(fidlbredr::AudioBitsPerSample::BPS_16);
  config->set_channel_mode(fidlbredr::AudioChannelMode::MONO);
  config->set_encoded_bit_rate(10);
  config->set_encoder_settings(std::move(*encoder_settings));

  fidlbredr::AudioOffloadExtPtr audio_offload_ext_client =
      response_channel->mutable_ext_audio_offload()->Bind();
  fidlbredr::AudioOffloadControllerHandle controller_handle;
  fidl::InterfaceRequest<fidlbredr::AudioOffloadController> controller_request =
      controller_handle.NewRequest();
  audio_offload_ext_client->StartAudioOffload(std::move(*config),
                                              std::move(controller_request));

  fidlbredr::AudioOffloadControllerPtr audio_offload_controller_client;
  audio_offload_controller_client.Bind(std::move(controller_handle));

  std::optional<zx_status_t> audio_offload_controller_epitaph;
  audio_offload_controller_client.set_error_handler(
      [&](zx_status_t status) { audio_offload_controller_epitaph = status; });

  size_t cb_count = 0;
  audio_offload_controller_client.events().OnStarted = [&]() { cb_count++; };

  RunLoopUntilIdle();
  EXPECT_EQ(cb_count, 0u);

  // Verify that |audio_offload_controller_client| was closed with
  // |ZX_ERR_INTERNAL| epitaph
  ASSERT_TRUE(audio_offload_controller_epitaph.has_value());
  EXPECT_EQ(audio_offload_controller_epitaph.value(), ZX_ERR_INTERNAL);

  bt::l2cap::A2dpOffloadStatus a2dp_offload_status =
      fake_channel->a2dp_offload_status();
  EXPECT_EQ(bt::l2cap::A2dpOffloadStatus::kStopped, a2dp_offload_status);
}

TEST_P(AndroidSupportedFeaturesTest,
       AudioOffloadExtStartAudioOffloadInProgress) {
  FakeChannel::WeakPtr fake_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { fake_channel = std::move(chan); });

  const bool android_vendor_ext_support = GetParam().first;
  const uint32_t a2dp_offload_capabilities = GetParam().second;

  if (android_vendor_ext_support) {
    AddAndroidVendorExtSupport(adapter(), a2dp_offload_capabilities);
  }

  const bt::PeerId peer_id(1);
  const fuchsia::bluetooth::PeerId fidl_peer_id{peer_id.value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(
      fidl_peer_id,
      std::move(conn_params),
      [&response_channel](fidlbredr::Profile_Connect_Result result) {
        ASSERT_TRUE(result.is_response());
        response_channel = std::move(result.response().channel);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(response_channel.has_value());
  if (!android_vendor_ext_support || !a2dp_offload_capabilities) {
    EXPECT_FALSE(response_channel->has_ext_audio_offload());
    return;
  }
  ASSERT_TRUE(response_channel->has_ext_audio_offload());

  // Make A2DP offloading fail, resulting in |ZX_ERR_ALREADY_BOUND| epitaph
  ASSERT_TRUE(fake_channel.is_alive());
  fake_channel->set_a2dp_offload_fails(bt::HostError::kInProgress);

  // Set Audio Offload Configuration Values
  std::unique_ptr<fidlbredr::AudioOffloadFeatures> codec =
      fidlbredr::AudioOffloadFeatures::New();
  std::unique_ptr<fidlbredr::AudioSbcSupport> codec_value =
      fidlbredr::AudioSbcSupport::New();
  codec->set_sbc(std::move(*codec_value));

  std::unique_ptr<fidlbredr::AudioEncoderSettings> encoder_settings =
      std::make_unique<fidlbredr::AudioEncoderSettings>();
  std::unique_ptr<fuchsia::media::SbcEncoderSettings> encoder_settings_value =
      fuchsia::media::SbcEncoderSettings::New();
  encoder_settings->set_sbc(*encoder_settings_value);

  std::unique_ptr<fidlbredr::AudioOffloadConfiguration> config =
      std::make_unique<fidlbredr::AudioOffloadConfiguration>();
  config->set_codec(std::move(*codec));
  config->set_max_latency(10);
  config->set_scms_t_enable(true);
  config->set_sampling_frequency(fidlbredr::AudioSamplingFrequency::HZ_44100);
  config->set_bits_per_sample(fidlbredr::AudioBitsPerSample::BPS_16);
  config->set_channel_mode(fidlbredr::AudioChannelMode::MONO);
  config->set_encoded_bit_rate(10);
  config->set_encoder_settings(std::move(*encoder_settings));

  fidlbredr::AudioOffloadExtPtr audio_offload_ext_client =
      response_channel->mutable_ext_audio_offload()->Bind();
  fidlbredr::AudioOffloadControllerHandle controller_handle;
  fidl::InterfaceRequest<fidlbredr::AudioOffloadController> controller_request =
      controller_handle.NewRequest();
  audio_offload_ext_client->StartAudioOffload(std::move(*config),
                                              std::move(controller_request));

  fidlbredr::AudioOffloadControllerPtr audio_offload_controller_client;
  audio_offload_controller_client.Bind(std::move(controller_handle));

  std::optional<zx_status_t> audio_offload_controller_epitaph;
  audio_offload_controller_client.set_error_handler(
      [&](zx_status_t status) { audio_offload_controller_epitaph = status; });

  size_t cb_count = 0;
  audio_offload_controller_client.events().OnStarted = [&]() { cb_count++; };

  RunLoopUntilIdle();
  EXPECT_EQ(cb_count, 0u);

  // Verify that |audio_offload_controller_client| was closed with
  // |ZX_ERR_ALREADY_BOUND| epitaph
  ASSERT_TRUE(audio_offload_controller_epitaph.has_value());
  EXPECT_EQ(audio_offload_controller_epitaph.value(), ZX_ERR_ALREADY_BOUND);
}

TEST_P(AndroidSupportedFeaturesTest,
       AudioOffloadExtStartAudioOffloadControllerError) {
  FakeChannel::WeakPtr fake_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { fake_channel = std::move(chan); });

  const bool android_vendor_ext_support = GetParam().first;
  const uint32_t a2dp_offload_capabilities = GetParam().second;

  if (android_vendor_ext_support) {
    AddAndroidVendorExtSupport(adapter(), a2dp_offload_capabilities);
  }

  const bt::PeerId peer_id(1);
  const fuchsia::bluetooth::PeerId fidl_peer_id{peer_id.value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(
      fidl_peer_id,
      std::move(conn_params),
      [&response_channel](fidlbredr::Profile_Connect_Result result) {
        ASSERT_TRUE(result.is_response());
        response_channel = std::move(result.response().channel);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(response_channel.has_value());
  if (!android_vendor_ext_support || !a2dp_offload_capabilities) {
    EXPECT_FALSE(response_channel->has_ext_audio_offload());
    return;
  }
  ASSERT_TRUE(response_channel->has_ext_audio_offload());

  // Set Audio Offload Configuration Values
  std::unique_ptr<fidlbredr::AudioOffloadFeatures> codec =
      fidlbredr::AudioOffloadFeatures::New();
  std::unique_ptr<fidlbredr::AudioSbcSupport> codec_value =
      fidlbredr::AudioSbcSupport::New();
  codec->set_sbc(std::move(*codec_value));

  std::unique_ptr<fidlbredr::AudioEncoderSettings> encoder_settings =
      std::make_unique<fidlbredr::AudioEncoderSettings>();
  std::unique_ptr<fuchsia::media::SbcEncoderSettings> encoder_settings_value =
      fuchsia::media::SbcEncoderSettings::New();
  encoder_settings->set_sbc(*encoder_settings_value);

  std::unique_ptr<fidlbredr::AudioOffloadConfiguration> config =
      std::make_unique<fidlbredr::AudioOffloadConfiguration>();
  config->set_codec(std::move(*codec));
  config->set_max_latency(10);
  config->set_scms_t_enable(true);
  config->set_sampling_frequency(fidlbredr::AudioSamplingFrequency::HZ_44100);
  config->set_bits_per_sample(fidlbredr::AudioBitsPerSample::BPS_16);
  config->set_channel_mode(fidlbredr::AudioChannelMode::MONO);
  config->set_encoded_bit_rate(10);
  config->set_encoder_settings(std::move(*encoder_settings));

  fidlbredr::AudioOffloadExtPtr audio_offload_ext_client =
      response_channel->mutable_ext_audio_offload()->Bind();
  fidlbredr::AudioOffloadControllerHandle controller_handle;
  fidl::InterfaceRequest<fidlbredr::AudioOffloadController> controller_request =
      controller_handle.NewRequest();
  audio_offload_ext_client->StartAudioOffload(std::move(*config),
                                              std::move(controller_request));

  fidlbredr::AudioOffloadControllerPtr audio_offload_controller_client;
  audio_offload_controller_client.Bind(std::move(controller_handle));

  std::optional<zx_status_t> audio_offload_controller_epitaph;
  audio_offload_controller_client.set_error_handler(
      [&](zx_status_t status) { audio_offload_controller_epitaph = status; });

  size_t cb_count = 0;
  audio_offload_controller_client.events().OnStarted = [&]() { cb_count++; };

  // Close client end of protocol to trigger audio offload error handler
  audio_offload_controller_client.Unbind();

  RunLoopUntilIdle();
  EXPECT_EQ(cb_count, 0u);
  ASSERT_FALSE(audio_offload_controller_epitaph.has_value());
}

TEST_P(AndroidSupportedFeaturesTest, AudioOffloadControllerStopSuccess) {
  const bool android_vendor_ext_support = GetParam().first;
  const uint32_t a2dp_offload_capabilities = GetParam().second;

  if (android_vendor_ext_support) {
    AddAndroidVendorExtSupport(adapter(), a2dp_offload_capabilities);
  }

  const bt::PeerId peer_id(1);
  const fuchsia::bluetooth::PeerId fidl_peer_id{peer_id.value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(
      fidl_peer_id,
      std::move(conn_params),
      [&response_channel](fidlbredr::Profile_Connect_Result result) {
        ASSERT_TRUE(result.is_response());
        response_channel = std::move(result.response().channel);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(response_channel.has_value());
  if (!android_vendor_ext_support || !a2dp_offload_capabilities) {
    EXPECT_FALSE(response_channel->has_ext_audio_offload());
    return;
  }
  ASSERT_TRUE(response_channel->has_ext_audio_offload());

  // Set Audio Offload Configuration Values
  std::unique_ptr<fidlbredr::AudioOffloadFeatures> codec =
      fidlbredr::AudioOffloadFeatures::New();
  std::unique_ptr<fidlbredr::AudioSbcSupport> codec_value =
      fidlbredr::AudioSbcSupport::New();
  codec->set_sbc(std::move(*codec_value));

  std::unique_ptr<fidlbredr::AudioEncoderSettings> encoder_settings =
      std::make_unique<fidlbredr::AudioEncoderSettings>();
  std::unique_ptr<fuchsia::media::SbcEncoderSettings> encoder_settings_value =
      fuchsia::media::SbcEncoderSettings::New();
  encoder_settings->set_sbc(*encoder_settings_value);

  std::unique_ptr<fidlbredr::AudioOffloadConfiguration> config =
      std::make_unique<fidlbredr::AudioOffloadConfiguration>();
  config->set_codec(std::move(*codec));
  config->set_max_latency(10);
  config->set_scms_t_enable(true);
  config->set_sampling_frequency(fidlbredr::AudioSamplingFrequency::HZ_44100);
  config->set_bits_per_sample(fidlbredr::AudioBitsPerSample::BPS_16);
  config->set_channel_mode(fidlbredr::AudioChannelMode::MONO);
  config->set_encoded_bit_rate(10);
  config->set_encoder_settings(std::move(*encoder_settings));

  fidlbredr::AudioOffloadExtPtr audio_offload_ext_client =
      response_channel->mutable_ext_audio_offload()->Bind();
  fidlbredr::AudioOffloadControllerHandle controller_handle;
  fidl::InterfaceRequest<fidlbredr::AudioOffloadController> controller_request =
      controller_handle.NewRequest();
  audio_offload_ext_client->StartAudioOffload(std::move(*config),
                                              std::move(controller_request));

  fidlbredr::AudioOffloadControllerPtr audio_offload_controller_client;
  audio_offload_controller_client.Bind(std::move(controller_handle));

  std::optional<zx_status_t> audio_offload_controller_epitaph;
  audio_offload_controller_client.set_error_handler(
      [&](zx_status_t status) { audio_offload_controller_epitaph = status; });

  size_t on_started_count = 0;
  audio_offload_controller_client.events().OnStarted = [&]() {
    on_started_count++;
  };

  RunLoopUntilIdle();

  // Verify that OnStarted event was sent successfully
  EXPECT_EQ(on_started_count, 1u);

  // Verify that |audio_offload_controller_client| was not closed with an
  // epitaph
  ASSERT_FALSE(audio_offload_controller_epitaph.has_value());

  bool stop_callback_called = false;
  auto callback = [&stop_callback_called](
                      fidlbredr::AudioOffloadController_Stop_Result result) {
    stop_callback_called = true;
  };

  audio_offload_controller_client->Stop(std::move(callback));

  RunLoopUntilIdle();

  // Verify that audio offload was stopped successfully
  ASSERT_TRUE(stop_callback_called);
}

TEST_P(AndroidSupportedFeaturesTest, AudioOffloadControllerStopFail) {
  FakeChannel::WeakPtr fake_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { fake_channel = std::move(chan); });

  const bool android_vendor_ext_support = GetParam().first;
  const uint32_t a2dp_offload_capabilities = GetParam().second;

  if (android_vendor_ext_support) {
    AddAndroidVendorExtSupport(adapter(), a2dp_offload_capabilities);
  }

  const bt::PeerId peer_id(1);
  const fuchsia::bluetooth::PeerId fidl_peer_id{peer_id.value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(
      fidl_peer_id,
      std::move(conn_params),
      [&response_channel](fidlbredr::Profile_Connect_Result result) {
        ASSERT_TRUE(result.is_response());
        response_channel = std::move(result.response().channel);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(response_channel.has_value());
  if (!android_vendor_ext_support || !a2dp_offload_capabilities) {
    EXPECT_FALSE(response_channel->has_ext_audio_offload());
    return;
  }
  ASSERT_TRUE(response_channel->has_ext_audio_offload());

  // Set Audio Offload Configuration Values
  std::unique_ptr<fidlbredr::AudioOffloadFeatures> codec =
      fidlbredr::AudioOffloadFeatures::New();
  std::unique_ptr<fidlbredr::AudioSbcSupport> codec_value =
      fidlbredr::AudioSbcSupport::New();
  codec->set_sbc(std::move(*codec_value));

  std::unique_ptr<fidlbredr::AudioEncoderSettings> encoder_settings =
      std::make_unique<fidlbredr::AudioEncoderSettings>();
  std::unique_ptr<fuchsia::media::SbcEncoderSettings> encoder_settings_value =
      fuchsia::media::SbcEncoderSettings::New();
  encoder_settings->set_sbc(*encoder_settings_value);

  std::unique_ptr<fidlbredr::AudioOffloadConfiguration> config =
      std::make_unique<fidlbredr::AudioOffloadConfiguration>();
  config->set_codec(std::move(*codec));
  config->set_max_latency(10);
  config->set_scms_t_enable(true);
  config->set_sampling_frequency(fidlbredr::AudioSamplingFrequency::HZ_44100);
  config->set_bits_per_sample(fidlbredr::AudioBitsPerSample::BPS_16);
  config->set_channel_mode(fidlbredr::AudioChannelMode::MONO);
  config->set_encoded_bit_rate(10);
  config->set_encoder_settings(std::move(*encoder_settings));

  fidlbredr::AudioOffloadExtPtr audio_offload_ext_client =
      response_channel->mutable_ext_audio_offload()->Bind();
  fidlbredr::AudioOffloadControllerHandle controller_handle;
  fidl::InterfaceRequest<fidlbredr::AudioOffloadController> controller_request =
      controller_handle.NewRequest();
  audio_offload_ext_client->StartAudioOffload(std::move(*config),
                                              std::move(controller_request));

  fidlbredr::AudioOffloadControllerPtr audio_offload_controller_client;
  audio_offload_controller_client.Bind(std::move(controller_handle));

  std::optional<zx_status_t> audio_offload_controller_epitaph;
  audio_offload_controller_client.set_error_handler(
      [&](zx_status_t status) { audio_offload_controller_epitaph = status; });

  size_t on_started_count = 0;
  audio_offload_controller_client.events().OnStarted = [&]() {
    on_started_count++;
  };

  RunLoopUntilIdle();

  // Verify that OnStarted event was sent successfully
  EXPECT_EQ(on_started_count, 1u);

  // Verify that |audio_offload_controller_client| was not closed with an
  // epitaph
  ASSERT_FALSE(audio_offload_controller_epitaph.has_value());

  // Make A2DP offloading fail, resulting in |ZX_ERR_UNAVAILABLE| epitaph
  ASSERT_TRUE(fake_channel.is_alive());
  fake_channel->set_a2dp_offload_fails(bt::HostError::kInProgress);

  size_t cb_count = 0;
  audio_offload_controller_client->Stop(
      [&cb_count](fidlbredr::AudioOffloadController_Stop_Result result) {
        cb_count++;
      });

  RunLoopUntilIdle();

  EXPECT_EQ(cb_count, 0u);

  // Verify that |audio_offload_controller_client| was closed with
  // |ZX_ERR_UNAVAILABLE| epitaph
  ASSERT_TRUE(audio_offload_controller_epitaph.has_value());
  EXPECT_EQ(audio_offload_controller_epitaph.value(), ZX_ERR_UNAVAILABLE);
}

TEST_P(AndroidSupportedFeaturesTest,
       AudioOffloadControllerStopAfterAlreadyStopped) {
  const bool android_vendor_ext_support = GetParam().first;
  const uint32_t a2dp_offload_capabilities = GetParam().second;

  if (android_vendor_ext_support) {
    AddAndroidVendorExtSupport(adapter(), a2dp_offload_capabilities);
  }

  const bt::PeerId peer_id(1);
  const fuchsia::bluetooth::PeerId fidl_peer_id{peer_id.value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(
      fidl_peer_id,
      std::move(conn_params),
      [&response_channel](fidlbredr::Profile_Connect_Result result) {
        ASSERT_TRUE(result.is_response());
        response_channel = std::move(result.response().channel);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(response_channel.has_value());
  if (!android_vendor_ext_support || !a2dp_offload_capabilities) {
    EXPECT_FALSE(response_channel->has_ext_audio_offload());
    return;
  }
  ASSERT_TRUE(response_channel->has_ext_audio_offload());

  // Set Audio Offload Configuration Values
  std::unique_ptr<fidlbredr::AudioOffloadFeatures> codec =
      fidlbredr::AudioOffloadFeatures::New();
  std::unique_ptr<fidlbredr::AudioSbcSupport> codec_value =
      fidlbredr::AudioSbcSupport::New();
  codec->set_sbc(std::move(*codec_value));

  std::unique_ptr<fidlbredr::AudioEncoderSettings> encoder_settings =
      std::make_unique<fidlbredr::AudioEncoderSettings>();
  std::unique_ptr<fuchsia::media::SbcEncoderSettings> encoder_settings_value =
      fuchsia::media::SbcEncoderSettings::New();
  encoder_settings->set_sbc(*encoder_settings_value);

  std::unique_ptr<fidlbredr::AudioOffloadConfiguration> config =
      std::make_unique<fidlbredr::AudioOffloadConfiguration>();
  config->set_codec(std::move(*codec));
  config->set_max_latency(10);
  config->set_scms_t_enable(true);
  config->set_sampling_frequency(fidlbredr::AudioSamplingFrequency::HZ_44100);
  config->set_bits_per_sample(fidlbredr::AudioBitsPerSample::BPS_16);
  config->set_channel_mode(fidlbredr::AudioChannelMode::MONO);
  config->set_encoded_bit_rate(10);
  config->set_encoder_settings(std::move(*encoder_settings));

  fidlbredr::AudioOffloadExtPtr audio_offload_ext_client =
      response_channel->mutable_ext_audio_offload()->Bind();
  fidlbredr::AudioOffloadControllerHandle controller_handle;
  fidl::InterfaceRequest<fidlbredr::AudioOffloadController> controller_request =
      controller_handle.NewRequest();
  audio_offload_ext_client->StartAudioOffload(std::move(*config),
                                              std::move(controller_request));

  fidlbredr::AudioOffloadControllerPtr audio_offload_controller_client;
  audio_offload_controller_client.Bind(std::move(controller_handle));

  std::optional<zx_status_t> audio_offload_controller_epitaph;
  audio_offload_controller_client.set_error_handler(
      [&](zx_status_t status) { audio_offload_controller_epitaph = status; });

  size_t on_started_count = 0;
  audio_offload_controller_client.events().OnStarted = [&]() {
    on_started_count++;
  };

  RunLoopUntilIdle();

  // Verify that OnStarted event was sent successfully
  EXPECT_EQ(on_started_count, 1u);

  // Verify that |audio_offload_controller_client| was not closed with an
  // epitaph
  ASSERT_FALSE(audio_offload_controller_epitaph.has_value());

  bool stop_callback_called = false;
  auto callback = [&stop_callback_called](
                      fidlbredr::AudioOffloadController_Stop_Result result) {
    stop_callback_called = true;
  };

  audio_offload_controller_client->Stop(std::move(callback));

  RunLoopUntilIdle();

  // Verify that audio offload stopped successfully
  ASSERT_TRUE(stop_callback_called);

  size_t cb_count = 0;
  audio_offload_controller_client->Stop(
      [&cb_count](fidlbredr::AudioOffloadController_Stop_Result result) {
        cb_count++;
      });

  RunLoopUntilIdle();

  // Verify that stopping audio offload has no effect when it's already stopped
  EXPECT_EQ(cb_count, 1u);
}

TEST_P(AndroidSupportedFeaturesTest,
       AudioOffloadControllerUnbindStopsAudioOffload) {
  FakeChannel::WeakPtr fake_channel;
  adapter()->fake_bredr()->set_l2cap_channel_callback(
      [&](FakeChannel::WeakPtr chan) { fake_channel = std::move(chan); });

  const bool android_vendor_ext_support = GetParam().first;
  const uint32_t a2dp_offload_capabilities = GetParam().second;

  if (android_vendor_ext_support) {
    AddAndroidVendorExtSupport(adapter(), a2dp_offload_capabilities);
  }

  const bt::PeerId peer_id(1);
  const fuchsia::bluetooth::PeerId fidl_peer_id{peer_id.value()};

  // Set L2CAP channel parameters
  fidlbredr::L2capParameters l2cap_params;
  fidlbredr::ConnectParameters conn_params;
  l2cap_params.set_psm(fidlbredr::PSM_AVDTP);
  l2cap_params.set_parameters(fbt::ChannelParameters());
  conn_params.set_l2cap(std::move(l2cap_params));

  std::optional<fidlbredr::Channel> response_channel;
  client()->Connect(
      fidl_peer_id,
      std::move(conn_params),
      [&response_channel](fidlbredr::Profile_Connect_Result result) {
        ASSERT_TRUE(result.is_response());
        response_channel = std::move(result.response().channel);
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(response_channel.has_value());
  if (!android_vendor_ext_support || !a2dp_offload_capabilities) {
    EXPECT_FALSE(response_channel->has_ext_audio_offload());
    return;
  }
  ASSERT_TRUE(response_channel->has_ext_audio_offload());

  // Set Audio Offload Configuration Values
  std::unique_ptr<fidlbredr::AudioOffloadFeatures> codec =
      fidlbredr::AudioOffloadFeatures::New();
  std::unique_ptr<fidlbredr::AudioSbcSupport> codec_value =
      fidlbredr::AudioSbcSupport::New();
  codec->set_sbc(std::move(*codec_value));

  std::unique_ptr<fidlbredr::AudioEncoderSettings> encoder_settings =
      std::make_unique<fidlbredr::AudioEncoderSettings>();
  std::unique_ptr<fuchsia::media::SbcEncoderSettings> encoder_settings_value =
      fuchsia::media::SbcEncoderSettings::New();
  encoder_settings->set_sbc(*encoder_settings_value);

  std::unique_ptr<fidlbredr::AudioOffloadConfiguration> config =
      std::make_unique<fidlbredr::AudioOffloadConfiguration>();
  config->set_codec(std::move(*codec));
  config->set_max_latency(10);
  config->set_scms_t_enable(true);
  config->set_sampling_frequency(fidlbredr::AudioSamplingFrequency::HZ_44100);
  config->set_bits_per_sample(fidlbredr::AudioBitsPerSample::BPS_16);
  config->set_channel_mode(fidlbredr::AudioChannelMode::MONO);
  config->set_encoded_bit_rate(10);
  config->set_encoder_settings(std::move(*encoder_settings));

  fidlbredr::AudioOffloadExtPtr audio_offload_ext_client =
      response_channel->mutable_ext_audio_offload()->Bind();
  fidlbredr::AudioOffloadControllerHandle controller_handle;
  fidl::InterfaceRequest<fidlbredr::AudioOffloadController> controller_request =
      controller_handle.NewRequest();
  audio_offload_ext_client->StartAudioOffload(std::move(*config),
                                              std::move(controller_request));

  fidlbredr::AudioOffloadControllerPtr audio_offload_controller_client;
  audio_offload_controller_client.Bind(std::move(controller_handle));

  std::optional<zx_status_t> audio_offload_controller_epitaph;
  audio_offload_controller_client.set_error_handler(
      [&](zx_status_t status) { audio_offload_controller_epitaph = status; });

  size_t on_started_count = 0;
  audio_offload_controller_client.events().OnStarted = [&]() {
    on_started_count++;
  };

  RunLoopUntilIdle();

  // Verify that OnStarted event was sent successfully
  EXPECT_EQ(on_started_count, 1u);

  // Verify that |audio_offload_controller_client| was not closed with an
  // epitaph
  ASSERT_FALSE(audio_offload_controller_epitaph.has_value());

  bt::l2cap::A2dpOffloadStatus a2dp_offload_status =
      fake_channel->a2dp_offload_status();

  // Verify that |a2dp_offload_status| is set to started
  ASSERT_EQ(bt::l2cap::A2dpOffloadStatus::kStarted, a2dp_offload_status);

  audio_offload_controller_client.Unbind();

  RunLoopUntilIdle();

  // Verify that client is unbound from fidl channel
  ASSERT_FALSE(audio_offload_controller_client.is_bound());

  a2dp_offload_status = fake_channel->a2dp_offload_status();

  // Verify that |a2dp_offload_status| is set to stopped
  ASSERT_EQ(bt::l2cap::A2dpOffloadStatus::kStopped, a2dp_offload_status);
}

const std::vector<std::pair<bool, uint32_t>> kVendorCapabilitiesParams = {
    {{true, static_cast<uint32_t>(android_emb::A2dpCodecType::SBC)},
     {true, static_cast<uint32_t>(android_emb::A2dpCodecType::AAC)},
     {true,
      static_cast<uint32_t>(android_emb::A2dpCodecType::SBC) |
          static_cast<uint32_t>(android_emb::A2dpCodecType::AAC)},
     {true, 0},
     {false, 0}}};
INSTANTIATE_TEST_SUITE_P(ProfileServerTestFakeAdapter,
                         AndroidSupportedFeaturesTest,
                         ::testing::ValuesIn(kVendorCapabilitiesParams));

TEST_F(ProfileServerTestFakeAdapter,
       ServiceUuidSearchResultRelayedToFidlClient) {
  fidlbredr::SearchResultsHandle search_results_handle;
  FakeSearchResults search_results(search_results_handle.NewRequest(),
                                   dispatcher());
  int result_cb_count = 0;
  search_results.set_result_cb([this, &result_cb_count]() {
    EXPECT_NE(lease_provider().lease_count(), 0u);
    result_cb_count++;
  });

  fidlbredr::ServiceClassProfileIdentifier search_uuid =
      fidlbredr::ServiceClassProfileIdentifier::AUDIO_SINK;

  EXPECT_EQ(adapter()->fake_bredr()->registered_searches().size(), 0u);
  EXPECT_EQ(search_results.service_found_count(), 0u);

  // FIDL client registers a service search.
  fidlbredr::ProfileSearchRequest request;
  request.set_service_uuid(search_uuid);
  request.set_attr_ids({});
  request.set_results(std::move(search_results_handle));
  client()->Search(std::move(request));
  RunLoopUntilIdle();
  EXPECT_EQ(lease_provider().lease_count(), 0u);
  EXPECT_EQ(adapter()->fake_bredr()->registered_searches().size(), 1u);

  // Trigger a match on the service search with some data. Should be received by
  // the FIDL client.
  bt::PeerId peer_id = bt::PeerId{10};
  bt::UUID uuid(static_cast<uint32_t>(search_uuid));

  bt::sdp::AttributeId attr_id = 50;  // Random Attribute ID
  bt::sdp::DataElement elem = bt::sdp::DataElement();
  elem.SetUrl("https://foobar.dev");  // Random URL
  auto attributes = std::map<bt::sdp::AttributeId, bt::sdp::DataElement>();
  attributes.emplace(attr_id, std::move(elem));
  adapter()->fake_bredr()->TriggerServiceFound(
      peer_id, uuid, std::move(attributes));

  RunLoopUntilIdle();

  EXPECT_EQ(result_cb_count, 1);
  EXPECT_EQ(search_results.service_found_count(), 1u);
  EXPECT_EQ(search_results.peer_id().value().value, peer_id.value());
  EXPECT_EQ(search_results.attributes().value().size(), 1u);
  EXPECT_EQ(search_results.attributes().value()[0].id(), attr_id);
  EXPECT_EQ(search_results.attributes().value()[0].element().url(),
            std::string("https://foobar.dev"));
  EXPECT_EQ(lease_provider().lease_count(), 0u);
}

TEST_F(ProfileServerTestFakeAdapter, FullUuidSearchResultRelayedToFidlClient) {
  fidlbredr::SearchResultsHandle search_results_handle;
  FakeSearchResults search_results(search_results_handle.NewRequest(),
                                   dispatcher());

  EXPECT_EQ(adapter()->fake_bredr()->registered_searches().size(), 0u);
  EXPECT_EQ(search_results.service_found_count(), 0u);

  // FIDL client registers a service search.
  fuchsia::bluetooth::Uuid search_uuid =
      fidl_helpers::UuidToFidl(bt::sdp::profile::kHandsfree);
  fidlbredr::ProfileSearchRequest request;
  request.set_full_uuid(search_uuid);
  request.set_attr_ids({});
  request.set_results(std::move(search_results_handle));
  client()->Search(std::move(request));
  RunLoopUntilIdle();

  EXPECT_EQ(adapter()->fake_bredr()->registered_searches().size(), 1u);

  // Trigger a match on the service search with some data. Should be received by
  // the FIDL client.
  bt::PeerId peer_id = bt::PeerId{10};
  bt::UUID uuid = bt::sdp::profile::kHandsfree;

  bt::sdp::AttributeId attr_id = 50;  // Random Attribute ID
  bt::sdp::DataElement elem = bt::sdp::DataElement();
  elem.SetUrl("https://foobar.dev");  // Random URL
  auto attributes = std::map<bt::sdp::AttributeId, bt::sdp::DataElement>();
  attributes.emplace(attr_id, std::move(elem));
  adapter()->fake_bredr()->TriggerServiceFound(
      peer_id, uuid, std::move(attributes));

  RunLoopUntilIdle();

  EXPECT_EQ(search_results.service_found_count(), 1u);
  EXPECT_EQ(search_results.peer_id().value().value, peer_id.value());
  EXPECT_EQ(search_results.attributes().value().size(), 1u);
  EXPECT_EQ(search_results.attributes().value()[0].id(), attr_id);
  EXPECT_EQ(search_results.attributes().value()[0].element().url(),
            std::string("https://foobar.dev"));
}

TEST_F(ProfileServerTestFakeAdapter, SearchResultsFlowControl) {
  fidlbredr::SearchResultsHandle search_results_handle;
  FakeSearchResults search_results(search_results_handle.NewRequest(),
                                   dispatcher());

  std::deque<FakeSearchResults::ServiceFoundCallback> result_cbs;
  search_results.set_result_cb([this, &result_cbs](auto&& cb) {
    EXPECT_NE(lease_provider().lease_count(), 0u);
    result_cbs.push_back(std::move(cb));
  });

  auto complete_one_cb = [&result_cbs] {
    ASSERT_FALSE(result_cbs.empty());
    auto cb = std::move(result_cbs.front());
    result_cbs.pop_front();
    cb(fidlbredr::SearchResults_ServiceFound_Result::WithResponse(
        fidlbredr::SearchResults_ServiceFound_Response()));
  };

  fidlbredr::ServiceClassProfileIdentifier search_uuid =
      fidlbredr::ServiceClassProfileIdentifier::AUDIO_SINK;

  EXPECT_EQ(adapter()->fake_bredr()->registered_searches().size(), 0u);
  EXPECT_EQ(search_results.service_found_count(), 0u);

  // FIDL client registers a service search.
  fidlbredr::ProfileSearchRequest request;
  request.set_service_uuid(search_uuid);
  request.set_attr_ids({});
  request.set_results(std::move(search_results_handle));
  client()->Search(std::move(request));
  RunLoopUntilIdle();
  EXPECT_EQ(lease_provider().lease_count(), 0u);
  EXPECT_EQ(adapter()->fake_bredr()->registered_searches().size(), 1u);

  // Trigger a match on the service search with some data. Should be received by
  // the FIDL client.
  bt::PeerId peer_id = bt::PeerId{10};
  bt::UUID uuid(static_cast<uint32_t>(search_uuid));

  // Send max unacked results.
  for (size_t i = 0; i < ProfileServer::kMaxUnackedSearchResults; ++i) {
    bt::sdp::AttributeId attr_id = 50 + i;  // Random Attribute ID
    bt::sdp::DataElement elem = bt::sdp::DataElement();
    elem.SetUrl("https://foobar.dev/" + std::to_string(i));
    auto attributes = std::map<bt::sdp::AttributeId, bt::sdp::DataElement>();
    attributes.emplace(attr_id, std::move(elem));

    adapter()->fake_bredr()->TriggerServiceFound(
        peer_id, uuid, std::move(attributes));
    RunLoopUntilIdle();

    EXPECT_EQ(result_cbs.size(), i + 1);
    EXPECT_EQ(search_results.service_found_count(), i + 1);
    EXPECT_EQ(search_results.attributes().value()[0].id(), attr_id);
    EXPECT_EQ(search_results.attributes().value()[0].element().url(),
              "https://foobar.dev/" + std::to_string(i));
  }

  // Send two more results. These should be queued and not sent to the client.
  bt::sdp::AttributeId attr_id_excess_1 = 100;
  bt::sdp::DataElement elem_excess_1 = bt::sdp::DataElement();
  elem_excess_1.SetUrl("https://foobar.dev/excess_1");
  auto attributes_excess_1 =
      std::map<bt::sdp::AttributeId, bt::sdp::DataElement>();
  attributes_excess_1.emplace(attr_id_excess_1, std::move(elem_excess_1));
  adapter()->fake_bredr()->TriggerServiceFound(
      peer_id, uuid, std::move(attributes_excess_1));
  RunLoopUntilIdle();

  EXPECT_EQ(result_cbs.size(), ProfileServer::kMaxUnackedSearchResults);
  EXPECT_EQ(search_results.service_found_count(),
            ProfileServer::kMaxUnackedSearchResults);

  bt::sdp::AttributeId attr_id_excess_2 = 101;
  bt::sdp::DataElement elem_excess_2 = bt::sdp::DataElement();
  elem_excess_2.SetUrl("https://foobar.dev/excess_2");
  auto attributes_excess_2 =
      std::map<bt::sdp::AttributeId, bt::sdp::DataElement>();
  attributes_excess_2.emplace(attr_id_excess_2, std::move(elem_excess_2));
  adapter()->fake_bredr()->TriggerServiceFound(
      peer_id, uuid, std::move(attributes_excess_2));
  RunLoopUntilIdle();

  EXPECT_EQ(result_cbs.size(), ProfileServer::kMaxUnackedSearchResults);
  EXPECT_EQ(search_results.service_found_count(),
            ProfileServer::kMaxUnackedSearchResults);

  // Ack one result. The first excess result should now be sent.
  complete_one_cb();
  RunLoopUntilIdle();

  EXPECT_EQ(result_cbs.size(), ProfileServer::kMaxUnackedSearchResults);
  EXPECT_EQ(search_results.service_found_count(),
            ProfileServer::kMaxUnackedSearchResults + 1);
  EXPECT_EQ(search_results.attributes().value()[0].id(), attr_id_excess_1);
  EXPECT_EQ(search_results.attributes().value()[0].element().url(),
            "https://foobar.dev/excess_1");

  // Ack another result. The second excess result should now be sent.
  complete_one_cb();
  RunLoopUntilIdle();

  EXPECT_EQ(result_cbs.size(), ProfileServer::kMaxUnackedSearchResults);
  EXPECT_EQ(search_results.service_found_count(),
            ProfileServer::kMaxUnackedSearchResults + 2);
  EXPECT_EQ(search_results.attributes().value()[0].id(), attr_id_excess_2);
  EXPECT_EQ(search_results.attributes().value()[0].element().url(),
            "https://foobar.dev/excess_2");

  // Ack all remaining results.
  while (!result_cbs.empty()) {
    complete_one_cb();
    RunLoopUntilIdle();
  }

  EXPECT_EQ(search_results.service_found_count(),
            ProfileServer::kMaxUnackedSearchResults + 2);
}

TEST_F(ProfileServerTestFakeAdapter, SearchWithMissingUuidFails) {
  fidlbredr::SearchResultsHandle search_results_handle;
  FakeSearchResults search_results(search_results_handle.NewRequest(),
                                   dispatcher());

  // Neither `service_uuid` nor `full_uid` is set
  fidlbredr::ProfileSearchRequest request;
  request.set_results(std::move(search_results_handle));
  client()->Search(std::move(request));
  RunLoopUntilIdle();
  EXPECT_EQ(adapter()->fake_bredr()->registered_searches().size(), 0u);
  EXPECT_TRUE(search_results.closed());
}

TEST_F(ProfileServerTestFakeAdapter, SearchWithServiceAndFullUuidFails) {
  fidlbredr::SearchResultsHandle search_results_handle;
  FakeSearchResults search_results(search_results_handle.NewRequest(),
                                   dispatcher());

  fidlbredr::ServiceClassProfileIdentifier search_uuid =
      fidlbredr::ServiceClassProfileIdentifier::AUDIO_SINK;

  fidlbredr::ProfileSearchRequest request;
  request.set_results(std::move(search_results_handle));
  request.set_service_uuid(search_uuid);
  request.set_full_uuid(fidl_helpers::UuidToFidl(bt::sdp::profile::kAudioSink));
  client()->Search(std::move(request));
  RunLoopUntilIdle();
  EXPECT_EQ(adapter()->fake_bredr()->registered_searches().size(), 0u);
  EXPECT_TRUE(search_results.closed());
}

TEST_F(ProfileServerTestFakeAdapter, SearchWithMissingResultsClientFails) {
  // results is not set
  fidlbredr::ProfileSearchRequest request;
  request.set_service_uuid(
      fidlbredr::ServiceClassProfileIdentifier::AUDIO_SINK);
  client()->Search(std::move(request));
  RunLoopUntilIdle();
  EXPECT_EQ(adapter()->fake_bredr()->registered_searches().size(), 0u);
}

TEST_F(ProfileServerTestFakeAdapter, SearchWithMissingAttrIdsSucceeds) {
  fidlbredr::SearchResultsHandle search_results_handle;
  FakeSearchResults search_results(search_results_handle.NewRequest(),
                                   dispatcher());

  fidlbredr::ProfileSearchRequest request;
  request.set_service_uuid(
      fidlbredr::ServiceClassProfileIdentifier::AUDIO_SINK);
  request.set_results(std::move(search_results_handle));
  client()->Search(std::move(request));
  RunLoopUntilIdle();
  EXPECT_EQ(adapter()->fake_bredr()->registered_searches().size(), 1u);
}

TEST_F(ProfileServerTestScoConnected, ScoConnectionRead2Packets) {
  // Queue a read request before the packet is received.
  std::optional<fidlbredr::RxPacketStatus> packet_status;
  std::optional<std::vector<uint8_t>> packet;
  sco_connection()->Read(
      [&](::fuchsia::bluetooth::bredr::ScoConnection_Read_Result result) {
        ASSERT_TRUE(result.is_response());
        packet_status = result.response().status_flag();
        packet = std::move(*result.response().mutable_data());
      });
  RunLoopUntilIdle();
  EXPECT_FALSE(packet_status);
  EXPECT_FALSE(packet);

  bt::StaticByteBuffer packet_buffer_0(
      bt::LowerBits(sco_handle()),
      bt::UpperBits(sco_handle()) |
          0x30,  // handle + packet status flag: kDataPartiallyLost
      0x01,      // payload length
      0x00       // payload
  );
  bt::BufferView packet_buffer_0_payload =
      packet_buffer_0.view(sizeof(bt::hci_spec::SynchronousDataHeader));
  test_device()->SendScoDataChannelPacket(packet_buffer_0);
  RunLoopUntilIdle();
  ASSERT_TRUE(packet_status);
  EXPECT_EQ(packet_status.value(),
            fidlbredr::RxPacketStatus::DATA_PARTIALLY_LOST);
  ASSERT_TRUE(packet);
  EXPECT_THAT(packet.value(),
              ::testing::ElementsAreArray(packet_buffer_0_payload));
  packet_status.reset();
  packet.reset();

  // Receive a second packet. This time, receive the packet before Read() is
  // called.
  bt::StaticByteBuffer packet_buffer_1(
      bt::LowerBits(sco_handle()),
      bt::UpperBits(
          sco_handle()),  // handle + packet status flag: kCorrectlyReceived
      0x01,               // payload length
      0x01                // payload
  );
  bt::BufferView packet_buffer_1_payload =
      packet_buffer_1.view(sizeof(bt::hci_spec::SynchronousDataHeader));
  test_device()->SendScoDataChannelPacket(packet_buffer_1);
  RunLoopUntilIdle();

  sco_connection()->Read(
      [&](::fuchsia::bluetooth::bredr::ScoConnection_Read_Result result) {
        ASSERT_TRUE(result.is_response());
        packet_status = result.response().status_flag();
        packet = std::move(*result.response().mutable_data());
      });
  RunLoopUntilIdle();
  ASSERT_TRUE(packet_status);
  EXPECT_EQ(packet_status.value(),
            fidlbredr::RxPacketStatus::CORRECTLY_RECEIVED_DATA);
  ASSERT_TRUE(packet);
  EXPECT_THAT(packet.value(),
              ::testing::ElementsAreArray(packet_buffer_1_payload));
}

TEST_F(ProfileServerTestScoConnected,
       ScoConnectionReadWhileReadPendingClosesConnection) {
  std::optional<fidlbredr::RxPacketStatus> packet_status_0;
  std::optional<std::vector<uint8_t>> packet_0;
  sco_connection()->Read(
      [&](::fuchsia::bluetooth::bredr::ScoConnection_Read_Result result) {
        ASSERT_TRUE(result.is_response());
        packet_status_0 = result.response().status_flag();
        packet_0 = std::move(*result.response().mutable_data());
      });

  RunLoopUntilIdle();
  EXPECT_FALSE(packet_status_0);
  EXPECT_FALSE(packet_0);

  std::optional<fidlbredr::RxPacketStatus> packet_status_1;
  std::optional<std::vector<uint8_t>> packet_1;
  sco_connection()->Read(
      [&](::fuchsia::bluetooth::bredr::ScoConnection_Read_Result result) {
        ASSERT_TRUE(result.is_response());
        packet_status_1 = result.response().status_flag();
        packet_1 = std::move(*result.response().mutable_data());
      });

  RunLoopUntilIdle();
  EXPECT_FALSE(packet_status_0);
  EXPECT_FALSE(packet_0);
  EXPECT_FALSE(packet_status_1);
  EXPECT_FALSE(packet_1);
  EXPECT_FALSE(sco_connection());
  ASSERT_TRUE(sco_conn_error());
  EXPECT_EQ(sco_conn_error().value(), ZX_ERR_BAD_STATE);
}

TEST_F(ProfileServerTestOffloadedScoConnected, ScoConnectionReadFails) {
  sco_connection()->Read(
      [&](::fuchsia::bluetooth::bredr::ScoConnection_Read_Result result) {
        FAIL();
      });
  RunLoopUntilIdle();
  EXPECT_FALSE(sco_connection());
  ASSERT_TRUE(sco_conn_error());
  EXPECT_EQ(sco_conn_error().value(), ZX_ERR_IO_NOT_PRESENT);
}

TEST_F(ProfileServerTestScoConnected, ScoConnectionWriteTwice) {
  bt::StaticByteBuffer payload_0(0x00);
  bt::DynamicByteBuffer packet_buffer_0 = bt::testing::ScoDataPacket(
      sco_handle(),
      bt::hci_spec::SynchronousDataPacketStatusFlag::kCorrectlyReceived,
      payload_0.view());

  bt::StaticByteBuffer payload_1(0x01);
  bt::DynamicByteBuffer packet_buffer_1 = bt::testing::ScoDataPacket(
      sco_handle(),
      bt::hci_spec::SynchronousDataPacketStatusFlag::kCorrectlyReceived,
      payload_1.view());

  int sco_cb_count = 0;
  test_device()->SetScoDataCallback([&](const bt::ByteBuffer& buffer) {
    if (sco_cb_count == 0) {
      EXPECT_THAT(buffer, ::testing::ElementsAreArray(packet_buffer_0));
    } else if (sco_cb_count == 1) {
      EXPECT_THAT(buffer, ::testing::ElementsAreArray(packet_buffer_1));
    } else {
      ADD_FAILURE() << "Unexpected packet sent";
    }
    sco_cb_count++;
  });
  int write_cb_0_count = 0;
  fidlbredr::ScoConnectionWriteRequest request_0;
  request_0.set_data(payload_0.ToVector());
  sco_connection()->Write(std::move(request_0),
                          [&](fidlbredr::ScoConnection_Write_Result result) {
                            ASSERT_TRUE(result.is_response());
                            write_cb_0_count++;
                          });
  RunLoopUntilIdle();
  EXPECT_EQ(sco_cb_count, 1);
  EXPECT_EQ(write_cb_0_count, 1);

  int write_cb_1_count = 0;
  fidlbredr::ScoConnectionWriteRequest request_1;
  request_1.set_data(payload_1.ToVector());
  sco_connection()->Write(std::move(request_1),
                          [&](fidlbredr::ScoConnection_Write_Result result) {
                            ASSERT_TRUE(result.is_response());
                            write_cb_1_count++;
                          });
  RunLoopUntilIdle();
  EXPECT_EQ(sco_cb_count, 2);
  EXPECT_EQ(write_cb_1_count, 1);

  test_device()->ClearScoDataCallback();
}

TEST_F(ProfileServerTestScoConnected, ScoConnectionWriteMissingDataField) {
  int write_cb_count = 0;
  // The `data` field is not set.
  fidlbredr::ScoConnectionWriteRequest request;
  sco_connection()->Write(
      std::move(request),
      [&](fidlbredr::ScoConnection_Write_Result result) { write_cb_count++; });
  RunLoopUntilIdle();
  EXPECT_EQ(write_cb_count, 0);
  EXPECT_FALSE(sco_connection());
  ASSERT_TRUE(sco_conn_error());
  EXPECT_EQ(sco_conn_error().value(), ZX_ERR_INVALID_ARGS);
}

TEST_F(ProfileServerTestOffloadedScoConnected, ScoConnectionWriteFails) {
  int write_cb_count = 0;
  fidlbredr::ScoConnectionWriteRequest request;
  request.set_data({0x00});
  sco_connection()->Write(
      std::move(request),
      [&](fidlbredr::ScoConnection_Write_Result result) { write_cb_count++; });
  RunLoopUntilIdle();
  EXPECT_EQ(write_cb_count, 0);
  EXPECT_FALSE(sco_connection());
  ASSERT_TRUE(sco_conn_error());
  EXPECT_EQ(sco_conn_error().value(), ZX_ERR_IO_NOT_PRESENT);
}

TEST_F(ProfileServerTestFakeAdapter,
       ConnectionReceiver2ConnectedHoldsWakeLease) {
  const bt::hci_spec::ConnectionHandle kHandle(1);
  fidlbredr::ProfileAdvertiseRequest adv_request;

  fidlbredr::ConnectionReceiver2Handle receiver_handle;
  FakeConnectionReceiver connection_receiver(receiver_handle.NewRequest(),
                                             dispatcher());
  adv_request.set_connection_receiver(std::move(receiver_handle));

  std::vector<fidlbredr::ServiceDefinition> services;
  services.emplace_back(MakeFIDLServiceDefinition());
  adv_request.set_services(std::move(services));

  size_t cb_count = 0;
  auto cb = [&](fidlbredr::Profile_Advertise_Result result) {
    cb_count++;
    EXPECT_TRUE(result.is_response());
  };

  client()->Advertise(std::move(adv_request), std::move(cb));
  RunLoopUntilIdle();
  ASSERT_EQ(cb_count, 1u);
  ASSERT_EQ(adapter()->fake_bredr()->registered_services().size(), 1u);
  auto service_iter = adapter()->fake_bredr()->registered_services().begin();

  bt::l2cap::ChannelInfo chan_info = bt::l2cap::ChannelInfo::MakeBasicMode(
      bt::l2cap::kDefaultMTU, bt::l2cap::kDefaultMTU, bt::l2cap::kAVDTP);
  auto channel =
      std::make_unique<FakeChannel>(bt::l2cap::kFirstDynamicChannelId,
                                    bt::l2cap::kFirstDynamicChannelId,
                                    kHandle,
                                    bt::LinkType::kACL,
                                    chan_info);

  EXPECT_EQ(lease_provider().lease_count(), 0u);
  service_iter->second.connect_callback(channel->GetWeakPtr(),
                                        MakeL2capProtocolListElement());
  // Wake lease should be held while waiting for ConnectionReceiver2.Connected
  // response.
  EXPECT_NE(lease_provider().lease_count(), 0u);
  ASSERT_FALSE(connection_receiver.channel().has_value());

  // Wait for ConnectionReceiver2.Connected response from
  // FakeConnectionReceiver.
  RunLoopUntilIdle();
  ASSERT_TRUE(connection_receiver.channel().has_value());
  EXPECT_EQ(lease_provider().lease_count(), 0u);

  channel->Close();
  RunLoopUntilIdle();
}

INSTANTIATE_TEST_SUITE_P(Bool,
                         ProfileServerTestConnectedPeerWithBoolParam,
                         ::testing::Bool());
INSTANTIATE_TEST_SUITE_P(Bool, PriorityTestWithBoolParam, ::testing::Bool());
INSTANTIATE_TEST_SUITE_P(Bool,
                         ProfileServerTestWithBoolParam,
                         ::testing::Bool());
INSTANTIATE_TEST_SUITE_P(Bool,
                         ProfileServerTestFakeAdapterWithBoolParam,
                         ::testing::Bool());

}  // namespace
}  // namespace bthost
