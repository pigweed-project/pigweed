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

// inclusive-language: disable

#include "pw_bluetooth_sapphire/internal/host/gap/bredr_connection_manager.h"

#include <cpp-string/string_printf.h>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/error.h"
#include "pw_bluetooth_sapphire/internal/host/gap/fake_pairing_delegate.h"
#include "pw_bluetooth_sapphire/internal/host/gap/peer_cache.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/link_key.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/protocol.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_l2cap.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/l2cap_defs.h"
#include "pw_bluetooth_sapphire/internal/host/sdp/sdp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/test_security_manager.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/gtest_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"
#include "pw_bluetooth_sapphire/internal/host/transport/fake_acl_connection.h"

namespace bt::gap {
namespace {

using namespace inspect::testing;

using pw::bluetooth::emboss::AuthenticationRequirements;
using pw::bluetooth::emboss::IoCapability;

using TestingBase =
    bt::testing::FakeDispatcherControllerTest<bt::testing::MockController>;

constexpr hci_spec::ConnectionHandle kConnectionHandle = 0x0BAA;
constexpr hci_spec::ConnectionHandle kConnectionHandle2 = 0x0BAB;
const DeviceAddress kLocalDevLEAddr(DeviceAddress::Type::kLEPublic, {0});
const DeviceAddress kLocalDevAddr(DeviceAddress::Type::kBREDR, {0});
const DeviceAddress kTestDevAddr(DeviceAddress::Type::kBREDR, {1});
const DeviceAddress kTestDevAddrLe(DeviceAddress::Type::kLEPublic, {2});
const DeviceAddress kTestDevAddr2(DeviceAddress::Type::kBREDR, {3});
const UInt128 kIrk{{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}};
constexpr uint32_t kPasskey = 123456;
constexpr uint16_t kDefaultPinCode = 0000;
const hci_spec::LinkKey kRawKey({0xc0,
                                 0xde,
                                 0xfa,
                                 0x57,
                                 0x4b,
                                 0xad,
                                 0xf0,
                                 0x0d,
                                 0xa7,
                                 0x60,
                                 0x06,
                                 0x1e,
                                 0xca,
                                 0x1e,
                                 0xca,
                                 0xfe},
                                0,
                                0);
const hci_spec::LinkKey kChangedKey({0xfa,
                                     0xce,
                                     0xb0,
                                     0x0c,
                                     0xa5,
                                     0x1c,
                                     0xcd,
                                     0x15,
                                     0xea,
                                     0x5e,
                                     0xfe,
                                     0xdb,
                                     0x1d,
                                     0x0d,
                                     0x0a,
                                     0xd5},
                                    0,
                                    0);
const hci_spec::LinkKey kLegacyKey({0x41,
                                    0x33,
                                    0x7c,
                                    0x0d,
                                    0xef,
                                    0xee,
                                    0xda,
                                    0xda,
                                    0xba,
                                    0xad,
                                    0x0f,
                                    0xf1,
                                    0xce,
                                    0xc0,
                                    0xff,
                                    0xee},
                                   0,
                                   0);
const sm::LTK kLinkKey(
    sm::SecurityProperties(hci_spec::LinkKeyType::kAuthenticatedCombination192),
    kRawKey);
const bt::sm::LTK kLELtk(sm::SecurityProperties(/*encrypted=*/true,
                                                /*authenticated=*/true,
                                                /*secure_connections=*/true,
                                                sm::kMaxEncryptionKeySize),
                         hci_spec::LinkKey(UInt128{4}, 5, 6));

constexpr BrEdrSecurityRequirements kNoSecurityRequirements{
    .authentication = false, .secure_connections = false};
constexpr BrEdrSecurityRequirements kAuthSecurityRequirements{
    .authentication = true, .secure_connections = false};

// A default size for PDUs when generating responses for testing.
const uint16_t PDU_MAX = 0xFFF;

const DeviceAddress TEST_DEV_ADDR(DeviceAddress::Type::kLEPublic,
                                  {0x01, 0x00, 0x00, 0x00, 0x00, 0x00});

const auto kReadScanEnable = testing::ReadScanEnable();
const auto kReadScanEnableRspNone = testing::ReadScanEnableResponse(0x00);
const auto kReadScanEnableRspInquiry = testing::ReadScanEnableResponse(0x01);
const auto kReadScanEnableRspPage = testing::ReadScanEnableResponse(0x02);
const auto kReadScanEnableRspBoth = testing::ReadScanEnableResponse(0x03);

const auto kWriteScanEnableNone = testing::WriteScanEnable(0x00);
const auto kWriteScanEnableInq = testing::WriteScanEnable(0x01);
const auto kWriteScanEnablePage = testing::WriteScanEnable(0x02);
const auto kWriteScanEnableBoth = testing::WriteScanEnable(0x03);
const auto kWriteScanEnableRsp =
    testing::CommandCompletePacket(hci_spec::kWriteScanEnable);

#define COMMAND_COMPLETE_RSP(opcode)                    \
  StaticByteBuffer(hci_spec::kCommandCompleteEventCode, \
                   0x04,                                \
                   0xF0,                                \
                   LowerBits((opcode)),                 \
                   UpperBits((opcode)),                 \
                   pw::bluetooth::emboss::StatusCode::SUCCESS)

const uint16_t kScanInterval = 0x0800;  // 1280 ms
const uint16_t kScanWindow = 0x0011;    // 10.625 ms
const auto kWritePageScanActivity =
    testing::WritePageScanActivityPacket(kScanInterval, kScanWindow);
const auto kWritePageScanActivityRsp =
    testing::CommandCompletePacket(hci_spec::kWritePageScanActivity);

const uint8_t kScanType = 0x01;  // Interlaced scan
const auto kWritePageScanType = testing::WritePageScanTypePacket(kScanType);
const auto kWritePageScanTypeRsp =
    testing::CommandCompletePacket(hci_spec::kWritePageScanType);

#define COMMAND_STATUS_RSP(opcode, statuscode)        \
  StaticByteBuffer(hci_spec::kCommandStatusEventCode, \
                   0x04,                              \
                   (statuscode),                      \
                   0xF0,                              \
                   LowerBits((opcode)),               \
                   UpperBits((opcode)))

const auto kWritePageTimeoutRsp =
    COMMAND_COMPLETE_RSP(hci_spec::kWritePageTimeout);

const auto kWritePinTypeRsp =
    testing::CommandCompletePacket(hci_spec::kWritePinType);

const auto kConnectionRequest = testing::ConnectionRequestPacket(kTestDevAddr);

const auto kAcceptConnectionRequest =
    testing::AcceptConnectionRequestPacket(kTestDevAddr);

const auto kAcceptConnectionRequestRsp =
    testing::CommandStatusPacket(hci_spec::kAcceptConnectionRequest);

const auto kConnectionComplete =
    testing::ConnectionCompletePacket(kTestDevAddr, kConnectionHandle);

const auto kConnectionCompletePageTimeout = testing::ConnectionCompletePacket(
    kTestDevAddr,
    kConnectionHandle,
    pw::bluetooth::emboss::StatusCode::PAGE_TIMEOUT);

const auto kConnectionCompleteError = testing::ConnectionCompletePacket(
    TEST_DEV_ADDR,
    0x0000,
    pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);

const auto kConnectionCompleteCanceled = testing::ConnectionCompletePacket(
    TEST_DEV_ADDR,
    0x0000,
    pw::bluetooth::emboss::StatusCode::UNKNOWN_CONNECTION_ID);

const auto kCreateConnection = testing::CreateConnectionPacket(TEST_DEV_ADDR);

const auto kCreateConnectionRsp = COMMAND_STATUS_RSP(
    hci_spec::kCreateConnection, pw::bluetooth::emboss::StatusCode::SUCCESS);

const auto kCreateConnectionRspError = COMMAND_STATUS_RSP(
    hci_spec::kCreateConnection,
    pw::bluetooth::emboss::StatusCode::CONNECTION_FAILED_TO_BE_ESTABLISHED);

const auto kCreateConnectionCancel =
    testing::CreateConnectionCancelPacket(TEST_DEV_ADDR);

const auto kCreateConnectionCancelRsp =
    testing::CommandCompletePacket(hci_spec::kCreateConnectionCancel);

const auto kRemoteNameRequest = testing::RemoteNameRequestPacket(TEST_DEV_ADDR);
const auto kRemoteNameRequestRsp = COMMAND_STATUS_RSP(
    hci_spec::kRemoteNameRequest, pw::bluetooth::emboss::StatusCode::SUCCESS);

const auto kRemoteNameRequestComplete =
    testing::RemoteNameRequestCompletePacket(
        kTestDevAddr,
        {'F',    'u',    'c',    'h',    's',    'i',    'a',    '\xF0', '\x9F',
         '\x92', '\x96', '\x00', '\x14', '\x15', '\x16', '\x17', '\x18', '\x19',
         '\x1a', '\x1b', '\x1c', '\x1d', '\x1e', '\x1f', '\x20'}
        // remote name (Fuchsia💖)
        // Everything after the 0x00 should be ignored.
    );
const StaticByteBuffer kReadRemoteVersionInfo(
    LowerBits(hci_spec::kReadRemoteVersionInfo),
    UpperBits(hci_spec::kReadRemoteVersionInfo),
    0x02,  // Parameter_total_size (2 bytes)
    0xAA,
    0x0B  // connection_handle
);

const auto kReadRemoteVersionInfoRsp =
    COMMAND_STATUS_RSP(hci_spec::kReadRemoteVersionInfo,
                       pw::bluetooth::emboss::StatusCode::SUCCESS);

const auto kRemoteVersionInfoComplete = StaticByteBuffer(
    hci_spec::kReadRemoteVersionInfoCompleteEventCode,
    0x08,  // parameter_total_size (8 bytes)
    pw::bluetooth::emboss::StatusCode::SUCCESS,  // status
    0xAA,
    0x0B,                                                   // connection_handle
    pw::bluetooth::emboss::CoreSpecificationVersion::V4_2,  // version
    0xE0,
    0x00,  // company_identifier (Google)
    0xAD,
    0xDE  // subversion (anything)
);

const auto kReadRemoteSupportedFeaturesRsp =
    COMMAND_STATUS_RSP(hci_spec::kReadRemoteSupportedFeatures,
                       pw::bluetooth::emboss::StatusCode::SUCCESS);

const auto kReadRemoteSupportedFeaturesComplete =
    StaticByteBuffer(hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode,
                     0x0B,  // parameter_total_size (11 bytes)
                     pw::bluetooth::emboss::StatusCode::SUCCESS,  // status
                     0xAA,
                     0x0B,  // connection_handle,
                     0xFF,
                     0x00,
                     0x00,
                     0x00,
                     0x40,
                     0x00,
                     0x08,
                     0x80
                     // lmp_features_page0: 3 slot packets, 5 slot packets,
                     // Encryption, Slot Offset, Timing Accuracy, Role Switch,
                     // Hold Mode, Sniff Mode, Secure Simple Pairing (Controller
                     // Support), LE Supported, Extended Features
    );

const auto kReadRemoteExtended1 =
    StaticByteBuffer(LowerBits(hci_spec::kReadRemoteExtendedFeatures),
                     UpperBits(hci_spec::kReadRemoteExtendedFeatures),
                     0x03,  // parameter_total_size (3 bytes)
                     0xAA,
                     0x0B,  // connection_handle
                     0x01   // page_number (1)
    );

const auto kReadRemoteExtendedFeaturesRsp =
    COMMAND_STATUS_RSP(hci_spec::kReadRemoteExtendedFeatures,
                       pw::bluetooth::emboss::StatusCode::SUCCESS);

const auto kReadRemoteExtended1Complete =
    testing::ReadRemoteExtended1CompletePacket(kConnectionHandle);

const auto kReadRemoteExtended1CompleteNoSsp =
    testing::ReadRemoteExtended1CompletePacketNoSsp(kConnectionHandle);

const auto kReadRemoteExtended2 =
    StaticByteBuffer(LowerBits(hci_spec::kReadRemoteExtendedFeatures),
                     UpperBits(hci_spec::kReadRemoteExtendedFeatures),
                     0x03,  // parameter_total_size (3 bytes)
                     0xAA,
                     0x0B,  // connection_handle
                     0x02   // page_number (2)
    );

const auto kReadRemoteExtended2Complete =
    StaticByteBuffer(hci_spec::kReadRemoteExtendedFeaturesCompleteEventCode,
                     0x0D,  // parameter_total_size (13 bytes)
                     pw::bluetooth::emboss::StatusCode::SUCCESS,  // status
                     0xAA,
                     0x0B,  // connection_handle,
                     0x02,  // page_number
                     0x02,  // max_page_number
                     0x00,
                     0x00,
                     0x00,
                     0x00,
                     0x02,
                     0x00,
                     0xFF,
                     0x00
                     // lmp_features_page2  - All the bits should be ignored.
    );

const auto kDisconnect = testing::DisconnectPacket(kConnectionHandle);

const auto kDisconnectRsp = COMMAND_STATUS_RSP(
    hci_spec::kDisconnect, pw::bluetooth::emboss::StatusCode::SUCCESS);

const auto kDisconnectionComplete = testing::DisconnectionCompletePacket(
    kConnectionHandle,
    pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION);

const auto kAuthenticationRequested =
    testing::AuthenticationRequestedPacket(kConnectionHandle);

const auto kAuthenticationRequestedStatus =
    COMMAND_STATUS_RSP(hci_spec::kAuthenticationRequested,
                       pw::bluetooth::emboss::StatusCode::SUCCESS);

const auto kAuthenticationComplete =
    StaticByteBuffer(hci_spec::kAuthenticationCompleteEventCode,
                     0x03,  // parameter_total_size (3 bytes)
                     pw::bluetooth::emboss::StatusCode::SUCCESS,  // status
                     0xAA,
                     0x0B  // connection_handle
    );

const auto kAuthenticationCompleteFailed = StaticByteBuffer(
    hci_spec::kAuthenticationCompleteEventCode,
    0x03,  // parameter_total_size (3 bytes)
    pw::bluetooth::emboss::StatusCode::PAIRING_NOT_ALLOWED,  // status
    0xAA,
    0x0B  // connection_handle
);

const auto kLinkKeyRequest = testing::LinkKeyRequestPacket(TEST_DEV_ADDR);

const auto kLinkKeyRequestNegativeReply =
    testing::LinkKeyRequestNegativeReplyPacket(TEST_DEV_ADDR);

const auto kLinkKeyRequestNegativeReplyRsp =
    testing::LinkKeyRequestNegativeReplyResponse(TEST_DEV_ADDR);

DynamicByteBuffer MakeIoCapabilityResponse(
    IoCapability io_cap, AuthenticationRequirements auth_req) {
  return testing::IoCapabilityResponsePacket(TEST_DEV_ADDR, io_cap, auth_req);
}

const auto kIoCapabilityRequest =
    testing::IoCapabilityRequestPacket(TEST_DEV_ADDR);

DynamicByteBuffer MakeIoCapabilityRequestReply(
    IoCapability io_cap, AuthenticationRequirements auth_req) {
  return testing::IoCapabilityRequestReplyPacket(
      TEST_DEV_ADDR, io_cap, auth_req);
}

const auto kIoCapabilityRequestReplyRsp =
    testing::IoCapabilityRequestReplyResponse(TEST_DEV_ADDR);

const auto kIoCapabilityRequestNegativeReply =
    testing::IoCapabilityRequestNegativeReplyPacket(
        TEST_DEV_ADDR, pw::bluetooth::emboss::StatusCode::PAIRING_NOT_ALLOWED);

const auto kIoCapabilityRequestNegativeReplyRsp =
    testing::IoCapabilityRequestNegativeReplyResponse(TEST_DEV_ADDR);

DynamicByteBuffer MakeUserConfirmationRequest(uint32_t) {
  return testing::UserConfirmationRequestPacket(TEST_DEV_ADDR, kPasskey);
}

const auto kUserConfirmationRequestReply =
    testing::UserConfirmationRequestReplyPacket(TEST_DEV_ADDR);

const auto kUserConfirmationRequestReplyRsp =
    COMMAND_COMPLETE_RSP(hci_spec::kUserConfirmationRequestReply);

const auto kUserConfirmationRequestNegativeReply =
    testing::UserConfirmationRequestNegativeReplyPacket(TEST_DEV_ADDR);

const auto kUserConfirmationRequestNegativeReplyRsp =
    COMMAND_COMPLETE_RSP(hci_spec::kUserConfirmationRequestNegativeReply);

const auto kSimplePairingCompleteSuccess = testing::SimplePairingCompletePacket(
    TEST_DEV_ADDR, pw::bluetooth::emboss::StatusCode::SUCCESS);

const auto kSimplePairingCompleteError = testing::SimplePairingCompletePacket(
    TEST_DEV_ADDR, pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE);

const auto kPinCodeRequest = testing::PinCodeRequestPacket(TEST_DEV_ADDR);
const auto kPinCodeRequestReply = testing::PinCodeRequestReplyPacket(
    TEST_DEV_ADDR, /*pin_length=*/4, std::to_string(kDefaultPinCode));
const auto kPinCodeRequestReplyRsp =
    testing::PinCodeRequestReplyResponse(TEST_DEV_ADDR);
const auto kPinCodeRequestNegativeReply =
    testing::PinCodeRequestNegativeReplyPacket(TEST_DEV_ADDR);
const auto kPinCodeRequestNegativeReplyRsp =
    testing::PinCodeRequestNegativeReplyResponse(TEST_DEV_ADDR);

DynamicByteBuffer MakeLinkKeyNotification(hci_spec::LinkKeyType key_type) {
  return testing::LinkKeyNotificationPacket(
      TEST_DEV_ADDR, kRawKey.value(), key_type);
}

const auto kLinkKeyNotificationLegacy = testing::LinkKeyNotificationPacket(
    TEST_DEV_ADDR, kLegacyKey.value(), hci_spec::LinkKeyType::kCombination);

const auto kLinkKeyNotification = MakeLinkKeyNotification(
    hci_spec::LinkKeyType::kAuthenticatedCombination192);

const auto kLinkKeyRequestReply =
    testing::LinkKeyRequestReplyPacket(TEST_DEV_ADDR, kRawKey.value());

const auto kLinkKeyRequestReplyRsp =
    testing::LinkKeyRequestReplyResponse(TEST_DEV_ADDR);

const auto kLinkKeyNotificationChanged = testing::LinkKeyNotificationPacket(
    TEST_DEV_ADDR,
    kChangedKey.value(),
    hci_spec::LinkKeyType::kChangedCombination);

const StaticByteBuffer kSetConnectionEncryption(
    LowerBits(hci_spec::kSetConnectionEncryption),
    UpperBits(hci_spec::kSetConnectionEncryption),
    0x03,  // parameter total size
    0xAA,
    0x0B,  // connection handle
    0x01   // encryption enable
);

const auto kSetConnectionEncryptionRsp =
    COMMAND_STATUS_RSP(hci_spec::kSetConnectionEncryption,
                       pw::bluetooth::emboss::StatusCode::SUCCESS);

const StaticByteBuffer kEncryptionChangeEvent(
    hci_spec::kEncryptionChangeEventCode,
    4,     // parameter total size
    0x00,  // status
    0xAA,
    0x0B,  // connection handle
    0x01   // encryption enabled: E0 for BR/EDR or AES-CCM for LE
);

const StaticByteBuffer kReadEncryptionKeySize(
    LowerBits(hci_spec::kReadEncryptionKeySize),
    UpperBits(hci_spec::kReadEncryptionKeySize),
    0x02,  // parameter size
    0xAA,
    0x0B  // connection handle
);

const StaticByteBuffer kReadEncryptionKeySizeRsp(
    hci_spec::kCommandCompleteEventCode,
    0x07,  // parameters total size
    0xFF,  // num command packets allowed (255)
    LowerBits(hci_spec::kReadEncryptionKeySize),
    UpperBits(hci_spec::kReadEncryptionKeySize),
    pw::bluetooth::emboss::StatusCode::SUCCESS,  // status
    0xAA,
    0x0B,  // connection handle
    0x10   // encryption key size: 16
);

DynamicByteBuffer MakeUserPasskeyRequestReply() {
  return testing::UserPasskeyRequestReplyPacket(TEST_DEV_ADDR, kPasskey);
}

const auto kUserPasskeyRequestReplyRsp =
    testing::UserPasskeyRequestReplyResponse(TEST_DEV_ADDR);

DynamicByteBuffer MakeUserPasskeyNotification(uint32_t) {
  return testing::UserPasskeyNotificationPacket(TEST_DEV_ADDR, kPasskey);
}

const hci::DataBufferInfo kBrEdrBufferInfo(1024, 1);
const hci::DataBufferInfo kLeBufferInfo(1024, 1);

constexpr l2cap::ChannelParameters kChannelParams;

::testing::AssertionResult IsInitializing(Peer* peer) {
  if (Peer::ConnectionState::kInitializing !=
      peer->bredr()->connection_state()) {
    return ::testing::AssertionFailure()
           << "Expected peer connection_state: kInitializing, found "
           << Peer::ConnectionStateToString(peer->bredr()->connection_state());
  }
  return ::testing::AssertionSuccess()
         << "Peer connection state is kInitializing";
}
::testing::AssertionResult IsConnected(Peer* peer) {
  if (Peer::ConnectionState::kConnected != peer->bredr()->connection_state()) {
    return ::testing::AssertionFailure()
           << "Expected peer to be in a connected state: kConnected, found "
           << Peer::ConnectionStateToString(peer->bredr()->connection_state());
  }
  if (peer->temporary()) {
    return ::testing::AssertionFailure()
           << "Expected peer to be non-temporary, but found temporary";
  }
  return ::testing::AssertionSuccess() << "Peer connection state is kConnected";
}
::testing::AssertionResult IsNotConnected(Peer* peer) {
  if (Peer::ConnectionState::kNotConnected !=
      peer->bredr()->connection_state()) {
    return ::testing::AssertionFailure()
           << "Expected peer connection_state: kNotConnected, found "
           << Peer::ConnectionStateToString(peer->bredr()->connection_state());
  }
  return ::testing::AssertionSuccess()
         << "Peer connection state is kNotConnected";
  ;
}

::testing::AssertionResult HasConnectionTo(Peer* peer, BrEdrConnection* conn) {
  if (!conn) {
    return ::testing::AssertionFailure()
           << "Expected BrEdrConnection, but found nullptr";
  }
  if (peer->identifier() != conn->peer_id()) {
    return ::testing::AssertionFailure()
           << "Expected connection peer_id " << peer->identifier()
           << " but found " << conn->peer_id();
  }
  return ::testing::AssertionSuccess() << "Peer " << peer->identifier()
                                       << " connected to the connection given";
}

#define CALLBACK_EXPECT_FAILURE(status_param)       \
  ([&status_param](auto cb_status, auto conn_ref) { \
    EXPECT_FALSE(conn_ref);                         \
    status_param = cb_status;                       \
  })

class BrEdrConnectionManagerTest : public TestingBase,
                                   public hci::LocalAddressDelegate {
 public:
  BrEdrConnectionManagerTest() = default;
  ~BrEdrConnectionManagerTest() override = default;

  void SetUp() override {
    TestingBase::SetUp();
    InitializeACLDataChannel(kBrEdrBufferInfo, kLeBufferInfo);

    peer_cache_ = std::make_unique<PeerCache>(dispatcher());
    l2cap_ = std::make_unique<l2cap::testing::FakeL2cap>(dispatcher());

    // Respond to BrEdrConnectionManager controller setup with success.
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::WritePageTimeoutPacket(static_cast<uint16_t>(
                              pw::bluetooth::emboss::PageTimeout::DEFAULT)),
                          &kWritePageTimeoutRsp);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::WritePinTypePacket(static_cast<uint8_t>(
                              pw::bluetooth::emboss::PinType::VARIABLE)),
                          &kWritePinTypeRsp);

    connection_manager_ = std::make_unique<BrEdrConnectionManager>(
        transport()->GetWeakPtr(),
        peer_cache_.get(),
        kLocalDevAddr,
        /*low_energy_address_delegate=*/this,
        l2cap_.get(),
        /*use_interlaced_scan=*/true,
        /*local_secure_connections_supported=*/true,
        /*legacy_pairing_enabled=*/false,
        /*controller_remote_public_key_validation_supported=*/true,
        fit::bind_member<&sm::testing::TestSecurityManagerFactory::CreateBrEdr>(
            &security_manager_factory_),
        dispatcher());

    RunUntilIdle();

    test_device()->SetTransactionCallback([this] { transaction_count_++; });
  }

  void TearDown() override {
    int expected_transaction_count = transaction_count();
    if (connection_manager_ != nullptr) {
      expected_transaction_count += 2;
      // deallocating the connection manager disables connectivity.
      EXPECT_CMD_PACKET_OUT(
          test_device(), kReadScanEnable, &kReadScanEnableRspBoth);
      EXPECT_CMD_PACKET_OUT(
          test_device(), kWriteScanEnableInq, &kWriteScanEnableRsp);
      connection_manager_ = nullptr;
    }
    RunUntilIdle();
    // A disconnection may also occur for a queued disconnection, allow up to 1
    // extra transaction.
    EXPECT_LE(expected_transaction_count, transaction_count());
    EXPECT_GE(expected_transaction_count + 1, transaction_count());
    // Don't trigger the transaction callback for the rest.
    test_device()->ClearTransactionCallback();
    test_device()->Stop();
    l2cap_ = nullptr;
    peer_cache_ = nullptr;
    TestingBase::TearDown();
  }

  inspect::Inspector inspector() { return inspector_; }

 protected:
  static constexpr const int kShortInterrogationTransactions = 3;
  static constexpr const int kInterrogationTransactions =
      kShortInterrogationTransactions + 2;
  static constexpr const int kIncomingConnTransactions =
      1 + kInterrogationTransactions;
  static constexpr const int kDisconnectionTransactions = 1;
  // Currently unused, for reference:
  // static constexpr const int kIncomingConnShortTransactions = 1 +
  // kShortInterrogationTransactions;

  BrEdrConnectionManager* connmgr() const { return connection_manager_.get(); }
  void SetConnectionManager(std::unique_ptr<BrEdrConnectionManager> mgr) {
    connection_manager_ = std::move(mgr);
  }

  PeerCache* peer_cache() const { return peer_cache_.get(); }

  l2cap::testing::FakeL2cap* l2cap() const { return l2cap_.get(); }

  int transaction_count() const { return transaction_count_; }

  // Expect an incoming connection that is accepted.
  void QueueSuccessfulAccept(
      DeviceAddress addr = kTestDevAddr,
      hci_spec::ConnectionHandle handle = kConnectionHandle,
      std::optional<pw::bluetooth::emboss::ConnectionRole> role_change =
          std::nullopt) const {
    const auto connection_complete =
        testing::ConnectionCompletePacket(addr, handle);
    if (role_change) {
      const auto role_change_event =
          testing::RoleChangePacket(addr, role_change.value());
      EXPECT_CMD_PACKET_OUT(test_device(),
                            testing::AcceptConnectionRequestPacket(addr),
                            &kAcceptConnectionRequestRsp,
                            &role_change_event,
                            &connection_complete);
    } else {
      EXPECT_CMD_PACKET_OUT(test_device(),
                            testing::AcceptConnectionRequestPacket(addr),
                            &kAcceptConnectionRequestRsp,
                            &connection_complete);
    }
  }

  // Add expectations and simulated responses for the outbound commands sent
  // after an inbound Connection Request Event is received, for a peer that is
  // already interrogated. Results in kIncomingConnShortTransactions
  // transaction.
  void QueueRepeatIncomingConn(
      DeviceAddress addr = kTestDevAddr,
      hci_spec::ConnectionHandle handle = kConnectionHandle,
      std::optional<pw::bluetooth::emboss::ConnectionRole> role_change =
          std::nullopt) const {
    QueueSuccessfulAccept(addr, handle, role_change);
    QueueShortInterrogation(handle);
  }

  // Add expectations and simulated responses for the outbound commands sent
  // after an inbound Connection Request Event is received, for a peer that is
  // already interrogated.
  //  Results in |kIncomingConnTransactions| transactions.
  void QueueSuccessfulIncomingConn(
      DeviceAddress addr = kTestDevAddr,
      hci_spec::ConnectionHandle handle = kConnectionHandle,
      std::optional<pw::bluetooth::emboss::ConnectionRole> role_change =
          std::nullopt) const {
    QueueSuccessfulAccept(addr, handle, role_change);
    QueueSuccessfulInterrogation(addr, handle);
  }

  void QueueSuccessfulCreateConnection(Peer* peer,
                                       hci_spec::ConnectionHandle conn) const {
    const DynamicByteBuffer complete_packet =
        testing::ConnectionCompletePacket(peer->address(), conn);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::CreateConnectionPacket(peer->address()),
                          &kCreateConnectionRsp,
                          &complete_packet);
  }

  void QueueShortInterrogation(hci_spec::ConnectionHandle conn) const {
    const DynamicByteBuffer remote_extended1_complete_packet =
        testing::ReadRemoteExtended1CompletePacket(conn);
    const DynamicByteBuffer remote_extended2_complete_packet =
        testing::ReadRemoteExtended2CompletePacket(conn);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteExtended1Packet(conn),
                          &kReadRemoteExtendedFeaturesRsp,
                          &remote_extended1_complete_packet);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteExtended2Packet(conn),
                          &kReadRemoteExtendedFeaturesRsp,
                          &remote_extended2_complete_packet);
  }

  void QueueSuccessfulInterrogation(DeviceAddress addr,
                                    hci_spec::ConnectionHandle conn) const {
    const DynamicByteBuffer remote_name_complete_packet =
        testing::RemoteNameRequestCompletePacket(addr);
    const DynamicByteBuffer remote_version_complete_packet =
        testing::ReadRemoteVersionInfoCompletePacket(conn);
    const DynamicByteBuffer remote_supported_complete_packet =
        testing::ReadRemoteSupportedFeaturesCompletePacket(
            conn, /*extended_features=*/true);

    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::RemoteNameRequestPacket(addr),
                          &kRemoteNameRequestRsp,
                          &remote_name_complete_packet);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteVersionInfoPacket(conn),
                          &kReadRemoteVersionInfoRsp,
                          &remote_version_complete_packet);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteSupportedFeaturesPacket(conn),
                          &kReadRemoteSupportedFeaturesRsp,
                          &remote_supported_complete_packet);
    QueueShortInterrogation(conn);
  }

  void QueueSuccessfulInterrogationNoSsp(
      DeviceAddress addr, hci_spec::ConnectionHandle conn) const {
    const DynamicByteBuffer remote_name_complete_packet =
        testing::RemoteNameRequestCompletePacket(addr);
    const DynamicByteBuffer remote_version_complete_packet =
        testing::ReadRemoteVersionInfoCompletePacket(conn);
    const DynamicByteBuffer remote_supported_complete_packet =
        testing::ReadRemoteSupportedFeaturesCompletePacket(
            conn, /*extended_features=*/true);
    const DynamicByteBuffer remote_extended1_complete_packet =
        testing::ReadRemoteExtended1CompletePacketNoSsp(conn);

    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::RemoteNameRequestPacket(addr),
                          &kRemoteNameRequestRsp,
                          &remote_name_complete_packet);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteVersionInfoPacket(conn),
                          &kReadRemoteVersionInfoRsp,
                          &remote_version_complete_packet);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteSupportedFeaturesPacket(conn),
                          &kReadRemoteSupportedFeaturesRsp,
                          &remote_supported_complete_packet);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteExtended1Packet(conn),
                          &kReadRemoteExtendedFeaturesRsp,
                          &remote_extended1_complete_packet);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteExtended2Packet(conn),
                          &kReadRemoteExtendedFeaturesRsp,
                          &remote_extended1_complete_packet);
  }

  // Queue all interrogation packets except for the remote extended complete
  // packet 2.
  void QueueIncompleteInterrogation(DeviceAddress addr,
                                    hci_spec::ConnectionHandle conn) const {
    const DynamicByteBuffer remote_name_complete_packet =
        testing::RemoteNameRequestCompletePacket(addr);
    const DynamicByteBuffer remote_version_complete_packet =
        testing::ReadRemoteVersionInfoCompletePacket(conn);
    const DynamicByteBuffer remote_supported_complete_packet =
        testing::ReadRemoteSupportedFeaturesCompletePacket(
            conn, /*extended_features=*/true);
    const DynamicByteBuffer remote_extended1_complete_packet =
        testing::ReadRemoteExtended1CompletePacket(conn);

    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::RemoteNameRequestPacket(addr),
                          &kRemoteNameRequestRsp,
                          &remote_name_complete_packet);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteVersionInfoPacket(conn),
                          &kReadRemoteVersionInfoRsp,
                          &remote_version_complete_packet);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteSupportedFeaturesPacket(conn),
                          &kReadRemoteSupportedFeaturesRsp,
                          &remote_supported_complete_packet);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteExtended1Packet(conn),
                          &kReadRemoteExtendedFeaturesRsp,
                          &remote_extended1_complete_packet);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::ReadRemoteExtended2Packet(conn),
                          &kReadRemoteExtendedFeaturesRsp);
  }

  // Completes an interrogation started with QueueIncompleteInterrogation.
  void CompleteInterrogation(hci_spec::ConnectionHandle conn) {
    const DynamicByteBuffer remote_extended2_complete_packet =
        testing::ReadRemoteExtended2CompletePacket(conn);

    test_device()->SendCommandChannelPacket(remote_extended2_complete_packet);
  }

  void QueueSuccessfulPairing(
      hci_spec::LinkKeyType key_type =
          hci_spec::LinkKeyType::kAuthenticatedCombination192) {
    EXPECT_CMD_PACKET_OUT(test_device(),
                          kAuthenticationRequested,
                          &kAuthenticationRequestedStatus,
                          &kLinkKeyRequest);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          kLinkKeyRequestNegativeReply,
                          &kLinkKeyRequestNegativeReplyRsp,
                          &kIoCapabilityRequest);
    const auto kIoCapabilityResponse = MakeIoCapabilityResponse(
        IoCapability::DISPLAY_YES_NO,
        AuthenticationRequirements::MITM_GENERAL_BONDING);
    const auto kUserConfirmationRequest = MakeUserConfirmationRequest(kPasskey);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          MakeIoCapabilityRequestReply(
                              IoCapability::DISPLAY_YES_NO,
                              AuthenticationRequirements::MITM_GENERAL_BONDING),
                          &kIoCapabilityRequestReplyRsp,
                          &kIoCapabilityResponse,
                          &kUserConfirmationRequest);
    const auto kLinkKeyNotificationWithKeyType =
        MakeLinkKeyNotification(key_type);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          kUserConfirmationRequestReply,
                          &kUserConfirmationRequestReplyRsp,
                          &kSimplePairingCompleteSuccess,
                          &kLinkKeyNotificationWithKeyType,
                          &kAuthenticationComplete);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          kSetConnectionEncryption,
                          &kSetConnectionEncryptionRsp,
                          &kEncryptionChangeEvent);
    EXPECT_CMD_PACKET_OUT(
        test_device(), kReadEncryptionKeySize, &kReadEncryptionKeySizeRsp);
  }

  // Use when pairing with no IO, where authenticated pairing is not possible.
  void QueueSuccessfulUnauthenticatedPairing(
      hci_spec::LinkKeyType key_type =
          hci_spec::LinkKeyType::kUnauthenticatedCombination192) {
    EXPECT_CMD_PACKET_OUT(test_device(),
                          kAuthenticationRequested,
                          &kAuthenticationRequestedStatus,
                          &kLinkKeyRequest);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          kLinkKeyRequestNegativeReply,
                          &kLinkKeyRequestNegativeReplyRsp,
                          &kIoCapabilityRequest);
    const auto kIoCapabilityReply = MakeIoCapabilityRequestReply(
        IoCapability::NO_INPUT_NO_OUTPUT,
        AuthenticationRequirements::GENERAL_BONDING);
    const auto kIoCapabilityResponse =
        MakeIoCapabilityResponse(IoCapability::NO_INPUT_NO_OUTPUT,
                                 AuthenticationRequirements::GENERAL_BONDING);
    const auto kUserConfirmationRequest = MakeUserConfirmationRequest(kPasskey);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          kIoCapabilityReply,
                          &kIoCapabilityRequestReplyRsp,
                          &kIoCapabilityResponse,
                          &kUserConfirmationRequest);
    const auto kLinkKeyNotificationWithKeyType =
        MakeLinkKeyNotification(key_type);
    // User Confirmation Request Reply will be automatic due to no IO.
    EXPECT_CMD_PACKET_OUT(test_device(),
                          kUserConfirmationRequestReply,
                          &kUserConfirmationRequestReplyRsp,
                          &kSimplePairingCompleteSuccess,
                          &kLinkKeyNotificationWithKeyType,
                          &kAuthenticationComplete);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          kSetConnectionEncryption,
                          &kSetConnectionEncryptionRsp,
                          &kEncryptionChangeEvent);
    EXPECT_CMD_PACKET_OUT(
        test_device(), kReadEncryptionKeySize, &kReadEncryptionKeySizeRsp);
  }

  void QueueDisconnection(
      hci_spec::ConnectionHandle conn,
      pw::bluetooth::emboss::StatusCode reason =
          pw::bluetooth::emboss::StatusCode::REMOTE_USER_TERMINATED_CONNECTION)
      const {
    const DynamicByteBuffer disconnect_complete =
        testing::DisconnectionCompletePacket(conn, reason);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::DisconnectPacket(conn, reason),
                          &kDisconnectRsp,
                          &disconnect_complete);
  }

  sm::testing::TestSecurityManagerFactory* security_manager_factory() {
    return &security_manager_factory_;
  }

 private:
  std::optional<UInt128> irk() const override { return kIrk; }
  DeviceAddress identity_address() const override { return kLocalDevLEAddr; }
  void EnsureLocalAddress(std::optional<DeviceAddress::Type>,
                          AddressCallback) override {
    ADD_FAILURE();
  }

  sm::testing::TestSecurityManagerFactory security_manager_factory_;
  std::unique_ptr<BrEdrConnectionManager> connection_manager_;
  std::unique_ptr<PeerCache> peer_cache_;
  std::unique_ptr<l2cap::testing::FakeL2cap> l2cap_;
  int transaction_count_ = 0;

  inspect::Inspector inspector_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(BrEdrConnectionManagerTest);
};

class BrEdrConnectionManagerLegacyPairingTest
    : public BrEdrConnectionManagerTest {
 public:
  void SetUp() override {
    TestingBase::SetUp();
    InitializeACLDataChannel(kBrEdrBufferInfo, kLeBufferInfo);

    peer_cache_ = std::make_unique<PeerCache>(dispatcher());
    l2cap_ = std::make_unique<l2cap::testing::FakeL2cap>(dispatcher());

    // Respond to BrEdrConnectionManager controller setup with success.
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::WritePageTimeoutPacket(static_cast<uint16_t>(
                              pw::bluetooth::emboss::PageTimeout::DEFAULT)),
                          &kWritePageTimeoutRsp);
    EXPECT_CMD_PACKET_OUT(test_device(),
                          testing::WritePinTypePacket(static_cast<uint8_t>(
                              pw::bluetooth::emboss::PinType::VARIABLE)),
                          &kWritePinTypeRsp);

    connection_manager_ = std::make_unique<BrEdrConnectionManager>(
        transport()->GetWeakPtr(),
        peer_cache_.get(),
        kLocalDevAddr,
        /*low_energy_address_delegate=*/this,
        l2cap_.get(),
        /*use_interlaced_scan=*/true,
        /*local_secure_connections_supported=*/true,
        /*legacy_pairing_enabled=*/true,
        /*controller_remote_public_key_validation_supported=*/true,
        fit::bind_member<&sm::testing::TestSecurityManagerFactory::CreateBrEdr>(
            security_manager_factory()),
        dispatcher());

    RunUntilIdle();

    test_device()->SetTransactionCallback([this] { transaction_count_++; });
  }

  void TearDown() override {
    int expected_transaction_count = transaction_count();
    if (connection_manager_ != nullptr) {
      expected_transaction_count += 2;
      // deallocating the connection manager disables connectivity.
      EXPECT_CMD_PACKET_OUT(
          test_device(), kReadScanEnable, &kReadScanEnableRspBoth);
      EXPECT_CMD_PACKET_OUT(
          test_device(), kWriteScanEnableInq, &kWriteScanEnableRsp);
      connection_manager_ = nullptr;
    }
    RunUntilIdle();
    // A disconnection may also occur for a queued disconnection, allow up to 1
    // extra transaction.
    EXPECT_LE(expected_transaction_count, transaction_count());
    EXPECT_GE(expected_transaction_count + 1, transaction_count());
    // Don't trigger the transaction callback for the rest.
    test_device()->ClearTransactionCallback();
    test_device()->Stop();
    l2cap_ = nullptr;
    peer_cache_ = nullptr;
    TestingBase::TearDown();
  }

 protected:
  BrEdrConnectionManager* connmgr() const { return connection_manager_.get(); }
  PeerCache* peer_cache() const { return peer_cache_.get(); }
  l2cap::testing::FakeL2cap* l2cap() const { return l2cap_.get(); }
  int transaction_count() const { return transaction_count_; }

 private:
  std::unique_ptr<BrEdrConnectionManager> connection_manager_;
  std::unique_ptr<PeerCache> peer_cache_;
  std::unique_ptr<l2cap::testing::FakeL2cap> l2cap_;
  int transaction_count_ = 0;
};

// Legacy pairing requires a PIN code to be displayed for the peer to enter, so
// this cannot happen when we do not have any display output capabilities.
TEST_F(BrEdrConnectionManagerLegacyPairingTest,
       NeverInitiateLegacyPairingWithoutDisplayOutputCapability) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Complete connection and interrogation successfully.
  QueueSuccessfulAccept();
  QueueSuccessfulInterrogationNoSsp(kTestDevAddr, kConnectionHandle);
  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  ASSERT_EQ(kIncomingConnTransactions, transaction_count());
  Peer* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bredr()->bonded());

  // Initiating pairing results in disconnection with peer because we have no
  // display output capabilities. Because of this, the pairing callback passed
  // into Pair() should never be called.
  QueueDisconnection(kConnectionHandle);
  connmgr()->Pair(peer->identifier(),
                  kNoSecurityRequirements,
                  [](hci::Result<>) { FAIL(); });

  RunUntilIdle();

  ASSERT_TRUE(IsNotConnected(peer));
}

// Responding to a legacy pairing request (HCI_Link_Key_Request event) after
// connection and after interrogation completes should succeed.
TEST_F(
    BrEdrConnectionManagerLegacyPairingTest,
    RespondToLegacyPairingLinkKeyRequestAfterAclConnectionAndInterrogationCompletesSucceeds) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Approve pairing requests
  pairing_delegate.SetRequestPasskeyCallback([](PeerId, auto response_cb) {
    ASSERT_TRUE(response_cb);
    response_cb(kDefaultPinCode);
  });

  // Complete connection and interrogation successfully.
  QueueSuccessfulAccept();
  QueueSuccessfulInterrogationNoSsp(kTestDevAddr, kConnectionHandle);
  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  ASSERT_EQ(kIncomingConnTransactions, transaction_count());
  Peer* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bredr()->bonded());

  EXPECT_TRUE(l2cap()->IsLinkConnected(kConnectionHandle));

  // Initiate pairing from the peer with an HCI_Link_Key_Request event
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kLinkKeyRequestNegativeReply,
                        &kLinkKeyRequestNegativeReplyRsp,
                        &kPinCodeRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kPinCodeRequestReply,
                        &kPinCodeRequestReplyRsp,
                        &kLinkKeyNotificationLegacy);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEvent);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadEncryptionKeySize, &kReadEncryptionKeySizeRsp);
  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RETURN_IF_FATAL(RunUntilIdle());

  ASSERT_TRUE(peer->bredr()->bonded());

  QueueDisconnection(kConnectionHandle);
}

// Responding to a legacy pairing request (HCI_Link_Key_Request event) before
// connection should succeed.
TEST_F(BrEdrConnectionManagerLegacyPairingTest,
       RespondToLegacyPairingLinkKeyRequestBeforeAclConnection) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  ASSERT_TRUE(
      peer_cache()->AddBondedPeer(BondingData{.identifier = PeerId(999),
                                              .address = kTestDevAddr,
                                              .name = std::nullopt,
                                              .device_class = {},
                                              .le_pairing_data = {},
                                              .bredr_link_key = kLinkKey,
                                              .bredr_services = {}}));
  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsNotConnected(peer));
  ASSERT_TRUE(peer->bonded());

  EXPECT_CMD_PACKET_OUT(test_device(), kAcceptConnectionRequest);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  test_device()->SendCommandChannelPacket(testing::RoleChangePacket(
      kTestDevAddr, pw::bluetooth::emboss::ConnectionRole::CENTRAL));
  RunUntilIdle();

  EXPECT_CMD_PACKET_OUT(
      test_device(), kLinkKeyRequestReply, &kLinkKeyRequestReplyRsp);

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);
  RunUntilIdle();

  // Complete connection and interrogation successfully.
  QueueSuccessfulInterrogation(kTestDevAddr, kConnectionHandle);
  test_device()->SendCommandChannelPacket(kConnectionComplete);
  RunUntilIdle();

  // Our pairing delegate should not have been invalidated at any point
  EXPECT_TRUE(connmgr()->pairing_delegate().is_alive());

  QueueDisconnection(kConnectionHandle);
}

// Responding to a legacy pairing request (HCI_PIN_Code_Request event) before
// the ACL connection is complete should succeed.
TEST_F(
    BrEdrConnectionManagerLegacyPairingTest,
    RespondToLegacyPairingPinCodeRequestBeforeAclConnectionCompletesSucceeds) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Approve pairing requests
  pairing_delegate.SetRequestPasskeyCallback([](PeerId, auto response_cb) {
    ASSERT_TRUE(response_cb);
    response_cb(kDefaultPinCode);
  });

  // Trigger inbound connection but don't complete the connection
  EXPECT_CMD_PACKET_OUT(
      test_device(), kAcceptConnectionRequest, &kAcceptConnectionRequestRsp);
  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(1, transaction_count());
  Peer* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bredr()->bonded());

  // Initiate pairing from the peer with an HCI_PIN_Code_Request event before
  // the connection completes
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kPinCodeRequestReply,
                        &kPinCodeRequestReplyRsp,
                        &kLinkKeyNotificationLegacy);
  test_device()->SendCommandChannelPacket(kPinCodeRequest);

  RETURN_IF_FATAL(RunUntilIdle());

  // At this point the peer is still not bonded so the host-side L2CAP should
  // still be inactive on this link (though it may be buffering packets).
  EXPECT_FALSE(l2cap()->IsLinkConnected(kConnectionHandle));

  // Complete connection and interrogation successfully.
  QueueSuccessfulInterrogationNoSsp(kTestDevAddr, kConnectionHandle);
  test_device()->SendCommandChannelPacket(kConnectionComplete);

  RETURN_IF_FATAL(RunUntilIdle());

  EXPECT_TRUE(l2cap()->IsLinkConnected(kConnectionHandle));

  // Our pairing delegate should not have been invalidated at any point
  EXPECT_TRUE(connmgr()->pairing_delegate().is_alive());

  QueueDisconnection(kConnectionHandle);
}

// Responding to a legacy pairing request (HCI_PIN_Code_Request event) after
// connection but before interrogation completes should succeed.
TEST_F(
    BrEdrConnectionManagerLegacyPairingTest,
    RespondToLegacyPairingPinCodeRequestAfterAclConnectionButBeforeInterrogationSucceeds) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Approve pairing requests
  pairing_delegate.SetRequestPasskeyCallback([](PeerId, auto response_cb) {
    ASSERT_TRUE(response_cb);
    response_cb(kDefaultPinCode);
  });

  // Trigger inbound connection and respond to some (but not all) of
  // interrogation.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAcceptConnectionRequest,
                        &kAcceptConnectionRequestRsp,
                        &kConnectionComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kRemoteNameRequest,
                        &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteVersionInfo,
                        &kReadRemoteVersionInfoRsp,
                        &kRemoteVersionInfoComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        hci_spec::kReadRemoteSupportedFeatures,
                        &kReadRemoteSupportedFeaturesRsp);
  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  // Ensure that the interrogation has begun but the peer hasn't yet bonded
  EXPECT_EQ(4, transaction_count());
  Peer* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bredr()->bonded());

  // Initiate pairing from the peer with an HCI_PIN_Code_Request event before
  // interrogation completes
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kPinCodeRequestReply,
                        &kPinCodeRequestReplyRsp,
                        &kLinkKeyNotificationLegacy);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEvent);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadEncryptionKeySize, &kReadEncryptionKeySizeRsp);
  test_device()->SendCommandChannelPacket(kPinCodeRequest);

  RETURN_IF_FATAL(RunUntilIdle());

  // At this point the peer is bonded and the link is encrypted but
  // interrogation has not completed so host-side L2CAP should still be inactive
  // on this link (though it may be buffering packets).
  EXPECT_FALSE(l2cap()->IsLinkConnected(kConnectionHandle));

  bool socket_cb_called = false;
  auto socket_fails_cb = [&socket_cb_called](const auto& chan_sock) {
    EXPECT_FALSE(chan_sock.is_alive());
    socket_cb_called = true;
  };
  connmgr()->OpenL2capChannel(peer->identifier(),
                              l2cap::kAVDTP,
                              kNoSecurityRequirements,
                              kChannelParams,
                              socket_fails_cb);

  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_TRUE(socket_cb_called);

  // Complete interrogation successfully.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended1,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1CompleteNoSsp);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended2,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1CompleteNoSsp);
  test_device()->SendCommandChannelPacket(kReadRemoteSupportedFeaturesComplete);

  RETURN_IF_FATAL(RunUntilIdle());

  EXPECT_TRUE(l2cap()->IsLinkConnected(kConnectionHandle));

  QueueDisconnection(kConnectionHandle);
}

// Responding to a legacy pairing request (HCI_Link_Key_Request event) after
// the ACL connection is complete but before interrogation completes stops
// pairing because we assume SSP.
TEST_F(
    BrEdrConnectionManagerLegacyPairingTest,
    RespondToLegacyPairingLinkKeyRequestAfterAclConnectionButBeforeInterrogationFails) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Trigger inbound connection and respond to some (but not all) of
  // interrogation.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAcceptConnectionRequest,
                        &kAcceptConnectionRequestRsp,
                        &kConnectionComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kRemoteNameRequest,
                        &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteVersionInfo,
                        &kReadRemoteVersionInfoRsp,
                        &kRemoteVersionInfoComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        hci_spec::kReadRemoteSupportedFeatures,
                        &kReadRemoteSupportedFeaturesRsp);
  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  // Ensure that the interrogation has begun but the peer hasn't yet bonded
  EXPECT_EQ(4, transaction_count());
  Peer* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bredr()->bonded());

  // Initiate pairing from the peer with an HCI_Link_Key_Request event before
  // interrogation completes
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kLinkKeyRequestNegativeReply,
                        &kLinkKeyRequestNegativeReplyRsp,
                        &kPinCodeRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kPinCodeRequestNegativeReply,
                        &kPinCodeRequestNegativeReplyRsp);
  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RETURN_IF_FATAL(RunUntilIdle());

  // At this point the peer is bonded and the link is encrypted but
  // interrogation has not completed so host-side L2CAP should still be inactive
  // on this link (though it may be buffering packets).
  EXPECT_FALSE(l2cap()->IsLinkConnected(kConnectionHandle));

  QueueDisconnection(kConnectionHandle);
}

// Responding to an SSP request (HCI_Link_Key_Request event) after the ACL
// connection is complete but before interrogation completes should succeed.
TEST_F(
    BrEdrConnectionManagerLegacyPairingTest,
    RespondToSspLinkKeyRequestAfterAclConnectionButBeforeInterrogationSucceeds) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Approve pairing requests.
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) {
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  // Trigger inbound connection and respond to some (but not all) of
  // interrogation.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAcceptConnectionRequest,
                        &kAcceptConnectionRequestRsp,
                        &kConnectionComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kRemoteNameRequest,
                        &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteVersionInfo,
                        &kReadRemoteVersionInfoRsp,
                        &kRemoteVersionInfoComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        hci_spec::kReadRemoteSupportedFeatures,
                        &kReadRemoteSupportedFeaturesRsp);
  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  // Ensure that the interrogation has begun but the peer hasn't yet bonded
  EXPECT_EQ(4, transaction_count());
  Peer* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bredr()->bonded());

  // Initiate pairing from the peer with an HCI_Link_Key_Request event before
  // interrogation completes
  const auto kIoCapabilityResponse = MakeIoCapabilityResponse(
      IoCapability::DISPLAY_YES_NO,
      AuthenticationRequirements::MITM_GENERAL_BONDING);
  const auto kUserConfirmationRequest = MakeUserConfirmationRequest(kPasskey);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kLinkKeyRequestNegativeReply,
                        &kLinkKeyRequestNegativeReplyRsp,
                        &kIoCapabilityResponse,
                        &kIoCapabilityRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::DISPLAY_YES_NO,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp,
                        &kUserConfirmationRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kUserConfirmationRequestReply,
                        &kUserConfirmationRequestReplyRsp,
                        &kSimplePairingCompleteSuccess,
                        &kLinkKeyNotification);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEvent);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadEncryptionKeySize, &kReadEncryptionKeySizeRsp);
  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RETURN_IF_FATAL(RunUntilIdle());

  // At this point the peer is bonded and the link is encrypted but
  // interrogation has not completed so host-side L2CAP should still be inactive
  // on this link (though it may be buffering packets).
  EXPECT_FALSE(l2cap()->IsLinkConnected(kConnectionHandle));

  bool socket_cb_called = false;
  auto socket_fails_cb = [&socket_cb_called](const auto& chan_sock) {
    EXPECT_FALSE(chan_sock.is_alive());
    socket_cb_called = true;
  };
  connmgr()->OpenL2capChannel(peer->identifier(),
                              l2cap::kAVDTP,
                              kNoSecurityRequirements,
                              kChannelParams,
                              socket_fails_cb);

  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_TRUE(socket_cb_called);

  // Complete interrogation successfully.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended1,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1CompleteNoSsp);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended2,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1CompleteNoSsp);
  test_device()->SendCommandChannelPacket(kReadRemoteSupportedFeaturesComplete);

  RETURN_IF_FATAL(RunUntilIdle());

  EXPECT_TRUE(l2cap()->IsLinkConnected(kConnectionHandle));

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest, DisableConnectivity) {
  size_t cb_count = 0;
  auto cb = [&cb_count](const auto& status) {
    cb_count++;
    EXPECT_EQ(fit::ok(), status);
  };

  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadScanEnable, &kReadScanEnableRspPage);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kWriteScanEnableNone, &kWriteScanEnableRsp);

  connmgr()->SetConnectable(/*connectable=*/false, cb);

  RunUntilIdle();

  EXPECT_EQ(1u, cb_count);

  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadScanEnable, &kReadScanEnableRspBoth);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kWriteScanEnableInq, &kWriteScanEnableRsp);

  connmgr()->SetConnectable(/*connectable=*/false, cb);

  RunUntilIdle();

  EXPECT_EQ(2u, cb_count);
}

TEST_F(BrEdrConnectionManagerTest, EnableConnectivity) {
  size_t cb_count = 0;
  auto cb = [&cb_count](const auto& status) {
    cb_count++;
    EXPECT_EQ(fit::ok(), status);
  };

  EXPECT_CMD_PACKET_OUT(
      test_device(), kWritePageScanActivity, &kWritePageScanActivityRsp);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kWritePageScanType, &kWritePageScanTypeRsp);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadScanEnable, &kReadScanEnableRspNone);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kWriteScanEnablePage, &kWriteScanEnableRsp);

  connmgr()->SetConnectable(/*connectable=*/true, cb);

  RunUntilIdle();

  EXPECT_EQ(1u, cb_count);

  EXPECT_CMD_PACKET_OUT(
      test_device(), kWritePageScanActivity, &kWritePageScanActivityRsp);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kWritePageScanType, &kWritePageScanTypeRsp);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadScanEnable, &kReadScanEnableRspInquiry);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kWriteScanEnableBoth, &kWriteScanEnableRsp);

  connmgr()->SetConnectable(/*connectable=*/true, cb);

  RunUntilIdle();

  EXPECT_EQ(2u, cb_count);
}

// Test: An incoming connection request should trigger an acceptance and
// interrogation should allow a peer that only report the first Extended
// Features page.
TEST_F(BrEdrConnectionManagerTest,
       IncomingConnection_BrokenExtendedPageResponse) {
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAcceptConnectionRequest,
                        &kAcceptConnectionRequestRsp,
                        &kConnectionComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kRemoteNameRequest,
                        &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteVersionInfo,
                        &kReadRemoteVersionInfoRsp,
                        &kRemoteVersionInfoComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        hci_spec::kReadRemoteSupportedFeatures,
                        &kReadRemoteSupportedFeaturesRsp,
                        &kReadRemoteSupportedFeaturesComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended1,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1Complete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended2,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1Complete);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(6, transaction_count());

  // When we deallocate the connection manager during teardown, we should
  // disconnect.
  QueueDisconnection(kConnectionHandle);
}

// Test: An incoming connection request should trigger an acceptance and an
// interrogation to discover capabilities.
TEST_F(BrEdrConnectionManagerTest, IncomingConnectionSuccess) {
  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));

  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->identifier(), connmgr()->GetPeerId(kConnectionHandle));
  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  // Confirm remote name request during interrogation sets proper name source.
  EXPECT_EQ(peer->name_source(), Peer::NameSource::kNameDiscoveryProcedure);
  // We should have set the Class Of Device from the incoming connection request
  ASSERT_TRUE(peer->bredr()->device_class().has_value());
  // endianness so this magic number is backwards from the packet definition
  EXPECT_EQ(*peer->bredr()->device_class(), DeviceClass(0x000C425A));

  // When we deallocate the connection manager during teardown, we should
  // disconnect.
  QueueDisconnection(kConnectionHandle);
}

// Test: An incoming connection request should upgrade a known LE peer with a
// matching address to a dual mode peer.
TEST_F(BrEdrConnectionManagerTest,
       IncomingConnectionUpgradesKnownLowEnergyPeerToDualMode) {
  const DeviceAddress le_alias_addr(DeviceAddress::Type::kLEPublic,
                                    kTestDevAddr.value());
  Peer* const peer = peer_cache()->NewPeer(le_alias_addr, /*connectable=*/true);
  ASSERT_TRUE(peer);
  ASSERT_EQ(TechnologyType::kLowEnergy, peer->technology());

  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  ASSERT_EQ(peer, peer_cache()->FindByAddress(kTestDevAddr));
  EXPECT_EQ(peer->identifier(), connmgr()->GetPeerId(kConnectionHandle));
  EXPECT_EQ(TechnologyType::kDualMode, peer->technology());

  // Prepare for disconnection upon teardown.
  QueueDisconnection(kConnectionHandle);
}

// Test: A remote disconnect should correctly remove the connection.
TEST_F(BrEdrConnectionManagerTest, RemoteDisconnect) {
  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->identifier(), connmgr()->GetPeerId(kConnectionHandle));

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  // Remote end disconnects.
  test_device()->SendCommandChannelPacket(kDisconnectionComplete);

  RunUntilIdle();

  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));
}

const auto kRemoteNameRequestCompleteFailed =
    StaticByteBuffer(hci_spec::kRemoteNameRequestCompleteEventCode,
                     0x01,  // parameter_total_size (1 bytes)
                     pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE);

const auto kReadRemoteSupportedFeaturesCompleteFailed =
    StaticByteBuffer(hci_spec::kReadRemoteSupportedFeaturesCompleteEventCode,
                     0x01,  // parameter_total_size (1 bytes)
                     pw::bluetooth::emboss::StatusCode::HARDWARE_FAILURE);

// Test: if the interrogation fails, we disconnect.
//  - Receiving extra responses after a command fails will not fail
//  - We don't query extended features if we don't receive an answer.
TEST_F(BrEdrConnectionManagerTest, IncomingConnectionFailedInterrogation) {
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAcceptConnectionRequest,
                        &kAcceptConnectionRequestRsp,
                        &kConnectionComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kRemoteNameRequest,
                        &kRemoteNameRequestRsp,
                        &kRemoteNameRequestCompleteFailed);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteVersionInfo,
                        &kReadRemoteVersionInfoRsp,
                        &kRemoteVersionInfoComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        hci_spec::kReadRemoteSupportedFeatures,
                        &kReadRemoteSupportedFeaturesRsp,
                        &kReadRemoteSupportedFeaturesCompleteFailed);

  EXPECT_CMD_PACKET_OUT(
      test_device(), kDisconnect, &kDisconnectRsp, &kDisconnectionComplete);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(5, transaction_count());
}

// Test: replies negative to IO Capability Requests before PairingDelegate is
// set
TEST_F(BrEdrConnectionManagerTest,
       IoCapabilityRequestNegativeReplyWithNoPairingDelegate) {
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kIoCapabilityRequestNegativeReply,
                        &kIoCapabilityRequestNegativeReplyRsp);

  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);

  RunUntilIdle();

  EXPECT_EQ(1, transaction_count());
}

// Test: replies negative to IO Capability Requests for unconnected peers
TEST_F(BrEdrConnectionManagerTest,
       IoCapabilityRequestNegativeReplyWhenNotConnected) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kIoCapabilityRequestNegativeReply,
                        &kIoCapabilityRequestNegativeReplyRsp);

  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);

  RunUntilIdle();

  EXPECT_EQ(1, transaction_count());
}

// Test: replies to IO Capability Requests for connected peers
TEST_F(BrEdrConnectionManagerTest, IoCapabilityRequestReplyWhenConnected) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kNoInputNoOutput);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  ASSERT_EQ(kIncomingConnTransactions, transaction_count());

  EXPECT_CMD_PACKET_OUT(
      test_device(),
      MakeIoCapabilityRequestReply(IoCapability::NO_INPUT_NO_OUTPUT,
                                   AuthenticationRequirements::GENERAL_BONDING),
      &kIoCapabilityRequestReplyRsp);

  test_device()->SendCommandChannelPacket(MakeIoCapabilityResponse(
      IoCapability::DISPLAY_ONLY,
      AuthenticationRequirements::MITM_GENERAL_BONDING));
  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());

  QueueDisconnection(kConnectionHandle);
}

// Test: Responds to Secure Simple Pairing with user rejection of Numeric
// Comparison association
TEST_F(BrEdrConnectionManagerTest,
       RespondToNumericComparisonPairingAfterUserRejects) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::DISPLAY_YES_NO,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp);

  test_device()->SendCommandChannelPacket(MakeIoCapabilityResponse(
      IoCapability::DISPLAY_ONLY, AuthenticationRequirements::GENERAL_BONDING));
  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);

  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t passkey, auto method, auto confirm_cb) {
        EXPECT_EQ(kPasskey, passkey);
        EXPECT_EQ(PairingDelegate::DisplayMethod::kComparison, method);
        ASSERT_TRUE(confirm_cb);
        confirm_cb(false);
      });

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kUserConfirmationRequestNegativeReply,
                        &kUserConfirmationRequestNegativeReplyRsp);
  test_device()->SendCommandChannelPacket(
      MakeUserConfirmationRequest(kPasskey));

  pairing_delegate.SetCompletePairingCallback([](PeerId, sm::Result<> status) {
    EXPECT_EQ(ToResult(HostError::kFailed), status);
  });

  test_device()->SendCommandChannelPacket(kSimplePairingCompleteError);

  // We disconnect the peer when authentication fails.
  QueueDisconnection(kConnectionHandle);

  RunUntilIdle();
}

const auto kUserPasskeyRequest =
    testing::UserPasskeyRequestPacket(TEST_DEV_ADDR);

const auto kUserPasskeyRequestNegativeReply =
    testing::UserPasskeyRequestNegativeReply(TEST_DEV_ADDR);

const auto kUserPasskeyRequestNegativeReplyRsp =
    testing::UserPasskeyRequestNegativeReplyResponse(TEST_DEV_ADDR);

// Test: Responds to Secure Simple Pairing as the input side of Passkey Entry
// association after the user declines or provides invalid input
TEST_F(BrEdrConnectionManagerTest,
       RespondToPasskeyEntryPairingAfterUserProvidesInvalidPasskey) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  FakePairingDelegate pairing_delegate(sm::IOCapability::kKeyboardOnly);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::KEYBOARD_ONLY,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp);

  test_device()->SendCommandChannelPacket(MakeIoCapabilityResponse(
      IoCapability::DISPLAY_ONLY, AuthenticationRequirements::GENERAL_BONDING));
  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);

  pairing_delegate.SetRequestPasskeyCallback([](PeerId, auto response_cb) {
    ASSERT_TRUE(response_cb);
    response_cb(-128);  // Negative values indicate rejection.
  });

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kUserPasskeyRequestNegativeReply,
                        &kUserPasskeyRequestNegativeReplyRsp);
  test_device()->SendCommandChannelPacket(kUserPasskeyRequest);

  pairing_delegate.SetCompletePairingCallback([](PeerId, sm::Result<> status) {
    EXPECT_EQ(ToResult(HostError::kFailed), status);
  });

  test_device()->SendCommandChannelPacket(kSimplePairingCompleteError);

  // We disconnect the peer when authentication fails.
  QueueDisconnection(kConnectionHandle);

  RunUntilIdle();
}

// Test: replies negative to Link Key Requests for unknown and unbonded peers
TEST_F(BrEdrConnectionManagerTest, LinkKeyRequestAndNegativeReply) {
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kLinkKeyRequestNegativeReply,
                        &kLinkKeyRequestNegativeReplyRsp);

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunUntilIdle();

  EXPECT_EQ(1, transaction_count());

  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_FALSE(IsNotConnected(peer));
  ASSERT_FALSE(peer->bonded());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kLinkKeyRequestNegativeReply,
                        &kLinkKeyRequestNegativeReplyRsp);

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 2, transaction_count());

  // Queue disconnection for teardown
  QueueDisconnection(kConnectionHandle);
}

// Test: Link Key request arrives after we have received a connect request, but
// before we have been notified of connect completion (see http://b/393629914).
TEST_F(BrEdrConnectionManagerTest, ConnectLinkKeySandwich) {
  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::AcceptConnectionRequestPacket(kTestDevAddr),
                        &kAcceptConnectionRequestRsp);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_FALSE(IsNotConnected(peer));  // connecting
  ASSERT_FALSE(peer->bonded());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kLinkKeyRequestNegativeReply,
                        &kLinkKeyRequestNegativeReplyRsp);
  test_device()->SendCommandChannelPacket(kLinkKeyRequest);
  RunUntilIdle();

  QueueSuccessfulInterrogation(kTestDevAddr, kConnectionHandle);
  test_device()->SendCommandChannelPacket(kConnectionComplete);
  RunUntilIdle();

  // Queue disconnection for teardown
  QueueDisconnection(kConnectionHandle);
}

// Test: replies to Link Key Requests for bonded peer
TEST_F(BrEdrConnectionManagerTest, RecallLinkKeyForBondedPeer) {
  ASSERT_TRUE(
      peer_cache()->AddBondedPeer(BondingData{.identifier = PeerId(999),
                                              .address = kTestDevAddr,
                                              .name = std::nullopt,
                                              .device_class = {},
                                              .le_pairing_data = {},
                                              .bredr_link_key = kLinkKey,
                                              .bredr_services = {}}));
  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsNotConnected(peer));
  ASSERT_TRUE(peer->bonded());

  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  ASSERT_TRUE(IsInitializing(peer));

  EXPECT_CMD_PACKET_OUT(
      test_device(), kLinkKeyRequestReply, &kLinkKeyRequestReplyRsp);

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunUntilIdle();
  /// Peer is still initializing until the Pairing is complete
  /// (OnPairingComplete)
  ASSERT_TRUE(IsInitializing(peer));

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());

  // Queue disconnection for teardown.
  QueueDisconnection(kConnectionHandle);
}

// Test: Responds to Secure Simple Pairing as the input side of Passkey Entry
// association after the user provides the correct passkey
TEST_F(BrEdrConnectionManagerTest,
       EncryptAfterPasskeyEntryPairingAndUserProvidesAcceptedPasskey) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bonded());

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  FakePairingDelegate pairing_delegate(sm::IOCapability::kKeyboardOnly);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::KEYBOARD_ONLY,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp);

  test_device()->SendCommandChannelPacket(MakeIoCapabilityResponse(
      IoCapability::DISPLAY_ONLY, AuthenticationRequirements::GENERAL_BONDING));
  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);

  pairing_delegate.SetRequestPasskeyCallback([](PeerId, auto response_cb) {
    ASSERT_TRUE(response_cb);
    response_cb(kPasskey);
  });

  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeUserPasskeyRequestReply(),
                        &kUserPasskeyRequestReplyRsp);
  test_device()->SendCommandChannelPacket(kUserPasskeyRequest);

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  test_device()->SendCommandChannelPacket(kSimplePairingCompleteSuccess);
  test_device()->SendCommandChannelPacket(kLinkKeyNotification);

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEvent);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadEncryptionKeySize, &kReadEncryptionKeySizeRsp);

  RETURN_IF_FATAL(RunUntilIdle());
  ASSERT_TRUE(IsConnected(peer));
  EXPECT_TRUE(peer->bonded());

  QueueDisconnection(kConnectionHandle);
}

// Test: Responds to Secure Simple Pairing as the display side of Passkey Entry
// association after the user provides the correct passkey on the peer
TEST_F(BrEdrConnectionManagerTest, EncryptAfterPasskeyDisplayPairing) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bonded());

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayOnly);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::DISPLAY_ONLY,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp);

  test_device()->SendCommandChannelPacket(
      MakeIoCapabilityResponse(IoCapability::KEYBOARD_ONLY,
                               AuthenticationRequirements::GENERAL_BONDING));
  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);

  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t passkey, auto method, auto confirm_cb) {
        EXPECT_EQ(kPasskey, passkey);
        EXPECT_EQ(PairingDelegate::DisplayMethod::kPeerEntry, method);
        EXPECT_TRUE(confirm_cb);
      });

  test_device()->SendCommandChannelPacket(
      MakeUserPasskeyNotification(kPasskey));

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  RETURN_IF_FATAL(RunUntilIdle());
  ASSERT_TRUE(IsInitializing(peer));

  test_device()->SendCommandChannelPacket(kSimplePairingCompleteSuccess);
  test_device()->SendCommandChannelPacket(kLinkKeyNotification);

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEvent);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadEncryptionKeySize, &kReadEncryptionKeySizeRsp);

  RETURN_IF_FATAL(RunUntilIdle());
  ASSERT_TRUE(IsConnected(peer));
  EXPECT_TRUE(peer->bonded());

  QueueDisconnection(kConnectionHandle);
}

// Test: Responds to Secure Simple Pairing and user confirmation of Numeric
// Comparison association, then bonds and encrypts using resulting link key
TEST_F(BrEdrConnectionManagerTest,
       EncryptAndBondAfterNumericComparisonPairingAndUserConfirms) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bonded());

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::DISPLAY_YES_NO,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp);

  test_device()->SendCommandChannelPacket(
      MakeIoCapabilityResponse(IoCapability::DISPLAY_YES_NO,
                               AuthenticationRequirements::GENERAL_BONDING));
  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);

  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t passkey, auto method, auto confirm_cb) {
        EXPECT_EQ(kPasskey, passkey);
        EXPECT_EQ(PairingDelegate::DisplayMethod::kComparison, method);
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kUserConfirmationRequestReply,
                        &kUserConfirmationRequestReplyRsp);
  test_device()->SendCommandChannelPacket(
      MakeUserConfirmationRequest(kPasskey));

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  RETURN_IF_FATAL(RunUntilIdle());
  ASSERT_TRUE(IsInitializing(peer));

  test_device()->SendCommandChannelPacket(kSimplePairingCompleteSuccess);
  test_device()->SendCommandChannelPacket(kLinkKeyNotification);

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEvent);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadEncryptionKeySize, &kReadEncryptionKeySizeRsp);

  RETURN_IF_FATAL(RunUntilIdle());
  ASSERT_TRUE(IsConnected(peer));
  EXPECT_TRUE(peer->bonded());

  EXPECT_CMD_PACKET_OUT(
      test_device(), kLinkKeyRequestReply, &kLinkKeyRequestReplyRsp);
  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunUntilIdle();

  QueueDisconnection(kConnectionHandle);
}

// Test: can't change the link key of an unbonded peer
TEST_F(BrEdrConnectionManagerTest, UnbondedPeerChangeLinkKey) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bonded());

  // Change the link key.
  test_device()->SendCommandChannelPacket(kLinkKeyNotificationChanged);

  RunUntilIdle();
  ASSERT_FALSE(IsConnected(peer));
  EXPECT_FALSE(peer->bonded());

  EXPECT_CMD_PACKET_OUT(
      test_device(), kLinkKeyRequestNegativeReply, &kLinkKeyRequestReplyRsp);

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  RunUntilIdle();

  ASSERT_FALSE(IsConnected(peer));
  EXPECT_FALSE(peer->bonded());
  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());

  QueueDisconnection(kConnectionHandle);
}

// Test: if L2CAP gets a link error, we disconnect the connection
TEST_F(BrEdrConnectionManagerTest, DisconnectOnLinkError) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  // When we deallocate the connection manager next, we should disconnect.
  QueueDisconnection(kConnectionHandle);

  l2cap()->TriggerLinkError(kConnectionHandle);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());
}

TEST_F(BrEdrConnectionManagerTest, InitializingPeerDoesNotTimeout) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_FALSE(IsNotConnected(peer));
  EXPECT_FALSE(peer->bonded());

  // We want to make sure the connection doesn't expire just because they didn't
  // pair.
  RunFor(std::chrono::seconds(600));

  auto* peer_still = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer_still);
  ASSERT_EQ(peer->identifier(), peer_still->identifier());

  // Remote end disconnects.
  test_device()->SendCommandChannelPacket(kDisconnectionComplete);

  RunUntilIdle();

  // Peer should still be there, but not connected anymore, until they time out.
  peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_TRUE(IsNotConnected(peer));
  EXPECT_FALSE(peer->bonded());
  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));
}

inline uint16_t tid_from_sdp_packet(const ByteBufferPtr& packet) {
  return (*packet)[1] << CHAR_BIT | (*packet)[2];
}

TEST_F(BrEdrConnectionManagerTest,
       PeerServicesAddedBySearchAndRetainedIfNotSearchedFor) {
  constexpr UUID kServiceUuid1 = sdp::profile::kAudioSink;
  auto* const peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  peer->MutBrEdr().AddService(kServiceUuid1);

  // Search for different service.
  constexpr UUID kServiceUuid2 = sdp::profile::kAudioSource;
  size_t search_cb_count = 0;
  connmgr()->AddServiceSearch(kServiceUuid2,
                              {sdp::kServiceId},
                              [&](auto, auto&) { search_cb_count++; });

  l2cap::testing::FakeChannel::WeakPtr sdp_chan;
  std::optional<uint32_t> sdp_request_tid;

  l2cap()->set_channel_callback(
      [this, &sdp_chan, &sdp_request_tid](auto new_chan) {
        new_chan->SetSendCallback(
            [&sdp_request_tid](auto packet) {
              sdp_request_tid = tid_from_sdp_packet(packet);
            },
            dispatcher());
        sdp_chan = std::move(new_chan);
      });

  // No searches in this connection.
  QueueSuccessfulIncomingConn();
  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kSDP, 0x40, 0x41, kChannelParams);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  ASSERT_TRUE(sdp_chan.is_alive());
  ASSERT_EQ(0u, search_cb_count);

  // Positive response to search.
  sdp::ServiceSearchAttributeResponse rsp;
  rsp.SetAttribute(0, sdp::kServiceId, sdp::DataElement(UUID()));
  auto rsp_ptr = rsp.GetPDU(0xFFFF /* max attribute bytes */,
                            *sdp_request_tid,
                            PDU_MAX,
                            BufferView());

  sdp_chan->Receive(*rsp_ptr);

  RunUntilIdle();

  ASSERT_EQ(1u, search_cb_count);

  // Prior connections' services retained and newly discovered service added.
  EXPECT_EQ(1u, peer->bredr()->services().count(kServiceUuid1));
  EXPECT_EQ(1u, peer->bredr()->services().count(kServiceUuid2));

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest,
       PeerServiceNotErasedByEmptyResultsForSearchOfSameService) {
  constexpr UUID kServiceUuid = sdp::profile::kAudioSink;
  auto* const peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  peer->MutBrEdr().AddService(kServiceUuid);

  size_t search_cb_count = 0;
  connmgr()->AddServiceSearch(
      kServiceUuid, {sdp::kServiceId}, [&](auto, auto&) { search_cb_count++; });

  l2cap::testing::FakeChannel::WeakPtr sdp_chan;
  std::optional<uint32_t> sdp_request_tid;

  l2cap()->set_channel_callback(
      [this, &sdp_chan, &sdp_request_tid](auto new_chan) {
        new_chan->SetSendCallback(
            [&sdp_request_tid](auto packet) {
              sdp_request_tid = tid_from_sdp_packet(packet);
            },
            dispatcher());
        sdp_chan = std::move(new_chan);
      });

  QueueSuccessfulIncomingConn();
  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kSDP, 0x40, 0x41, kChannelParams);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  ASSERT_TRUE(sdp_chan.is_alive());
  ASSERT_EQ(0u, search_cb_count);

  sdp::ServiceSearchAttributeResponse empty_rsp;
  auto rsp_ptr = empty_rsp.GetPDU(0xFFFF /* max attribute bytes */,
                                  *sdp_request_tid,
                                  PDU_MAX,
                                  BufferView());

  sdp_chan->Receive(*rsp_ptr);

  RunUntilIdle();

  // Search callback isn't called by empty attribute list from peer.
  ASSERT_EQ(0u, search_cb_count);

  EXPECT_EQ(1u, peer->bredr()->services().count(kServiceUuid));

  QueueDisconnection(kConnectionHandle);
}

l2cap::testing::FakeChannel::SendCallback MakeAudioSinkSearchExpected(
    std::optional<uint16_t>* tid) {
  return [tid](auto packet) {
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
        0x03,  // uint16_t (kServiceId)
        0x00   // No continuation state
    );
    // First byte should be type.
    ASSERT_LE(3u, packet->size());
    ASSERT_EQ(sdp::kServiceSearchAttributeRequest, (*packet)[0]);
    ASSERT_EQ(kSearchExpectedParams, packet->view(sizeof(bt::sdp::Header)));
    if (tid != nullptr) {
    }
    *tid = tid_from_sdp_packet(packet);
  };
}

TEST_F(BrEdrConnectionManagerTest, ServiceSearch) {
  size_t search_cb_count = 0;
  auto search_cb = [&](auto id, const auto& attributes) {
    auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
    ASSERT_TRUE(peer);
    ASSERT_EQ(id, peer->identifier());
    ASSERT_EQ(1u, attributes.count(sdp::kServiceId));
    search_cb_count++;
  };

  auto search_id = connmgr()->AddServiceSearch(
      sdp::profile::kAudioSink, {sdp::kServiceId}, search_cb);

  l2cap::testing::FakeChannel::WeakPtr sdp_chan;
  std::optional<uint16_t> sdp_request_tid;

  l2cap()->set_channel_callback(
      [&sdp_chan, &sdp_request_tid, this](auto new_chan) {
        new_chan->SetSendCallback(MakeAudioSinkSearchExpected(&sdp_request_tid),
                                  dispatcher());
        sdp_chan = std::move(new_chan);
      });

  QueueSuccessfulIncomingConn();
  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kSDP, 0x40, 0x41, kChannelParams);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  ASSERT_TRUE(sdp_chan.is_alive());
  ASSERT_TRUE(sdp_request_tid);
  ASSERT_EQ(0u, search_cb_count);

  sdp::ServiceSearchAttributeResponse rsp;
  rsp.SetAttribute(0, sdp::kServiceId, sdp::DataElement(UUID()));
  auto rsp_ptr = rsp.GetPDU(0xFFFF /* max attribute bytes */,
                            *sdp_request_tid,
                            PDU_MAX,
                            BufferView());

  sdp_chan->Receive(*rsp_ptr);

  RunUntilIdle();

  ASSERT_EQ(1u, search_cb_count);

  // Remote end disconnects.
  test_device()->SendCommandChannelPacket(kDisconnectionComplete);

  RunUntilIdle();

  sdp_request_tid.reset();

  EXPECT_TRUE(connmgr()->RemoveServiceSearch(search_id));
  EXPECT_FALSE(connmgr()->RemoveServiceSearch(search_id));

  // Second connection is shortened because we have already interrogated,
  // and we don't search for SDP services because none are registered
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAcceptConnectionRequest,
                        &kAcceptConnectionRequestRsp,
                        &kConnectionComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended1,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1Complete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended2,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended2Complete);

  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  // We shouldn't have searched for anything.
  ASSERT_FALSE(sdp_request_tid);
  ASSERT_EQ(1u, search_cb_count);

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest, SearchAfterConnected) {
  // We have no services registered, so this will not start a SDP search.
  QueueSuccessfulIncomingConn();
  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  size_t search_cb_count = 0;
  auto search_cb = [&](auto id, const auto& attributes) {
    auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
    ASSERT_TRUE(peer);
    ASSERT_EQ(id, peer->identifier());
    ASSERT_EQ(1u, attributes.count(sdp::kServiceId));
    search_cb_count++;
  };

  l2cap::testing::FakeChannel::WeakPtr sdp_chan;
  std::optional<uint16_t> sdp_request_tid;

  l2cap()->set_channel_callback(
      [&sdp_chan, &sdp_request_tid, this](auto new_chan) {
        new_chan->SetSendCallback(MakeAudioSinkSearchExpected(&sdp_request_tid),
                                  dispatcher());
        sdp_chan = std::move(new_chan);
      });

  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kSDP, 0x40, 0x41, kChannelParams);

  // When this gets added, the service search will immediately be done on the
  // already-connected peer.
  auto search_id = connmgr()->AddServiceSearch(
      sdp::profile::kAudioSink, {sdp::kServiceId}, search_cb);

  ASSERT_NE(sdp::ServiceDiscoverer::kInvalidSearchId, search_id);

  RunUntilIdle();

  ASSERT_TRUE(sdp_chan.is_alive());
  ASSERT_TRUE(sdp_request_tid);
  ASSERT_EQ(0u, search_cb_count);

  sdp::ServiceSearchAttributeResponse rsp;
  rsp.SetAttribute(0, sdp::kServiceId, sdp::DataElement(UUID()));
  auto rsp_ptr = rsp.GetPDU(0xFFFF /* max attribute bytes */,
                            *sdp_request_tid,
                            PDU_MAX,
                            BufferView());

  sdp_chan->Receive(*rsp_ptr);

  RunUntilIdle();

  ASSERT_EQ(1u, search_cb_count);

  // Remote end disconnects.
  test_device()->SendCommandChannelPacket(kDisconnectionComplete);

  RunUntilIdle();

  sdp_request_tid.reset();
  sdp_chan.reset();

  // Second connection is shortened because we have already interrogated,
  // we repeat the search for SDP services.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAcceptConnectionRequest,
                        &kAcceptConnectionRequestRsp,
                        &kConnectionComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended1,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1Complete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended2,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended2Complete);

  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kSDP, 0x40, 0x41, kChannelParams);

  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  ASSERT_TRUE(sdp_chan.is_alive());
  ASSERT_TRUE(sdp_request_tid);
  ASSERT_EQ(1u, search_cb_count);

  // Reusing the (empty) answer from before
  rsp_ptr = rsp.GetPDU(0xFFFF /* max attribute bytes */,
                       *sdp_request_tid,
                       PDU_MAX,
                       BufferView());

  sdp_chan->Receive(*rsp_ptr);

  // We should have another search callback.
  ASSERT_EQ(2u, search_cb_count);

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerLegacyPairingTest, SearchOnReconnect) {
  size_t search_cb_count = 0;
  auto search_cb = [&](auto id, const auto& attributes) {
    auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
    ASSERT_TRUE(peer);
    ASSERT_EQ(id, peer->identifier());
    ASSERT_EQ(1u, attributes.count(sdp::kServiceId));
    search_cb_count++;
  };

  connmgr()->AddServiceSearch(
      sdp::profile::kAudioSink, {sdp::kServiceId}, search_cb);

  l2cap::testing::FakeChannel::WeakPtr sdp_chan;
  std::optional<uint16_t> sdp_request_tid;

  l2cap()->set_channel_callback(
      [&sdp_chan, &sdp_request_tid, this](auto new_chan) {
        new_chan->SetSendCallback(MakeAudioSinkSearchExpected(&sdp_request_tid),
                                  dispatcher());
        sdp_chan = std::move(new_chan);
      });

  // This test uses a modified peer and interrogation which doesn't use extended
  // pages.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAcceptConnectionRequest,
                        &kAcceptConnectionRequestRsp,
                        &kConnectionComplete);
  const DynamicByteBuffer remote_name_complete_packet =
      testing::RemoteNameRequestCompletePacket(kTestDevAddr);
  const DynamicByteBuffer remote_version_complete_packet =
      testing::ReadRemoteVersionInfoCompletePacket(kConnectionHandle);
  const DynamicByteBuffer remote_supported_complete_packet =
      testing::ReadRemoteSupportedFeaturesCompletePacket(
          kConnectionHandle,
          /*extended_features=*/false);

  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::RemoteNameRequestPacket(kTestDevAddr),
                        &kRemoteNameRequestRsp,
                        &remote_name_complete_packet);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::ReadRemoteVersionInfoPacket(kConnectionHandle),
                        &kReadRemoteVersionInfoRsp,
                        &remote_version_complete_packet);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      testing::ReadRemoteSupportedFeaturesPacket(kConnectionHandle),
      &kReadRemoteSupportedFeaturesRsp,
      &remote_supported_complete_packet);

  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kSDP, 0x40, 0x41, kChannelParams);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  ASSERT_TRUE(sdp_chan.is_alive());
  ASSERT_TRUE(sdp_request_tid);
  ASSERT_EQ(0u, search_cb_count);

  sdp::ServiceSearchAttributeResponse rsp;
  rsp.SetAttribute(0, sdp::kServiceId, sdp::DataElement(UUID()));
  auto rsp_ptr = rsp.GetPDU(0xFFFF /* max attribute bytes */,
                            *sdp_request_tid,
                            PDU_MAX,
                            BufferView());

  sdp_chan->Receive(*rsp_ptr);

  RunUntilIdle();

  ASSERT_EQ(1u, search_cb_count);

  // Remote end disconnects.
  test_device()->SendCommandChannelPacket(kDisconnectionComplete);

  RunUntilIdle();

  sdp_request_tid.reset();
  sdp_chan.reset();

  // Second connection is shortened because we have already interrogated.
  // We still search for SDP services.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAcceptConnectionRequest,
                        &kAcceptConnectionRequestRsp,
                        &kConnectionComplete);
  // We don't send any interrogation packets, because there is none to be done.

  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kSDP, 0x40, 0x41, kChannelParams);

  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  // We should have searched again.
  ASSERT_TRUE(sdp_chan.is_alive());
  ASSERT_TRUE(sdp_request_tid);
  ASSERT_EQ(1u, search_cb_count);

  rsp_ptr = rsp.GetPDU(0xFFFF /* max attribute bytes */,
                       *sdp_request_tid,
                       PDU_MAX,
                       BufferView());

  sdp_chan->Receive(*rsp_ptr);

  RunUntilIdle();

  ASSERT_EQ(2u, search_cb_count);

  QueueDisconnection(kConnectionHandle);
}

// Test: when opening an L2CAP channel on an unbonded peer, indicate that we
// have no link key then pair, authenticate, bond, and encrypt the link, then
// try to open the channel.
TEST_F(BrEdrConnectionManagerTest, OpenL2capPairsAndEncryptsThenRetries) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_FALSE(IsNotConnected(peer));

  std::optional<l2cap::Channel::WeakPtr> connected_chan;

  auto chan_cb = [&](auto chan) { connected_chan = std::move(chan); };

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Approve pairing requests.
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) {
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  // Initial connection request

  // Pairing initiation and flow that results in bonding then encryption, but
  // verifying the strength of the encryption key doesn't complete
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAuthenticationRequested,
                        &kAuthenticationRequestedStatus,
                        &kLinkKeyRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kLinkKeyRequestNegativeReply,
                        &kLinkKeyRequestNegativeReplyRsp,
                        &kIoCapabilityRequest);
  const auto kIoCapabilityResponse = MakeIoCapabilityResponse(
      IoCapability::DISPLAY_YES_NO,
      AuthenticationRequirements::MITM_GENERAL_BONDING);
  const auto kUserConfirmationRequest = MakeUserConfirmationRequest(kPasskey);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::DISPLAY_YES_NO,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp,
                        &kIoCapabilityResponse,
                        &kUserConfirmationRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kUserConfirmationRequestReply,
                        &kUserConfirmationRequestReplyRsp,
                        &kSimplePairingCompleteSuccess,
                        &kLinkKeyNotification,
                        &kAuthenticationComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEvent);
  EXPECT_CMD_PACKET_OUT(test_device(), kReadEncryptionKeySize, );

  connmgr()->OpenL2capChannel(peer->identifier(),
                              l2cap::kAVDTP,
                              kNoSecurityRequirements,
                              kChannelParams,
                              chan_cb);

  RETURN_IF_FATAL(RunUntilIdle());

  // We should not have a channel because the L2CAP open callback shouldn't have
  // been called, but the LTK should be stored since the link key got received.
  ASSERT_FALSE(connected_chan);
  // We should be initializing, since we have not completed pairing.
  ASSERT_TRUE(IsInitializing(peer));

  test_device()->SendCommandChannelPacket(kReadEncryptionKeySizeRsp);

  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kAVDTP, 0x40, 0x41, kChannelParams);

  RETURN_IF_FATAL(RunUntilIdle());
  // We should signal to PeerCache as connected once we finish pairing.
  ASSERT_TRUE(IsConnected(peer));

  // The socket should be returned.
  ASSERT_TRUE(connected_chan);

  connected_chan.reset();

  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kAVDTP, 0x40, 0x41, kChannelParams);

  // A second connection request should not require another authentication.
  connmgr()->OpenL2capChannel(peer->identifier(),
                              l2cap::kAVDTP,
                              kNoSecurityRequirements,
                              kChannelParams,
                              chan_cb);

  RunUntilIdle();

  ASSERT_TRUE(connected_chan);

  QueueDisconnection(kConnectionHandle);
}

// Test: when the peer is already bonded, the link key gets stored when it is
// provided to the connection.
TEST_F(BrEdrConnectionManagerTest, OpenL2capEncryptsForBondedPeerThenRetries) {
  ASSERT_TRUE(
      peer_cache()->AddBondedPeer(BondingData{.identifier = PeerId(999),
                                              .address = kTestDevAddr,
                                              .name = std::nullopt,
                                              .device_class = {},
                                              .le_pairing_data = {},
                                              .bredr_link_key = kLinkKey,
                                              .bredr_services = {}}));
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsNotConnected(peer));
  ASSERT_TRUE(peer->bonded());

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  ASSERT_FALSE(IsNotConnected(peer));

  std::optional<l2cap::Channel::WeakPtr> connected_chan;

  auto socket_cb = [&](auto chan) { connected_chan = std::move(chan); };

  // Initial connection request

  // Note: this skips some parts of the pairing flow, because the link key being
  // received is the important part of this. The key is not received when the
  // authentication fails.
  EXPECT_CMD_PACKET_OUT(
      test_device(), kAuthenticationRequested, &kAuthenticationRequestedStatus);

  connmgr()->OpenL2capChannel(peer->identifier(),
                              l2cap::kAVDTP,
                              kNoSecurityRequirements,
                              kChannelParams,
                              socket_cb);

  RunUntilIdle();

  // L2CAP connect shouldn't have been called, and callback shouldn't be called.
  // We should not have a socket.
  ASSERT_FALSE(connected_chan);
  ASSERT_FALSE(IsNotConnected(peer));

  // The authentication flow will request the existing link key, which should be
  // returned and stored, and then the authentication is complete.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kLinkKeyRequestReply,
                        &kLinkKeyRequestReplyRsp,
                        &kAuthenticationComplete);

  test_device()->SendCommandChannelPacket(kLinkKeyRequest);

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEvent);
  EXPECT_CMD_PACKET_OUT(test_device(), kReadEncryptionKeySize, );

  RunUntilIdle();

  // No socket until the encryption verification completes.
  ASSERT_FALSE(connected_chan);

  test_device()->SendCommandChannelPacket(kReadEncryptionKeySizeRsp);

  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kAVDTP, 0x40, 0x41, kChannelParams);

  RunUntilIdle();

  // Once the L2CAP channel has connected, we have connected.
  ASSERT_TRUE(IsConnected(peer));

  // The socket should be connected.
  ASSERT_TRUE(connected_chan);

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest,
       OpenL2capAuthenticationFailureReturnsInvalidSocketAndDisconnects) {
  QueueSuccessfulIncomingConn();

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_FALSE(IsNotConnected(peer));

  std::optional<l2cap::Channel::WeakPtr> connected_chan;

  auto socket_cb = [&](auto chan) { connected_chan = std::move(chan); };

  // Initial connection request

  // Note: this skips some parts of the pairing flow, because the link key being
  // received is the important part of this. The key is not received when the
  // authentication fails.
  EXPECT_CMD_PACKET_OUT(
      test_device(), kAuthenticationRequested, &kAuthenticationRequestedStatus);

  connmgr()->OpenL2capChannel(peer->identifier(),
                              l2cap::kAVDTP,
                              kNoSecurityRequirements,
                              kChannelParams,
                              socket_cb);

  RunUntilIdle();

  // The L2CAP shouldn't have been called
  // We should not have a channel, and the callback shouldn't have been called.
  ASSERT_FALSE(connected_chan);

  test_device()->SendCommandChannelPacket(kAuthenticationCompleteFailed);

  int count = transaction_count();

  // We disconnect the peer when authentication fails.
  QueueDisconnection(kConnectionHandle);

  RunUntilIdle();

  // An invalid channel should have been sent because the connection failed.
  ASSERT_TRUE(connected_chan);
  ASSERT_FALSE(connected_chan.value().is_alive());

  ASSERT_EQ(count + kDisconnectionTransactions, transaction_count());
}

TEST_F(BrEdrConnectionManagerTest, OpenL2capPairingFinishesButDisconnects) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_FALSE(IsNotConnected(peer));

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Approve pairing requests.
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) {
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  // Initial connection request

  // Pairing initiation and flow that results in bonding then encryption, but
  // verifying the strength of the encryption key doesn't complete
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAuthenticationRequested,
                        &kAuthenticationRequestedStatus,
                        &kLinkKeyRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kLinkKeyRequestNegativeReply,
                        &kLinkKeyRequestNegativeReplyRsp,
                        &kIoCapabilityRequest);
  const auto kIoCapabilityResponse = MakeIoCapabilityResponse(
      IoCapability::DISPLAY_YES_NO,
      AuthenticationRequirements::MITM_GENERAL_BONDING);
  const auto kUserConfirmationRequest = MakeUserConfirmationRequest(kPasskey);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::DISPLAY_YES_NO,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp,
                        &kIoCapabilityResponse,
                        &kUserConfirmationRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kUserConfirmationRequestReply,
                        &kUserConfirmationRequestReplyRsp,
                        &kSimplePairingCompleteSuccess,
                        &kLinkKeyNotification,
                        &kAuthenticationComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEvent);
  EXPECT_CMD_PACKET_OUT(test_device(), kReadEncryptionKeySize, );

  std::optional<l2cap::Channel::WeakPtr> connected_chan;

  auto chan_cb = [&](auto chan) { connected_chan = std::move(chan); };
  connmgr()->OpenL2capChannel(peer->identifier(),
                              l2cap::kAVDTP,
                              kNoSecurityRequirements,
                              kChannelParams,
                              chan_cb);

  RETURN_IF_FATAL(RunUntilIdle());

  // We should not have a channel because the L2CAP open callback shouldn't have
  // been called, but the LTK should be stored since the link key got received.
  ASSERT_FALSE(connected_chan);

  // The remote device disconnects now, when the pairing has been started, then
  // pairing completes.
  test_device()->SendCommandChannelPacket(kDisconnectionComplete);
  test_device()->SendCommandChannelPacket(kReadEncryptionKeySizeRsp);
  RunUntilIdle();

  // We should get a callback from the OpenL2capChannel
  ASSERT_TRUE(connected_chan);
  EXPECT_FALSE(connected_chan.value().is_alive());

  connected_chan.reset();

  connmgr()->OpenL2capChannel(peer->identifier(),
                              l2cap::kAVDTP,
                              kNoSecurityRequirements,
                              kChannelParams,
                              chan_cb);

  // The L2CAP should be called right away without a channel.
  ASSERT_TRUE(connected_chan);
  EXPECT_FALSE(connected_chan.value().is_alive());

  connected_chan.reset();
}

// Test: when pairing is in progress, opening an L2CAP channel waits for the
// pairing to complete before retrying.
TEST_F(BrEdrConnectionManagerTest,
       OpenL2capDuringPairingWaitsForPairingToComplete) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_FALSE(IsNotConnected(peer));

  std::optional<l2cap::Channel::WeakPtr> connected_chan;

  auto socket_cb = [&](auto chan) { connected_chan = std::move(chan); };

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Approve pairing requests
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) {
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  // Initiate pairing from the peer
  test_device()->SendCommandChannelPacket(MakeIoCapabilityResponse(
      IoCapability::DISPLAY_YES_NO,
      AuthenticationRequirements::MITM_GENERAL_BONDING));

  RETURN_IF_FATAL(RunUntilIdle());

  // Initial connection request

  // Pair and bond as the responder. Note that Authentication Requested is not
  // sent even though we are opening the L2CAP channel because the peer started
  // pairing first.
  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);
  const auto kUserConfirmationRequest = MakeUserConfirmationRequest(kPasskey);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::DISPLAY_YES_NO,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp,
                        &kUserConfirmationRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kUserConfirmationRequestReply,
                        &kUserConfirmationRequestReplyRsp,
                        &kSimplePairingCompleteSuccess,
                        &kLinkKeyNotification);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEvent);
  EXPECT_CMD_PACKET_OUT(test_device(), kReadEncryptionKeySize, );

  connmgr()->OpenL2capChannel(peer->identifier(),
                              l2cap::kAVDTP,
                              kNoSecurityRequirements,
                              kChannelParams,
                              socket_cb);

  RETURN_IF_FATAL(RunUntilIdle());

  // We should not have a socket because the L2CAP open callback shouldn't have
  // been called, but the LTK should be stored since the link key got received.
  ASSERT_FALSE(connected_chan);

  test_device()->SendCommandChannelPacket(kReadEncryptionKeySizeRsp);

  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kAVDTP, 0x40, 0x41, kChannelParams);

  RETURN_IF_FATAL(RunUntilIdle());

  // The socket should be returned.
  ASSERT_TRUE(connected_chan);

  QueueDisconnection(kConnectionHandle);
}

// Test: when pairing is in progress, opening an L2CAP channel waits for the
// pairing to complete before retrying.
TEST_F(BrEdrConnectionManagerTest,
       InterrogationInProgressAllowsBondingButNotL2cap) {
  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Trigger inbound connection and respond to some (but not all) of
  // interrogation.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAcceptConnectionRequest,
                        &kAcceptConnectionRequestRsp,
                        &kConnectionComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kRemoteNameRequest,
                        &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteVersionInfo,
                        &kReadRemoteVersionInfoRsp,
                        &kRemoteVersionInfoComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        hci_spec::kReadRemoteSupportedFeatures,
                        &kReadRemoteSupportedFeaturesRsp);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  // Ensure that the interrogation has begun but the peer hasn't yet bonded
  EXPECT_EQ(4, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bredr()->bonded());

  // Approve pairing requests
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) {
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  // Initiate pairing from the peer before interrogation completes
  test_device()->SendCommandChannelPacket(MakeIoCapabilityResponse(
      IoCapability::DISPLAY_YES_NO,
      AuthenticationRequirements::MITM_GENERAL_BONDING));
  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);
  const auto kUserConfirmationRequest = MakeUserConfirmationRequest(kPasskey);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::DISPLAY_YES_NO,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp,
                        &kUserConfirmationRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kUserConfirmationRequestReply,
                        &kUserConfirmationRequestReplyRsp,
                        &kSimplePairingCompleteSuccess,
                        &kLinkKeyNotification);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEvent);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadEncryptionKeySize, &kReadEncryptionKeySizeRsp);

  RETURN_IF_FATAL(RunUntilIdle());

  // At this point the peer is bonded and the link is encrypted but
  // interrogation has not completed so host-side L2CAP should still be inactive
  // on this link (though it may be buffering packets).
  EXPECT_FALSE(l2cap()->IsLinkConnected(kConnectionHandle));

  bool socket_cb_called = false;
  auto socket_fails_cb = [&socket_cb_called](auto chan_sock) {
    EXPECT_FALSE(chan_sock.is_alive());
    socket_cb_called = true;
  };
  connmgr()->OpenL2capChannel(peer->identifier(),
                              l2cap::kAVDTP,
                              kNoSecurityRequirements,
                              kChannelParams,
                              socket_fails_cb);

  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_TRUE(socket_cb_called);

  // Complete interrogation successfully.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended1,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1Complete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended2,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1Complete);
  test_device()->SendCommandChannelPacket(kReadRemoteSupportedFeaturesComplete);

  RETURN_IF_FATAL(RunUntilIdle());

  EXPECT_TRUE(l2cap()->IsLinkConnected(kConnectionHandle));

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest, ConnectUnknownPeer) {
  EXPECT_FALSE(connmgr()->Connect(PeerId(456), {}));
}

TEST_F(BrEdrConnectionManagerTest, ConnectLowEnergyPeer) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddrLe, /*connectable=*/true);
  EXPECT_FALSE(connmgr()->Connect(peer->identifier(), {}));
}

TEST_F(BrEdrConnectionManagerTest, DisconnectUnknownPeerDoesNothing) {
  EXPECT_TRUE(
      connmgr()->Disconnect(PeerId(999), DisconnectReason::kApiRequest));

  RunUntilIdle();

  EXPECT_EQ(0, transaction_count());
}

// Test: user-initiated disconnection
TEST_F(BrEdrConnectionManagerTest, DisconnectClosesHciConnection) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_FALSE(IsNotConnected(peer));

  QueueDisconnection(kConnectionHandle);

  EXPECT_TRUE(
      connmgr()->Disconnect(peer->identifier(), DisconnectReason::kApiRequest));
  EXPECT_TRUE(IsNotConnected(peer));

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());
  EXPECT_TRUE(IsNotConnected(peer));
}

TEST_F(BrEdrConnectionManagerTest, DisconnectSamePeerIsIdempotent) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_FALSE(IsNotConnected(peer));

  QueueDisconnection(kConnectionHandle);

  EXPECT_TRUE(
      connmgr()->Disconnect(peer->identifier(), DisconnectReason::kApiRequest));
  ASSERT_TRUE(IsNotConnected(peer));

  // Try to disconnect again while the first disconnect is in progress (HCI
  // Disconnection Complete not yet received).
  EXPECT_TRUE(
      connmgr()->Disconnect(peer->identifier(), DisconnectReason::kApiRequest));

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());
  ASSERT_TRUE(IsNotConnected(peer));

  // Try to disconnect once more, now that the link is gone.
  EXPECT_TRUE(
      connmgr()->Disconnect(peer->identifier(), DisconnectReason::kApiRequest));
}

TEST_F(BrEdrConnectionManagerTest, RemovePeerFromPeerCacheDuringDisconnection) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_FALSE(IsNotConnected(peer));

  QueueDisconnection(kConnectionHandle);

  const PeerId id = peer->identifier();
  EXPECT_TRUE(connmgr()->Disconnect(id, DisconnectReason::kApiRequest));
  ASSERT_TRUE(IsNotConnected(peer));

  // Remove the peer from PeerCache before receiving HCI Disconnection Complete.
  EXPECT_TRUE(peer_cache()->RemoveDisconnectedPeer(id));

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions + 1, transaction_count());
  EXPECT_FALSE(peer_cache()->FindById(id));
  EXPECT_FALSE(peer_cache()->FindByAddress(kTestDevAddr));
}

TEST_F(BrEdrConnectionManagerTest, AddServiceSearchAll) {
  size_t search_cb_count = 0;
  auto search_cb = [&](auto id, const auto&) {
    auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
    ASSERT_TRUE(peer);
    ASSERT_EQ(id, peer->identifier());
    search_cb_count++;
  };

  connmgr()->AddServiceSearch(sdp::profile::kAudioSink, {}, search_cb);

  l2cap::testing::FakeChannel::WeakPtr sdp_chan;
  std::optional<uint32_t> sdp_request_tid;

  l2cap()->set_channel_callback(
      [this, &sdp_chan, &sdp_request_tid](auto new_chan) {
        new_chan->SetSendCallback(
            [&sdp_request_tid](auto packet) {
              const StaticByteBuffer kSearchExpectedParams(
                  // ServiceSearchPattern
                  0x35,
                  0x03,  // Sequence uint8 3 bytes
                  0x19,
                  0x11,
                  0x0B,  // UUID (kAudioSink)
                  0xFF,
                  0xFF,  // MaxAttributeByteCount (none)
                  // Attribute ID list
                  0x35,
                  0x05,  // Sequence uint8 5 bytes
                  0x0A,
                  0x00,
                  0x00,
                  0xFF,
                  0xFF,  // uint32_t (all attributes)
                  0x00   // No continuation state
              );
              // First byte should be type.
              ASSERT_LE(3u, packet->size());
              ASSERT_EQ(sdp::kServiceSearchAttributeRequest, (*packet)[0]);
              ASSERT_EQ(kSearchExpectedParams,
                        packet->view(sizeof(bt::sdp::Header)));
              sdp_request_tid = tid_from_sdp_packet(packet);
            },
            dispatcher());
        sdp_chan = std::move(new_chan);
      });

  QueueSuccessfulIncomingConn();
  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kSDP, 0x40, 0x41, kChannelParams);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  ASSERT_TRUE(sdp_chan.is_alive());
  ASSERT_TRUE(sdp_request_tid);
  ASSERT_EQ(0u, search_cb_count);

  sdp::ServiceSearchAttributeResponse rsp;
  rsp.SetAttribute(0, sdp::kServiceId, sdp::DataElement(UUID()));
  auto rsp_ptr = rsp.GetPDU(0xFFFF /* max attribute bytes */,
                            *sdp_request_tid,
                            PDU_MAX,
                            BufferView());

  sdp_chan->Receive(*rsp_ptr);

  RunUntilIdle();

  ASSERT_EQ(1u, search_cb_count);

  QueueDisconnection(kConnectionHandle);
}

// An error is received via the HCI Command cb_status event
TEST_F(BrEdrConnectionManagerTest, ConnectSinglePeerErrorStatus) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);

  EXPECT_CMD_PACKET_OUT(
      test_device(), kCreateConnection, &kCreateConnectionRspError);

  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsNotConnected(peer));

  hci::Result<> status = fit::ok();
  EXPECT_TRUE(
      connmgr()->Connect(peer->identifier(), CALLBACK_EXPECT_FAILURE(status)));
  EXPECT_TRUE(IsInitializing(peer));
  RunUntilIdle();

  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::
                         CONNECTION_FAILED_TO_BE_ESTABLISHED),
            status);
  EXPECT_TRUE(IsNotConnected(peer));
}

// Connection Complete event reports error
TEST_F(BrEdrConnectionManagerTest, ConnectSinglePeerFailure) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnection,
                        &kCreateConnectionRsp,
                        &kConnectionCompleteError);

  hci::Result<> status = ToResult(HostError::kFailed);
  bool callback_run = false;

  auto callback = [&status, &callback_run](auto cb_status, auto conn_ref) {
    EXPECT_FALSE(conn_ref);
    status = cb_status;
    callback_run = true;
  };
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));

  RunUntilIdle();

  EXPECT_TRUE(callback_run);

  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::
                         CONNECTION_FAILED_TO_BE_ESTABLISHED),
            status);
  EXPECT_TRUE(IsNotConnected(peer));
}

TEST_F(BrEdrConnectionManagerTest, ConnectSinglePeerTimeout) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);

  EXPECT_CMD_PACKET_OUT(
      test_device(), kCreateConnection, &kCreateConnectionRsp);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnectionCancel,
                        &kCreateConnectionCancelRsp,
                        &kConnectionCompleteCanceled);

  hci::Result<> status = fit::ok();
  auto callback = [&status](auto cb_status, auto conn_ref) {
    EXPECT_FALSE(conn_ref);
    status = cb_status;
  };

  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));
  RunFor(kBrEdrCreateConnectionTimeout);
  RunFor(kBrEdrCreateConnectionTimeout);
  EXPECT_EQ(ToResult(HostError::kTimedOut), status);
  EXPECT_TRUE(IsNotConnected(peer));
}

// Successful connection to single peer
TEST_F(BrEdrConnectionManagerTest, ConnectSinglePeer) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  // Queue up the connection
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnection,
                        &kCreateConnectionRsp,
                        &kConnectionComplete);
  QueueSuccessfulInterrogation(peer->address(), kConnectionHandle);
  QueueDisconnection(kConnectionHandle);

  // Initialize as error to verify that |callback| assigns success.
  hci::Result<> status = ToResult(HostError::kFailed);
  BrEdrConnection* conn_ref = nullptr;
  auto callback = [&status, &conn_ref](auto cb_status, auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
  };

  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_TRUE(HasConnectionTo(peer, conn_ref));
  EXPECT_FALSE(IsNotConnected(peer));
  EXPECT_EQ(conn_ref->link().role(),
            pw::bluetooth::emboss::ConnectionRole::CENTRAL);
}

TEST_F(BrEdrConnectionManagerTest, ConnectSinglePeerFailedInterrogation) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  // Queue up outbound connection.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnection,
                        &kCreateConnectionRsp,
                        &kConnectionComplete);

  // Queue up most of interrogation.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kRemoteNameRequest,
                        &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteVersionInfo,
                        &kReadRemoteVersionInfoRsp,
                        &kRemoteVersionInfoComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        hci_spec::kReadRemoteSupportedFeatures,
                        &kReadRemoteSupportedFeaturesRsp);

  hci::Result<> status = fit::ok();
  BrEdrConnection* conn_ref = nullptr;
  auto callback = [&status, &conn_ref](auto cb_status, auto cb_conn_ref) {
    EXPECT_FALSE(cb_conn_ref);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
  };

  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  RETURN_IF_FATAL(RunUntilIdle());

  test_device()->SendCommandChannelPacket(
      kReadRemoteSupportedFeaturesCompleteFailed);
  QueueDisconnection(kConnectionHandle);
  RETURN_IF_FATAL(RunUntilIdle());

  EXPECT_EQ(ToResult(HostError::kNotSupported), status);
  EXPECT_TRUE(IsNotConnected(peer));
}

// Connecting to an already connected peer should complete instantly
TEST_F(BrEdrConnectionManagerTest, ConnectSinglePeerAlreadyConnected) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  // Queue up the connection
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnection,
                        &kCreateConnectionRsp,
                        &kConnectionComplete);
  QueueSuccessfulInterrogation(peer->address(), kConnectionHandle);
  QueueDisconnection(kConnectionHandle);

  // Initialize as error to verify that |callback| assigns success.
  hci::Result<> status = ToResult(HostError::kFailed);
  int num_callbacks = 0;
  BrEdrConnection* conn_ref = nullptr;
  auto callback = [&status, &conn_ref, &num_callbacks](auto cb_status,
                                                       auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
    ++num_callbacks;
  };

  // Connect to the peer for the first time
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_TRUE(HasConnectionTo(peer, conn_ref));
  EXPECT_FALSE(IsNotConnected(peer));
  EXPECT_EQ(num_callbacks, 1);

  // Attempt to connect again to the already connected peer. callback should be
  // called synchronously.
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  EXPECT_EQ(num_callbacks, 2);
  EXPECT_EQ(fit::ok(), status);
  EXPECT_TRUE(HasConnectionTo(peer, conn_ref));
  EXPECT_FALSE(IsNotConnected(peer));
}

// Initiating Two Connections to the same (currently unconnected) peer should
// successfully establish both
TEST_F(BrEdrConnectionManagerTest, ConnectSinglePeerTwoInFlight) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  // Queue up the connection
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnection,
                        &kCreateConnectionRsp,
                        &kConnectionComplete);
  QueueSuccessfulInterrogation(peer->address(), kConnectionHandle);
  QueueDisconnection(kConnectionHandle);

  // Initialize as error to verify that |callback| assigns success.
  hci::Result<> status = ToResult(HostError::kFailed);
  int num_callbacks = 0;
  BrEdrConnection* conn_ref = nullptr;
  auto callback = [&status, &conn_ref, &num_callbacks](auto cb_status,
                                                       auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
    ++num_callbacks;
  };

  // Launch one request, but don't run the loop
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));

  // Launch second inflight request
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));

  // Run the loop which should complete both requests
  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_TRUE(HasConnectionTo(peer, conn_ref));
  EXPECT_FALSE(IsNotConnected(peer));
  EXPECT_EQ(num_callbacks, 2);
}

TEST_F(BrEdrConnectionManagerTest,
       ConnectInterrogatingPeerOnlyCompletesAfterInterrogation) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnection,
                        &kCreateConnectionRsp,
                        &kConnectionComplete);
  // Prevent interrogation from completing so that we can queue a second request
  // during interrogation.
  QueueIncompleteInterrogation(kTestDevAddr, kConnectionHandle);
  QueueDisconnection(kConnectionHandle);

  // Initialize as error to verify that |callback| assigns success.
  hci::Result<> status = ToResult(HostError::kFailed);
  int num_callbacks = 0;
  BrEdrConnection* conn_ref = nullptr;
  auto callback = [&status, &conn_ref, &num_callbacks](auto cb_status,
                                                       auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
    ++num_callbacks;
  };

  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));
  RunUntilIdle();

  // Launch second request, which should not complete immediately.
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  EXPECT_EQ(num_callbacks, 0);

  // Finishing interrogation should complete both requests.
  CompleteInterrogation(kConnectionHandle);
  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_TRUE(HasConnectionTo(peer, conn_ref));
  EXPECT_FALSE(IsNotConnected(peer));
  EXPECT_EQ(num_callbacks, 2);
}

TEST_F(BrEdrConnectionManagerTest, ConnectSecondPeerFirstTimesOut) {
  auto* peer_a = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  auto* peer_b = peer_cache()->NewPeer(kTestDevAddr2, /*connectable=*/true);

  // Enqueue first connection request (which will timeout and be cancelled)
  EXPECT_CMD_PACKET_OUT(
      test_device(), kCreateConnection, &kCreateConnectionRsp);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnectionCancel,
                        &kCreateConnectionCancelRsp,
                        &kConnectionCompleteCanceled);

  // Enqueue second connection (which will succeed once previous has ended)
  QueueSuccessfulCreateConnection(peer_b, kConnectionHandle2);
  QueueSuccessfulInterrogation(peer_b->address(), kConnectionHandle2);
  QueueDisconnection(kConnectionHandle2);

  // Initialize as success to verify that |callback_a| assigns failure.
  hci::Result<> status_a = fit::ok();
  auto callback_a = [&status_a](auto cb_status, auto cb_conn_ref) {
    status_a = cb_status;
    EXPECT_FALSE(cb_conn_ref);
  };

  // Initialize as error to verify that |callback_b| assigns success.
  hci::Result<> status_b = ToResult(HostError::kFailed);
  BrEdrConnection* connection = nullptr;
  auto callback_b = [&status_b, &connection](auto cb_status, auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status_b = cb_status;
    connection = std::move(cb_conn_ref);
  };

  // Launch one request (this will timeout)
  EXPECT_TRUE(connmgr()->Connect(peer_a->identifier(), callback_a));
  ASSERT_TRUE(peer_a->bredr());
  EXPECT_TRUE(IsInitializing(peer_a));

  RunUntilIdle();

  // Launch second inflight request (this will wait for the first)
  EXPECT_TRUE(connmgr()->Connect(peer_b->identifier(), callback_b));
  ASSERT_TRUE(peer_b->bredr());

  // Run the loop which should complete both requests
  RunFor(kBrEdrCreateConnectionTimeout);
  RunFor(kBrEdrCreateConnectionTimeout);

  EXPECT_TRUE(status_a.is_error());
  EXPECT_EQ(fit::ok(), status_b);
  EXPECT_TRUE(HasConnectionTo(peer_b, connection));
  EXPECT_TRUE(IsNotConnected(peer_a));
  EXPECT_FALSE(IsNotConnected(peer_b));
}

class FirstLowEnergyOnlyPeer : public BrEdrConnectionManagerTest,
                               public ::testing::WithParamInterface<bool> {};

TEST_P(FirstLowEnergyOnlyPeer, ConnectToDualModePeerThatWasFirstLowEnergyOnly) {
  const DeviceAddress kTestDevAddrLeAlias(DeviceAddress::Type::kLEPublic,
                                          kTestDevAddr.value());
  auto* peer =
      peer_cache()->NewPeer(kTestDevAddrLeAlias, /*connectable=*/GetParam());
  EXPECT_TRUE(peer->temporary());
  EXPECT_EQ(TechnologyType::kLowEnergy, peer->technology());

  // Make peer dual mode
  peer->MutBrEdr();
  EXPECT_EQ(TechnologyType::kDualMode, peer->technology());

  // Queue up the connection
  EXPECT_CMD_PACKET_OUT(
      test_device(), kCreateConnection, &kCreateConnectionRsp);

  // Initialize as error to verify that |callback| assigns success.
  hci::Result<> status = ToResult(HostError::kFailed);
  BrEdrConnection* conn_ref = nullptr;
  auto callback = [&status, &conn_ref](auto cb_status, auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
  };

  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  EXPECT_TRUE(IsInitializing(peer));
  RunUntilIdle();

  test_device()->SendCommandChannelPacket(kConnectionComplete);
  QueueSuccessfulInterrogation(peer->address(), kConnectionHandle);
  QueueDisconnection(kConnectionHandle);
  RunUntilIdle();

  EXPECT_EQ(fit::ok(), status);
  EXPECT_TRUE(HasConnectionTo(peer, conn_ref));
  EXPECT_FALSE(IsNotConnected(peer));
  EXPECT_EQ(conn_ref->link().role(),
            pw::bluetooth::emboss::ConnectionRole::CENTRAL);
}

INSTANTIATE_TEST_SUITE_P(BrEdrConnectionManagerTest,
                         FirstLowEnergyOnlyPeer,
                         ::testing::Values(true, false));

// Tests the successful retry case. "don't retry for other error codes" is
// implicitly tested in ConnectSinglePeerFailure - MockController would error if
// we unexpectedly retried.
TEST_F(BrEdrConnectionManagerTest, SuccessfulHciRetriesAfterPageTimeout) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  // We send a first HCI Create Connection which will hang for 14s, and then
  // respond on the test device with a ConnectionCompletePageTimeout event,
  // which will cause a retry. The retry will also hang for 14s, then will
  // receive another PageTimeout response, which will cause another retry, which
  // will finally be permitted to succeed
  EXPECT_CMD_PACKET_OUT(
      test_device(), kCreateConnection, &kCreateConnectionRsp);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kCreateConnection, &kCreateConnectionRsp);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnection,
                        &kCreateConnectionRsp,
                        &kConnectionComplete);
  QueueSuccessfulInterrogation(peer->address(), kConnectionHandle);
  QueueDisconnection(kConnectionHandle);

  // Initialize as error to verify that |callback| assigns success.
  hci::Result<> status = ToResult(HostError::kFailed);
  BrEdrConnection* conn_ref = nullptr;
  auto callback = [&status, &conn_ref](auto cb_status, auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
  };

  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  // Cause the initial Create Connection to wait for 14s for Connection Complete
  RunFor(std::chrono::seconds(14));
  ASSERT_TRUE(
      test_device()->SendCommandChannelPacket(kConnectionCompletePageTimeout));
  // Verify higher layers have not been notified of failure.
  EXPECT_EQ(ToResult(HostError::kFailed), status);
  // Cause the first retry Create Connection to wait for 14s for Connection
  // Complete - now 28s since the first Create Connection, bumping up on the
  // retry window limit of 30s.
  RunFor(std::chrono::seconds(14));
  // Cause a second retry.
  ASSERT_TRUE(
      test_device()->SendCommandChannelPacket(kConnectionCompletePageTimeout));
  // Verify higher layers have not been notified of failure until the Connection
  // Complete propagates
  EXPECT_EQ(ToResult(HostError::kFailed), status);

  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);
  EXPECT_TRUE(HasConnectionTo(peer, conn_ref));
  EXPECT_EQ(conn_ref->link().role(),
            pw::bluetooth::emboss::ConnectionRole::CENTRAL);
}

TEST_F(BrEdrConnectionManagerTest, DontRetryAfterWindowClosed) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  // We send a first HCI Create Connection which will hang for 15s, and then
  // respond on the test device with a ConnectionCompletePageTimeout event,
  // which will cause a retry. The retry will hang for 16s, then will receive
  // another PageTimeout response. Because this will be 31s after the initial
  // HCI Create Connection, the retry window will be closed and the Connect()
  // will fail.
  EXPECT_CMD_PACKET_OUT(
      test_device(), kCreateConnection, &kCreateConnectionRsp);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kCreateConnection, &kCreateConnectionRsp);

  // Initialize as success to verify that |callback| assigns error.
  hci::Result<> status = fit::ok();
  auto callback = [&status](auto cb_status, auto cb_conn_ref) {
    EXPECT_FALSE(cb_conn_ref);
    status = cb_status;
  };

  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  RunFor(std::chrono::seconds(15));
  // Higher layers should not be notified yet.
  EXPECT_EQ(fit::ok(), status);
  ASSERT_TRUE(
      test_device()->SendCommandChannelPacket(kConnectionCompletePageTimeout));

  // Create Connection will retry, and it hangs for 16s before
  // ConnectionCompletePageTimeout
  RunFor(std::chrono::seconds(16));
  ASSERT_TRUE(
      test_device()->SendCommandChannelPacket(kConnectionCompletePageTimeout));
  RunUntilIdle();
  // Create Connection will *not* be tried again as we are outside of the retry
  // window.
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::PAGE_TIMEOUT), status);
}

TEST_F(BrEdrConnectionManagerTest,
       ConnectSecondPeerFirstFailsWithPageTimeoutAndDoesNotRetry) {
  auto* peer_a = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  auto* peer_b = peer_cache()->NewPeer(kTestDevAddr2, /*connectable=*/true);

  // First peer's Create Connection Request will complete with a page timeout
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnection,
                        &kCreateConnectionRsp,
                        &kConnectionCompletePageTimeout);

  // Immediately enqueue successful connection request to peer_b, without any
  // retry in between for the Connect() call to peer_a.
  QueueSuccessfulCreateConnection(peer_b, kConnectionHandle2);
  QueueSuccessfulInterrogation(peer_b->address(), kConnectionHandle2);
  QueueDisconnection(kConnectionHandle2);

  // Initialize as success to verify that |callback_a| assigns failure.
  hci::Result<> status_a = fit::ok();
  auto callback_a = [&status_a](auto cb_status, auto cb_conn_ref) {
    status_a = cb_status;
    EXPECT_FALSE(cb_conn_ref);
  };

  // Initialize as error to verify that |callback_b| assigns success.
  hci::Result<> status_b = ToResult(HostError::kFailed);
  BrEdrConnection* connection = nullptr;
  auto callback_b = [&status_b, &connection](auto cb_status, auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status_b = cb_status;
    connection = std::move(cb_conn_ref);
  };

  // Launch one request, which will cause a Connection Complete: page timeout
  // controller event.
  EXPECT_TRUE(connmgr()->Connect(peer_a->identifier(), callback_a));
  EXPECT_TRUE(IsInitializing(peer_a));

  // Launch second inflight request (this will wait for the first)
  EXPECT_TRUE(connmgr()->Connect(peer_b->identifier(), callback_b));
  EXPECT_TRUE(IsInitializing(peer_b));

  // Run the loop which should complete both requests
  RunUntilIdle();

  // The Connect() request to peer_a should fail with the Page Timeout status
  // code without retrying
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::PAGE_TIMEOUT),
            status_a);
  EXPECT_EQ(fit::ok(), status_b);
  EXPECT_TRUE(HasConnectionTo(peer_b, connection));
  EXPECT_TRUE(IsNotConnected(peer_a));
  EXPECT_FALSE(IsNotConnected(peer_b));
}

TEST_F(BrEdrConnectionManagerTest, DisconnectPendingConnections) {
  auto* peer_a = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  auto* peer_b = peer_cache()->NewPeer(kTestDevAddr2, /*connectable=*/true);

  // Enqueue first connection request (which will await Connection Complete)
  EXPECT_CMD_PACKET_OUT(
      test_device(), kCreateConnection, &kCreateConnectionRsp);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnectionCancel,
                        &kCreateConnectionCancelRsp,
                        &kConnectionCompleteCanceled);

  // No-op connection callbacks
  auto callback_a = [](auto, auto) {};
  auto callback_b = [](auto, auto) {};

  // Launch both requests (second one is queued. Neither completes.)
  EXPECT_TRUE(connmgr()->Connect(peer_a->identifier(), callback_a));
  EXPECT_TRUE(connmgr()->Connect(peer_b->identifier(), callback_b));

  // Put the first connection into flight.
  RETURN_IF_FATAL(RunUntilIdle());

  ASSERT_TRUE(IsInitializing(peer_a));
  ASSERT_TRUE(IsInitializing(peer_b));

  EXPECT_FALSE(connmgr()->Disconnect(peer_a->identifier(),
                                     DisconnectReason::kApiRequest));
  EXPECT_FALSE(connmgr()->Disconnect(peer_b->identifier(),
                                     DisconnectReason::kApiRequest));
}

TEST_F(BrEdrConnectionManagerTest, DisconnectCooldownIncoming) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);

  // Peer successfully connects to us.
  QueueSuccessfulIncomingConn(kTestDevAddr);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_FALSE(IsNotConnected(peer));

  // Disconnect locally from an API Request.
  QueueDisconnection(kConnectionHandle);
  EXPECT_TRUE(
      connmgr()->Disconnect(peer->identifier(), DisconnectReason::kApiRequest));

  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_TRUE(IsNotConnected(peer));

  // Peer tries to connect to us. We should reject the connection.
  auto status_event =
      testing::CommandStatusPacket(hci_spec::kRejectConnectionRequest,
                                   pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto reject_packet = testing::RejectConnectionRequestPacket(
      kTestDevAddr,
      pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_BAD_BD_ADDR);

  EXPECT_CMD_PACKET_OUT(test_device(), reject_packet, &status_event);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_TRUE(IsNotConnected(peer));

  // After the cooldown time, a successful incoming connection can happen.
  RunFor(BrEdrConnectionManager::kLocalDisconnectCooldownDuration);

  QueueRepeatIncomingConn(kTestDevAddr);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_FALSE(IsNotConnected(peer));

  // Can still connect out if we disconnect locally.
  QueueDisconnection(kConnectionHandle);
  EXPECT_TRUE(
      connmgr()->Disconnect(peer->identifier(), DisconnectReason::kApiRequest));
  RETURN_IF_FATAL(RunUntilIdle());

  EXPECT_TRUE(IsNotConnected(peer));

  QueueSuccessfulCreateConnection(peer, kConnectionHandle);
  // Interrogation is short because the peer is already known
  QueueShortInterrogation(kConnectionHandle);

  // Initialize as error to verify that |callback| assigns success.
  hci::Result<> status = ToResult(HostError::kFailed);
  BrEdrConnection* connection = nullptr;
  auto callback = [&status, &connection](auto cb_status, auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status = cb_status;
    connection = std::move(cb_conn_ref);
  };

  // Launch request.
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));

  // Complete connection.
  RETURN_IF_FATAL(RunUntilIdle());

  EXPECT_EQ(fit::ok(), status);
  EXPECT_TRUE(HasConnectionTo(peer, connection));
  EXPECT_FALSE(IsNotConnected(peer));

  // Remote disconnections can reconnect immediately
  test_device()->SendCommandChannelPacket(kDisconnectionComplete);
  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_TRUE(IsNotConnected(peer));

  QueueRepeatIncomingConn(kTestDevAddr);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_FALSE(IsNotConnected(peer));

  // If the reason is not kApiRequest, then the remote peer can reconnect
  // immediately.
  QueueDisconnection(kConnectionHandle);
  EXPECT_TRUE(connmgr()->Disconnect(peer->identifier(),
                                    DisconnectReason::kPairingFailed));
  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_TRUE(IsNotConnected(peer));

  QueueRepeatIncomingConn(kTestDevAddr);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_FALSE(IsNotConnected(peer));

  // Queue disconnection for teardown.
  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest, DisconnectCooldownCancelOnOutgoing) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);

  // Peer successfully connects to us.
  QueueSuccessfulIncomingConn(kTestDevAddr);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_FALSE(IsNotConnected(peer));

  // Disconnect locally from an API Request.
  QueueDisconnection(kConnectionHandle);
  EXPECT_TRUE(
      connmgr()->Disconnect(peer->identifier(), DisconnectReason::kApiRequest));

  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_TRUE(IsNotConnected(peer));

  // Peer tries to connect to us. We should reject the connection.
  auto status_event =
      testing::CommandStatusPacket(hci_spec::kRejectConnectionRequest,
                                   pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto reject_packet = testing::RejectConnectionRequestPacket(
      kTestDevAddr,
      pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_BAD_BD_ADDR);

  EXPECT_CMD_PACKET_OUT(test_device(), reject_packet, &status_event);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_TRUE(IsNotConnected(peer));

  // If we initiate a connection out, then an incoming connection can succeed,
  // even if we fail to make the connection out.
  EXPECT_CMD_PACKET_OUT(
      test_device(), kCreateConnection, &kCreateConnectionRspError);

  // Initialize as ok to verify that |callback| assigns failure
  hci::Result<> status = fit::ok();
  EXPECT_TRUE(
      connmgr()->Connect(peer->identifier(), CALLBACK_EXPECT_FAILURE(status)));
  EXPECT_TRUE(IsInitializing(peer));
  RunUntilIdle();

  // The outgoing connection failed to succeed
  EXPECT_TRUE(IsNotConnected(peer));

  // but an incoming connection can now succeed, since our intent is to connect
  // now
  QueueRepeatIncomingConn(kTestDevAddr);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RETURN_IF_FATAL(RunUntilIdle());
  EXPECT_FALSE(IsNotConnected(peer));

  // Queue disconnection for teardown.
  QueueDisconnection(kConnectionHandle);
}

// If SDP channel creation fails, null channel should be caught and
// not be dereferenced. Search should fail to return results.
TEST_F(BrEdrConnectionManagerTest, SDPChannelCreationFailsGracefully) {
  constexpr l2cap::ChannelId kLocalCId = 0x40;
  constexpr l2cap::ChannelId kRemoteCId = 0x41;

  // Channel creation should fail.
  l2cap()->set_channel_callback(
      [](auto new_chan) { ASSERT_FALSE(new_chan.is_alive()); });

  // Since SDP channel creation fails, search_cb should not be called by SDP.
  auto search_cb = [&](auto, const auto&) { FAIL(); };
  connmgr()->AddServiceSearch(
      sdp::profile::kAudioSink, {sdp::kServiceId}, search_cb);

  QueueSuccessfulIncomingConn();
  l2cap()->set_simulate_open_channel_failure(true);
  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, l2cap::kSDP, kLocalCId, kRemoteCId, kChannelParams);

  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  // Peer should still connect successfully.
  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->identifier(), connmgr()->GetPeerId(kConnectionHandle));
  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  EXPECT_FALSE(IsNotConnected(peer));

  test_device()->SendCommandChannelPacket(kDisconnectionComplete);
  RunUntilIdle();

  EXPECT_TRUE(IsNotConnected(peer));
}

TEST_F(
    BrEdrConnectionManagerTest,
    PendingPacketsNotClearedOnDisconnectAndClearedOnDisconnectionCompleteEvent) {
  constexpr size_t kMaxNumPackets = 1;

  ASSERT_EQ(kMaxNumPackets, kBrEdrBufferInfo.max_num_packets());

  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));
  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle2));

  QueueSuccessfulIncomingConn(kTestDevAddr, kConnectionHandle);
  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->identifier(), connmgr()->GetPeerId(kConnectionHandle));

  QueueSuccessfulIncomingConn(kTestDevAddr2, kConnectionHandle2);
  test_device()->SendCommandChannelPacket(
      testing::ConnectionRequestPacket(kTestDevAddr2));

  RunUntilIdle();

  auto* peer2 = peer_cache()->FindByAddress(kTestDevAddr2);
  ASSERT_TRUE(peer2);
  EXPECT_EQ(peer2->identifier(), connmgr()->GetPeerId(kConnectionHandle2));

  EXPECT_EQ(2 * kIncomingConnTransactions, transaction_count());

  size_t packet_count = 0;
  test_device()->SetDataCallback([&](const auto&) { packet_count++; });

  // Should register connection with ACL Data Channel.
  hci::FakeAclConnection connection_0(
      acl_data_channel(), kConnectionHandle, LinkType::kACL);
  hci::FakeAclConnection connection_1(
      acl_data_channel(), kConnectionHandle2, LinkType::kACL);

  acl_data_channel()->RegisterConnection(connection_0.GetWeakPtr());
  acl_data_channel()->RegisterConnection(connection_1.GetWeakPtr());

  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 0, length 1)
                            LowerBits(kConnectionHandle),
                            UpperBits(kConnectionHandle),
                            // payload length
                            0x01,
                            0x00,
                            // payload
                            1));
  // Create packet to send on |acl_connection_0|
  hci::ACLDataPacketPtr packet_0 = hci::ACLDataPacket::New(
      kConnectionHandle,
      hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
      hci_spec::ACLBroadcastFlag::kPointToPoint,
      /*payload_size=*/1);
  packet_0->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(1);
  connection_0.QueuePacket(std::move(packet_0));
  RunUntilIdle();

  EXPECT_ACL_PACKET_OUT(test_device(),
                        StaticByteBuffer(
                            // ACL data header (handle: 0, length 1)
                            LowerBits(kConnectionHandle2),
                            UpperBits(kConnectionHandle2),
                            // payload length
                            0x01,
                            0x00,
                            // payload
                            1));
  hci::ACLDataPacketPtr packet_1 = hci::ACLDataPacket::New(
      kConnectionHandle2,
      hci_spec::ACLPacketBoundaryFlag::kFirstNonFlushable,
      hci_spec::ACLBroadcastFlag::kPointToPoint,
      /*payload_size=*/1);
  packet_1->mutable_view()->mutable_payload_data()[0] = static_cast<uint8_t>(1);
  connection_1.QueuePacket(std::move(packet_1));
  RunUntilIdle();

  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 1u);
  EXPECT_FALSE(test_device()->AllExpectedDataPacketsSent());

  EXPECT_CMD_PACKET_OUT(test_device(), kDisconnect, &kDisconnectRsp);

  EXPECT_TRUE(
      connmgr()->Disconnect(peer->identifier(), DisconnectReason::kApiRequest));
  RunUntilIdle();

  // Packet for |kConnectionHandle2| should not have been sent before
  // Disconnection Complete event.
  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 1u);
  EXPECT_FALSE(test_device()->AllExpectedDataPacketsSent());

  acl_data_channel()->UnregisterConnection(kConnectionHandle);

  test_device()->SendCommandChannelPacket(kDisconnectionComplete);
  RunUntilIdle();

  EXPECT_TRUE(IsNotConnected(peer));

  // Disconnection Complete handler should clear controller packet counts, so
  // packet for |kConnectionHandle2| should be sent
  EXPECT_EQ(connection_0.queued_packets().size(), 0u);
  EXPECT_EQ(connection_1.queued_packets().size(), 0u);
  EXPECT_TRUE(test_device()->AllExpectedDataPacketsSent());

  // Connection handle |kConnectionHandle| should have been unregistered with
  // ACL Data Channel.
  QueueDisconnection(kConnectionHandle2);
}

TEST_F(BrEdrConnectionManagerTest, PairUnconnectedPeer) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());
  ASSERT_EQ(peer_cache()->count(), 1u);
  uint count_cb_called = 0;
  auto cb = [&count_cb_called](hci::Result<> status) {
    ASSERT_EQ(ToResult(bt::HostError::kNotFound), status);
    count_cb_called++;
  };
  connmgr()->Pair(peer->identifier(), kNoSecurityRequirements, cb);
  ASSERT_EQ(count_cb_called, 1u);
}

TEST_F(BrEdrConnectionManagerTest, Pair) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bonded());

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Approve pairing requests.
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) {
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  QueueSuccessfulPairing();

  // Make the pairing error a "bad" error to confirm the callback is called at
  // the end of the pairing process.
  hci::Result<> pairing_status = ToResult(HostError::kPacketMalformed);
  auto pairing_complete_cb = [&pairing_status](hci::Result<> status) {
    ASSERT_EQ(fit::ok(), status);
    pairing_status = status;
  };

  ASSERT_FALSE(l2cap()->AutosniffIsSuppressed(kConnectionHandle));
  connmgr()->Pair(
      peer->identifier(), kNoSecurityRequirements, pairing_complete_cb);
  ASSERT_TRUE(l2cap()->AutosniffIsSuppressed(kConnectionHandle));
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bonded());
  RunUntilIdle();
  ASSERT_FALSE(l2cap()->AutosniffIsSuppressed(kConnectionHandle));

  ASSERT_EQ(fit::ok(), pairing_status);
  ASSERT_TRUE(IsConnected(peer));
  ASSERT_TRUE(peer->bonded());

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest, PairTwice) {
  QueueSuccessfulIncomingConn();

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bonded());

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Approve pairing requests.
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) {
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  QueueSuccessfulPairing();

  // Make the pairing error a "bad" error to confirm the callback is called at
  // the end of the pairing process.
  hci::Result<> pairing_status = ToResult(HostError::kPacketMalformed);
  auto pairing_complete_cb = [&pairing_status](hci::Result<> status) {
    ASSERT_EQ(fit::ok(), status);
    pairing_status = status;
  };

  connmgr()->Pair(
      peer->identifier(), kNoSecurityRequirements, pairing_complete_cb);
  RunUntilIdle();

  ASSERT_EQ(fit::ok(), pairing_status);
  ASSERT_TRUE(IsConnected(peer));
  ASSERT_TRUE(peer->bonded());

  pairing_status = ToResult(HostError::kPacketMalformed);
  connmgr()->Pair(
      peer->identifier(), kNoSecurityRequirements, pairing_complete_cb);

  // Note that we do not call QueueSuccessfulPairing twice, even though we pair
  // twice - this is to test that pairing on an already-paired link succeeds
  // without sending any messages to the peer.
  RunUntilIdle();
  ASSERT_EQ(fit::ok(), pairing_status);
  ASSERT_TRUE(peer->bonded());

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest,
       OpenL2capChannelCreatesChannelWithChannelParameters) {
  constexpr l2cap::Psm kPsm = l2cap::kAVDTP;
  constexpr l2cap::ChannelId kLocalId = l2cap::kFirstDynamicChannelId;
  l2cap::ChannelParameters params;
  params.mode =
      l2cap::RetransmissionAndFlowControlMode::kEnhancedRetransmission;
  params.max_rx_sdu_size = l2cap::kMinACLMTU;

  QueueSuccessfulIncomingConn(kTestDevAddr, kConnectionHandle);
  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->identifier(), connmgr()->GetPeerId(kConnectionHandle));

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());
  // Approve pairing requests.
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) { confirm_cb(true); });
  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  QueueSuccessfulPairing();
  RunUntilIdle();

  l2cap()->ExpectOutboundL2capChannel(
      kConnectionHandle, kPsm, kLocalId, 0x41, params);

  std::optional<l2cap::ChannelInfo> chan_info;
  size_t sock_cb_count = 0;
  auto sock_cb = [&](auto chan) {
    sock_cb_count++;
    ASSERT_TRUE(chan.is_alive());
    chan_info = chan->info();
  };
  connmgr()->OpenL2capChannel(
      peer->identifier(), kPsm, kNoSecurityRequirements, params, sock_cb);

  RunUntilIdle();
  EXPECT_EQ(1u, sock_cb_count);
  ASSERT_TRUE(chan_info);
  EXPECT_EQ(*params.mode, chan_info->mode);
  EXPECT_EQ(*params.max_rx_sdu_size, chan_info->max_rx_sdu_size);

  QueueDisconnection(kConnectionHandle);
}

// Tests that the connection manager cleans up its connection map correctly
// following a disconnection due to encryption failure.
TEST_F(BrEdrConnectionManagerTest,
       ConnectionCleanUpFollowingEncryptionFailure) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  EXPECT_TRUE(peer->temporary());

  // Queue up the connection
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kCreateConnection,
                        &kCreateConnectionRsp,
                        &kConnectionComplete);
  QueueSuccessfulInterrogation(peer->address(), kConnectionHandle);
  QueueDisconnection(kConnectionHandle,
                     pw::bluetooth::emboss::StatusCode::AUTHENTICATION_FAILURE);

  // Initialize as error to verify that |callback| assigns success.
  hci::Result<> status = ToResult(HostError::kFailed);
  BrEdrConnection* conn_ref = nullptr;
  auto callback = [&status, &conn_ref](auto cb_status, auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
  };

  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(peer->bredr());
  RunUntilIdle();
  ASSERT_EQ(fit::ok(), status);

  test_device()->SendCommandChannelPacket(testing::EncryptionChangeEventPacket(
      pw::bluetooth::emboss::StatusCode::CONNECTION_TERMINATED_MIC_FAILURE,
      kConnectionHandle,
      hci_spec::EncryptionStatus::kOff));
  test_device()->SendCommandChannelPacket(testing::DisconnectionCompletePacket(
      kConnectionHandle,
      pw::bluetooth::emboss::StatusCode::CONNECTION_TERMINATED_MIC_FAILURE));
  RunUntilIdle();

  EXPECT_TRUE(IsNotConnected(peer));
}

TEST_F(BrEdrConnectionManagerTest, OpenL2capChannelUpgradesLinkKey) {
  QueueSuccessfulIncomingConn(kTestDevAddr, kConnectionHandle);
  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->identifier(), connmgr()->GetPeerId(kConnectionHandle));

  FakePairingDelegate pairing_delegate_no_io(
      sm::IOCapability::kNoInputNoOutput);
  connmgr()->SetPairingDelegate(pairing_delegate_no_io.GetWeakPtr());
  pairing_delegate_no_io.SetConfirmPairingCallback(
      [&peer](PeerId peer_id, auto cb) {
        EXPECT_EQ(peer->identifier(), peer_id);
        ASSERT_TRUE(cb);
        cb(true);
      });
  pairing_delegate_no_io.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  size_t sock_cb_count = 0;
  auto sock_cb = [&](auto chan_sock) {
    sock_cb_count++;
    EXPECT_TRUE(chan_sock.is_alive());
  };

  // Pairing caused by missing link key.
  QueueSuccessfulUnauthenticatedPairing();

  constexpr auto kPsm0 = l2cap::kHIDControl;
  constexpr l2cap::ChannelId kLocalId0 = l2cap::kFirstDynamicChannelId;
  constexpr l2cap::ChannelId kRemoteId0 = 0x41;
  l2cap()->ExpectOutboundL2capChannel(kConnectionHandle,
                                      kPsm0,
                                      kLocalId0,
                                      kRemoteId0,
                                      l2cap::ChannelParameters());
  connmgr()->OpenL2capChannel(peer->identifier(),
                              kPsm0,
                              kNoSecurityRequirements,
                              l2cap::ChannelParameters(),
                              sock_cb);

  RunUntilIdle();
  EXPECT_EQ(1u, sock_cb_count);

  // New pairing delegate with display can support authenticated pairing.
  FakePairingDelegate pairing_delegate_with_display(
      sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate_with_display.GetWeakPtr());
  pairing_delegate_with_display.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) { confirm_cb(true); });
  pairing_delegate_with_display.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  // Pairing caused by insufficient link key.
  QueueSuccessfulPairing();

  constexpr auto kPsm1 = l2cap::kHIDInteerup;
  constexpr l2cap::ChannelId kLocalId1 = kLocalId0 + 1;
  constexpr l2cap::ChannelId kRemoteId1 = kRemoteId0 + 1;
  l2cap()->ExpectOutboundL2capChannel(kConnectionHandle,
                                      kPsm1,
                                      kLocalId1,
                                      kRemoteId1,
                                      l2cap::ChannelParameters());
  connmgr()->OpenL2capChannel(peer->identifier(),
                              kPsm1,
                              kAuthSecurityRequirements,
                              l2cap::ChannelParameters(),
                              sock_cb);

  RunUntilIdle();
  EXPECT_EQ(2u, sock_cb_count);

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest, OpenL2capChannelUpgradeLinkKeyFails) {
  QueueSuccessfulIncomingConn(kTestDevAddr, kConnectionHandle);
  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->identifier(), connmgr()->GetPeerId(kConnectionHandle));

  FakePairingDelegate pairing_delegate_no_io(
      sm::IOCapability::kNoInputNoOutput);
  connmgr()->SetPairingDelegate(pairing_delegate_no_io.GetWeakPtr());
  pairing_delegate_no_io.SetConfirmPairingCallback(
      [&peer](PeerId peer_id, auto cb) {
        EXPECT_EQ(peer->identifier(), peer_id);
        ASSERT_TRUE(cb);
        cb(true);
      });
  pairing_delegate_no_io.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  size_t sock_cb_count = 0;
  auto sock_cb = [&](auto chan_sock) {
    if (sock_cb_count == 0) {
      EXPECT_TRUE(chan_sock.is_alive());
    } else {
      // Second OpenL2capChannel fails due to insufficient security.
      EXPECT_FALSE(chan_sock.is_alive());
    }
    sock_cb_count++;
  };

  // Initial pairing.
  QueueSuccessfulUnauthenticatedPairing();

  constexpr auto kPsm0 = l2cap::kHIDControl;
  constexpr l2cap::ChannelId kLocalId = l2cap::kFirstDynamicChannelId;
  constexpr l2cap::ChannelId kRemoteId = 0x41;
  l2cap()->ExpectOutboundL2capChannel(kConnectionHandle,
                                      kPsm0,
                                      kLocalId,
                                      kRemoteId,
                                      l2cap::ChannelParameters());
  connmgr()->OpenL2capChannel(peer->identifier(),
                              kPsm0,
                              kNoSecurityRequirements,
                              l2cap::ChannelParameters(),
                              sock_cb);

  RunUntilIdle();
  EXPECT_EQ(1u, sock_cb_count);

  // Pairing caused by insufficient link key.
  QueueSuccessfulUnauthenticatedPairing();

  constexpr auto kPsm1 = l2cap::kHIDInteerup;

  connmgr()->OpenL2capChannel(peer->identifier(),
                              kPsm1,
                              kAuthSecurityRequirements,
                              l2cap::ChannelParameters(),
                              sock_cb);

  RunUntilIdle();
  EXPECT_EQ(2u, sock_cb_count);

  // Pairing should not be attempted a third time.

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest,
       OpenScoConnectionWithoutExistingBrEdrConnectionFails) {
  std::optional<sco::ScoConnectionManager::OpenConnectionResult> conn_result;
  auto conn_cb = [&conn_result](auto result) {
    conn_result = std::move(result);
  };
  auto handle = connmgr()->OpenScoConnection(
      PeerId(1),
      {bt::StaticPacket<
          pw::bluetooth::emboss::SynchronousConnectionParametersWriter>{}},
      std::move(conn_cb));
  EXPECT_FALSE(handle.has_value());
  ASSERT_TRUE(conn_result.has_value());
  ASSERT_TRUE(conn_result->is_error());
  EXPECT_EQ(conn_result->error_value(), HostError::kNotFound);
}

TEST_F(BrEdrConnectionManagerTest, OpenScoConnectionInitiator) {
  QueueSuccessfulIncomingConn();
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();
  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);

  bt::StaticPacket<pw::bluetooth::emboss::SynchronousConnectionParametersWriter>
      kScoConnection;
  kScoConnection.SetToZeros();
  constexpr hci_spec::ConnectionHandle kScoConnectionHandle = 0x41;
  auto setup_status_packet = testing::CommandStatusPacket(
      hci_spec::kEnhancedSetupSynchronousConnection,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto conn_complete_packet = testing::SynchronousConnectionCompletePacket(
      kScoConnectionHandle,
      peer->address(),
      hci_spec::LinkType::kExtendedSCO,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      testing::EnhancedSetupSynchronousConnectionPacket(kConnectionHandle, {}),
      &setup_status_packet,
      &conn_complete_packet);

  std::optional<sco::ScoConnectionManager::OpenConnectionResult> conn_result;
  auto conn_cb = [&conn_result](auto result) {
    conn_result = std::move(result);
  };

  auto req_handle = connmgr()->OpenScoConnection(
      peer->identifier(), {kScoConnection}, std::move(conn_cb));

  RunUntilIdle();
  ASSERT_TRUE(conn_result.has_value());
  ASSERT_TRUE(conn_result->is_ok());
  EXPECT_EQ(conn_result->value()->handle(), kScoConnectionHandle);

  // Disconnecting from a peer should first disconnect SCO connections, then
  // disconnect the ACL connection.
  QueueDisconnection(kScoConnectionHandle);
  QueueDisconnection(kConnectionHandle);
  connmgr()->Disconnect(peer->identifier(), DisconnectReason::kApiRequest);
  RunUntilIdle();
}

class ScoLinkTypesTest
    : public BrEdrConnectionManagerTest,
      public ::testing::WithParamInterface<hci_spec::LinkType> {};

TEST_P(ScoLinkTypesTest, OpenScoConnectionResponder) {
  QueueSuccessfulIncomingConn();
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();
  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);

  bt::StaticPacket<pw::bluetooth::emboss::SynchronousConnectionParametersWriter>
      sco_conn_params;
  if (GetParam() == hci_spec::LinkType::kSCO) {
    sco_conn_params.view().packet_types().hv3().Write(true);
  } else {
    sco_conn_params.view().packet_types().ev3().Write(true);
  }
  std::optional<sco::ScoConnectionManager::AcceptConnectionResult> conn_result;
  auto conn_cb =
      [&conn_result](sco::ScoConnectionManager::AcceptConnectionResult result) {
        EXPECT_TRUE(result.is_ok());
        conn_result = std::move(result);
      };
  auto req_handle = connmgr()->AcceptScoConnection(
      peer->identifier(), {sco_conn_params}, std::move(conn_cb));

  auto conn_req_packet = testing::ConnectionRequestPacket(
      peer->address(), /*link_type=*/GetParam());
  test_device()->SendCommandChannelPacket(conn_req_packet);

  auto accept_status_packet = testing::CommandStatusPacket(
      hci_spec::kEnhancedAcceptSynchronousConnectionRequest,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      testing::EnhancedAcceptSynchronousConnectionRequestPacket(
          peer->address(), sco_conn_params),
      &accept_status_packet);
  RunUntilIdle();
  EXPECT_FALSE(conn_result.has_value());

  constexpr hci_spec::ConnectionHandle kScoConnectionHandle = 0x41;
  test_device()->SendCommandChannelPacket(
      testing::SynchronousConnectionCompletePacket(
          kScoConnectionHandle,
          peer->address(),
          /*link_type=*/GetParam(),
          pw::bluetooth::emboss::StatusCode::SUCCESS));

  RunUntilIdle();
  ASSERT_TRUE(conn_result.has_value());
  ASSERT_TRUE(conn_result->is_ok());
  EXPECT_EQ(conn_result->value().first->handle(), kScoConnectionHandle);

  // Disconnecting from a peer should first disconnect SCO connections, then
  // disconnect the ACL connection.
  QueueDisconnection(kScoConnectionHandle);
  QueueDisconnection(kConnectionHandle);
  connmgr()->Disconnect(peer->identifier(), DisconnectReason::kApiRequest);
  RunUntilIdle();
}

INSTANTIATE_TEST_SUITE_P(BrEdrConnectionManagerTest,
                         ScoLinkTypesTest,
                         ::testing::Values(hci_spec::LinkType::kSCO,
                                           hci_spec::LinkType::kExtendedSCO));

class UnconnectedLinkTypesTest
    : public BrEdrConnectionManagerTest,
      public ::testing::WithParamInterface<hci_spec::LinkType> {};

// Test that an unexpected SCO connection request is rejected for
// kUnacceptableConnectionParameters
TEST_P(UnconnectedLinkTypesTest, RejectUnsupportedSCOConnectionRequests) {
  auto status_event = testing::CommandStatusPacket(
      hci_spec::kRejectSynchronousConnectionRequest,
      pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto complete_event = testing::ConnectionCompletePacket(
      kTestDevAddr,
      kConnectionHandle,
      pw::bluetooth::emboss::StatusCode::UNACCEPTABLE_CONNECTION_PARAMETERS);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::RejectSynchronousConnectionRequest(
                            kTestDevAddr,
                            pw::bluetooth::emboss::StatusCode::
                                UNACCEPTABLE_CONNECTION_PARAMETERS),
                        &status_event,
                        &complete_event);
  test_device()->SendCommandChannelPacket(
      testing::ConnectionRequestPacket(kTestDevAddr, /*link_type=*/GetParam()));
  RunUntilIdle();
}

INSTANTIATE_TEST_SUITE_P(BrEdrConnectionManagerTest,
                         UnconnectedLinkTypesTest,
                         ::testing::Values(hci_spec::LinkType::kSCO,
                                           hci_spec::LinkType::kExtendedSCO));

// Test that an unexpected link type connection request is rejected for
// kUnsupportedFeatureOrParameter
TEST_F(BrEdrConnectionManagerTest, RejectUnsupportedConnectionRequest) {
  auto linktype = static_cast<hci_spec::LinkType>(0x09);
  auto status_event =
      testing::CommandStatusPacket(hci_spec::kRejectConnectionRequest,
                                   pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto complete_event = testing::ConnectionCompletePacket(
      kTestDevAddr,
      kConnectionHandle,
      pw::bluetooth::emboss::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
  EXPECT_CMD_PACKET_OUT(
      test_device(),
      testing::RejectConnectionRequestPacket(
          kTestDevAddr,
          pw::bluetooth::emboss::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER),
      &status_event,
      &complete_event);
  test_device()->SendCommandChannelPacket(
      testing::ConnectionRequestPacket(kTestDevAddr, linktype));
  RunUntilIdle();
}

TEST_F(BrEdrConnectionManagerTest, IncomingConnectionRacesOutgoing) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  ASSERT_TRUE(peer->bredr() && IsNotConnected(peer));

  hci::Result<> status = ToResult(HostError::kFailed);
  BrEdrConnection* conn_ref = nullptr;
  auto should_succeed = [&status, &conn_ref](auto cb_status, auto cb_conn_ref) {
    // We expect this callback to be executed, with a succesful connection
    EXPECT_TRUE(cb_conn_ref);
    EXPECT_EQ(fit::ok(), cb_status);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
  };

  // A client calls Connect() for the Peer, beginning an outgoing connection. We
  // expect a CreateConnection, and ack with a status response but don't
  // complete yet
  EXPECT_CMD_PACKET_OUT(
      test_device(), kCreateConnection, &kCreateConnectionRsp);
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), should_succeed));

  // Meanwhile, an incoming connection is requested from the Peer
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  // We expect it to be accepted, and then return a command status response, but
  // not a ConnectionComplete event yet
  EXPECT_CMD_PACKET_OUT(
      test_device(), kAcceptConnectionRequest, &kAcceptConnectionRequestRsp);
  RunUntilIdle();

  // The controller now establishes the link, but will respond to the outgoing
  // connection with the hci error: `ConnectionAlreadyExists` First, the
  // controller notifies us of the failed outgoing connection - as from its
  // perspective, we've already connected
  const auto complete_already = testing::ConnectionCompletePacket(
      kTestDevAddr,
      kConnectionHandle,
      pw::bluetooth::emboss::StatusCode::CONNECTION_ALREADY_EXISTS);
  test_device()->SendCommandChannelPacket(complete_already);
  // Then the controller notifies us of the successful incoming connection
  test_device()->SendCommandChannelPacket(kConnectionComplete);
  // We expect to connect and begin interrogation, and for our connect()
  // callback to have been run
  QueueSuccessfulInterrogation(kTestDevAddr, kConnectionHandle);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);

  // Peers are marked as initializing until a pairing procedure finishes.
  EXPECT_TRUE(IsInitializing(peer));
  // Prepare for disconnection upon teardown.
  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest, OutgoingConnectionRacesIncoming) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  ASSERT_TRUE(peer->bredr() && IsNotConnected(peer));
  hci::Result<> status = ToResult(HostError::kFailed);
  BrEdrConnection* conn_ref = nullptr;
  auto should_succeed = [&status, &conn_ref](auto cb_status, auto cb_conn_ref) {
    EXPECT_TRUE(cb_conn_ref);
    EXPECT_EQ(fit::ok(), cb_status);
    status = cb_status;
    conn_ref = std::move(cb_conn_ref);
  };

  // An incoming connection is requested from the Peer
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  // We expect it to be accepted, and then return a command status response, but
  // not a ConnectionComplete event yet
  EXPECT_CMD_PACKET_OUT(
      test_device(), kAcceptConnectionRequest, &kAcceptConnectionRequestRsp);
  RunUntilIdle();
  // Meanwhile, a client calls Connect() for the peer. We don't expect any
  // packets out as the connection manager will defer requests that have an
  // active incoming request. Instead, this request will be completed when the
  // incoming procedure completes.
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), should_succeed));
  // We should still expect to connect
  RunUntilIdle();

  // The controller now notifies us of the complete incoming connection
  test_device()->SendCommandChannelPacket(kConnectionComplete);
  // We expect to connect and begin interrogation, and for the callback passed
  // to Connect() to have been executed when the incoming connection succeeded.
  QueueSuccessfulInterrogation(kTestDevAddr, kConnectionHandle);
  RunUntilIdle();
  EXPECT_EQ(fit::ok(), status);

  // Peers are marked as initializing until a pairing procedure finishes.
  EXPECT_TRUE(IsInitializing(peer));
  // Prepare for disconnection upon teardown.
  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest,
       DuplicateIncomingConnectionsFromSamePeerRejected) {
  auto* peer = peer_cache()->NewPeer(kTestDevAddr, /*connectable=*/true);
  ASSERT_TRUE(peer->bredr() && IsNotConnected(peer));

  // Our first request should be accepted - we send back a success status, not
  // the connection complete yet
  EXPECT_CMD_PACKET_OUT(
      test_device(), kAcceptConnectionRequest, &kAcceptConnectionRequestRsp);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  auto status_event =
      testing::CommandStatusPacket(hci_spec::kRejectConnectionRequest,
                                   pw::bluetooth::emboss::StatusCode::SUCCESS);
  auto complete_error = testing::ConnectionCompletePacket(
      kTestDevAddr,
      kConnectionHandle,
      pw::bluetooth::emboss::StatusCode::UNSUPPORTED_FEATURE_OR_PARAMETER);
  auto reject_packet = testing::RejectConnectionRequestPacket(
      kTestDevAddr,
      pw::bluetooth::emboss::StatusCode::CONNECTION_REJECTED_BAD_BD_ADDR);

  // Our second request should be rejected - we already have an incoming request
  EXPECT_CMD_PACKET_OUT(test_device(), reject_packet, &status_event);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  QueueSuccessfulInterrogation(kTestDevAddr, kConnectionHandle);
  test_device()->SendCommandChannelPacket(kConnectionComplete);
  RunUntilIdle();
  test_device()->SendCommandChannelPacket(complete_error);

  RunUntilIdle();

  EXPECT_FALSE(IsNotConnected(peer));

  // Prepare for disconnection upon teardown.
  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest, IncomingRequestInitializesPeer) {
  // Initially, we should not have a peer for the given address
  auto peer = peer_cache()->FindByAddress(kTestDevAddr);
  EXPECT_FALSE(peer);
  // Send a request, and once accepted send back a success status but not the
  // connection complete yet
  EXPECT_CMD_PACKET_OUT(
      test_device(), kAcceptConnectionRequest, &kAcceptConnectionRequestRsp);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  // We should now have a peer in the cache to track our incoming request
  // address The peer is marked as 'Initializing` immediately.
  peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(peer->bredr());
  ASSERT_EQ(peer->bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);
}

#ifndef NINSPECT
TEST_F(BrEdrConnectionManagerTest, Inspect) {
  connmgr()->AttachInspect(inspector().GetRoot(), "bredr_connection_manager");

  // Don't receive connection complete yet in order to keep request pending.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::AcceptConnectionRequestPacket(kTestDevAddr),
                        &kAcceptConnectionRequestRsp);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  auto requests_one_request_matcher = AllOf(
      NodeMatches(NameMatches("connection_requests")),
      ChildrenMatch(ElementsAre(NodeMatches(NameMatches("request_0x0")))));

  auto conn_mgr_with_request_matcher = AllOf(
      NodeMatches(NameMatches("bredr_connection_manager")),
      ChildrenMatch(::testing::IsSupersetOf({requests_one_request_matcher})));

  EXPECT_THAT(inspect::ReadFromVmo(inspector().DuplicateVmo()).value(),
              ChildrenMatch(ElementsAre(conn_mgr_with_request_matcher)));

  QueueSuccessfulInterrogation(kTestDevAddr, kConnectionHandle);
  const auto connection_complete =
      testing::ConnectionCompletePacket(kTestDevAddr, kConnectionHandle);
  test_device()->SendCommandChannelPacket(connection_complete);
  RunUntilIdle();
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_EQ(peer->bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);

  auto empty_requests_matcher =
      AllOf(NodeMatches(NameMatches("connection_requests")),
            ChildrenMatch(::testing::IsEmpty()));

  auto connection_matcher =
      NodeMatches(AllOf(NameMatches("connection_0x1"),
                        PropertyList(ElementsAre(StringIs(
                            "peer_id", peer->identifier().ToString())))));

  auto connections_matcher =
      AllOf(NodeMatches(NameMatches("connections")),
            ChildrenMatch(ElementsAre(connection_matcher)));

  auto recent_conn_list_matcher =
      AllOf(NodeMatches(NameMatches("last_disconnected")),
            ChildrenMatch(::testing::IsEmpty()));

  auto incoming_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("incoming"),
                              PropertyList(UnorderedElementsAre(
                                  UintIs("connection_attempts", 1),
                                  UintIs("failed_connections", 0),
                                  UintIs("successful_connections", 1))))));

  auto outgoing_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("outgoing"),
                              PropertyList(UnorderedElementsAre(
                                  UintIs("connection_attempts", 0),
                                  UintIs("failed_connections", 0),
                                  UintIs("successful_connections", 0))))));

  auto conn_mgr_matcher = AllOf(
      NodeMatches(AllOf(NameMatches("bredr_connection_manager"),
                        PropertyList(UnorderedElementsAre(
                            UintIs("disconnect_acl_link_error_count", 0),
                            UintIs("disconnect_interrogation_failed_count", 0),
                            UintIs("disconnect_local_api_request_count", 0),
                            UintIs("disconnect_pairing_failed_count", 0),
                            UintIs("disconnect_peer_disconnection_count", 0),
                            UintIs("interrogation_complete_count", 1),
                            StringIs("security_mode", "Mode 4"))))),
      ChildrenMatch(UnorderedElementsAre(empty_requests_matcher,
                                         connections_matcher,
                                         recent_conn_list_matcher,
                                         incoming_matcher,
                                         outgoing_matcher)));

  auto hierarchy = inspect::ReadFromVmo(inspector().DuplicateVmo());
  EXPECT_THAT(hierarchy.value(), ChildrenMatch(ElementsAre(conn_mgr_matcher)));

  // Delay disconnect so connection has non-zero duration.
  RunFor(std::chrono::seconds(1));
  QueueDisconnection(kConnectionHandle);
  EXPECT_TRUE(
      connmgr()->Disconnect(peer->identifier(), DisconnectReason::kApiRequest));
  RunUntilIdle();

  auto incoming_matcher_after_disconnect =
      AllOf(NodeMatches(AllOf(NameMatches("incoming"),
                              PropertyList(UnorderedElementsAre(
                                  UintIs("connection_attempts", 1),
                                  UintIs("failed_connections", 0),
                                  UintIs("successful_connections", 1))))));

  auto requests_matcher = AllOf(NodeMatches(NameMatches("connection_requests")),
                                ChildrenMatch(::testing::IsEmpty()));
  auto connections_after_disconnect_matcher =
      AllOf(NodeMatches(NameMatches("connections")),
            ChildrenMatch(::testing::IsEmpty()));
  auto recent_conn_list_after_disconnect_matcher =
      AllOf(NodeMatches(NameMatches("last_disconnected")),
            ChildrenMatch(ElementsAre(NodeMatches(
                AllOf(NameMatches("0"),
                      PropertyList(UnorderedElementsAre(
                          StringIs("peer_id", peer->identifier().ToString()),
                          UintIs("duration_s", 1u),
                          IntIs("@time", 1'000'000'000))))))));

  auto conn_mgr_after_disconnect_matcher = AllOf(
      NodeMatches(AllOf(NameMatches("bredr_connection_manager"),
                        PropertyList(UnorderedElementsAre(
                            UintIs("disconnect_acl_link_error_count", 0),
                            UintIs("disconnect_interrogation_failed_count", 0),
                            UintIs("disconnect_local_api_request_count", 1),
                            UintIs("disconnect_pairing_failed_count", 0),
                            UintIs("disconnect_peer_disconnection_count", 0),
                            UintIs("interrogation_complete_count", 1),
                            StringIs("security_mode", "Mode 4"))))),
      ChildrenMatch(
          UnorderedElementsAre(empty_requests_matcher,
                               connections_after_disconnect_matcher,
                               outgoing_matcher,
                               incoming_matcher_after_disconnect,
                               recent_conn_list_after_disconnect_matcher)));

  hierarchy = inspect::ReadFromVmo(inspector().DuplicateVmo());
  EXPECT_THAT(hierarchy.value(),
              ChildrenMatch(ElementsAre(conn_mgr_after_disconnect_matcher)));
}

// Verify that a failed incoming BR/EDR connection is reflected in inspect data
TEST_F(BrEdrConnectionManagerTest, InspectDataAfterFailedIncomingConnection) {
  connmgr()->AttachInspect(inspector().GetRoot(), "bredr_connection_manager");

  const auto connection_complete_failed = testing::ConnectionCompletePacket(
      kTestDevAddr,
      kConnectionHandle,
      pw::bluetooth::emboss::StatusCode::CONNECTION_TIMEOUT);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        testing::AcceptConnectionRequestPacket(kTestDevAddr),
                        &kAcceptConnectionRequestRsp,
                        &connection_complete_failed);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();

  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);

  auto empty_requests_matcher =
      AllOf(NodeMatches(NameMatches("connection_requests")),
            ChildrenMatch(::testing::IsEmpty()));

  auto connections_matcher = AllOf(NodeMatches(NameMatches("connections")),
                                   ChildrenMatch(::testing::IsEmpty()));

  auto recent_conn_list_matcher =
      AllOf(NodeMatches(NameMatches("last_disconnected")),
            ChildrenMatch(::testing::IsEmpty()));

  auto incoming_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("incoming"),
                              PropertyList(UnorderedElementsAre(
                                  UintIs("connection_attempts", 1),
                                  UintIs("failed_connections", 1),
                                  UintIs("successful_connections", 0))))));

  auto outgoing_matcher =
      AllOf(NodeMatches(AllOf(NameMatches("outgoing"),
                              PropertyList(UnorderedElementsAre(
                                  UintIs("connection_attempts", 0),
                                  UintIs("failed_connections", 0),
                                  UintIs("successful_connections", 0))))));

  auto conn_mgr_matcher = AllOf(
      NodeMatches(AllOf(NameMatches("bredr_connection_manager"),
                        PropertyList(UnorderedElementsAre(
                            UintIs("disconnect_acl_link_error_count", 0),
                            UintIs("disconnect_interrogation_failed_count", 0),
                            UintIs("disconnect_local_api_request_count", 0),
                            UintIs("disconnect_pairing_failed_count", 0),
                            UintIs("disconnect_peer_disconnection_count", 0),
                            UintIs("interrogation_complete_count", 0),
                            StringIs("security_mode", "Mode 4"))))),
      ChildrenMatch(UnorderedElementsAre(empty_requests_matcher,
                                         connections_matcher,
                                         recent_conn_list_matcher,
                                         incoming_matcher,
                                         outgoing_matcher)));

  auto hierarchy = inspect::ReadFromVmo(inspector().DuplicateVmo());
  EXPECT_THAT(hierarchy.value(), ChildrenMatch(ElementsAre(conn_mgr_matcher)));
}
#endif  // NINSPECT

TEST_F(BrEdrConnectionManagerTest, RoleChangeAfterInboundConnection) {
  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));

  QueueSuccessfulIncomingConn();
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();
  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);

  // Request an outbound connection in order to get a pointer to the existing
  // connection. No packets should be sent.
  BrEdrConnection* conn_ref = nullptr;
  auto callback = [&conn_ref](auto /*status*/, auto cb_conn_ref) {
    conn_ref = cb_conn_ref;
  };

  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(conn_ref);
  EXPECT_EQ(conn_ref->link().role(),
            pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  test_device()->SendCommandChannelPacket(testing::RoleChangePacket(
      kTestDevAddr, pw::bluetooth::emboss::ConnectionRole::CENTRAL));
  RunUntilIdle();
  EXPECT_EQ(conn_ref->link().role(),
            pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest,
       RoleChangeWithFailureStatusAfterInboundConnection) {
  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));

  QueueSuccessfulIncomingConn();
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();
  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);

  // Request an outbound connection in order to get a pointer to the existing
  // connection. No packets should be sent.
  BrEdrConnection* conn_ref = nullptr;
  auto callback = [&conn_ref](auto /*status*/, auto cb_conn_ref) {
    conn_ref = cb_conn_ref;
  };
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(conn_ref);
  EXPECT_EQ(conn_ref->link().role(),
            pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  test_device()->SendCommandChannelPacket(testing::RoleChangePacket(
      kTestDevAddr,
      pw::bluetooth::emboss::ConnectionRole::CENTRAL,
      pw::bluetooth::emboss::StatusCode::UNSPECIFIED_ERROR));
  RunUntilIdle();
  // The role should not change.
  EXPECT_EQ(conn_ref->link().role(),
            pw::bluetooth::emboss::ConnectionRole::PERIPHERAL);

  QueueDisconnection(kConnectionHandle);
}

TEST_F(BrEdrConnectionManagerTest, RoleChangeDuringInboundConnectionProcedure) {
  EXPECT_EQ(kInvalidPeerId, connmgr()->GetPeerId(kConnectionHandle));

  QueueSuccessfulIncomingConn(
      kTestDevAddr,
      kConnectionHandle,
      /*role_change=*/pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();
  auto* peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  EXPECT_EQ(peer->bredr()->connection_state(),
            Peer::ConnectionState::kInitializing);

  // Request an outbound connection in order to get a pointer to the existing
  // connection. No packets should be sent.
  BrEdrConnection* conn_ref = nullptr;
  auto callback = [&conn_ref](auto /*status*/, auto cb_conn_ref) {
    conn_ref = cb_conn_ref;
  };
  EXPECT_TRUE(connmgr()->Connect(peer->identifier(), callback));
  ASSERT_TRUE(conn_ref);
  EXPECT_EQ(conn_ref->link().role(),
            pw::bluetooth::emboss::ConnectionRole::CENTRAL);

  QueueDisconnection(kConnectionHandle);
}

// Peer and local Secure Connections (SC) are supported and key is of SC type
TEST_F(BrEdrConnectionManagerTest,
       SecureConnectionsSupportedCorrectLinkKeyTypeSucceeds) {
  const auto kReadRemoteExtended2CompleteSc = StaticByteBuffer(
      hci_spec::kReadRemoteExtendedFeaturesCompleteEventCode,
      0x0D,  // parameter_total_size (13 bytes)
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // status
      0xAA,
      0x0B,  // connection_handle,
      0x02,  // page_number
      0x02,  // max_page_number
      0x00,
      0x01,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00
      // lmp_features_page2: Secure Connections (Controller Support)
  );
  const auto kLinkKeyNotificationSc = MakeLinkKeyNotification(
      hci_spec::LinkKeyType::kAuthenticatedCombination256);
  const StaticByteBuffer kEncryptionChangeEventSc(
      hci_spec::kEncryptionChangeEventCode,
      4,     // parameter total size
      0x00,  // status
      0xAA,
      0x0B,  // connection handle
      0x02   // encryption enabled: AES-CCM for BR/EDR
  );

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Trigger inbound connection and respond to interrogation. LMP features are
  // set to support peer host and controller Secure Connections.
  QueueSuccessfulAccept(kTestDevAddr,
                        kConnectionHandle,
                        pw::bluetooth::emboss::ConnectionRole::CENTRAL);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kRemoteNameRequest,
                        &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteVersionInfo,
                        &kReadRemoteVersionInfoRsp,
                        &kRemoteVersionInfoComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        hci_spec::kReadRemoteSupportedFeatures,
                        &kReadRemoteSupportedFeaturesRsp,
                        &kReadRemoteSupportedFeaturesComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended1,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1Complete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended2,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended2CompleteSc);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  // Ensure that the interrogation has begun but the peer hasn't yet bonded
  EXPECT_EQ(6, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bredr()->bonded());

  // Approve pairing requests
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) {
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  // Initiate pairing from the peer before interrogation completes
  test_device()->SendCommandChannelPacket(MakeIoCapabilityResponse(
      IoCapability::DISPLAY_YES_NO,
      AuthenticationRequirements::MITM_GENERAL_BONDING));
  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);
  const auto kUserConfirmationRequest = MakeUserConfirmationRequest(kPasskey);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::DISPLAY_YES_NO,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp,
                        &kUserConfirmationRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kUserConfirmationRequestReply,
                        &kUserConfirmationRequestReplyRsp,
                        &kSimplePairingCompleteSuccess,
                        &kLinkKeyNotificationSc);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kSetConnectionEncryption,
                        &kSetConnectionEncryptionRsp,
                        &kEncryptionChangeEventSc);
  EXPECT_CMD_PACKET_OUT(
      test_device(), kReadEncryptionKeySize, &kReadEncryptionKeySizeRsp);

  // Configure TestSecurityManager with bonding data so cross-transport key
  // derivation succeeds.
  sm::testing::TestSecurityManager::WeakPtr security_manager =
      security_manager_factory()->GetTestSm(kConnectionHandle);
  ASSERT_TRUE(security_manager.is_alive());
  sm::PairingData pairing_data;
  pairing_data.local_ltk = kLELtk;
  pairing_data.peer_ltk = kLELtk;
  security_manager->set_pairing_data(pairing_data);
  EXPECT_FALSE(peer->le());

  RETURN_IF_FATAL(RunUntilIdle());

  EXPECT_TRUE(l2cap()->IsLinkConnected(kConnectionHandle));
  ASSERT_TRUE(peer->bredr()->bonded());
  ASSERT_TRUE(peer->le());
  ASSERT_TRUE(peer->le()->bond_data().has_value());
  EXPECT_EQ(peer->le()->bond_data().value(), pairing_data);
  EXPECT_TRUE(security_manager->last_identity_info().has_value());

  QueueDisconnection(kConnectionHandle);
}

// Peer and local Secure Connections (SC) are supported, but key is not of SC
// type
TEST_F(BrEdrConnectionManagerTest,
       SecureConnectionsSupportedIncorrectLinkKeyTypeFails) {
  const auto kReadRemoteExtended2CompleteLktf = StaticByteBuffer(
      hci_spec::kReadRemoteExtendedFeaturesCompleteEventCode,
      0x0D,  // parameter_total_size (13 bytes)
      pw::bluetooth::emboss::StatusCode::SUCCESS,  // status
      0xAA,
      0x0B,  // connection_handle,
      0x02,  // page_number
      0x02,  // max_page_number
      0x00,
      0x01,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00
      // lmp_features_page2: Secure Connections (Controller Support)
  );

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Trigger inbound connection and respond to interrogation. LMP features are
  // set to support peer host and controller Secure Connections.
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kAcceptConnectionRequest,
                        &kAcceptConnectionRequestRsp,
                        &kConnectionComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kRemoteNameRequest,
                        &kRemoteNameRequestRsp,
                        &kRemoteNameRequestComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteVersionInfo,
                        &kReadRemoteVersionInfoRsp,
                        &kRemoteVersionInfoComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        hci_spec::kReadRemoteSupportedFeatures,
                        &kReadRemoteSupportedFeaturesRsp,
                        &kReadRemoteSupportedFeaturesComplete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended1,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended1Complete);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kReadRemoteExtended2,
                        &kReadRemoteExtendedFeaturesRsp,
                        &kReadRemoteExtended2CompleteLktf);

  test_device()->SendCommandChannelPacket(kConnectionRequest);

  RunUntilIdle();

  // Ensure that the interrogation has begun but the peer hasn't yet bonded
  EXPECT_EQ(6, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bredr()->bonded());

  // Approve pairing requests
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) {
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  // Initiate pairing from the peer
  test_device()->SendCommandChannelPacket(MakeIoCapabilityResponse(
      IoCapability::DISPLAY_YES_NO,
      AuthenticationRequirements::MITM_GENERAL_BONDING));
  test_device()->SendCommandChannelPacket(kIoCapabilityRequest);
  const auto kUserConfirmationRequest = MakeUserConfirmationRequest(kPasskey);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        MakeIoCapabilityRequestReply(
                            IoCapability::DISPLAY_YES_NO,
                            AuthenticationRequirements::MITM_GENERAL_BONDING),
                        &kIoCapabilityRequestReplyRsp,
                        &kUserConfirmationRequest);
  EXPECT_CMD_PACKET_OUT(test_device(),
                        kUserConfirmationRequestReply,
                        &kUserConfirmationRequestReplyRsp,
                        &kSimplePairingCompleteSuccess,
                        &kLinkKeyNotification);

  // Connection terminates because kLinkKeyNotification's key type is
  // kAuthenticatedCombination192. When SC is supported, key type must be of SC
  // type (kUnauthenticatedCombination256 or kAuthenticatedCombination256).
  QueueDisconnection(kConnectionHandle);
  RETURN_IF_FATAL(RunUntilIdle());
}

// Active connections that do not meeting the requirements for Secure
// Connections Only mode are disconnected when the security mode is changed to
// SC Only.
TEST_F(BrEdrConnectionManagerTest,
       SecureConnectionsOnlyDisconnectsInsufficientSecurity) {
  QueueSuccessfulIncomingConn();
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();
  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bonded());

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Approve pairing requests.
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) {
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  QueueSuccessfulPairing();  // kAuthenticatedCombination192 default is not of
                             // SC type

  // Initialize as error to verify that |pairing_complete_cb| assigns success.
  hci::Result<> pairing_status = ToResult(HostError::kInsufficientSecurity);
  auto pairing_complete_cb = [&pairing_status](hci::Result<> status) {
    ASSERT_EQ(fit::ok(), status);
    pairing_status = status;
  };

  connmgr()->Pair(
      peer->identifier(), kNoSecurityRequirements, pairing_complete_cb);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bonded());
  RunUntilIdle();

  ASSERT_EQ(fit::ok(), pairing_status);
  ASSERT_TRUE(IsConnected(peer));
  ASSERT_TRUE(peer->bonded());

  // Setting Secure Connections Only mode causes connections not allowed under
  // this mode to be disconnected. In this case, |peer| is encrypted,
  // authenticated, but not SC-generated.
  EXPECT_CMD_PACKET_OUT(test_device(), kDisconnect);
  connmgr()->SetSecurityMode(BrEdrSecurityMode::SecureConnectionsOnly);
  RunUntilIdle();
  EXPECT_EQ(BrEdrSecurityMode::SecureConnectionsOnly,
            connmgr()->security_mode());
  ASSERT_TRUE(IsNotConnected(peer));
}

// Active connections that meet the requirements for Secure Connections Only
// mode are not disconnected when the security mode is changed to SC Only.
TEST_F(BrEdrConnectionManagerTest,
       SecureConnectionsOnlySufficientSecuritySucceeds) {
  QueueSuccessfulIncomingConn();
  test_device()->SendCommandChannelPacket(kConnectionRequest);
  RunUntilIdle();
  EXPECT_EQ(kIncomingConnTransactions, transaction_count());
  auto* const peer = peer_cache()->FindByAddress(kTestDevAddr);
  ASSERT_TRUE(peer);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bonded());

  FakePairingDelegate pairing_delegate(sm::IOCapability::kDisplayYesNo);
  connmgr()->SetPairingDelegate(pairing_delegate.GetWeakPtr());

  // Approve pairing requests.
  pairing_delegate.SetDisplayPasskeyCallback(
      [](PeerId, uint32_t, auto, auto confirm_cb) {
        ASSERT_TRUE(confirm_cb);
        confirm_cb(true);
      });

  pairing_delegate.SetCompletePairingCallback(
      [](PeerId, sm::Result<> status) { EXPECT_EQ(fit::ok(), status); });

  QueueSuccessfulPairing(hci_spec::LinkKeyType::kAuthenticatedCombination256);

  // Initialize as error to verify that |pairing_complete_cb| assigns success.
  hci::Result<> pairing_status = ToResult(HostError::kInsufficientSecurity);
  auto pairing_complete_cb = [&pairing_status](hci::Result<> status) {
    ASSERT_EQ(fit::ok(), status);
    pairing_status = status;
  };

  connmgr()->Pair(
      peer->identifier(), kNoSecurityRequirements, pairing_complete_cb);
  ASSERT_TRUE(IsInitializing(peer));
  ASSERT_FALSE(peer->bonded());
  RunUntilIdle();

  ASSERT_EQ(fit::ok(), pairing_status);
  ASSERT_TRUE(IsConnected(peer));
  ASSERT_TRUE(peer->bonded());

  // Setting Secure Connections Only mode causes connections not allowed under
  // this mode to be disconnected. In this case, |peer| is encrypted,
  // authenticated, and SC-generated.
  connmgr()->SetSecurityMode(BrEdrSecurityMode::SecureConnectionsOnly);
  RunUntilIdle();
  EXPECT_EQ(BrEdrSecurityMode::SecureConnectionsOnly,
            connmgr()->security_mode());
  ASSERT_TRUE(IsConnected(peer));

  QueueDisconnection(kConnectionHandle);
}

#undef COMMAND_COMPLETE_RSP
#undef COMMAND_STATUS_RSP

}  // namespace
}  // namespace bt::gap
