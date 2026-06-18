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

#include "pw_bluetooth_sapphire/internal/host/sm/security_manager.h"

#include <pw_assert/check.h>
#include <pw_bytes/endian.h>

#include <chrono>
#include <cstdlib>

#include "pw_async/fake_dispatcher.h"
#include "pw_bluetooth_sapphire/fake_lease_provider.h"
#include "pw_bluetooth_sapphire/internal/host/common/macros.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"
#include "pw_bluetooth_sapphire/internal/host/gap/gap.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/link_key.h"
#include "pw_bluetooth_sapphire/internal/host/hci/connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_bredr_connection.h"
#include "pw_bluetooth_sapphire/internal/host/hci/fake_low_energy_connection.h"
#include "pw_bluetooth_sapphire/internal/host/l2cap/fake_channel_test.h"
#include "pw_bluetooth_sapphire/internal/host/sm/ecdh_key.h"
#include "pw_bluetooth_sapphire/internal/host/sm/error.h"
#include "pw_bluetooth_sapphire/internal/host/sm/packet.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/sm/types.h"
#include "pw_bluetooth_sapphire/internal/host/sm/util.h"
#include "pw_bluetooth_sapphire/internal/host/testing/controller_test.h"
#include "pw_bluetooth_sapphire/internal/host/testing/mock_controller.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_packets.h"
#include "pw_bluetooth_sapphire/internal/host/transport/error.h"

// inclusive-language: disable
namespace bt::sm {
namespace {

const DeviceAddress kLocalAddr(DeviceAddress::Type::kLEPublic,
                               {0xA6, 0xA5, 0xA4, 0xA3, 0xA2, 0xA1});
const DeviceAddress kLocalBrEdrAddr(DeviceAddress::Type::kBREDR,
                                    {0xA6, 0xA5, 0xA4, 0xA3, 0xA2, 0xA1});
const DeviceAddress kPeerAddr(DeviceAddress::Type::kLERandom,
                              {0xB6, 0xB5, 0xB4, 0xB3, 0xB2, 0xB1});
const DeviceAddress kPeerPublicAddr(DeviceAddress::Type::kLEPublic,
                                    {0xC6, 0xC5, 0xC4, 0xC3, 0xC2, 0xC1});
const DeviceAddress kPeerBrEdrAddr(DeviceAddress::Type::kBREDR,
                                   {0xB6, 0xB5, 0xB4, 0xB3, 0xB2, 0xB1});

const bt::sm::LTK kAuthenticatedSecureKey(
    sm::SecurityProperties(/*encrypted=*/true,
                           /*authenticated=*/true,
                           /*secure_connections=*/true,
                           sm::kMaxEncryptionKeySize),
    hci_spec::LinkKey(UInt128{4}, 5, 6));

const PeerId kPeerId(2);

const PairingRandomValue kHardCodedPairingRandom = {0x0,
                                                    0x1,
                                                    0x2,
                                                    0x3,
                                                    0x4,
                                                    0x5,
                                                    0x6,
                                                    0x7,
                                                    0x8,
                                                    0x9,
                                                    0xA,
                                                    0xB,
                                                    0xC,
                                                    0xD,
                                                    0xE,
                                                    0xF};

const UInt128 kLinkKeyBytes = {0x00,
                               0x01,
                               0x02,
                               0x03,
                               0x04,
                               0x05,
                               0x06,
                               0x07,
                               0x08,
                               0x09,
                               0x00,
                               0x01,
                               0x02,
                               0x03,
                               0x04,
                               0x05};
const UInt128 kExpectedLtkBytesH7 = {
    0x79,
    0xbc,
    0x11,
    0x32,
    0x13,
    0x8a,
    0x41,
    0x69,
    0xe2,
    0xb3,
    0xcc,
    0x5e,
    0xeb,
    0x09,
    0x5e,
    0xe8,
};

constexpr hci_spec::ConnectionHandle kConnectionHandle(1);

class SecurityManagerTest : public l2cap::testing::FakeChannelTest,
                            public sm::Delegate {
 public:
  SecurityManagerTest() : weak_delegate_(this) {}
  ~SecurityManagerTest() override = default;

 protected:
  void SetUp() override {
    static bool backend_set_up = false;
    if (!backend_set_up) {
      pw::crypto::ecdh::SetUpBackendForTesting();
      backend_set_up = true;
    }

    l2cap::testing::FakeChannelTest::SetUp();
    InitializeTransport();
  }
  void TearDown() override {
    RunUntilIdle();
    DestroySecurityManager();
    fake_le_link_.reset();
    fake_bredr_link_.reset();
    transport_.reset();
    l2cap::testing::FakeChannelTest::TearDown();
  }

  void NewSecurityManager(
      Role role,
      IOCapability ioc,
      BondableMode bondable_mode,
      gap::LESecurityMode security_mode = gap::LESecurityMode::Mode1) {
    // Setup fake SMP channel.
    ChannelOptions options(l2cap::kLESMPChannelId);
    fake_chan_ = CreateFakeChannel(options);
    fake_chan_->SetSendCallback(
        fit::bind_member<&SecurityManagerTest::OnDataReceived>(this),
        dispatcher());

    // Setup a fake logical link.
    auto link_role = role == Role::kInitiator
                         ? pw::bluetooth::emboss::ConnectionRole::CENTRAL
                         : pw::bluetooth::emboss::ConnectionRole::PERIPHERAL;

    if (fake_le_link_) {
      auto status_event = testing::CommandStatusPacket(
          hci_spec::kDisconnect, pw::bluetooth::emboss::StatusCode::SUCCESS);
      auto disconnect_complete =
          testing::DisconnectionCompletePacket(kConnectionHandle);
      EXPECT_CMD_PACKET_OUT(controller_,
                            testing::DisconnectPacket(kConnectionHandle),
                            &status_event,
                            &disconnect_complete);
      fake_le_link_.reset();
      RunUntilIdle();
    }
    fake_le_link_ = std::make_unique<hci::testing::FakeLowEnergyConnection>(
        kConnectionHandle,
        kLocalAddr,
        kPeerAddr,
        link_role,
        transport_->GetWeakPtr(),
        dispatcher());

    if (!peer_) {
      InitializePeer();
    }

    pairing_ = SecurityManager::CreateLE(fake_le_link_->GetWeakPtr(),
                                         fake_chan_->GetWeakPtr(),
                                         ioc,
                                         weak_delegate_.GetWeakPtr(),
                                         bondable_mode,
                                         security_mode,
                                         dispatcher(),
                                         peer_->GetWeakPtr());
  }

  void NewBrEdrSecurityManager(
      Role role,
      bool is_controller_remote_public_key_validation_supported = true) {
    ChannelOptions options(l2cap::kSMPChannelId);
    options.link_type = LinkType::kACL;
    fake_chan_ = CreateFakeChannel(options);
    fake_chan_->SetSendCallback(
        fit::bind_member<&SecurityManagerTest::OnDataReceived>(this),
        dispatcher());

    auto link_role = role == Role::kInitiator
                         ? pw::bluetooth::emboss::ConnectionRole::CENTRAL
                         : pw::bluetooth::emboss::ConnectionRole::PERIPHERAL;

    fake_bredr_link_ = std::make_unique<hci::testing::FakeBrEdrConnection>(
        kConnectionHandle,
        kLocalBrEdrAddr,
        kPeerBrEdrAddr,
        link_role,
        transport_->GetWeakPtr(),
        dispatcher());

    InitializePeer(/*is_bredr=*/true);

    pairing_ = SecurityManager::CreateBrEdr(
        fake_bredr_link_->GetWeakPtr(),
        fake_chan_->GetWeakPtr(),
        weak_delegate_.GetWeakPtr(),
        is_controller_remote_public_key_validation_supported,
        dispatcher(),
        peer_->GetWeakPtr());
  }

  void InitializePeer(bool is_bredr = false) {
    InitializePeer(is_bredr ? kPeerBrEdrAddr : kPeerAddr);
  }

  void InitializePeer(DeviceAddress address) {
    auto listeners_cb = [](const gap::Peer&, gap::Peer::NotifyListenersChange) {
    };
    auto expiry_cb = [](const gap::Peer&) {};
    auto dual_mode_cb = [](const gap::Peer&) {};
    auto store_le_bond_cb = [this](const sm::PairingData& data) {
      pairing_data_callback_count_++;
      pairing_data_ = data;
      return true;
    };
    peer_.emplace(std::move(listeners_cb),
                  std::move(expiry_cb),
                  std::move(dual_mode_cb),
                  std::move(store_le_bond_cb),
                  kPeerId,
                  address,
                  /*connectable=*/true,
                  &peer_metrics_,
                  dispatcher_);
  }

  void DestroySecurityManager() { pairing_ = nullptr; }

  // sm::Delegate override:
  void ConfirmPairing(ConfirmCallback confirm) override {
    if (confirm_delegate_) {
      confirm_delegate_(std::move(confirm));
    } else {
      confirm(true);
    }
  }

  // sm::Delegate override:
  void DisplayPasskey(uint32_t passkey,
                      DisplayMethod method,
                      ConfirmCallback confirm) override {
    if (display_delegate_) {
      display_delegate_(passkey, method, std::move(confirm));
    } else {
      ADD_FAILURE() << "No passkey display delegate set for "
                    << util::DisplayMethodToString(method) << " pairing";
    }
  }

  // sm::Delegate override:
  void RequestPasskey(PasskeyResponseCallback respond) override {
    if (request_passkey_delegate_) {
      request_passkey_delegate_(std::move(respond));
    } else {
      ADD_FAILURE()
          << "No passkey entry delegate set for passkey entry pairing";
    }
  }

  std::optional<IdentityInfo> OnIdentityInformationRequest() override {
    local_id_info_callback_count_++;
    return local_id_info_;
  }

  // Called by |pairing_| when the pairing procedure ends.
  void OnPairingComplete(Result<> status) override {
    pairing_complete_count_++;
    pairing_complete_status_ = status;
  }

  // Called by |pairing_| when any encryption procedure fails.
  void OnAuthenticationFailure(hci::Result<> status) override {
    auth_failure_callback_count_++;
    auth_failure_status_ = status;
  }

  // Called by |pairing_| when the link security properties change.
  void OnNewSecurityProperties(const SecurityProperties& sec) override {
    new_sec_props_count_++;
    new_sec_props_ = sec;
  }

  void UpgradeSecurity(SecurityLevel level) {
    PW_DCHECK(pairing_);
    pairing_->UpgradeSecurity(level, [this](auto status, const auto& props) {
      security_callback_count_++;
      security_status_ = status;
      sec_props_ = props;
    });
  }

  // Called when SMP sends a packet over the fake channel.
  void OnDataReceived(std::unique_ptr<const ByteBuffer> packet) {
    PW_DCHECK(packet);

    PacketReader reader(packet.get());
    switch (reader.code()) {
      case kPairingFailed:
        pairing_failed_count_++;
        received_error_code_ = reader.payload<PairingFailedParams>();
        break;
      case kSecurityRequest:
        security_request_count_++;
        security_auth_req_ = reader.payload<AuthReqField>();
        break;
      case kPairingRequest:
        pairing_request_count_++;
        packet->Copy(&local_pairing_cmd_);
        break;
      case kPairingResponse:
        pairing_response_count_++;
        packet->Copy(&local_pairing_cmd_);
        break;
      case kPairingPublicKey:
        pairing_public_key_count_++;
        public_ecdh_key_ = EcdhKey::ParseFromPublicKey(
            reader.payload<PairingPublicKeyParams>());
        break;
      case kPairingConfirm:
        pairing_confirm_count_++;
        pairing_confirm_ = reader.payload<PairingConfirmValue>();
        break;
      case kPairingRandom:
        pairing_random_count_++;
        pairing_random_ = reader.payload<PairingRandomValue>();
        break;
      case kPairingDHKeyCheck:
        pairing_dhkey_check_count_++;
        pairing_dhkey_check_ = reader.payload<PairingDHKeyCheckValueE>();
        break;
      case kEncryptionInformation:
        enc_info_count_++;
        enc_info_ = reader.payload<EncryptionInformationParams>();
        break;
      case kCentralIdentification: {
        const auto& params = reader.payload<CentralIdentificationParams>();
        central_ident_count_++;
        ediv_ = pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.ediv);
        rand_ = pw::bytes::ConvertOrderFrom(cpp20::endian::little, params.rand);
        break;
      }
      case kIdentityInformation:
        id_info_count_++;
        id_info_ = reader.payload<UInt128>();
        break;
      case kIdentityAddressInformation: {
        const auto& params = reader.payload<IdentityAddressInformationParams>();
        id_addr_info_count_++;
        id_addr_info_ = DeviceAddress(params.type == AddressType::kStaticRandom
                                          ? DeviceAddress::Type::kLERandom
                                          : DeviceAddress::Type::kLEPublic,
                                      params.bd_addr);
        break;
      }
      default:
        FAIL() << "Sent unsupported SMP command";
    }
  }

  // Emulates the receipt of pairing features (both as initiator and responder).
  void ReceivePairingFeatures(const PairingRequestParams& params,
                              bool peer_initiator = false) {
    PacketWriter writer(peer_initiator ? kPairingRequest : kPairingResponse,
                        &peer_pairing_cmd_);
    *writer.mutable_payload<PairingRequestParams>() = params;
    fake_chan()->Receive(peer_pairing_cmd_);
  }

  void ReceivePairingFeatures(IOCapability ioc = IOCapability::kNoInputNoOutput,
                              AuthReqField auth_req = 0,
                              uint8_t max_enc_key_size = kMaxEncryptionKeySize,
                              bool peer_initiator = false) {
    PairingRequestParams pairing_params;
    std::memset(&pairing_params, 0, sizeof(pairing_params));
    pairing_params.io_capability = ioc;
    pairing_params.auth_req = auth_req;
    pairing_params.max_encryption_key_size = max_enc_key_size;

    ReceivePairingFeatures(pairing_params, peer_initiator);
  }

  void ReceivePairingFailed(ErrorCode error_code) {
    StaticByteBuffer<sizeof(Header) + sizeof(ErrorCode)> buffer;
    PacketWriter writer(kPairingFailed, &buffer);
    *writer.mutable_payload<PairingFailedParams>() = error_code;
    fake_chan()->Receive(buffer);
  }

  void ReceivePairingPublicKey(const PairingPublicKeyParams& key) {
    StaticByteBuffer<util::PacketSize<PairingPublicKeyParams>()> buffer;
    PacketWriter writer(kPairingPublicKey, &buffer);
    std::memcpy(
        writer.mutable_payload_bytes(), &key, sizeof(PairingPublicKeyParams));
    fake_chan()->Receive(buffer);
  }

  void ReceivePairingConfirm(const UInt128& confirm) {
    Receive128BitCmd(kPairingConfirm, confirm);
  }

  void ReceivePairingRandom(const UInt128& random) {
    Receive128BitCmd(kPairingRandom, random);
  }

  void ReceivePairingDHKeyCheck(const UInt128& check) {
    Receive128BitCmd(kPairingDHKeyCheck, check);
  }

  void ReceiveEncryptionInformation(const UInt128& ltk) {
    Receive128BitCmd(kEncryptionInformation, ltk);
  }

  void ReceiveCentralIdentification(uint64_t random, uint16_t ediv) {
    StaticByteBuffer<sizeof(Header) + sizeof(CentralIdentificationParams)>
        buffer;
    PacketWriter writer(kCentralIdentification, &buffer);
    auto* params = writer.mutable_payload<CentralIdentificationParams>();
    params->ediv = pw::bytes::ConvertOrderTo(cpp20::endian::little, ediv);
    params->rand = pw::bytes::ConvertOrderTo(cpp20::endian::little, random);
    fake_chan()->Receive(buffer);
  }

  void ReceiveIdentityResolvingKey(const UInt128& irk) {
    Receive128BitCmd(kIdentityInformation, irk);
  }

  void ReceiveNonBondablePairingResponse() {
    // clang-format off
    const StaticByteBuffer kResponse(
      0x02,  // code: Pairing Response
      0x00,  // IO cap.: DisplayOnly
      0x00,  // OOB: not present
      0x00,  // AuthReq: no bonding, MITM not required
      0x07,  // encr. key size: 7 (default min)
      0x00,  // initiator keys: none
      0x00   // responder keys: none - nonbondable mode
    );
    // clang-format on
    fake_chan()->Receive(kResponse);
  }

  void ReceiveIdentityAddress(const DeviceAddress& address) {
    StaticByteBuffer<sizeof(Header) + sizeof(IdentityAddressInformationParams)>
        buffer;
    PacketWriter writer(kIdentityAddressInformation, &buffer);
    auto* params = writer.mutable_payload<IdentityAddressInformationParams>();
    params->type = address.type() == DeviceAddress::Type::kLEPublic
                       ? AddressType::kPublic
                       : AddressType::kStaticRandom;
    params->bd_addr = address.value();
    fake_chan()->Receive(buffer);
  }

  void ReceiveSecurityRequest(AuthReqField auth_req = 0u) {
    StaticByteBuffer<sizeof(Header) + sizeof(AuthReqField)> buffer;
    buffer[0] = kSecurityRequest;
    buffer[1] = auth_req;
    fake_chan()->Receive(buffer);
    RunUntilIdle();
  }

  void GenerateLegacyConfirmValue(const UInt128& random,
                                  UInt128* out_value,
                                  bool peer_initiator = false,
                                  uint32_t tk = 0) {
    PW_DCHECK(out_value);

    tk = pw::bytes::ConvertOrderTo(cpp20::endian::little, tk);
    UInt128 tk128;
    tk128.fill(0);
    std::memcpy(tk128.data(), &tk, sizeof(tk));

    const ByteBuffer *preq, *pres;
    const DeviceAddress *init_addr, *rsp_addr;
    if (peer_initiator) {
      preq = &peer_pairing_cmd();
      pres = &local_pairing_cmd();
      init_addr = &kPeerAddr;
      rsp_addr = &kLocalAddr;
    } else {
      preq = &local_pairing_cmd();
      pres = &peer_pairing_cmd();
      init_addr = &kLocalAddr;
      rsp_addr = &kPeerAddr;
    }

    std::optional<UInt128> result =
        util::C1(tk128, random, *preq, *pres, *init_addr, *rsp_addr);
    PW_CHECK(result.has_value());
    *out_value = *result;
  }

  UInt128 GenerateScConfirmValue(const LocalEcdhKey& peer_key,
                                 const UInt128& random,
                                 Role local_role,
                                 bool gen_initiator_confirm,
                                 uint8_t r = 0) {
    PW_CHECK(public_ecdh_key_.has_value(),
             "cannot compute confirm, missing key!");
    UInt256 pka = public_ecdh_key_->GetPublicKeyX(),
            pkb = peer_key.GetPublicKeyX();
    if (local_role == Role::kResponder) {
      std::swap(pka, pkb);
    }
    return gen_initiator_confirm ? util::F4(pka, pkb, random, r).value()
                                 : util::F4(pkb, pka, random, r).value();
  }

  SecurityManager* pairing() const { return pairing_.get(); }
  l2cap::testing::FakeChannel* fake_chan() const { return fake_chan_.get(); }
  hci::testing::FakeLowEnergyConnection* fake_link() const {
    return fake_le_link_.get();
  }

  int security_callback_count() const { return security_callback_count_; }
  const std::optional<ErrorCode>& received_error_code() const {
    return received_error_code_;
  }
  const Result<>& security_status() const { return security_status_; }
  const SecurityProperties& sec_props() const { return sec_props_; }

  int pairing_complete_count() const { return pairing_complete_count_; }
  const Result<>& pairing_complete_status() const {
    return pairing_complete_status_;
  }

  int pairing_data_callback_count() const {
    return pairing_data_callback_count_;
  }

  int auth_failure_callback_count() const {
    return auth_failure_callback_count_;
  }
  const hci::Result<>& auth_failure_status() const {
    return auth_failure_status_;
  }

  int local_id_info_callback_count() const {
    return local_id_info_callback_count_;
  }

  int new_sec_props_count() const { return new_sec_props_count_; }
  const SecurityProperties& new_sec_props() const { return new_sec_props_; }

  const std::optional<LTK>& peer_ltk() const { return pairing_data_.peer_ltk; }
  const std::optional<LTK>& local_ltk() const {
    return pairing_data_.local_ltk;
  }
  const std::optional<Key>& irk() const { return pairing_data_.irk; }
  const std::optional<DeviceAddress>& identity() const {
    return pairing_data_.identity_address;
  }
  const std::optional<Key>& csrk() const { return pairing_data_.csrk; }

  using ConfirmDelegate = fit::function<void(ConfirmCallback)>;
  void set_confirm_delegate(ConfirmDelegate delegate) {
    confirm_delegate_ = std::move(delegate);
  }

  using DisplayDelegate =
      fit::function<void(uint32_t, DisplayMethod, ConfirmCallback)>;
  void set_display_delegate(DisplayDelegate delegate) {
    display_delegate_ = std::move(delegate);
  }

  // sm::Delegate override:
  using RequestPasskeyDelegate = fit::function<void(PasskeyResponseCallback)>;
  void set_request_passkey_delegate(RequestPasskeyDelegate delegate) {
    request_passkey_delegate_ = std::move(delegate);
  }

  void set_local_id_info(std::optional<IdentityInfo> info) {
    local_id_info_ = info;
  }

  int security_request_count() const { return security_request_count_; }
  int pairing_failed_count() const { return pairing_failed_count_; }
  int pairing_request_count() const { return pairing_request_count_; }
  int pairing_response_count() const { return pairing_response_count_; }
  int pairing_public_key_count() const { return pairing_public_key_count_; }
  int pairing_confirm_count() const { return pairing_confirm_count_; }
  int pairing_random_count() const { return pairing_random_count_; }
  int pairing_dhkey_check_count() const { return pairing_dhkey_check_count_; }
  int enc_info_count() const { return enc_info_count_; }
  int id_info_count() const { return id_info_count_; }
  int id_addr_info_count() const { return id_addr_info_count_; }
  int central_ident_count() const { return central_ident_count_; }

  AuthReqField security_request_payload() const { return security_auth_req_; }
  const std::optional<EcdhKey>& public_ecdh_key() const {
    return public_ecdh_key_;
  }
  const UInt128& pairing_confirm() const { return pairing_confirm_; }
  const UInt128& pairing_random() const { return pairing_random_; }
  const UInt128& pairing_dhkey_check() const { return pairing_dhkey_check_; }
  const UInt128& enc_info() const { return enc_info_; }
  const UInt128& id_info() const { return id_info_; }
  const DeviceAddress& id_addr_info() const { return id_addr_info_; }
  uint16_t ediv() const { return ediv_; }
  uint64_t rand() const { return rand_; }
  const PairingData& pairing_data() const { return pairing_data_; }

  const ByteBuffer& local_pairing_cmd() const { return local_pairing_cmd_; }
  const ByteBuffer& peer_pairing_cmd() const { return peer_pairing_cmd_; }

  gap::Peer& peer() { return peer_.value(); }

  hci::testing::FakeBrEdrConnection* bredr_link() {
    return fake_bredr_link_.get();
  }

 private:
  void Receive128BitCmd(Code cmd_code, const UInt128& value) {
    StaticByteBuffer<sizeof(Header) + sizeof(UInt128)> buffer;
    PacketWriter writer(cmd_code, &buffer);
    *writer.mutable_payload<UInt128>() = value;
    fake_chan()->Receive(buffer);
  }

  void InitializeTransport() {
    // Ensure any tasks posted by an existing transport are dispatched.
    RunUntilIdle();
    auto mock_controller =
        std::make_unique<bt::testing::MockController>(dispatcher());
    controller_ = mock_controller->GetWeakPtr();
    transport_ = std::make_unique<hci::Transport>(
        std::move(mock_controller), dispatcher(), lease_provider_);
    std::optional<bool> init_success;
    transport_->Initialize([&](bool success) { init_success = success; });
    RunUntilIdle();
    ASSERT_TRUE(init_success.has_value());
    ASSERT_TRUE(init_success.value());
    transport_->InitializeACLDataChannel(hci::DataBufferInfo(1, 1),
                                         hci::DataBufferInfo(1, 1));
  }

  pw::async::test::FakeDispatcher dispatcher_;

  gap::PeerMetrics peer_metrics_;
  std::optional<gap::Peer> peer_;

  testing::MockController::WeakPtr controller_;
  std::unique_ptr<hci::Transport> transport_;

  // We store the preq/pres values here to generate a valid confirm value for
  // the fake side.
  StaticByteBuffer<sizeof(Header) + sizeof(PairingRequestParams)>
      local_pairing_cmd_, peer_pairing_cmd_;

  // Number of times the security callback given to UpgradeSecurity has been
  // called and the most recent parameters that it was called with.
  int security_callback_count_ = 0;
  Result<> security_status_ = fit::ok();
  SecurityProperties sec_props_;

  // Number of times the pairing data callback has been called and the most
  // recent value that it was called with.
  int pairing_data_callback_count_ = 0;
  PairingData pairing_data_;

  // Number of times the link security properties have been notified via
  // OnNewSecurityProperties().
  int new_sec_props_count_ = 0;
  SecurityProperties new_sec_props_;

  // State tracking the OnPairingComplete event.
  int pairing_complete_count_ = 0;
  Result<> pairing_complete_status_ = fit::ok();

  // State tracking the OnAuthenticationFailure event.
  int auth_failure_callback_count_ = 0;
  hci::Result<> auth_failure_status_ = fit::ok();

  // State tracking the OnIdentityInformationRequest event.
  int local_id_info_callback_count_ = 0;
  std::optional<IdentityInfo> local_id_info_;

  // Delegate functions used to respond to user input requests from the Security
  // Manager.
  ConfirmDelegate confirm_delegate_;
  DisplayDelegate display_delegate_;
  RequestPasskeyDelegate request_passkey_delegate_;

  // Counts of commands that we have sent out to the peer.
  int security_request_count_ = 0;
  int pairing_failed_count_ = 0;
  int pairing_request_count_ = 0;
  int pairing_response_count_ = 0;
  int pairing_public_key_count_ = 0;
  int pairing_confirm_count_ = 0;
  int pairing_random_count_ = 0;
  int pairing_dhkey_check_count_ = 0;
  int enc_info_count_ = 0;
  int id_info_count_ = 0;
  int id_addr_info_count_ = 0;
  int central_ident_count_ = 0;

  // Values that have we have sent to the peer.
  AuthReqField security_auth_req_ = 0;
  std::optional<EcdhKey> public_ecdh_key_;
  UInt128 pairing_confirm_ = {};
  UInt128 pairing_random_ = {};
  UInt128 pairing_dhkey_check_ = {};
  UInt128 enc_info_ = {};
  UInt128 id_info_ = {};
  DeviceAddress id_addr_info_;
  uint16_t ediv_ = 0;
  uint64_t rand_ = 0;

  std::optional<ErrorCode> received_error_code_;

  pw::bluetooth_sapphire::testing::FakeLeaseProvider lease_provider_;
  std::unique_ptr<l2cap::testing::FakeChannel> fake_chan_;
  std::unique_ptr<hci::testing::FakeLowEnergyConnection> fake_le_link_;
  std::unique_ptr<hci::testing::FakeBrEdrConnection> fake_bredr_link_;
  std::unique_ptr<SecurityManager> pairing_;

  WeakSelf<Delegate> weak_delegate_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(SecurityManagerTest);
};

class InitiatorPairingTest : public SecurityManagerTest {
 public:
  InitiatorPairingTest() = default;
  ~InitiatorPairingTest() override = default;

  void SetUp() override {
    SecurityManagerTest::SetUp();
    SetUpSecurityManager();
  }

  void SetUpSecurityManager(
      IOCapability ioc = IOCapability::kDisplayOnly,
      BondableMode bondable_mode = BondableMode::Bondable) {
    NewSecurityManager(Role::kInitiator, ioc, bondable_mode);
  }

  void GenerateMatchingLegacyConfirmAndRandom(UInt128* out_confirm,
                                              UInt128* out_random,
                                              uint32_t tk = 0) {
    PW_DCHECK(out_confirm);
    PW_DCHECK(out_random);
    *out_random = kHardCodedPairingRandom;
    GenerateLegacyConfirmValue(
        *out_random, out_confirm, /*peer_initiator=*/false, tk);
  }

  struct MatchingPair {
    UInt128 confirm;
    UInt128 random;
  };
  MatchingPair GenerateMatchingScConfirmAndRandom(const LocalEcdhKey& peer_key,
                                                  uint8_t r = 0) {
    MatchingPair pair;
    pair.random = kHardCodedPairingRandom;
    pair.confirm = GenerateScConfirmValue(peer_key,
                                          pair.random,
                                          Role::kInitiator,
                                          /*gen_initiator_confirm=*/false,
                                          r);
    return pair;
  }
  // Emulate legacy pairing up until before encryption with STK. Returns the STK
  // that the initiator is expected to encrypt the link with in |out_stk|.
  //
  // This will not resolve the encryption request that is made by using the STK
  // before this function returns (this is to unit test encryption failure). Use
  // FastForwardToPhase3() to also emulate successful encryption.
  void FastForwardToSTK(UInt128* out_stk,
                        SecurityLevel level = SecurityLevel::kEncrypted,
                        KeyDistGenField remote_keys = 0,
                        KeyDistGenField local_keys = 0,
                        uint8_t max_key_size = kMaxEncryptionKeySize,
                        BondableMode bondable_mode = BondableMode::Bondable) {
    UpgradeSecurity(level);
    EXPECT_TRUE(peer().MutLe().is_pairing());

    PairingRequestParams pairing_params;
    pairing_params.io_capability = IOCapability::kNoInputNoOutput;
    AuthReqField bondable = (bondable_mode == BondableMode::Bondable)
                                ? AuthReq::kBondingFlag
                                : 0,
                 mitm_protected = (level >= SecurityLevel::kAuthenticated)
                                      ? AuthReq::kMITM
                                      : 0;
    pairing_params.auth_req = mitm_protected | bondable;
    pairing_params.max_encryption_key_size = max_key_size;
    pairing_params.initiator_key_dist_gen = local_keys;
    pairing_params.responder_key_dist_gen = remote_keys;
    ReceivePairingFeatures(pairing_params);

    // Run the loop until the harness caches the feature exchange PDUs (preq &
    // pres) so that we can generate a valid confirm value.
    RunUntilIdle();

    UInt128 sconfirm, srand;
    GenerateMatchingLegacyConfirmAndRandom(&sconfirm, &srand);
    ReceivePairingConfirm(sconfirm);
    ReceivePairingRandom(srand);
    RunUntilIdle();

    EXPECT_EQ(1, pairing_confirm_count());
    EXPECT_EQ(1, pairing_random_count());
    EXPECT_EQ(0, pairing_failed_count());
    EXPECT_EQ(0, security_callback_count());
    EXPECT_TRUE(peer().MutLe().is_pairing());

    PW_DCHECK(out_stk);

    UInt128 tk;
    tk.fill(0);
    util::S1(tk, srand, pairing_random(), out_stk);
  }

  void FastForwardToPhase3(
      UInt128* out_encryption_key,
      bool secure_connections = false,
      SecurityLevel level = SecurityLevel::kEncrypted,
      KeyDistGenField remote_keys = 0,
      KeyDistGenField local_keys = 0,
      uint8_t max_key_size = kMaxEncryptionKeySize,
      BondableMode bondable_mode = BondableMode::Bondable) {
    EXPECT_FALSE(peer().MutLe().is_pairing());
    if (secure_connections) {
      FastForwardToScLtk(
          out_encryption_key, level, remote_keys, local_keys, bondable_mode);
    } else {
      FastForwardToSTK(out_encryption_key,
                       level,
                       remote_keys,
                       local_keys,
                       max_key_size,
                       bondable_mode);
    }

    ASSERT_TRUE(fake_link()->ltk());
    EXPECT_EQ(1, fake_link()->start_encryption_count());

    // Resolve the encryption request.
    fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
    RunUntilIdle();
    EXPECT_EQ(1, new_sec_props_count());
    EXPECT_EQ(level, new_sec_props().level());
  }

  void FastForwardToScLtk(UInt128* out_ltk,
                          SecurityLevel level = SecurityLevel::kEncrypted,
                          KeyDistGenField peer_keys = 0,
                          KeyDistGenField local_keys = 0,
                          BondableMode bondable = BondableMode::Bondable) {
    UpgradeSecurity(level);
    FastForwardToScLtkAfterUpgradeSecurity(
        out_ltk, level, peer_keys, local_keys, bondable);
  }

  void FastForwardToScLtkAfterUpgradeSecurity(
      UInt128* out_ltk,
      SecurityLevel level = SecurityLevel::kEncrypted,
      KeyDistGenField peer_keys = 0,
      KeyDistGenField local_keys = 0,
      BondableMode bondable = BondableMode::Bondable) {
    EXPECT_TRUE(peer().MutLe().is_pairing());
    RunUntilIdle();

    ASSERT_EQ(1, pairing_request_count());
    PairingRequestParams pres;
    pres.io_capability = IOCapability::kDisplayYesNo;
    pres.oob_data_flag = OOBDataFlag::kNotPresent;
    AuthReqField bondable_flag =
        (bondable == BondableMode::Bondable) ? AuthReq::kBondingFlag : 0;
    AuthReqField mitm_flag =
        (level >= SecurityLevel::kAuthenticated) ? AuthReq::kMITM : 0;
    pres.auth_req = AuthReq::kSC | mitm_flag | bondable_flag;
    pres.max_encryption_key_size = kMaxEncryptionKeySize;
    pres.initiator_key_dist_gen = local_keys;
    pres.responder_key_dist_gen = peer_keys;
    ReceivePairingFeatures(pres);
    RunUntilIdle();

    ASSERT_TRUE(public_ecdh_key().has_value());
    LocalEcdhKey peer_key = *LocalEcdhKey::Create();
    ReceivePairingPublicKey(peer_key.GetSerializedPublicKey());
    RunUntilIdle();
    // We're in SC Numeric Comparison/Just Works, so as initiator we should not
    // send a confirm.
    ASSERT_EQ(0, pairing_confirm_count());
    MatchingPair phase_2_vals = GenerateMatchingScConfirmAndRandom(peer_key);
    ReceivePairingConfirm(phase_2_vals.confirm);
    RunUntilIdle();
    ASSERT_EQ(1, pairing_random_count());
    // If using MITM, we expect to be in Numeric Comparison
    ConfirmCallback display_cb = nullptr;
    if (mitm_flag != 0) {
      uint32_t kExpectedDisplayVal =
          *util::G2(public_ecdh_key()->GetPublicKeyX(),
                    peer_key.GetPublicKeyX(),
                    pairing_random(),
                    phase_2_vals.random) %
          1000000;
      set_display_delegate(
          [kExpectedDisplayVal, &display_cb](uint32_t compare_value,
                                             Delegate::DisplayMethod method,
                                             ConfirmCallback cb) {
            ASSERT_TRUE(method == Delegate::DisplayMethod::kComparison);
            ASSERT_EQ(kExpectedDisplayVal, compare_value);
            display_cb = std::move(cb);
          });
    }
    ReceivePairingRandom(phase_2_vals.random);
    RunUntilIdle();
    if (mitm_flag != 0) {
      ASSERT_TRUE(display_cb);
      display_cb(true);
    }  // Else we are content to use the default confirm delegate behavior to
       // accept the pairing.

    util::F5Results f5 = *util::F5(peer_key.CalculateDhKey(*public_ecdh_key()),
                                   pairing_random(),
                                   phase_2_vals.random,
                                   kLocalAddr,
                                   kPeerAddr);

    UInt128 r_array{0};
    PacketReader reader(&local_pairing_cmd());
    PairingResponseParams preq = reader.payload<PairingRequestParams>();
    UInt128 dhkey_check_a = *util::F6(f5.mac_key,
                                      pairing_random(),
                                      phase_2_vals.random,
                                      r_array,
                                      preq.auth_req,
                                      preq.oob_data_flag,
                                      preq.io_capability,
                                      kLocalAddr,
                                      kPeerAddr);
    UInt128 dhkey_check_b = *util::F6(f5.mac_key,
                                      phase_2_vals.random,
                                      pairing_random(),
                                      r_array,
                                      pres.auth_req,
                                      pres.oob_data_flag,
                                      pres.io_capability,
                                      kPeerAddr,
                                      kLocalAddr);
    RunUntilIdle();
    EXPECT_EQ(1, pairing_dhkey_check_count());
    ASSERT_EQ(dhkey_check_a, pairing_dhkey_check());
    ReceivePairingDHKeyCheck(dhkey_check_b);
    RunUntilIdle();
    EXPECT_EQ(0, pairing_failed_count());
    EXPECT_EQ(0, security_callback_count());
    EXPECT_TRUE(peer().MutLe().is_pairing());

    ASSERT_TRUE(out_ltk);
    *out_ltk = f5.ltk;
  }

 private:
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(InitiatorPairingTest);
};

class ResponderPairingTest : public SecurityManagerTest {
 public:
  ResponderPairingTest() = default;
  ~ResponderPairingTest() override = default;

  void SetUp() override {
    SecurityManagerTest::SetUp();
    SetUpSecurityManager();
  }

  void SetUpSecurityManager(
      IOCapability ioc = IOCapability::kDisplayOnly,
      BondableMode bondable_mode = BondableMode::Bondable) {
    NewSecurityManager(Role::kResponder, ioc, bondable_mode);
  }

  void GenerateMatchingLegacyConfirmAndRandom(UInt128* out_confirm,
                                              UInt128* out_random,
                                              uint32_t tk = 0) {
    PW_DCHECK(out_confirm);
    PW_DCHECK(out_random);
    *out_random = kHardCodedPairingRandom;
    GenerateLegacyConfirmValue(
        *out_random, out_confirm, /*peer_initiator=*/true, tk);
  }
  struct MatchingPair {
    UInt128 confirm;
    UInt128 random;
  };
  MatchingPair GenerateMatchingScConfirmAndRandom(const LocalEcdhKey& peer_key,
                                                  uint8_t r = 0) {
    MatchingPair pair;
    pair.random = kHardCodedPairingRandom;
    pair.confirm = GenerateScConfirmValue(peer_key,
                                          pair.random,
                                          Role::kResponder,
                                          /*gen_initiator_confirm=*/true,
                                          r);
    return pair;
  }
  void ReceivePairingRequest(IOCapability ioc = IOCapability::kNoInputNoOutput,
                             AuthReqField auth_req = 0,
                             uint8_t max_enc_key_size = kMaxEncryptionKeySize) {
    ReceivePairingFeatures(
        ioc, auth_req, max_enc_key_size, /*peer_initiator=*/true);
  }

  void FastForwardToSTK(UInt128* out_stk,
                        SecurityLevel level = SecurityLevel::kEncrypted,
                        KeyDistGenField remote_keys = 0,
                        KeyDistGenField local_keys = 0,
                        uint8_t max_key_size = kMaxEncryptionKeySize,
                        BondableMode bondable_mode = BondableMode::Bondable) {
    PairingRequestParams pairing_params;
    pairing_params.io_capability = IOCapability::kNoInputNoOutput;
    AuthReqField bondable = (bondable_mode == BondableMode::Bondable)
                                ? AuthReq::kBondingFlag
                                : 0,
                 mitm_protected = (level >= SecurityLevel::kAuthenticated)
                                      ? AuthReq::kMITM
                                      : 0;
    pairing_params.auth_req = mitm_protected | bondable;
    pairing_params.max_encryption_key_size = max_key_size;
    pairing_params.initiator_key_dist_gen = remote_keys;
    pairing_params.responder_key_dist_gen = local_keys;
    ReceivePairingFeatures(pairing_params, /*peer_initiator=*/true);
    EXPECT_TRUE(peer().MutLe().is_pairing());

    // Run the loop until the harness caches the feature exchange PDUs (preq &
    // pres) so that we can generate a valid confirm value.
    RunUntilIdle();
    EXPECT_EQ(0, pairing_request_count());
    EXPECT_EQ(1, pairing_response_count());

    // The initiator should start the confirm/random exchange to generate the
    // Phase 2 keys.
    EXPECT_EQ(0, pairing_confirm_count());
    EXPECT_EQ(0, pairing_random_count());

    UInt128 mconfirm, mrand;
    GenerateMatchingLegacyConfirmAndRandom(&mconfirm, &mrand);
    ReceivePairingConfirm(mconfirm);
    RunUntilIdle();
    EXPECT_EQ(1, pairing_confirm_count());
    EXPECT_EQ(0, pairing_random_count());

    ReceivePairingRandom(mrand);
    RunUntilIdle();
    EXPECT_EQ(1, pairing_confirm_count());
    EXPECT_EQ(1, pairing_random_count());
    EXPECT_EQ(0, pairing_failed_count());
    EXPECT_EQ(0, security_callback_count());

    PW_DCHECK(out_stk);

    UInt128 tk;
    tk.fill(0);
    util::S1(tk, pairing_random(), mrand, out_stk);
  }

  void FastForwardToPhase3(
      UInt128* out_encryption_key,
      bool secure_connections = false,
      SecurityLevel level = SecurityLevel::kEncrypted,
      KeyDistGenField remote_keys = 0,
      KeyDistGenField local_keys = 0,
      uint8_t max_key_size = kMaxEncryptionKeySize,
      BondableMode bondable_mode = BondableMode::Bondable) {
    if (secure_connections) {
      FastForwardToScLtk(
          out_encryption_key, level, remote_keys, local_keys, bondable_mode);
    } else {
      FastForwardToSTK(out_encryption_key,
                       level,
                       remote_keys,
                       local_keys,
                       max_key_size,
                       bondable_mode);
    }

    ASSERT_TRUE(fake_link()->ltk());

    // No local start encryption request should be made.
    EXPECT_EQ(0, fake_link()->start_encryption_count());

    // Pretend that the initiator succeeded in encrypting the connection.
    fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
    RunUntilIdle();
    EXPECT_EQ(1, new_sec_props_count());
    EXPECT_EQ(level, new_sec_props().level());
  }

  void FastForwardToScLtk(UInt128* out_ltk,
                          SecurityLevel level = SecurityLevel::kEncrypted,
                          KeyDistGenField peer_keys = 0,
                          KeyDistGenField local_keys = 0,
                          BondableMode bondable = BondableMode::Bondable) {
    PairingRequestParams preq;
    preq.io_capability = IOCapability::kDisplayYesNo;
    preq.oob_data_flag = OOBDataFlag::kNotPresent;
    AuthReqField bondable_flag =
        (bondable == BondableMode::Bondable) ? AuthReq::kBondingFlag : 0;
    AuthReqField mitm_flag =
        (level >= SecurityLevel::kAuthenticated) ? AuthReq::kMITM : 0;
    preq.auth_req = AuthReq::kSC | mitm_flag | bondable_flag;
    preq.max_encryption_key_size = kMaxEncryptionKeySize;
    preq.initiator_key_dist_gen = local_keys;
    preq.responder_key_dist_gen = peer_keys;
    ReceivePairingFeatures(preq, /*peer_initiator=*/true);
    EXPECT_TRUE(peer().MutLe().is_pairing());
    RunUntilIdle();
    ASSERT_EQ(1, pairing_response_count());

    LocalEcdhKey peer_key = *LocalEcdhKey::Create();
    ReceivePairingPublicKey(peer_key.GetSerializedPublicKey());
    RunUntilIdle();
    ASSERT_TRUE(public_ecdh_key().has_value());
    ASSERT_EQ(1, pairing_public_key_count());

    // We are in Just Works or Numeric Comparison based on IOCapabilities/MITM
    // preferences, so we expect the confirm value immediately after the public
    // key.
    ASSERT_EQ(1, pairing_confirm_count());
    std::optional<uint32_t> display_val;
    if (mitm_flag != 0) {
      set_display_delegate([&](uint32_t compare_value,
                               Delegate::DisplayMethod method,
                               ConfirmCallback cb) {
        ASSERT_TRUE(method == Delegate::DisplayMethod::kComparison);
        display_val = compare_value;
        cb(true);
      });
    }  // Else we are content to use the default confirm delegate behavior to
       // accept the pairing.
    auto peer_rand = Random<PairingRandomValue>();
    ReceivePairingRandom(peer_rand);
    RunUntilIdle();
    ASSERT_EQ(1, pairing_random_count());
    ASSERT_EQ(GenerateScConfirmValue(
                  peer_key, pairing_random(), Role::kResponder, false),
              pairing_confirm());
    if (mitm_flag != 0) {
      ASSERT_TRUE(display_val.has_value());
      uint32_t kExpectedDisplayVal =
          *util::G2(peer_key.GetPublicKeyX(),
                    public_ecdh_key()->GetPublicKeyX(),
                    peer_rand,
                    pairing_random()) %
          1000000;

      EXPECT_EQ(kExpectedDisplayVal, display_val);
    }

    util::F5Results f5 = *util::F5(peer_key.CalculateDhKey(*public_ecdh_key()),
                                   peer_rand,
                                   pairing_random(),
                                   kPeerAddr,
                                   kLocalAddr);

    UInt128 r_array{0};
    PacketReader reader(&local_pairing_cmd());
    PairingResponseParams pres = reader.payload<PairingResponseParams>();
    UInt128 dhkey_check_a = *util::F6(f5.mac_key,
                                      peer_rand,
                                      pairing_random(),
                                      r_array,
                                      preq.auth_req,
                                      preq.oob_data_flag,
                                      preq.io_capability,
                                      kPeerAddr,
                                      kLocalAddr);
    UInt128 dhkey_check_b = *util::F6(f5.mac_key,
                                      pairing_random(),
                                      peer_rand,
                                      r_array,
                                      pres.auth_req,
                                      pres.oob_data_flag,
                                      pres.io_capability,
                                      kLocalAddr,
                                      kPeerAddr);
    ReceivePairingDHKeyCheck(dhkey_check_a);
    RunUntilIdle();
    EXPECT_EQ(1, pairing_dhkey_check_count());
    ASSERT_EQ(dhkey_check_b, pairing_dhkey_check());
    EXPECT_EQ(0, pairing_failed_count());
    EXPECT_EQ(0, pairing_complete_count());
    EXPECT_TRUE(peer().MutLe().is_pairing());

    ASSERT_TRUE(out_ltk);
    *out_ltk = f5.ltk;
  }

 private:
  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ResponderPairingTest);
};

// Calling `Abort` with no in-progress security upgrade should not cause a
// PairingComplete event.
TEST_F(InitiatorPairingTest, AbortNoSecurityUpgradeInProgress) {
  pairing()->Abort();
  RunUntilIdle();
  EXPECT_EQ(0, pairing_complete_count());
}

// Disconnecting with no in-progress security upgrade should not cause a
// PairingComplete event.
TEST_F(InitiatorPairingTest, DisconnectNoSecurityUpgradeInProgress) {
  fake_chan()->Close();
  RunUntilIdle();
  EXPECT_EQ(0, pairing_complete_count());
}

// Requesting pairing at the current security level should succeed immediately.
TEST_F(InitiatorPairingTest, UpgradeSecurityCurrentLevel) {
  UpgradeSecurity(SecurityLevel::kNoSecurity);
  RunUntilIdle();

  // No pairing requests should have been made.
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(0, pairing_complete_count());
  EXPECT_FALSE(peer().MutLe().is_pairing());

  // Pairing should succeed.
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(SecurityLevel::kNoSecurity, sec_props().level());
  EXPECT_EQ(0u, sec_props().enc_key_size());
  EXPECT_FALSE(sec_props().secure_connections());
}

// Peer aborts during Phase 1.
TEST_F(InitiatorPairingTest, PairingFailedInPhase1) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  EXPECT_TRUE(peer().MutLe().is_pairing());
  RunUntilIdle();

  // Pairing not complete yet but we should be in Phase 1.
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(1, pairing_request_count());

  ReceivePairingFailed(ErrorCode::kPairingNotSupported);
  RunUntilIdle();

  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_request_count());
  EXPECT_EQ(ToResult(ErrorCode::kPairingNotSupported), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
  EXPECT_FALSE(peer().MutLe().is_pairing());
}

// Local aborts during Phase 1.
TEST_F(InitiatorPairingTest, PairingAbortedInPhase1) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  EXPECT_TRUE(peer().MutLe().is_pairing());
  RunUntilIdle();

  // Pairing not complete yet but we should be in Phase 1.
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(1, pairing_request_count());

  pairing()->Abort();
  RunUntilIdle();

  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_request_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
  EXPECT_FALSE(peer().MutLe().is_pairing());
}

// Local resets I/O capabilities while pairing. This should abort any ongoing
// pairing and the new I/O capabilities should be used in following pairing
// requests.
TEST_F(InitiatorPairingTest, SecurityManagerResetDuringPairing) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  // Pairing not complete yet but we should be in Phase 1.
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(1, pairing_request_count());

  pairing()->Reset(IOCapability::kNoInputNoOutput);
  RunUntilIdle();

  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_request_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
  EXPECT_FALSE(peer().MutLe().is_pairing());

  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  // Should have sent a new pairing request.
  EXPECT_EQ(2, pairing_request_count());

  // Make sure that the new request has the new I/O capabilities.
  const auto& params = local_pairing_cmd().view(1).To<PairingRequestParams>();
  EXPECT_EQ(IOCapability::kNoInputNoOutput, params.io_capability);
}

TEST_F(InitiatorPairingTest, ReceiveConfirmValueWhileNotPairing) {
  UInt128 confirm;
  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  // Nothing should happen.
  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
}

TEST_F(InitiatorPairingTest, ReceiveConfirmValueInPhase1) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  UInt128 confirm;
  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

TEST_F(InitiatorPairingTest,
       RejectUnauthenticatedPairingInSecureConnectionsOnlyMode) {
  SetUpSecurityManager(IOCapability::kKeyboardDisplay);
  pairing()->set_security_mode(gap::LESecurityMode::SecureConnectionsOnly);
  // In SC Only mode, SM should translate this "encrypted" request into a MITM
  // requirement.
  UpgradeSecurity(SecurityLevel::kEncrypted);
  // The peer has NoInputNoOutput IOCapabilities, thus cannot perform
  // authenticated pairing.
  ReceivePairingFeatures(IOCapability::kNoInputNoOutput,
                         AuthReq::kBondingFlag | AuthReq::kSC);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kAuthenticationRequirements),
            security_status());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

TEST_F(InitiatorPairingTest,
       RejectUnauthenticatedEncryptionInSecureConnectionsOnlyMode) {
  const LTK kUnauthenticatedLtk(SecurityProperties(/*encrypted=*/true,
                                                   /*authenticated=*/false,
                                                   /*secure_connections=*/true,
                                                   kMaxEncryptionKeySize),
                                hci_spec::LinkKey());
  sm::PairingData bond_data;
  bond_data.peer_ltk = kUnauthenticatedLtk;
  bond_data.local_ltk = kUnauthenticatedLtk;
  peer().MutLe().SetBondData(bond_data);

  NewSecurityManager(Role::kInitiator,
                     IOCapability::kDisplayOnly,
                     BondableMode::Bondable,
                     gap::LESecurityMode::SecureConnectionsOnly);
  RunUntilIdle();
  // In SC Only mode, encrypting with an unauthenticated LTK should
  // cause the channel to be disconnected with an authentication failure.
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();

  EXPECT_EQ(1, auth_failure_callback_count());
  EXPECT_EQ(ToResult(HostError::kInsufficientSecurity), auth_failure_status());
  EXPECT_TRUE(fake_chan()->link_error());
}

TEST_F(InitiatorPairingTest,
       AllowSecureAuthenticatedPairingInSecureConnectionsOnlyMode) {
  SetUpSecurityManager(IOCapability::kDisplayYesNo);
  pairing()->set_security_mode(gap::LESecurityMode::SecureConnectionsOnly);
  UInt128 enc_key;
  FastForwardToPhase3(&enc_key,
                      /*secure_connections=*/true,
                      SecurityLevel::kSecureAuthenticated);
  RunUntilIdle();
  // After setting SC Only mode, secure authenticated pairing should still
  // complete successfully.
  EXPECT_EQ(1, pairing_data_callback_count());
  EXPECT_TRUE(peer_ltk().has_value());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());
  EXPECT_FALSE(peer().MutLe().is_pairing());

  EXPECT_EQ(SecurityLevel::kSecureAuthenticated, sec_props().level());
}

// In Phase 2 but still waiting to receive TK.
TEST_F(InitiatorPairingTest, ReceiveConfirmValueWhileWaitingForUserInput) {
  bool tk_requested = false;
  set_confirm_delegate([&](ConfirmCallback) { tk_requested = true; });

  UpgradeSecurity(SecurityLevel::kEncrypted);
  ReceivePairingFeatures();
  RunUntilIdle();
  EXPECT_TRUE(tk_requested);

  UInt128 confirm;
  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

// SecurityManager destroyed when waiting for Just Works user confirmation.
TEST_F(InitiatorPairingTest,
       SecurityManagerDestroyedStateWhileWaitingForUserInput) {
  ConfirmCallback respond;
  set_confirm_delegate([&](ConfirmCallback rsp) { respond = std::move(rsp); });

  UpgradeSecurity(SecurityLevel::kEncrypted);
  ReceivePairingFeatures();
  RunUntilIdle();
  EXPECT_TRUE(respond);

  DestroySecurityManager();

  // This should proceed safely.
  respond(true);
  RunUntilIdle();
}

// Pairing no longer in progress when waiting for Just Works user confirmation.
TEST_F(InitiatorPairingTest, PairingAbortedWhileWaitingForUserInput) {
  ConfirmCallback respond;
  set_confirm_delegate([&](ConfirmCallback rsp) { respond = std::move(rsp); });

  UpgradeSecurity(SecurityLevel::kEncrypted);
  ReceivePairingFeatures();
  RunUntilIdle();
  EXPECT_TRUE(respond);

  ReceivePairingFailed(ErrorCode::kPairingNotSupported);
  RunUntilIdle();
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kPairingNotSupported), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());

  // This should have no effect.
  respond(true);
  RunUntilIdle();
  EXPECT_EQ(1, pairing_request_count());
  EXPECT_EQ(0, pairing_response_count());
  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(0, pairing_data_callback_count());
}

// Pairing procedure stopped and restarted when TKResponse runs. The TKResponse
// does not belong to the current pairing.
TEST_F(InitiatorPairingTest, PairingRestartedWhileWaitingForTK) {
  ConfirmCallback respond;
  set_confirm_delegate([&](ConfirmCallback rsp) { respond = std::move(rsp); });

  UpgradeSecurity(SecurityLevel::kEncrypted);
  ReceivePairingFeatures();
  RunUntilIdle();
  EXPECT_TRUE(respond);

  // Stop pairing.
  ReceivePairingFailed(ErrorCode::kPairingNotSupported);
  RunUntilIdle();
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kPairingNotSupported), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());

  // Reset the delegate so that |respond| doesn't get overwritten by the second
  // pairing.
  set_confirm_delegate(nullptr);

  UpgradeSecurity(SecurityLevel::kEncrypted);
  ReceivePairingFeatures();
  RunUntilIdle();
  EXPECT_EQ(2, pairing_request_count());
  EXPECT_EQ(0, pairing_response_count());
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(0, pairing_data_callback_count());

  // This should have no effect.
  respond(true);
  RunUntilIdle();
  EXPECT_EQ(2, pairing_request_count());
  EXPECT_EQ(0, pairing_response_count());
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(0, pairing_data_callback_count());
}

TEST_F(InitiatorPairingTest, ReceiveRandomValueWhileNotPairing) {
  UInt128 random;
  ReceivePairingRandom(random);
  RunUntilIdle();

  // Nothing should happen.
  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
}

TEST_F(InitiatorPairingTest, ReceiveRandomValueInPhase1) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  UInt128 random;
  ReceivePairingRandom(random);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

// In Phase 2 but still waiting to receive TK.
TEST_F(InitiatorPairingTest, ReceiveRandomValueWhileWaitingForTK) {
  bool confirmation_requested = false;
  set_confirm_delegate([&](ConfirmCallback) { confirmation_requested = true; });

  UpgradeSecurity(SecurityLevel::kEncrypted);
  ReceivePairingFeatures();
  RunUntilIdle();
  EXPECT_TRUE(confirmation_requested);

  UInt128 random;
  ReceivePairingRandom(random);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

TEST_F(InitiatorPairingTest, LegacyPhase2SconfirmValueReceivedTwice) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  ReceivePairingFeatures();
  RunUntilIdle();

  // Should have received Mconfirm.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, security_callback_count());

  UInt128 confirm;
  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  // Should have received Mrand.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(0, security_callback_count());

  // Send Mconfirm again
  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

TEST_F(InitiatorPairingTest, LegacyPhase2ReceiveRandomValueInWrongOrder) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  ReceivePairingFeatures();
  RunUntilIdle();

  // Should have received Mconfirm.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, security_callback_count());

  UInt128 random;
  ReceivePairingRandom(random);
  RunUntilIdle();

  // Should have aborted pairing if Srand arrives before Srand.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

TEST_F(InitiatorPairingTest, LegacyPhase2SconfirmValueInvalid) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  // Pick I/O capabilities and MITM flags that will result in Just Works
  // pairing.
  ReceivePairingFeatures();
  RunUntilIdle();

  // Should have received Mconfirm.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, security_callback_count());

  // Receive Sconfirm and Srand values that don't match.
  UInt128 confirm, random;
  confirm.fill(0);
  random.fill(1);

  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  // Should have received Mrand.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(0, security_callback_count());

  // Our Mconfirm/Mrand should be correct.
  UInt128 expected_confirm;
  GenerateLegacyConfirmValue(pairing_random(), &expected_confirm);
  EXPECT_EQ(expected_confirm, pairing_confirm());

  // Send the non-matching Srandom.
  ReceivePairingRandom(random);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kConfirmValueFailed), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

TEST_F(InitiatorPairingTest, LegacyPhase2RandomValueReceivedTwice) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  // Pick I/O capabilities and MITM flags that will result in Just Works
  // pairing.
  ReceivePairingFeatures();
  RunUntilIdle();

  // Should have received Mconfirm.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, security_callback_count());

  // Receive Sconfirm and Srand values that match.
  UInt128 confirm, random;
  GenerateMatchingLegacyConfirmAndRandom(&confirm, &random);

  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  // Should have received Mrand.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(0, security_callback_count());

  // Our Mconfirm/Mrand should be correct.
  UInt128 expected_confirm;
  GenerateLegacyConfirmValue(pairing_random(), &expected_confirm);
  EXPECT_EQ(expected_confirm, pairing_confirm());

  // Send Srandom.
  ReceivePairingRandom(random);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());

  // Send Srandom again.
  ReceivePairingRandom(random);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());

  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

TEST_F(InitiatorPairingTest, LegacyPhase2ConfirmValuesExchanged) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  // Pick I/O capabilities and MITM flags that will result in Just Works
  // pairing.
  ReceivePairingFeatures();
  RunUntilIdle();

  // Should have received Mconfirm.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, security_callback_count());

  // Receive Sconfirm and Srand values that match.
  UInt128 confirm, random;
  GenerateMatchingLegacyConfirmAndRandom(&confirm, &random);

  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  // Should have received Mrand.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(0, security_callback_count());

  // Our Mconfirm/Mrand should be correct.
  UInt128 expected_confirm;
  GenerateLegacyConfirmValue(pairing_random(), &expected_confirm);
  EXPECT_EQ(expected_confirm, pairing_confirm());

  // Send Srandom.
  ReceivePairingRandom(random);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
}

// TK delegate rejects pairing. When pairing method is "PasskeyEntryInput", this
// should result in a "Passkey Entry Failed" error.
TEST_F(InitiatorPairingTest, LegacyPhase2TKDelegateRejectsPasskeyInput) {
  SetUpSecurityManager(IOCapability::kKeyboardOnly);

  bool tk_requested = false;
  PasskeyResponseCallback respond;
  set_request_passkey_delegate([&](PasskeyResponseCallback cb_rsp) {
    tk_requested = true;
    respond = std::move(cb_rsp);
  });

  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  // Pick I/O capabilities and MITM flags that will result in Passkey Entry
  // pairing.
  ReceivePairingFeatures(IOCapability::kDisplayOnly, AuthReq::kMITM);
  RunUntilIdle();
  ASSERT_TRUE(tk_requested);

  // Reject pairing.
  respond(-1);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kPasskeyEntryFailed), security_status());
}

// TK delegate rejects pairing.
TEST_F(InitiatorPairingTest, LegacyPhase2TKDelegateRejectsPairing) {
  bool tk_requested = false;
  ConfirmCallback respond;
  set_confirm_delegate([&](ConfirmCallback cb_rsp) {
    tk_requested = true;
    respond = std::move(cb_rsp);
  });

  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  ReceivePairingFeatures();
  RunUntilIdle();
  ASSERT_TRUE(tk_requested);

  // Reject pairing.
  respond(false);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());
}

TEST_F(InitiatorPairingTest, IgnoresExpiredConfirmRequestCallback) {
  ConfirmCallback respond = nullptr;
  set_confirm_delegate([&](ConfirmCallback rsp) { respond = std::move(rsp); });

  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  ReceivePairingFeatures();
  RunUntilIdle();
  ASSERT_TRUE(respond);
  ConfirmCallback first_pairing_cb = std::move(respond);
  pairing()->Abort();
  RunUntilIdle();
  EXPECT_EQ(1, pairing_failed_count());

  // We reset the respond variable so we can "catch" the next PairingDelegate
  // request in the same variable (the `set_confirm_delegate` callback still has
  // the reference to `respond`)
  respond = nullptr;

  // Start a separate pairing from the one captured in `first_pairing_cb`, which
  // was `Abort`ed
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  ReceivePairingFeatures();
  RunUntilIdle();
  first_pairing_cb(true);
  RunUntilIdle();
  // The callback from the `Abort`ed pairing should be ignored, while calling
  // `respond`, which is associated with the active pairing, should cause the
  // expected Pairing Confirm to be sent.
  EXPECT_EQ(0, pairing_confirm_count());
  ASSERT_TRUE(respond);
  respond(true);
  RunUntilIdle();
  EXPECT_EQ(1, pairing_confirm_count());
}

TEST_F(InitiatorPairingTest, IgnoresExpiredDisplayRequestCallback) {
  SetUpSecurityManager(IOCapability::kDisplayOnly);
  ConfirmCallback respond = nullptr;
  set_display_delegate(
      [&](uint32_t /**/, Delegate::DisplayMethod method, ConfirmCallback rsp) {
        ASSERT_EQ(Delegate::DisplayMethod::kPeerEntry, method);
        respond = std::move(rsp);
      });

  // Must request MITM to test PasskeyEntryDisplay instead of JustWorks pairing
  UpgradeSecurity(SecurityLevel::kAuthenticated);
  RunUntilIdle();

  ReceivePairingFeatures(IOCapability::kKeyboardOnly);
  RunUntilIdle();
  ASSERT_TRUE(respond);
  ConfirmCallback first_pairing_cb = std::move(respond);
  pairing()->Abort();
  RunUntilIdle();
  EXPECT_EQ(1, pairing_failed_count());

  // We reset the respond variable so we can "catch" the next PairingDelegate
  // request in the same variable (the `set_display_delegate` callback still has
  // the reference to `respond`)
  respond = nullptr;

  // Start a separate pairing from the one captured in `first_pairing_cb`, which
  // was `Abort`ed
  UpgradeSecurity(SecurityLevel::kAuthenticated);
  RunUntilIdle();

  ReceivePairingFeatures(IOCapability::kKeyboardOnly);
  RunUntilIdle();
  first_pairing_cb(true);
  RunUntilIdle();
  // The callback from the `Abort`ed pairing should be ignored, while calling
  // `respond`, which is associated with the active pairing, should cause the
  // expected Pairing Confirm to be sent.
  EXPECT_EQ(0, pairing_confirm_count());
  ASSERT_TRUE(respond);
  respond(true);
  RunUntilIdle();
  EXPECT_EQ(1, pairing_confirm_count());
}

TEST_F(InitiatorPairingTest, IgnoresExpiredPasskeyEntryInputCallback) {
  SetUpSecurityManager(IOCapability::kKeyboardOnly);
  PasskeyResponseCallback passkey_cb = nullptr;
  set_request_passkey_delegate(
      [&](PasskeyResponseCallback cb) { passkey_cb = std::move(cb); });

  // Must request MITM to test PasskeyEntryInput instead of JustWorks pairing
  UpgradeSecurity(SecurityLevel::kAuthenticated);
  RunUntilIdle();

  ReceivePairingFeatures(IOCapability::kDisplayOnly);
  RunUntilIdle();
  ASSERT_TRUE(passkey_cb);
  PasskeyResponseCallback first_pairing_cb = std::move(passkey_cb);
  pairing()->Abort();
  RunUntilIdle();
  EXPECT_EQ(1, pairing_failed_count());

  // We reset the respond variable so we can "catch" the next PairingDelegate
  // request in the same variable (the `set_display_delegate` callback still has
  // the reference to `respond`)
  passkey_cb = nullptr;

  // Start a separate pairing from the one captured in `first_pairing_cb`, which
  // was `Abort`ed
  UpgradeSecurity(SecurityLevel::kAuthenticated);
  RunUntilIdle();

  ReceivePairingFeatures(IOCapability::kDisplayOnly);
  RunUntilIdle();
  const int32_t kGenericPositive6DigitNumber = 123456;
  first_pairing_cb(kGenericPositive6DigitNumber);
  RunUntilIdle();
  // The callback from the `Abort`ed pairing should be ignored, while calling
  // `respond`, which is associated with the active pairing, should cause the
  // expected Pairing Confirm to be sent.
  EXPECT_EQ(0, pairing_confirm_count());
  ASSERT_TRUE(passkey_cb);
  passkey_cb(kGenericPositive6DigitNumber);
  RunUntilIdle();
  EXPECT_EQ(1, pairing_confirm_count());
}

// The TK delegate is called with the correct pairing method and the TK is
// factored into the confirm value generation.
TEST_F(InitiatorPairingTest, LegacyPhase2ConfirmValuesExchangedWithUserTK) {
  std::optional<uint32_t> tk = std::nullopt;
  auto method = Delegate::DisplayMethod::kComparison;
  ConfirmCallback respond;
  set_display_delegate([&](uint32_t passkey,
                           Delegate::DisplayMethod cb_method,
                           ConfirmCallback cb_rsp) {
    tk = passkey;
    method = cb_method;
    respond = std::move(cb_rsp);
  });

  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();

  // Pick I/O capabilities and MITM flags that will result in Passkey Entry
  // pairing.
  ReceivePairingFeatures(IOCapability::kKeyboardOnly, AuthReq::kMITM);
  RunUntilIdle();
  ASSERT_TRUE(tk.has_value());

  // DisplayMethod should be kPeerEntry, as Comparison is only for Secure
  // Connections, not Legacy.
  ASSERT_EQ(Delegate::DisplayMethod::kPeerEntry, method);

  // Notify that TK was displayed.
  respond(true);
  RunUntilIdle();

  // Should have received Mconfirm.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, security_callback_count());

  // Receive Sconfirm and Srand values that match.
  UInt128 confirm, random;
  GenerateMatchingLegacyConfirmAndRandom(&confirm, &random, *tk);

  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  // Should have received Mrand.
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(0, security_callback_count());

  // Our Mconfirm/Mrand should be correct.
  UInt128 expected_confirm;
  GenerateLegacyConfirmValue(
      pairing_random(), &expected_confirm, /*peer_initiator=*/false, *tk);
  EXPECT_EQ(expected_confirm, pairing_confirm());

  // Send Srandom.
  ReceivePairingRandom(random);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
}

// Peer aborts during Phase 2.
TEST_F(InitiatorPairingTest, PairingFailedInPhase2) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  ReceivePairingFeatures();
  RunUntilIdle();

  UInt128 confirm, random;
  GenerateMatchingLegacyConfirmAndRandom(&confirm, &random);

  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());

  ReceivePairingFailed(ErrorCode::kConfirmValueFailed);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kConfirmValueFailed), security_status());
}

// Encryption with STK fails.
TEST_F(InitiatorPairingTest, EncryptionWithSTKFails) {
  UInt128 stk;
  FastForwardToSTK(&stk);

  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(stk, fake_link()->ltk()->value());
  EXPECT_EQ(0u, fake_link()->ltk()->ediv());
  EXPECT_EQ(0u, fake_link()->ltk()->rand());

  // The host should have requested encryption.
  EXPECT_EQ(SecurityProperties(), pairing()->security());
  EXPECT_EQ(1, fake_link()->start_encryption_count());

  fake_link()->TriggerEncryptionChangeCallback(
      ToResult(pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING)
          .take_error());
  RunUntilIdle();

  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, auth_failure_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());
  EXPECT_EQ(ErrorCode::kUnspecifiedReason, received_error_code());
  EXPECT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING),
            auth_failure_status());

  // No security property update should have been sent since the security
  // properties have not changed.
  EXPECT_EQ(0, new_sec_props_count());
  EXPECT_EQ(SecurityProperties(), pairing()->security());
}

TEST_F(InitiatorPairingTest, EncryptionDisabledInPhase2) {
  UInt128 stk;
  FastForwardToSTK(&stk);

  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(stk, fake_link()->ltk()->value());
  EXPECT_EQ(0u, fake_link()->ltk()->ediv());
  EXPECT_EQ(0u, fake_link()->ltk()->rand());

  // The host should have requested encryption.
  EXPECT_EQ(1, fake_link()->start_encryption_count());
  EXPECT_EQ(SecurityProperties(), pairing()->security());

  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/false));
  RunUntilIdle();

  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(0, auth_failure_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());
  EXPECT_EQ(ErrorCode::kUnspecifiedReason, received_error_code());

  // No security property update should have been sent since the security
  // properties have not changed.
  EXPECT_EQ(0, new_sec_props_count());
  EXPECT_EQ(SecurityProperties(), pairing()->security());
}

// Tests that the STK is generated according to the max length provided
TEST_F(InitiatorPairingTest, StkLengthGeneration) {
  UInt128 stk;
  uint8_t max_key_size = 10;
  FastForwardToSTK(&stk, SecurityLevel::kEncrypted, 0, 0, max_key_size);

  // At this stage, the stk is stored here
  ASSERT_TRUE(fake_link()->ltk());

  // Ensure that most significant (16 - max_key_size) bytes are zero. The key
  // should be generated up to the max_key_size.
  for (auto i = max_key_size; i < fake_link()->ltk()->value().size(); i++) {
    EXPECT_TRUE(fake_link()->ltk()->value()[i] == 0);
  }
}

// Tests that the pairing procedure ends after encryption with the STK if there
// are no keys to distribute, and that no keys are notified for Legacy pairing
// in this case.
TEST_F(InitiatorPairingTest, LegacyPhase3CompleteWithoutKeyExchange) {
  UInt128 stk;
  FastForwardToSTK(&stk);

  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(stk, fake_link()->ltk()->value());
  EXPECT_EQ(0u, fake_link()->ltk()->ediv());
  EXPECT_EQ(0u, fake_link()->ltk()->rand());

  // The host should have requested encryption.
  EXPECT_EQ(1, fake_link()->start_encryption_count());

  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();

  // Pairing should succeed without any pairing data.
  EXPECT_EQ(0, pairing_data_callback_count());
  EXPECT_FALSE(peer_ltk());
  EXPECT_FALSE(irk());
  EXPECT_FALSE(identity());
  EXPECT_FALSE(csrk());

  // Should have been called at least once to determine local identity
  // availability.
  EXPECT_NE(0, local_id_info_callback_count());

  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(0, id_info_count());
  EXPECT_EQ(0, id_addr_info_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  EXPECT_EQ(SecurityLevel::kEncrypted, sec_props().level());
  EXPECT_EQ(16u, sec_props().enc_key_size());
  EXPECT_FALSE(sec_props().secure_connections());

  // The security properties should have been updated to match the STK.
  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(sec_props(), pairing()->security());

  ASSERT_TRUE(fake_link()->ltk());
}

// Tests that for Secure Connections, the pairing procedure ends after
// encryption with the LTK if there are no keys to distribute, and that the LTK
// is notified.
TEST_F(InitiatorPairingTest, ScPhase3CompleteWithoutKeyExchange) {
  UInt128 ltk_bytes;
  const SecurityProperties kExpectedSecurity(SecurityLevel::kEncrypted,
                                             kMaxEncryptionKeySize,
                                             /*secure_connections=*/true);
  FastForwardToPhase3(&ltk_bytes,
                      /*secure_connections=*/true,
                      kExpectedSecurity.level(),
                      KeyDistGenField{0},
                      KeyDistGenField{0});

  const LTK kExpectedLtk(kExpectedSecurity, hci_spec::LinkKey(ltk_bytes, 0, 0));
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(kExpectedLtk.key(), fake_link()->ltk());

  // Pairing should succeed with the LTK as the SC pairing is bondable, even
  // though no keys need to be distributed in Phase 3.
  EXPECT_EQ(1, pairing_data_callback_count());
  EXPECT_TRUE(peer_ltk().has_value());
  EXPECT_EQ(kExpectedLtk, peer_ltk());
  EXPECT_EQ(peer_ltk(), local_ltk());
  EXPECT_FALSE(irk());
  EXPECT_FALSE(identity());
  EXPECT_FALSE(csrk());

  // Should have been called at least once to determine local identity
  // availability.
  EXPECT_NE(0, local_id_info_callback_count());

  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(0, id_info_count());
  EXPECT_EQ(0, id_addr_info_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  EXPECT_EQ(kExpectedSecurity, sec_props());

  // The security properties should have been updated to match the LTK.
  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(sec_props(), pairing()->security());

  ASSERT_TRUE(fake_link()->ltk());
}

// Tests that Secure Connections ignores the EncKey bit in the key distribution
// field.
TEST_F(InitiatorPairingTest, ScPhase3EncKeyBitSetNotDistributed) {
  UInt128 ltk_bytes;
  const SecurityProperties kExpectedSecurity(SecurityLevel::kEncrypted,
                                             kMaxEncryptionKeySize,
                                             /*secure_connections=*/true);
  // We will request the EncKey from the peer and the peer will respond that it
  // is capable of sending it, but as this is SC pairing that should not occur.
  KeyDistGenField remote_keys{KeyDistGen::kEncKey}, local_keys{0};
  FastForwardToScLtk(&ltk_bytes,
                     kExpectedSecurity.level(),
                     remote_keys,
                     local_keys,
                     BondableMode::Bondable);

  const LTK kExpectedLtk(kExpectedSecurity, hci_spec::LinkKey(ltk_bytes, 0, 0));
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(kExpectedLtk.key(), fake_link()->ltk());

  // The host should have requested encryption.
  EXPECT_EQ(1, fake_link()->start_encryption_count());

  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();

  // Pairing should succeed without any messages being sent in "Phase 3". The
  // LTK was generated in SC Phase 2, and as the pairing is bondable, it is
  // included in the callback.
  EXPECT_EQ(1, pairing_data_callback_count());
  EXPECT_TRUE(peer_ltk().has_value());
  EXPECT_EQ(kExpectedLtk, peer_ltk());
  EXPECT_FALSE(irk());
  EXPECT_FALSE(identity());
  EXPECT_FALSE(csrk());

  // Should have been called at least once to determine local identity
  // availability.
  EXPECT_NE(0, local_id_info_callback_count());

  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(0, id_info_count());
  EXPECT_EQ(0, id_addr_info_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  EXPECT_EQ(kExpectedSecurity, sec_props());

  // The security properties should have been updated to match the LTK.
  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(sec_props(), pairing()->security());

  ASSERT_TRUE(fake_link()->ltk());
}

// Tests that for Secure Connections non-bondable mode, the pairing procedure
// ends after encryption with the key generated in Phase 2, but the upper layers
// are not notified of that key as an LTK.
TEST_F(InitiatorPairingTest, ScPhase3NonBondableCompleteWithoutKeyExchange) {
  // Must have DisplayYesNo IOC to generate Authenticated security per
  // kExpectedSecurity
  SetUpSecurityManager(IOCapability::kDisplayYesNo);
  const SecurityProperties kExpectedSecurity(SecurityLevel::kAuthenticated,
                                             kMaxEncryptionKeySize,
                                             /*secure_connections=*/true);
  UInt128 ltk_bytes;
  FastForwardToScLtk(&ltk_bytes,
                     kExpectedSecurity.level(),
                     KeyDistGenField{0},
                     KeyDistGenField{0},
                     BondableMode::NonBondable);

  const hci_spec::LinkKey kExpectedLinkKey(ltk_bytes, 0, 0);
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(kExpectedLinkKey, fake_link()->ltk());

  // The host should have requested encryption.
  EXPECT_EQ(1, fake_link()->start_encryption_count());

  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();

  // Pairing should succeed with the LTK as we are in SC, but as the pairing is
  // non-bondable, no LTK should be relayed up to the delegate.
  EXPECT_EQ(0, pairing_data_callback_count());

  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(0, id_info_count());
  EXPECT_EQ(0, id_addr_info_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  EXPECT_EQ(kExpectedSecurity, sec_props());

  // The security properties should have been updated to match the LTK.
  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(sec_props(), pairing()->security());

  ASSERT_TRUE(fake_link()->ltk());
}

TEST_F(InitiatorPairingTest, Phase3EncryptionInformationReceivedTwice) {
  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey);

  // Pairing should still be in progress.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_data_callback_count());

  ReceiveEncryptionInformation(UInt128());
  RunUntilIdle();

  // Waiting for EDIV and Rand
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());

  // Send the LTK twice. This should cause pairing to fail.
  ReceiveEncryptionInformation(UInt128());
  RunUntilIdle();
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());
  EXPECT_EQ(ErrorCode::kUnspecifiedReason, received_error_code());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

// The responder sends EDIV and Rand before LTK.
TEST_F(InitiatorPairingTest, Phase3CentralIdentificationReceivedInWrongOrder) {
  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey);

  // Pairing should still be in progress.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_data_callback_count());

  // Send central identification before encryption information. This should
  // cause pairing to fail.
  ReceiveCentralIdentification(1, 2);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());
  EXPECT_EQ(ErrorCode::kUnspecifiedReason, received_error_code());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

// The responder sends the sample LTK from the specification doc
TEST_F(InitiatorPairingTest, Phase3CentralIdentificationReceiveSampleLTK) {
  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey);

  const UInt128 kLtkSample{{0xBF,
                            0x01,
                            0xFB,
                            0x9D,
                            0x4E,
                            0xF3,
                            0xBC,
                            0x36,
                            0xD8,
                            0x74,
                            0xF5,
                            0x39,
                            0x41,
                            0x38,
                            0x68,
                            0x4C}};

  // Pairing should still be in progress.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_data_callback_count());

  // Send a bad LTK, this should cause pairing to fail.
  ReceiveEncryptionInformation(kLtkSample);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());
  EXPECT_EQ(ErrorCode::kUnspecifiedReason, received_error_code());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

// The responder sends the sample Rand from the specification doc
TEST_F(InitiatorPairingTest, Phase3CentralIdentificationReceiveExampleRand) {
  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey);

  uint64_t kRandSample = 0xABCDEF1234567890;
  uint16_t kEDiv = 20;

  // Pairing should still be in progress.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_data_callback_count());

  // Send a bad Rand, this should cause pairing to fail.
  ReceiveEncryptionInformation(UInt128());
  ReceiveCentralIdentification(kRandSample, kEDiv);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());
  EXPECT_EQ(ErrorCode::kUnspecifiedReason, received_error_code());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

// The responder sends an LTK that is longer than the max key size
TEST_F(InitiatorPairingTest, Phase3CentralIdentificationReceiveLongLTK) {
  UInt128 stk;
  auto max_key_size = 8;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey,
                      0,
                      max_key_size);

  const UInt128 kLtk{{1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8}};

  // Pairing should still be in progress.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_data_callback_count());

  // Send a long LTK, this should cause pairing to fail.
  ReceiveEncryptionInformation(kLtk);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(ErrorCode::kInvalidParameters), security_status());
  EXPECT_EQ(ErrorCode::kInvalidParameters, received_error_code());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

TEST_F(InitiatorPairingTest, Phase3CentralIdentificationReceivedTwice) {
  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey);

  // Pairing should still be in progress.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_data_callback_count());

  constexpr uint16_t kEdiv = 1;
  constexpr uint64_t kRand = 2;
  constexpr uint16_t kDupEdiv = 3;
  constexpr uint64_t kDupRand = 4;

  // Send duplicate central identification. Pairing should complete with the
  // first set of information. The second set should get ignored.
  ReceiveEncryptionInformation(UInt128());
  ReceiveCentralIdentification(kRand, kEdiv);
  ReceiveCentralIdentification(kDupRand, kDupEdiv);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(1, pairing_data_callback_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());
  ASSERT_TRUE(pairing_data().peer_ltk.has_value());
  EXPECT_EQ(kEdiv, pairing_data().peer_ltk->key().ediv());
  EXPECT_EQ(kRand, pairing_data().peer_ltk->key().rand());
}

// Pairing completes after obtaining peer encryption information only.
TEST_F(InitiatorPairingTest, Phase3CompleteWithReceivingEncKey) {
  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey);

  const UInt128 kLTK{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  uint64_t kRand = 5;
  uint16_t kEDiv = 20;

  ReceiveEncryptionInformation(kLTK);
  ReceiveCentralIdentification(kRand, kEDiv);
  RunUntilIdle();

  // Pairing should have succeeded.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  // LTK should have been assigned to the link.
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(kLTK, fake_link()->ltk()->value());
  EXPECT_EQ(kRand, fake_link()->ltk()->rand());
  EXPECT_EQ(kEDiv, fake_link()->ltk()->ediv());

  // We don't re-encrypt with the LTK while the link is already authenticated
  // with the STK.
  EXPECT_EQ(1, fake_link()->start_encryption_count());

  EXPECT_EQ(SecurityLevel::kEncrypted, sec_props().level());
  EXPECT_EQ(16u, sec_props().enc_key_size());
  EXPECT_FALSE(sec_props().secure_connections());

  // Should have been called at least once to determine local identity
  // availability.
  EXPECT_NE(0, local_id_info_callback_count());

  // Local identity information should not have been distributed by us since it
  // isn't available.
  EXPECT_EQ(0, id_info_count());
  EXPECT_EQ(0, id_addr_info_count());

  // Should have notified the LTK.
  EXPECT_EQ(1, pairing_data_callback_count());
  ASSERT_TRUE(peer_ltk());
  ASSERT_FALSE(irk());
  ASSERT_FALSE(identity());
  ASSERT_FALSE(csrk());
  EXPECT_EQ(sec_props(), peer_ltk()->security());
  EXPECT_EQ(kLTK, peer_ltk()->key().value());
  EXPECT_EQ(kRand, peer_ltk()->key().rand());
  EXPECT_EQ(kEDiv, peer_ltk()->key().ediv());

  // No security property update should have been sent for the LTK. This is
  // because the LTK and the STK are expected to have the same properties.
  EXPECT_EQ(1, new_sec_props_count());
}

TEST_F(InitiatorPairingTest, Phase3CompleteWithSendingEncKey) {
  UInt128 stk;
  KeyDistGenField remote_keys{0u}, local_keys{KeyDistGen::kEncKey};
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      remote_keys,
                      local_keys);
  RunUntilIdle();

  // Pairing should have succeeded.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  // Only the STK should be assigned to the link, as the distributed LTK was
  // initiator-generated. This means it can only be used to encrypt future
  // connections where the roles are reversed.
  EXPECT_EQ(stk, fake_link()->ltk()->value());
  EXPECT_EQ(1, fake_link()->start_encryption_count());

  // Should have been called at least once to determine local identity
  // availability.
  EXPECT_NE(0, local_id_info_callback_count());

  // Should have notified pairing data callback with the LTK.
  EXPECT_EQ(1, pairing_data_callback_count());
  ASSERT_TRUE(local_ltk());

  // LTK sent OTA should match what we notified the pairing data callback with.
  EXPECT_EQ(local_ltk()->key(), hci_spec::LinkKey(enc_info(), rand(), ediv()));
}

// Pairing completes after obtaining short encryption information only.
TEST_F(InitiatorPairingTest, Phase3CompleteWithShortEncKey) {
  UInt128 stk;
  uint8_t max_key_size = 12;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey,
                      0u,
                      max_key_size);

  // This LTK is within the max_key_size specified above.
  const UInt128 kLTK{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 0, 0, 0, 0}};
  uint64_t kRand = 5;
  uint16_t kEDiv = 20;

  ReceiveEncryptionInformation(kLTK);
  ReceiveCentralIdentification(kRand, kEDiv);
  RunUntilIdle();

  // Pairing should have succeeded.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  // LTK should have been assigned to the link.
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(kLTK, fake_link()->ltk()->value());
  EXPECT_EQ(kRand, fake_link()->ltk()->rand());
  EXPECT_EQ(kEDiv, fake_link()->ltk()->ediv());

  // We don't re-encrypt with the LTK while the link is already authenticated
  // with the STK.
  EXPECT_EQ(1, fake_link()->start_encryption_count());

  EXPECT_EQ(SecurityLevel::kEncrypted, sec_props().level());
  EXPECT_EQ(max_key_size, sec_props().enc_key_size());
  EXPECT_FALSE(sec_props().secure_connections());

  // Should have been called at least once to determine local identity
  // availability.
  EXPECT_NE(0, local_id_info_callback_count());

  // Local identity information should not have been distributed by us since it
  // isn't available.
  EXPECT_EQ(0, id_info_count());
  EXPECT_EQ(0, id_addr_info_count());

  // Should have notified the LTK.
  EXPECT_EQ(1, pairing_data_callback_count());
  ASSERT_TRUE(peer_ltk());
  ASSERT_FALSE(irk());
  ASSERT_FALSE(identity());
  ASSERT_FALSE(csrk());
  EXPECT_EQ(sec_props(), peer_ltk()->security());
  EXPECT_EQ(kLTK, peer_ltk()->key().value());
  EXPECT_EQ(kRand, peer_ltk()->key().rand());
  EXPECT_EQ(kEDiv, peer_ltk()->key().ediv());

  // No security property update should have been sent for the LTK. This is
  // because the LTK and the STK are expected to have the same properties.
  EXPECT_EQ(1, new_sec_props_count());
}

TEST_F(InitiatorPairingTest, Phase3WithLocalIdKey) {
  IdentityInfo local_id_info;
  local_id_info.irk = UInt128{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  local_id_info.address = kLocalAddr;
  set_local_id_info(local_id_info);

  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      0,                    // remote keys
                      KeyDistGen::kIdKey);  // local keys

  // Local identity information should have been sent.
  EXPECT_EQ(1, id_info_count());
  EXPECT_EQ(local_id_info.irk, id_info());
  EXPECT_EQ(1, id_addr_info_count());
  EXPECT_EQ(local_id_info.address, id_addr_info());

  // Pairing should succeed without notifying any keys.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

// Tests that pairing results in an error if a local ID key was initially
// negotiated but gets removed before the distribution phase.
TEST_F(InitiatorPairingTest, Phase3IsAbortedIfLocalIdKeyIsRemoved) {
  IdentityInfo local_id_info;
  local_id_info.irk = UInt128{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  local_id_info.address = kLocalAddr;
  set_local_id_info(local_id_info);

  UInt128 stk;
  FastForwardToSTK(&stk,
                   SecurityLevel::kEncrypted,
                   0,                    // remote keys
                   KeyDistGen::kIdKey);  // local keys

  // Local identity information should not have been sent yet.
  EXPECT_EQ(0, id_info_count());
  EXPECT_EQ(0, id_addr_info_count());

  // Pairing still in progress.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // Remove the local identity information.
  set_local_id_info(std::nullopt);

  // Encrypt with the STK to finish phase 2.
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();

  // Pairing should have been aborted.
  EXPECT_EQ(0, id_info_count());
  EXPECT_EQ(0, id_addr_info_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());
}

TEST_F(InitiatorPairingTest, Phase3IRKReceivedTwice) {
  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kIdKey);

  // Pairing should still be in progress.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_data_callback_count());

  ReceiveIdentityResolvingKey(UInt128());
  RunUntilIdle();

  // Waiting for identity address.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // Send an IRK again. This should cause pairing to fail.
  ReceiveIdentityResolvingKey(UInt128());
  RunUntilIdle();
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());
  EXPECT_EQ(ErrorCode::kUnspecifiedReason, received_error_code());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

// The responder sends its identity address before sending its IRK.
TEST_F(InitiatorPairingTest, Phase3IdentityAddressReceivedInWrongOrder) {
  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kIdKey);

  // Pairing should still be in progress.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_data_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // Send identity address before the IRK. This should cause pairing to fail.
  ReceiveIdentityAddress(kPeerAddr);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());
  EXPECT_EQ(ErrorCode::kUnspecifiedReason, received_error_code());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

TEST_F(InitiatorPairingTest, Phase3IdentityAddressReceivedTwice) {
  UInt128 stk;
  // Request enc key to prevent pairing from completing after sending the first
  // identity address.
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey | KeyDistGen::kIdKey);

  // Pairing should still be in progress.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_data_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  ReceiveIdentityResolvingKey(UInt128());
  ReceiveIdentityAddress(kPeerAddr);
  ReceiveIdentityAddress(kPeerAddr);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), security_status());
  EXPECT_EQ(ErrorCode::kUnspecifiedReason, received_error_code());
  EXPECT_EQ(security_status(), pairing_complete_status());
}

// Pairing completes after obtaining identity information only.
TEST_F(InitiatorPairingTest, Phase3CompleteWithIdKey) {
  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kIdKey);

  // Pairing should still be in progress.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_data_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  const UInt128 kIRK{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};

  ReceiveIdentityResolvingKey(kIRK);
  ReceiveIdentityAddress(kPeerAddr);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  // The link remains encrypted with the STK.
  EXPECT_EQ(SecurityLevel::kEncrypted, sec_props().level());
  EXPECT_EQ(16u, sec_props().enc_key_size());
  EXPECT_FALSE(sec_props().secure_connections());

  // No LTK was exchanged, so the pairing data callback should not be called.
  EXPECT_EQ(0, pairing_data_callback_count());
}

TEST_F(InitiatorPairingTest, Phase3CompleteWithAllKeys) {
  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey | KeyDistGen::kIdKey);

  const UInt128 kLTK{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  const UInt128 kIRK{{8, 7, 6, 5, 4, 3, 2, 1, 1, 2, 3, 4, 5, 6, 7, 8}};
  uint64_t kRand = 5;
  uint16_t kEDiv = 20;

  // The link should be assigned the STK as its link key.
  EXPECT_EQ(stk, fake_link()->ltk()->value());

  // Receive EncKey
  ReceiveEncryptionInformation(kLTK);
  ReceiveCentralIdentification(kRand, kEDiv);
  RunUntilIdle();

  // Pairing still pending. SMP does not assign the LTK to the link until
  // pairing completes.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // Receive IdKey
  ReceiveIdentityResolvingKey(kIRK);
  ReceiveIdentityAddress(kPeerAddr);
  RunUntilIdle();

  // Pairing should have succeeded
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  // LTK should have been assigned to the link.
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(kLTK, fake_link()->ltk()->value());
  EXPECT_EQ(kRand, fake_link()->ltk()->rand());
  EXPECT_EQ(kEDiv, fake_link()->ltk()->ediv());

  // We don't re-encrypt with the LTK while the link is already authenticated
  // with the STK.
  EXPECT_EQ(1, fake_link()->start_encryption_count());

  EXPECT_EQ(SecurityLevel::kEncrypted, sec_props().level());
  EXPECT_EQ(16u, sec_props().enc_key_size());
  EXPECT_FALSE(sec_props().secure_connections());

  // Should have notified the LTK.
  EXPECT_EQ(1, pairing_data_callback_count());
  ASSERT_TRUE(peer_ltk());
  ASSERT_TRUE(irk());
  ASSERT_TRUE(identity());
  ASSERT_FALSE(csrk());
  EXPECT_EQ(sec_props(), peer_ltk()->security());
  EXPECT_EQ(kLTK, peer_ltk()->key().value());
  EXPECT_EQ(kRand, peer_ltk()->key().rand());
  EXPECT_EQ(kEDiv, peer_ltk()->key().ediv());
  EXPECT_EQ(sec_props(), irk()->security());
  EXPECT_EQ(kIRK, irk()->value());
  EXPECT_EQ(kPeerAddr, *identity());
}

TEST_F(InitiatorPairingTest, GenerateCrossTransportLinkKey) {
  UInt128 stk;
  // Indicate support for SC and for link keys in both directions to enable
  // CTKG.
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/true,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kLinkKey,
                      KeyDistGen::kLinkKey);
  RunUntilIdle();

  // Pairing should have succeeded
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  // The PairingData should contain the CTKGenerated BR/EDR link key.
  EXPECT_TRUE(pairing_data().cross_transport_key.has_value());
}

TEST_F(InitiatorPairingTest, AssignLongTermKey) {
  SecurityProperties sec_props(
      SecurityLevel::kAuthenticated, 16, /*secure_connections=*/false);
  LTK ltk(sec_props, hci_spec::LinkKey());
  sm::PairingData bond_data;
  bond_data.peer_ltk = ltk;
  bond_data.local_ltk = ltk;
  peer().MutLe().SetBondData(bond_data);

  NewSecurityManager(
      Role::kInitiator, IOCapability::kDisplayOnly, BondableMode::Bondable);
  RunUntilIdle();

  EXPECT_EQ(1, fake_link()->start_encryption_count());
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(ltk.key(), *fake_link()->ltk());

  // The link security level is not assigned until successful encryption.
  EXPECT_EQ(SecurityProperties(), pairing()->security());
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();

  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(sec_props, new_sec_props());
  EXPECT_EQ(sec_props, pairing()->security());
}

TEST_F(InitiatorPairingTest, ReceiveSecurityRequest) {
  ReceiveSecurityRequest(AuthReq::kMITM);
  RunUntilIdle();

  // Should have requested pairing with MITM protection.
  EXPECT_EQ(1, pairing_request_count());
  const auto& params = local_pairing_cmd().view(1).To<PairingRequestParams>();
  EXPECT_TRUE(params.auth_req & AuthReq::kMITM);
}

TEST_F(InitiatorPairingTest, ReceiveSecurityRequestWhenPaired) {
  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey);
  EXPECT_EQ(stk, fake_link()->ltk()->value());
  EXPECT_EQ(1, pairing_request_count());

  // Receive EncKey and wait until the link is encrypted with the LTK.
  UInt128 kLTK{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  uint64_t kRand = 5;
  uint16_t kEDiv = 20;
  ReceiveEncryptionInformation(kLTK);
  ReceiveCentralIdentification(kRand, kEDiv);
  RunUntilIdle();
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();
  ASSERT_EQ(1, pairing_complete_count());
  ASSERT_EQ(fit::ok(), security_status());
  ASSERT_TRUE(peer_ltk());
  ASSERT_EQ(SecurityLevel::kEncrypted, sec_props().level());
  ASSERT_EQ(1, fake_link()->start_encryption_count());  // Once for the STK
  ASSERT_EQ(1, pairing_request_count());

  // Receive a security request with the no MITM requirement. This should
  // trigger an encryption key refresh and no pairing request.
  ReceiveSecurityRequest();
  EXPECT_EQ(1, pairing_request_count());

  // Once for the STK and once again due to the locally initiated key refresh.
  ASSERT_EQ(2, fake_link()->start_encryption_count());

  // Receive a security request with a higher security requirement. This should
  // trigger a pairing request.
  ReceiveSecurityRequest(AuthReq::kMITM);
  EXPECT_EQ(2, pairing_request_count());
  const auto& params = local_pairing_cmd().view(1).To<PairingRequestParams>();
  EXPECT_TRUE(params.auth_req & AuthReq::kMITM);
}

TEST_F(InitiatorPairingTest,
       ReceiveMITMSecurityRequestLocalIoCapNoInputNoOutput) {
  SetUpSecurityManager(IOCapability::kNoInputNoOutput);
  ReceiveSecurityRequest(AuthReq::kMITM);
  // We should notify the peer that we cannot complete the security request due
  // to authentication requirements.
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(ErrorCode::kAuthenticationRequirements, received_error_code());

  // When we receive a Security Request, we start a timer. Run the loop to
  // ensure that when we can't fulfill the Security Request, we stop the timer
  // before it expires as we never started pairing.
  RunFor(kPairingTimeout + std::chrono::seconds(1));
  // Double check we haven't sent any more Pairing Failed messages
  EXPECT_EQ(1, pairing_failed_count());
  // We should not notify local clients of any pairing completion, because no
  // pairing ever started.
  EXPECT_EQ(0, pairing_complete_count());
}

TEST_F(InitiatorPairingTest, RejectPairingRequest) {
  // Although we are the initiator, set the peer_initiator=true for this test so
  // that we emulate reception of the Pairing Request command, not the Pairing
  // Response command.
  ReceivePairingFeatures(IOCapability::kDisplayYesNo,
                         AuthReqField{0},
                         kMaxEncryptionKeySize,
                         /*peer_initiator=*/true);
  RunUntilIdle();
  // We should reject the security request with CommandNotSupported as
  // initiator.
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(ErrorCode::kCommandNotSupported, received_error_code());

  // Run for the full pairing timeout to ensure we do not timeout due to sending
  // a message.
  RunFor(kPairingTimeout + std::chrono::seconds(1));
  // No pairing occurred, as we rejected the security request command.
  EXPECT_EQ(0, pairing_complete_count());
  EXPECT_EQ(1, pairing_failed_count());
}

TEST_F(InitiatorPairingTest, PairingTimeoutWorks) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();
  ASSERT_EQ(1, pairing_request_count());
  // Expiration of the pairing timeout should trigger the link error callback
  // per v5.2 Vol. 3 Part H 3.4. Link disconnection will generally cause channel
  // closure, so this simulates that behavior to validate that SM handles this
  // safely.
  fake_chan()->SetLinkErrorCallback([chan = fake_chan()]() { chan->Close(); });
  RunFor(kPairingTimeout);
  EXPECT_TRUE(fake_chan()->link_error());
  ASSERT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(HostError::kTimedOut), security_status());
  ASSERT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(HostError::kTimedOut), pairing_complete_status());
}

TEST_F(InitiatorPairingTest, NoTimeoutAfterSuccessfulPairing) {
  UInt128 out_stk;
  FastForwardToPhase3(&out_stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGenField{0},
                      KeyDistGenField{0});
  ASSERT_EQ(1, pairing_complete_count());
  ASSERT_EQ(1, security_callback_count());
  ASSERT_EQ(fit::ok(), security_status());
  ASSERT_EQ(fit::ok(), pairing_complete_status());
  // Verify that no timeout occurs after a successful pairing followed by a long
  // interval.
  RunFor(kPairingTimeout * 2);
  ASSERT_EQ(1, pairing_complete_count());
  ASSERT_EQ(1, security_callback_count());
  EXPECT_NE(ToResult(HostError::kTimedOut), pairing_complete_status());
  EXPECT_NE(ToResult(HostError::kTimedOut), security_status());
}

TEST_F(InitiatorPairingTest, AbortStopsPairingTimer) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();
  ASSERT_EQ(1, pairing_request_count());
  pairing()->Abort();
  // Calling Abort should stop the pairing procedure and the timer.
  ASSERT_EQ(1, pairing_complete_count());
  ASSERT_EQ(1, security_callback_count());
  // Run the loop for a time that would cause a timeout if a timer were active.
  RunFor(kPairingTimeout * 2);
  ASSERT_EQ(1, pairing_complete_count());
  ASSERT_EQ(1, security_callback_count());
  EXPECT_NE(ToResult(HostError::kTimedOut), pairing_complete_status());
  EXPECT_NE(ToResult(HostError::kTimedOut), security_status());
}

TEST_F(InitiatorPairingTest, ResetStopsPairingTimer) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();
  ASSERT_EQ(1, pairing_request_count());
  pairing()->Reset(IOCapability::kDisplayYesNo);
  // Resetting the pairing aborts the current procedure.
  ASSERT_EQ(1, pairing_complete_count());
  ASSERT_EQ(1, security_callback_count());
  // Run the loop for a time that would cause a timeout if a timer were active.
  RunFor(kPairingTimeout * 2);
  ASSERT_EQ(1, pairing_complete_count());
  ASSERT_EQ(1, security_callback_count());
  EXPECT_NE(ToResult(HostError::kTimedOut), pairing_complete_status());
  EXPECT_NE(ToResult(HostError::kTimedOut), security_status());
}

TEST_F(InitiatorPairingTest, SendingMessageRestartsTimer) {
  // SM will send the Pairing Request, which is special-cased to "reset and
  // start" the pairing timer (v5.2 Vol. 3 Part H 3.4), and thus not under test
  // here.
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();
  ASSERT_EQ(1, pairing_request_count());
  // Run the loop until the pairing timeout has almost expired.
  RunFor(kPairingTimeout - pw::chrono::SystemClock::duration(1));
  // Receive the not special-cased Pairing Response, which should trigger SM to
  // send the also not special-cased Pairing Confirm.
  ReceivePairingFeatures();
  // Run the loop for 1 more second, which would timeout if the timer had not
  // been reset.
  RunFor(pw::chrono::SystemClock::duration(1));
  // The timeout should not have triggered, so there should be no notification
  // of pairing failure.
  ASSERT_EQ(0, pairing_complete_count());
  ASSERT_EQ(0, security_callback_count());
  // Verify that the timer is in fact still active; without receiving further
  // messages, the timeout should trigger.
  RunFor(kPairingTimeout);
  ASSERT_EQ(1, pairing_complete_count());
  ASSERT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(HostError::kTimedOut), pairing_complete_status());
  EXPECT_EQ(ToResult(HostError::kTimedOut), security_status());
}

TEST_F(InitiatorPairingTest,
       ModifyAssignedLinkLtkBeforeSecurityRequestCausesDisconnect) {
  SecurityProperties sec_props(
      SecurityLevel::kAuthenticated, 16, /*secure_connections=*/false);
  const LTK kOriginalLtk(sec_props, hci_spec::LinkKey({1}, 2, 3));
  sm::PairingData bond_data;
  bond_data.peer_ltk = kOriginalLtk;
  bond_data.local_ltk = kOriginalLtk;
  peer().MutLe().SetBondData(bond_data);

  NewSecurityManager(
      Role::kInitiator, IOCapability::kDisplayOnly, BondableMode::Bondable);
  RunUntilIdle();
  EXPECT_EQ(1, fake_link()->start_encryption_count());
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));

  const hci_spec::LinkKey kModifiedLtk(hci_spec::LinkKey({4}, 5, 6));
  fake_link()->set_ltk(kModifiedLtk);
  // When we receive the Security Request on a bonded connection, we will
  // refresh the encryption key. This checks that the link LTK = the SMP LTK
  // which is not the case.
  ReceiveSecurityRequest(AuthReqField{0});
  RunUntilIdle();
  ASSERT_TRUE(fake_chan()->link_error());
  ASSERT_EQ(1, auth_failure_callback_count());
  ASSERT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING),
            auth_failure_status());
}

TEST_F(ResponderPairingTest, SuccessfulPairAfterResetInProgressPairing) {
  ReceivePairingRequest();
  RunUntilIdle();
  // At this point, we expect to have completed Phase 1, and pairing should
  // still be in progress.
  EXPECT_EQ(1, pairing_response_count());

  pairing()->Abort();
  RunUntilIdle();
  // Pairing should have failed and ended.
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, pairing_complete_count());

  // Verify that the next pairing request is properly handled
  ReceivePairingRequest();
  RunUntilIdle();
  // At this point, we expect to have completed Phase 1, and pairing should
  // still be in progress.
  EXPECT_EQ(2, pairing_response_count());
}

TEST_F(ResponderPairingTest, SecurityRequestCausesPairing) {
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();
  AuthReqField expected_auth_req = AuthReq::kBondingFlag;
  EXPECT_EQ(1, security_request_count());
  EXPECT_EQ(expected_auth_req, security_request_payload());
  UInt128 ltk_bytes;
  FastForwardToPhase3(&ltk_bytes, /*secure_connections=*/true);
  // Pairing should have succeeded
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  // LTK should have been assigned to the link.
  hci_spec::LinkKey kExpectedLinkKey(ltk_bytes, 0, 0);
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(kExpectedLinkKey, fake_link()->ltk());

  EXPECT_EQ(SecurityLevel::kEncrypted, sec_props().level());
  EXPECT_EQ(16u, sec_props().enc_key_size());
  EXPECT_TRUE(sec_props().secure_connections());

  // Should have notified the LTK.
  EXPECT_EQ(1, pairing_data_callback_count());
  ASSERT_TRUE(local_ltk());
  EXPECT_EQ(sec_props(), local_ltk()->security());
  EXPECT_EQ(kExpectedLinkKey, local_ltk()->key());
}

TEST_F(ResponderPairingTest, SecurityRequestWithExistingLtk) {
  const SecurityProperties kProps(SecurityLevel::kAuthenticated,
                                  kMaxEncryptionKeySize,
                                  /*secure_connections=*/true);
  const LTK kLtk(kProps, hci_spec::LinkKey({1, 2, 3}, 0, 0));
  sm::PairingData bond_data;
  bond_data.peer_ltk = kLtk;
  bond_data.local_ltk = kLtk;
  peer().MutLe().SetBondData(bond_data);

  NewSecurityManager(
      Role::kResponder, IOCapability::kDisplayOnly, BondableMode::Bondable);

  // LTK should have been assigned to the link.
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(kLtk.key(), fake_link()->ltk());

  // Make the Security Upgrade request
  UpgradeSecurity(SecurityLevel::kAuthenticated);
  RunUntilIdle();
  AuthReqField expected_auth_req = AuthReq::kBondingFlag | AuthReq::kMITM;
  EXPECT_EQ(1, security_request_count());
  EXPECT_EQ(expected_auth_req, security_request_payload());
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));

  // Security should be upgraded.
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(kProps.level(), sec_props().level());
  EXPECT_EQ(16u, sec_props().enc_key_size());
  EXPECT_TRUE(sec_props().secure_connections());

  // No pairing should have taken place - we had an already-bonded LTK.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, pairing_complete_count());
  EXPECT_EQ(0, pairing_data_callback_count());
}

TEST_F(ResponderPairingTest,
       SecurityRequestInitiatorEncryptsWithInsufficientSecurityLtk) {
  const SecurityProperties kProps(SecurityLevel::kEncrypted,
                                  kMaxEncryptionKeySize,
                                  /*secure_connections=*/true);
  const LTK kLtk(kProps, hci_spec::LinkKey({1, 2, 3}, 0, 0));
  sm::PairingData bond_data;
  bond_data.peer_ltk = kLtk;
  bond_data.local_ltk = kLtk;
  peer().MutLe().SetBondData(bond_data);

  NewSecurityManager(
      Role::kResponder, IOCapability::kDisplayOnly, BondableMode::Bondable);

  // LTK should have been assigned to the link.
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(kLtk.key(), fake_link()->ltk());

  // Make a security request for authenticated security
  UpgradeSecurity(SecurityLevel::kAuthenticated);
  RunUntilIdle();
  AuthReqField expected_auth_req = AuthReq::kBondingFlag | AuthReq::kMITM;
  EXPECT_EQ(1, security_request_count());
  EXPECT_EQ(expected_auth_req, security_request_payload());

  // Pretend the SMP initiator started encryption with the bonded LTK of
  // SecurityLevel::kEncrypted.
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));

  // If the peer responds to our MITM security request by encrypting with an
  // unauthenticated key, they stored the LTK/handle security request
  // incorrectly - either way, disconnect the link.
  ASSERT_TRUE(fake_chan()->link_error());
}

TEST_F(ResponderPairingTest,
       AuthenticatedSecurityRequestWithInsufficientIoCapRejected) {
  SetUpSecurityManager(IOCapability::kNoInputNoOutput);
  // Make a security request for authenticated security
  UpgradeSecurity(SecurityLevel::kAuthenticated);
  RunUntilIdle();
  // The security callback should have been rejected w/o sending any messages,
  // as our IOCap cannot perform authenticated pairing.
  EXPECT_EQ(0, security_request_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kAuthenticationRequirements),
            security_status());
  EXPECT_EQ(SecurityLevel::kNoSecurity, sec_props().level());
}

TEST_F(ResponderPairingTest, HandlesMultipleSecurityRequestsCorrectly) {
  // Make a security request for encrypted security
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();
  AuthReqField expected_auth_req = AuthReq::kBondingFlag;
  EXPECT_EQ(1, security_request_count());
  EXPECT_EQ(expected_auth_req, security_request_payload());

  // Making another security request, this time for authenticated security,
  // while the first is still pending should not cause another Security Request
  // message to be sent.
  UpgradeSecurity(SecurityLevel::kAuthenticated);
  RunUntilIdle();
  EXPECT_EQ(1, security_request_count());

  // Handle the first Security Request
  UInt128 ltk_bytes;
  FastForwardToPhase3(
      &ltk_bytes, /*secure_connections=*/true, SecurityLevel::kEncrypted);
  // Pairing should have succeeded
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  EXPECT_EQ(SecurityLevel::kEncrypted, sec_props().level());

  // Should have notified the LTK.
  EXPECT_EQ(1, pairing_data_callback_count());
  ASSERT_TRUE(local_ltk());
  EXPECT_EQ(sec_props(), local_ltk()->security());
  EXPECT_EQ(ltk_bytes, local_ltk()->key().value());

  // After the first pairing satisfied the kEncrypted Security Request, the
  // pending kAuthenticated Security Request should have been sent immediately.
  EXPECT_EQ(2, security_request_count());
}

TEST_F(ResponderPairingTest, ReceiveSecondPairingRequestWhilePairing) {
  ReceivePairingRequest();
  RunUntilIdle();

  // We should have sent a pairing response and should now be in Phase 2,
  // waiting for the peer to send us Mconfirm.
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(1, pairing_response_count());
  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // This should cause pairing to be aborted.
  ReceivePairingRequest();
  RunUntilIdle();
  EXPECT_EQ(0, pairing_request_count());
  // We will abort the second pairing request without responding if we're
  // already in progress
  EXPECT_EQ(1, pairing_response_count());
  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  ASSERT_EQ(ErrorCode::kUnspecifiedReason, received_error_code());
  EXPECT_EQ(ToResult(received_error_code().value()), pairing_complete_status());
}

TEST_F(ResponderPairingTest, ReceiveConfirmValueWhileWaitingForTK) {
  bool tk_requested = false;
  ConfirmCallback respond;
  set_confirm_delegate([&](ConfirmCallback cb) {
    tk_requested = true;
    respond = std::move(cb);
  });

  ReceivePairingRequest();
  RunUntilIdle();
  ASSERT_TRUE(tk_requested);

  UInt128 confirm;
  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  // Pairing should still be in progress without sending out any packets.
  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // Respond with the TK. This should cause us to send Sconfirm.
  respond(true);
  RunUntilIdle();
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());
}

TEST_F(ResponderPairingTest, LegacyPhase2ReceivePairingRandomInWrongOrder) {
  ReceivePairingRequest();
  RunUntilIdle();

  // We should have sent a pairing response and should now be in Phase 2,
  // waiting for the peer to send us Mconfirm.
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(1, pairing_response_count());
  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // Peer sends Mrand before Mconfirm.
  UInt128 random;
  ReceivePairingRandom(random);
  RunUntilIdle();
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(1, pairing_response_count());
  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ErrorCode::kUnspecifiedReason, received_error_code());
  EXPECT_EQ(ToResult(ErrorCode::kUnspecifiedReason), pairing_complete_status());
}

TEST_F(ResponderPairingTest, LegacyPhase2MconfirmValueInvalid) {
  ReceivePairingRequest();
  RunUntilIdle();

  // We should have sent a pairing response and should now be in Phase 2,
  // waiting for the peer to send us Mconfirm.
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(1, pairing_response_count());
  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // Set up values that don't match.
  UInt128 confirm, random;
  confirm.fill(0);
  random.fill(1);

  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  // We should have sent Sconfirm.
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(1, pairing_response_count());
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // Peer sends Mrand that doesn't match. We should reject the pairing
  // without sending Srand.
  ReceivePairingRandom(random);
  RunUntilIdle();
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(1, pairing_response_count());
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ErrorCode::kConfirmValueFailed, received_error_code());
  EXPECT_EQ(ToResult(ErrorCode::kConfirmValueFailed),
            pairing_complete_status());
}

TEST_F(ResponderPairingTest, LegacyPhase2ConfirmValuesExchanged) {
  ReceivePairingRequest();
  RunUntilIdle();

  // We should have sent a pairing response and should now be in Phase 2,
  // waiting for the peer to send us Mconfirm.
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(1, pairing_response_count());
  EXPECT_EQ(0, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // Set up Sconfirm and Srand values that match.
  UInt128 confirm, random;
  GenerateMatchingLegacyConfirmAndRandom(&confirm, &random);

  // Peer sends Mconfirm.
  ReceivePairingConfirm(confirm);
  RunUntilIdle();

  // We should have sent Sconfirm.
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(1, pairing_response_count());
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(0, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // Peer sends Mrand.
  ReceivePairingRandom(random);
  RunUntilIdle();

  // We should have sent Srand.
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(1, pairing_response_count());
  EXPECT_EQ(1, pairing_confirm_count());
  EXPECT_EQ(1, pairing_random_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, pairing_complete_count());

  // Sconfirm/Srand values we sent should be correct.
  UInt128 expected_confirm;
  GenerateLegacyConfirmValue(
      pairing_random(), &expected_confirm, /*peer_initiator=*/true);
  EXPECT_EQ(expected_confirm, pairing_confirm());
}

TEST_F(ResponderPairingTest, LegacyPhase3LocalLTKDistributionNoRemoteKeys) {
  EXPECT_EQ(0, enc_info_count());
  EXPECT_EQ(0, central_ident_count());

  UInt128 stk;
  KeyDistGenField remote_keys{0}, local_keys{KeyDistGen::kEncKey};
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      remote_keys,
                      local_keys);

  // Local LTK, EDiv, and Rand should be sent to the peer.
  EXPECT_EQ(1, enc_info_count());
  EXPECT_EQ(1, central_ident_count());
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(enc_info(), fake_link()->ltk()->value());
  EXPECT_EQ(ediv(), fake_link()->ltk()->ediv());
  EXPECT_EQ(rand(), fake_link()->ltk()->rand());

  // This LTK should be stored with the pairing data but the pairing callback
  // shouldn't be called because pairing wasn't initiated by UpgradeSecurity().
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());

  // Pairing is considered complete when all keys have been distributed even if
  // we're still encrypted with the STK. This is because the initiator may not
  // always re-encrypt the link with the LTK until a reconnection.
  EXPECT_EQ(1, pairing_data_callback_count());

  // Nonetheless the link should have been assigned the LTK.
  ASSERT_TRUE(pairing_data().local_ltk.has_value());
  EXPECT_EQ(fake_link()->ltk(), pairing_data().local_ltk->key());

  // Make sure that an encryption change has no effect.
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();
  EXPECT_EQ(1, pairing_data_callback_count());

  // No additional security property update should have been sent since the STK
  // and LTK have the same properties.
  EXPECT_EQ(1, new_sec_props_count());
}

TEST_F(ResponderPairingTest, LegacyPhase3LocalLTKDistributionWithRemoteKeys) {
  EXPECT_EQ(0, enc_info_count());
  EXPECT_EQ(0, central_ident_count());

  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kIdKey,    // remote keys
                      KeyDistGen::kEncKey);  // local keys

  // Local LTK, EDiv, and Rand should be sent to the peer - we don't assign the
  // new LTK to the link until pairing is complete.
  EXPECT_EQ(1, enc_info_count());
  EXPECT_EQ(1, central_ident_count());

  // No local identity information should have been sent.
  EXPECT_EQ(0, id_info_count());
  EXPECT_EQ(0, id_addr_info_count());

  // This LTK should be stored with the pairing data but the pairing callback
  // shouldn't be called because pairing wasn't initiated by UpgradeSecurity().
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());

  // Still waiting for initiator's keys.
  EXPECT_EQ(0, pairing_data_callback_count());

  const auto kIrk = Random<UInt128>();
  ReceiveIdentityResolvingKey(kIrk);
  ReceiveIdentityAddress(kPeerAddr);
  RunUntilIdle();

  // Pairing is considered complete when all keys have been distributed even if
  // we're still encrypted with the STK. This is because the initiator may not
  // always re-encrypt the link with the LTK until a reconnection.
  EXPECT_EQ(1, pairing_data_callback_count());

  // The peer should have sent us its identity information.
  ASSERT_TRUE(pairing_data().irk);
  EXPECT_EQ(kIrk, pairing_data().irk->value());
  ASSERT_TRUE(pairing_data().identity_address);
  EXPECT_EQ(kPeerAddr, *pairing_data().identity_address);

  // Nonetheless the link should have been assigned the LTK.
  ASSERT_TRUE(pairing_data().local_ltk.has_value());
  EXPECT_EQ(fake_link()->ltk(), pairing_data().local_ltk->key());
}

// Locally generated ltk length should match max key length specified
TEST_F(ResponderPairingTest, LegacyPhase3LocalLTKMaxLength) {
  EXPECT_EQ(0, enc_info_count());
  EXPECT_EQ(0, central_ident_count());

  UInt128 stk;
  uint16_t max_key_size = 7;

  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      0u,                   // remote keys
                      KeyDistGen::kEncKey,  // local keys
                      max_key_size);

  // Local LTK, EDiv, and Rand should be sent to the peer.
  EXPECT_EQ(1, enc_info_count());
  EXPECT_EQ(1, central_ident_count());
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(enc_info(), fake_link()->ltk()->value());
  EXPECT_EQ(ediv(), fake_link()->ltk()->ediv());
  EXPECT_EQ(rand(), fake_link()->ltk()->rand());

  // This LTK should be stored with the pairing data but the pairing callback
  // shouldn't be called because pairing wasn't initiated by UpgradeSecurity().
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(0, security_callback_count());

  // Pairing is considered complete when all keys have been distributed even if
  // we're still encrypted with the STK. This is because the initiator may not
  // always re-encrypt the link with the LTK until a reconnection.
  EXPECT_EQ(1, pairing_data_callback_count());

  // The link should have been assigned the LTK.
  ASSERT_TRUE(pairing_data().local_ltk.has_value());
  EXPECT_EQ(fake_link()->ltk(), pairing_data().local_ltk->key());

  // Ensure that most significant (16 - max_key_size) bytes are zero. The key
  // should be generated up to the max_key_size.
  auto ltk = pairing_data().local_ltk->key().value();
  for (auto i = max_key_size; i < ltk.size(); i++) {
    EXPECT_TRUE(ltk[i] == 0);
  }
}

TEST_F(ResponderPairingTest, LegacyPhase3ReceiveInitiatorEncKey) {
  UInt128 stk;
  KeyDistGenField remote_keys{KeyDistGen::kEncKey}, local_keys{0u};
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      remote_keys,
                      local_keys);

  const uint64_t kRand = 5;
  const uint16_t kEDiv = 20;
  const hci_spec::LinkKey kLTK(
      {1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}, kRand, kEDiv);

  ReceiveEncryptionInformation(kLTK.value());
  ReceiveCentralIdentification(kRand, kEDiv);
  RunUntilIdle();

  // Pairing should have succeeded.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());

  // No pairing callbacks needed as this is a peer-initiated pairing.
  EXPECT_EQ(0, security_callback_count());

  // Only the STK should be assigned to the link, as the distributed LTK was
  // initiator-generated. This means it can only be used to encrypt future
  // connections where the roles are reversed.
  EXPECT_EQ(stk, fake_link()->ltk()->value());

  // Should have been called at least once to determine local identity
  // availability.
  EXPECT_NE(0, local_id_info_callback_count());

  // Should have notified pairing data callback with the LTK.
  EXPECT_EQ(1, pairing_data_callback_count());
  ASSERT_TRUE(peer_ltk());

  // LTK received OTA should match what we notified the pairing data callback
  // with.
  EXPECT_EQ(kLTK, peer_ltk()->key());
}

TEST_F(ResponderPairingTest, LegacyPhase3LocalIdKeyDistributionWithRemoteKeys) {
  IdentityInfo local_id_info;
  local_id_info.irk = UInt128{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  local_id_info.address = kLocalAddr;
  set_local_id_info(local_id_info);

  EXPECT_EQ(0, enc_info_count());
  EXPECT_EQ(0, central_ident_count());

  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kIdKey,   // remote keys
                      KeyDistGen::kIdKey);  // local keys

  // No local LTK, EDiv, and Rand should be sent to the peer.
  EXPECT_EQ(0, enc_info_count());
  EXPECT_EQ(0, central_ident_count());

  // Local identity information should have been sent.
  EXPECT_EQ(1, id_info_count());
  EXPECT_EQ(local_id_info.irk, id_info());
  EXPECT_EQ(1, id_addr_info_count());
  EXPECT_EQ(local_id_info.address, id_addr_info());

  // Still waiting for initiator's keys.
  EXPECT_EQ(0, pairing_data_callback_count());

  const auto kIrk = Random<UInt128>();
  ReceiveIdentityResolvingKey(kIrk);
  ReceiveIdentityAddress(kPeerAddr);
  RunUntilIdle();

  // Pairing is considered complete when all keys have been distributed even if
  // we're still encrypted with the STK. This is because the initiator may not
  // always re-encrypt the link with the LTK until a reconnection.
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(fit::ok(), security_status());
  EXPECT_EQ(security_status(), pairing_complete_status());
  EXPECT_FALSE(peer().MutLe().is_pairing());
  EXPECT_EQ(0, pairing_data_callback_count());
}

TEST_F(ResponderPairingTest, AssignLongTermKey) {
  SecurityProperties sec_props(
      SecurityLevel::kAuthenticated, 16, /*secure_connections=*/false);
  LTK ltk(sec_props, hci_spec::LinkKey());
  sm::PairingData bond_data;
  bond_data.peer_ltk = ltk;
  bond_data.local_ltk = ltk;
  peer().MutLe().SetBondData(bond_data);

  NewSecurityManager(
      Role::kResponder, IOCapability::kDisplayOnly, BondableMode::Bondable);

  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(ltk.key(), *fake_link()->ltk());

  // No encryption request should have been made as the initiator is expected to
  // do it.
  EXPECT_EQ(0, fake_link()->start_encryption_count());

  // The link security level is not assigned until successful encryption.
  EXPECT_EQ(SecurityProperties(), pairing()->security());
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();

  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(sec_props, new_sec_props());
  EXPECT_EQ(sec_props, pairing()->security());
}

TEST_F(ResponderPairingTest, EncryptWithLinkKeyModifiedOutsideSmDisconnects) {
  SecurityProperties sec_props(
      SecurityLevel::kAuthenticated, 16, /*secure_connections=*/false);
  const LTK kOriginalLtk(sec_props, hci_spec::LinkKey({1}, 2, 3));
  sm::PairingData bond_data;
  bond_data.peer_ltk = kOriginalLtk;
  bond_data.local_ltk = kOriginalLtk;
  peer().MutLe().SetBondData(bond_data);

  NewSecurityManager(
      Role::kResponder, IOCapability::kDisplayOnly, BondableMode::Bondable);

  const hci_spec::LinkKey kModifiedLtk(hci_spec::LinkKey({4}, 5, 6));
  fake_link()->set_ltk(kModifiedLtk);
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();
  ASSERT_TRUE(fake_chan()->link_error());
  ASSERT_EQ(1, auth_failure_callback_count());
  ASSERT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING),
            auth_failure_status());
}

TEST_F(ResponderPairingTest, EncryptWithLinkKeyButNoSmLtkDisconnects) {
  // The LE link LTK should always be assigned through SM, so while encryption
  // could succeed with a link LTK but no SM LTK, this is a violation of bt-host
  // assumptions and we will disconnect.
  fake_link()->set_ltk(hci_spec::LinkKey({1}, 2, 3));
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();
  ASSERT_TRUE(fake_chan()->link_error());
  ASSERT_EQ(1, auth_failure_callback_count());
  ASSERT_EQ(ToResult(pw::bluetooth::emboss::StatusCode::PIN_OR_KEY_MISSING),
            auth_failure_status());
}

// As responder, we reject security requests, as the initiator should never send
// them.
TEST_F(ResponderPairingTest, RejectSecurityRequest) {
  ReceiveSecurityRequest();
  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(ErrorCode::kCommandNotSupported, received_error_code());

  // Run for the full pairing timeout to ensure we do not timeout due to sending
  // a message.
  RunFor(kPairingTimeout + std::chrono::seconds(1));
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(0, fake_link()->start_encryption_count());
  EXPECT_EQ(1, pairing_failed_count());
}

// Test that LTK is generated and passed up to SecurityManager when both sides
// request bonding
TEST_F(ResponderPairingTest, BothSidesRequestBondingLTKCreated) {
  UInt128 stk;
  SetUpSecurityManager(IOCapability::kDisplayOnly);
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      0u,                   // remote keys
                      KeyDistGen::kEncKey,  // local keys
                      kMaxEncryptionKeySize,
                      BondableMode::Bondable);

  // The link should have been assigned the LTK.
  EXPECT_TRUE(pairing_data().local_ltk.has_value());
}

// Test that LTK is not passed up to SecurityManager when local side requests
// non-bondable mode and peer requests bondable mode.
TEST_F(ResponderPairingTest, LocalRequestsNonBondableNoLTKCreated) {
  UInt128 stk;
  SetUpSecurityManager(IOCapability::kDisplayOnly, BondableMode::NonBondable);
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      0u,                   // remote keys
                      KeyDistGen::kEncKey,  // local keys
                      kMaxEncryptionKeySize,
                      BondableMode::Bondable);

  // The link should not have been assigned the LTK.
  EXPECT_FALSE(pairing_data().local_ltk.has_value() ||
               pairing_data().peer_ltk.has_value());
}

// Test that LTK is not passed up to SecurityManager when local side requests
// bondable mode and peer requests non-bondable mode.
TEST_F(ResponderPairingTest, PeerRequestsNonBondableNoLTKCreated) {
  UInt128 stk;
  SetUpSecurityManager(IOCapability::kDisplayOnly, BondableMode::Bondable);
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      0u,  // remote keys
                      0u,  // local keys
                      kMaxEncryptionKeySize,
                      BondableMode::NonBondable);

  // The link should not have been assigned the LTK.
  EXPECT_FALSE(pairing_data().local_ltk.has_value() ||
               pairing_data().peer_ltk.has_value());
}

// Test that LTK is not generated and passed up to SecurityManager when both
// sides request non-bondable mode.
TEST_F(ResponderPairingTest, BothSidesRequestNonBondableNoLTKCreated) {
  UInt128 stk;
  SetUpSecurityManager(IOCapability::kDisplayOnly, BondableMode::NonBondable);
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/false,
                      SecurityLevel::kEncrypted,
                      0u,  // remote keys
                      0u,  // local keys
                      kMaxEncryptionKeySize,
                      BondableMode::NonBondable);

  // The link should not have been assigned the LTK.
  EXPECT_FALSE(pairing_data().local_ltk.has_value() ||
               pairing_data().peer_ltk.has_value());
}

TEST_F(ResponderPairingTest, PairingRequestStartsPairingTimer) {
  ReceivePairingRequest();
  RunFor(kPairingTimeout);
  EXPECT_TRUE(fake_chan()->link_error());
  // Pairing should fail, but no callbacks should be notified because the
  // pairing was initiated remotely, not through UpgradeSecurity locally
  ASSERT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(HostError::kTimedOut), pairing_complete_status());
  EXPECT_EQ(0, security_callback_count());
}

TEST_F(ResponderPairingTest,
       RejectUnauthenticatedPairingInSecureConnectionsOnlyMode) {
  SetUpSecurityManager(IOCapability::kKeyboardDisplay);
  pairing()->set_security_mode(gap::LESecurityMode::SecureConnectionsOnly);
  // In SC Only mode, SM should translate this "encrypted" request into a MITM
  // requirement.
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();
  EXPECT_EQ(1, security_request_count());
  EXPECT_EQ(AuthReq::kBondingFlag | AuthReq::kMITM | AuthReq::kSC,
            security_request_payload());
  // The peer has NoInputNoOutput IOCapabilities, thus cannot perform
  // authenticated pairing.
  ReceivePairingRequest(IOCapability::kNoInputNoOutput,
                        AuthReq::kBondingFlag | AuthReq::kSC);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, security_callback_count());
  EXPECT_EQ(ToResult(ErrorCode::kAuthenticationRequirements),
            security_status());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(security_status(), pairing_complete_status());
  EXPECT_FALSE(peer().MutLe().is_pairing());
}

TEST_F(ResponderPairingTest,
       RejectNoInputNoOutputLocalCapabilityInSecureConnectionsOnlyMode) {
  SetUpSecurityManager(IOCapability::kNoInputNoOutput);
  pairing()->set_security_mode(gap::LESecurityMode::SecureConnectionsOnly);
  // The local device has NoInputNoOutput capabilities, thus cannot perform
  // authenticated pairing which is required in Secure Connections Only mode.
  ReceivePairingRequest(IOCapability::kDisplayYesNo,
                        AuthReq::kBondingFlag | AuthReq::kSC);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_response_count());
  EXPECT_EQ(1, pairing_failed_count());
  ASSERT_TRUE(received_error_code().has_value());
  EXPECT_EQ(received_error_code().value(),
            ErrorCode::kAuthenticationRequirements);
  EXPECT_EQ(0, pairing_complete_count());
  EXPECT_FALSE(peer().MutLe().is_pairing());
}

TEST_F(ResponderPairingTest,
       RejectInsufficientKeySizeRequestInSecureConnectionsOnlyMode) {
  SetUpSecurityManager(IOCapability::kKeyboardDisplay);
  pairing()->set_security_mode(gap::LESecurityMode::SecureConnectionsOnly);
  // The peer encryption key size is not kMaxEncryptionKeySize, thus does not
  // meet the Secure Connections Only requirements.
  ReceivePairingRequest(IOCapability::kDisplayYesNo,
                        AuthReq::kBondingFlag | AuthReq::kSC,
                        kMaxEncryptionKeySize - 1);
  RunUntilIdle();

  EXPECT_EQ(1, pairing_failed_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(ToResult(ErrorCode::kEncryptionKeySize), pairing_complete_status());
}

// Tests that Secure Connections works as responder
TEST_F(ResponderPairingTest, SecureConnectionsWorks) {
  // Must have DisplayYesNo IOC to generate Authenticated security per
  // kExpectedSecurity
  SetUpSecurityManager(IOCapability::kDisplayYesNo);
  UInt128 ltk_bytes;
  const SecurityProperties kExpectedSecurity(SecurityLevel::kAuthenticated,
                                             kMaxEncryptionKeySize,
                                             /*secure_connections=*/true);
  FastForwardToPhase3(
      &ltk_bytes, /*secure_connections=*/true, kExpectedSecurity.level());

  const LTK kExpectedLtk(kExpectedSecurity, hci_spec::LinkKey(ltk_bytes, 0, 0));
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(kExpectedLtk.key(), fake_link()->ltk());

  // Pairing should succeed with the LTK as the SC pairing is bondable, even
  // though no keys need to be distributed in Phase 3.
  EXPECT_EQ(1, pairing_data_callback_count());
  EXPECT_TRUE(local_ltk().has_value());
  EXPECT_EQ(kExpectedLtk, local_ltk());
  EXPECT_EQ(local_ltk(), peer_ltk());
  EXPECT_FALSE(irk());
  EXPECT_FALSE(identity());
  EXPECT_FALSE(csrk());

  // Should have been called at least once to determine local identity
  // availability.
  EXPECT_NE(0, local_id_info_callback_count());
  // Pairing should complete successfully
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(fit::ok(), pairing_complete_status());
  EXPECT_FALSE(peer().MutLe().is_pairing());

  // No callbacks are notified as the peer started this pairing, not a call to
  // UpgradeSecurity.
  EXPECT_EQ(0, security_callback_count());
  EXPECT_EQ(0, id_info_count());
  EXPECT_EQ(0, id_addr_info_count());

  EXPECT_EQ(kExpectedSecurity, pairing()->security());

  // The security properties should have been updated to match the LTK.
  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(pairing()->security(), new_sec_props());

  ASSERT_TRUE(fake_link()->ltk());
}

TEST_F(SecurityManagerTest, BrEdrResponderCtkdH7NoKeysToDistributeSuccess) {
  NewBrEdrSecurityManager(Role::kResponder);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR);
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kUnauthenticatedCombination256);

  PairingRequestParams preq;
  preq.io_capability = IOCapability::kNoInputNoOutput;
  preq.oob_data_flag = OOBDataFlag::kNotPresent;
  preq.auth_req = AuthReq::kCT2;
  preq.max_encryption_key_size = kMaxEncryptionKeySize;
  preq.initiator_key_dist_gen = KeyDistGen::kEncKey;
  preq.responder_key_dist_gen = KeyDistGen::kEncKey;
  ReceivePairingFeatures(preq, /*peer_initiator=*/true);
  RunUntilIdle();

  const auto kResponse =
      StaticByteBuffer(0x02,  // code: Pairing Response
                       0x03,  // IO cap.: no i/o
                       0x00,  // OOB: not present
                       AuthReq::kCT2,
                       0x10,                 // encr. key size: 16 (default max)
                       KeyDistGen::kEncKey,  // initiator keys
                       KeyDistGen::kEncKey   // responder keys
      );
  EXPECT_EQ(1, pairing_response_count());
  EXPECT_EQ(local_pairing_cmd(), kResponse);

  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(1, pairing_data_callback_count());
  ASSERT_TRUE(pairing_data().local_ltk.has_value());
  EXPECT_EQ(pairing_data().local_ltk->key().value(), kExpectedLtkBytesH7);
  EXPECT_EQ(pairing_data().local_ltk->security().GetLinkKeyType(),
            hci_spec::LinkKeyType::kUnauthenticatedCombination256);
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
}

TEST_F(SecurityManagerTest, BrEdrResponderCtkdH7DistributeIdKeysSuccess) {
  NewBrEdrSecurityManager(Role::kResponder);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR);
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kAuthenticatedCombination256);

  IdentityInfo local_id_info;
  local_id_info.irk = UInt128{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  local_id_info.address = kLocalAddr;
  set_local_id_info(local_id_info);

  // Phase 1
  PairingRequestParams preq;
  preq.io_capability = IOCapability::kNoInputNoOutput;
  preq.oob_data_flag = OOBDataFlag::kNotPresent;
  preq.auth_req = AuthReq::kCT2;
  preq.max_encryption_key_size = kMaxEncryptionKeySize;
  preq.initiator_key_dist_gen = KeyDistGen::kEncKey | KeyDistGen::kIdKey;
  preq.responder_key_dist_gen = KeyDistGen::kEncKey | KeyDistGen::kIdKey;
  ReceivePairingFeatures(preq, /*peer_initiator=*/true);
  EXPECT_TRUE(peer().MutBrEdr().is_pairing());
  RunUntilIdle();

  EXPECT_EQ(1, pairing_response_count());
  const auto kResponse = StaticByteBuffer(
      0x02,  // code: Pairing Response
      0x03,  // IO cap.: no i/o
      0x00,  // OOB: not present
      AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kIdKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey   // responder keys
  );
  EXPECT_EQ(local_pairing_cmd(), kResponse);

  // Phase 3
  const UInt128 kIrk =
      UInt128{{2, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  ReceiveIdentityResolvingKey(kIrk);
  ReceiveIdentityAddress(kPeerAddr);
  RunUntilIdle();

  // Local SM should have sent ID and address messages in phase 3.
  EXPECT_EQ(1, id_info_count());
  EXPECT_EQ(1, id_addr_info_count());

  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, pairing_complete_count());
  EXPECT_EQ(1, pairing_data_callback_count());
  ASSERT_TRUE(pairing_data().local_ltk.has_value());
  EXPECT_EQ(pairing_data().local_ltk->key().value(), kExpectedLtkBytesH7);
  EXPECT_EQ(pairing_data().local_ltk->security().GetLinkKeyType(),
            hci_spec::LinkKeyType::kAuthenticatedCombination256);
  ASSERT_TRUE(pairing_data().irk.has_value());
  EXPECT_EQ(pairing_data().irk.value().value(), kIrk);
  ASSERT_TRUE(pairing_data().identity_address.has_value());
  EXPECT_EQ(pairing_data().identity_address.value(), kPeerAddr);
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
}

TEST_F(SecurityManagerTest, BrEdrInitiatorCtkdH7NoKeysToDistributeSuccess) {
  NewBrEdrSecurityManager(Role::kInitiator);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR);
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kUnauthenticatedCombination256);

  // Phase 1
  std::optional<Result<>> ctkd_result;
  auto ctkd_cb = [&ctkd_result](Result<> result) { ctkd_result = result; };
  pairing()->InitiateBrEdrCrossTransportKeyDerivation(std::move(ctkd_cb));
  RunUntilIdle();

  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x00,  // IO cap.: display only
      0x00,  // OOB: not present
      AuthReq::kCT2,
      0x10,                 // encr. key size: 16 (default max)
      KeyDistGen::kEncKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey  // responder keys
  );
  EXPECT_EQ(1, pairing_request_count());
  EXPECT_EQ(local_pairing_cmd(), kRequest);

  PairingRequestParams preq;
  preq.io_capability = IOCapability::kNoInputNoOutput;
  preq.oob_data_flag = OOBDataFlag::kNotPresent;
  preq.auth_req = AuthReq::kCT2;
  preq.max_encryption_key_size = kMaxEncryptionKeySize;
  preq.initiator_key_dist_gen = KeyDistGen::kEncKey;
  preq.responder_key_dist_gen = KeyDistGen::kEncKey;
  ReceivePairingFeatures(preq, /*peer_initiator=*/false);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, pairing_complete_count());
  ASSERT_TRUE(ctkd_result.has_value());
  EXPECT_TRUE(ctkd_result.value().is_ok());
  EXPECT_EQ(1, pairing_data_callback_count());
  ASSERT_TRUE(pairing_data().local_ltk.has_value());
  EXPECT_EQ(pairing_data().local_ltk->key().value(), kExpectedLtkBytesH7);
  EXPECT_EQ(pairing_data().local_ltk->security().GetLinkKeyType(),
            hci_spec::LinkKeyType::kUnauthenticatedCombination256);
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
}

TEST_F(SecurityManagerTest, BrEdrInitiatorCtkdH7DistributeIdKeysSuccess) {
  NewBrEdrSecurityManager(Role::kInitiator);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR);
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kUnauthenticatedCombination256);

  IdentityInfo local_id_info;
  local_id_info.irk = UInt128{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  local_id_info.address = kLocalAddr;
  set_local_id_info(local_id_info);

  // Phase 1
  std::optional<Result<>> ctkd_result;
  auto ctkd_cb = [&ctkd_result](Result<> result) { ctkd_result = result; };
  pairing()->InitiateBrEdrCrossTransportKeyDerivation(std::move(ctkd_cb));
  EXPECT_TRUE(peer().MutBrEdr().is_pairing());
  RunUntilIdle();

  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x00,  // IO cap.: display only
      0x00,  // OOB: not present
      AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kIdKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey   // responder keys
  );
  EXPECT_EQ(1, pairing_request_count());
  EXPECT_EQ(local_pairing_cmd(), kRequest);

  PairingRequestParams preq;
  preq.io_capability = IOCapability::kNoInputNoOutput;
  preq.oob_data_flag = OOBDataFlag::kNotPresent;
  preq.auth_req = AuthReq::kCT2;
  preq.max_encryption_key_size = kMaxEncryptionKeySize;
  preq.initiator_key_dist_gen = KeyDistGen::kEncKey | KeyDistGen::kIdKey;
  preq.responder_key_dist_gen = KeyDistGen::kEncKey | KeyDistGen::kIdKey;
  ReceivePairingFeatures(preq, /*peer_initiator=*/false);
  RunUntilIdle();
  EXPECT_TRUE(peer().MutBrEdr().is_pairing());
  ASSERT_FALSE(ctkd_result.has_value());

  // Phase 3
  const UInt128 kIrk =
      UInt128{{2, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  ReceiveIdentityResolvingKey(kIrk);
  ReceiveIdentityAddress(kPeerAddr);
  RunUntilIdle();

  // Local SM should have sent ID and address messages in phase 3.
  EXPECT_EQ(1, id_info_count());
  EXPECT_EQ(1, id_addr_info_count());

  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, pairing_complete_count());
  ASSERT_TRUE(ctkd_result.has_value());
  EXPECT_TRUE(ctkd_result.value().is_ok());
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
  EXPECT_EQ(1, pairing_data_callback_count());
  ASSERT_TRUE(pairing_data().local_ltk.has_value());
  EXPECT_EQ(pairing_data().local_ltk->key().value(), kExpectedLtkBytesH7);
  EXPECT_EQ(pairing_data().local_ltk->security().GetLinkKeyType(),
            hci_spec::LinkKeyType::kUnauthenticatedCombination256);
  ASSERT_TRUE(pairing_data().irk.has_value());
  EXPECT_EQ(pairing_data().irk.value().value(), kIrk);
  ASSERT_TRUE(pairing_data().identity_address.has_value());
  EXPECT_EQ(pairing_data().identity_address.value(), kPeerAddr);
}

TEST_F(SecurityManagerTest, BrEdrInitiatorNoPublicKeyValidationFailure) {
  NewBrEdrSecurityManager(
      Role::kInitiator,
      /*is_controller_remote_public_key_validation_supported=*/false);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR);
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kUnauthenticatedCombination256);

  std::optional<Result<>> ctkd_result;
  auto ctkd_cb = [&ctkd_result](Result<> result) { ctkd_result = result; };
  pairing()->InitiateBrEdrCrossTransportKeyDerivation(std::move(ctkd_cb));
  RunUntilIdle();
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
  ASSERT_TRUE(ctkd_result.has_value());
  ASSERT_TRUE(ctkd_result->is_error());
  EXPECT_TRUE(ctkd_result->error_value().is(HostError::kInsufficientSecurity));
}

TEST_F(SecurityManagerTest, BrEdrInitiatorLinkNotEncryptedFailure) {
  NewBrEdrSecurityManager(Role::kInitiator);
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kUnauthenticatedCombination256);

  std::optional<Result<>> ctkd_result;
  auto ctkd_cb = [&ctkd_result](Result<> result) { ctkd_result = result; };
  pairing()->InitiateBrEdrCrossTransportKeyDerivation(std::move(ctkd_cb));
  RunUntilIdle();
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
  ASSERT_TRUE(ctkd_result.has_value());
  ASSERT_TRUE(ctkd_result->is_error());
  EXPECT_TRUE(ctkd_result->error_value().is(HostError::kInsufficientSecurity));
}

TEST_F(SecurityManagerTest, BrEdrInitiatorLinkKeyNotSecureFailure) {
  NewBrEdrSecurityManager(Role::kInitiator);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR);
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kAuthenticatedCombination192);  // 192,not 256!

  std::optional<Result<>> ctkd_result;
  auto ctkd_cb = [&ctkd_result](Result<> result) { ctkd_result = result; };
  pairing()->InitiateBrEdrCrossTransportKeyDerivation(std::move(ctkd_cb));
  RunUntilIdle();
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
  ASSERT_TRUE(ctkd_result.has_value());
  ASSERT_TRUE(ctkd_result->is_error());
  EXPECT_TRUE(ctkd_result->error_value().is(HostError::kInsufficientSecurity));
}

TEST_F(SecurityManagerTest, BrEdrResponderNoPublicKeyValidationFailure) {
  NewBrEdrSecurityManager(
      Role::kResponder,
      /*is_controller_remote_public_key_validation_supported=*/false);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR);
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kUnauthenticatedCombination256);

  PairingRequestParams preq;
  preq.io_capability = IOCapability::kNoInputNoOutput;
  preq.oob_data_flag = OOBDataFlag::kNotPresent;
  preq.auth_req = AuthReq::kCT2;
  preq.max_encryption_key_size = kMaxEncryptionKeySize;
  preq.initiator_key_dist_gen = KeyDistGen::kEncKey;
  preq.responder_key_dist_gen = KeyDistGen::kEncKey;
  ReceivePairingFeatures(preq, /*peer_initiator=*/true);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_response_count());
  EXPECT_EQ(1, pairing_failed_count());
  ASSERT_TRUE(received_error_code().has_value());
  EXPECT_EQ(received_error_code().value(),
            ErrorCode::kCrossTransportKeyDerivationNotAllowed);
  EXPECT_EQ(0, pairing_complete_count());
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
}

TEST_F(SecurityManagerTest, BrEdrResponderInsufficientEncryptionFailure) {
  NewBrEdrSecurityManager(Role::kResponder);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::
          ON_WITH_E0_FOR_BREDR_OR_AES_FOR_LE);  // not AES for BREDR!
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kUnauthenticatedCombination256);

  PairingRequestParams preq;
  preq.io_capability = IOCapability::kNoInputNoOutput;
  preq.oob_data_flag = OOBDataFlag::kNotPresent;
  preq.auth_req = AuthReq::kCT2;
  preq.max_encryption_key_size = kMaxEncryptionKeySize;
  preq.initiator_key_dist_gen = KeyDistGen::kEncKey;
  preq.responder_key_dist_gen = KeyDistGen::kEncKey;
  ReceivePairingFeatures(preq, /*peer_initiator=*/true);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_response_count());
  EXPECT_EQ(1, pairing_failed_count());
  ASSERT_TRUE(received_error_code().has_value());
  EXPECT_EQ(received_error_code().value(),
            ErrorCode::kCrossTransportKeyDerivationNotAllowed);
  EXPECT_EQ(0, pairing_complete_count());
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
}

TEST_F(SecurityManagerTest, BrEdrResponderInsufficientLinkKeyFailure) {
  NewBrEdrSecurityManager(Role::kResponder);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR);
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kUnauthenticatedCombination192);  // not 256!

  PairingRequestParams preq;
  preq.io_capability = IOCapability::kNoInputNoOutput;
  preq.oob_data_flag = OOBDataFlag::kNotPresent;
  preq.auth_req = AuthReq::kCT2;
  preq.max_encryption_key_size = kMaxEncryptionKeySize;
  preq.initiator_key_dist_gen = KeyDistGen::kEncKey;
  preq.responder_key_dist_gen = KeyDistGen::kEncKey;
  ReceivePairingFeatures(preq, /*peer_initiator=*/true);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_response_count());
  EXPECT_EQ(1, pairing_failed_count());
  ASSERT_TRUE(received_error_code().has_value());
  EXPECT_EQ(received_error_code().value(),
            ErrorCode::kCrossTransportKeyDerivationNotAllowed);
  EXPECT_EQ(0, pairing_complete_count());
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
}

TEST_F(SecurityManagerTest, BrEdrResponderLELtkStrongerThanLinkKeyFailure) {
  NewBrEdrSecurityManager(Role::kResponder);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR);
  // The LE LTK is authenticated, but BR/EDR link key is not.
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kUnauthenticatedCombination256);
  sm::PairingData pairing_data;
  pairing_data.local_ltk = kAuthenticatedSecureKey;
  pairing_data.peer_ltk = kAuthenticatedSecureKey;
  peer().MutLe().SetBondData(pairing_data);

  PairingRequestParams preq;
  preq.io_capability = IOCapability::kNoInputNoOutput;
  preq.oob_data_flag = OOBDataFlag::kNotPresent;
  preq.auth_req = AuthReq::kCT2;
  preq.max_encryption_key_size = kMaxEncryptionKeySize;
  preq.initiator_key_dist_gen = KeyDistGen::kEncKey;
  preq.responder_key_dist_gen = KeyDistGen::kEncKey;
  ReceivePairingFeatures(preq, /*peer_initiator=*/true);
  RunUntilIdle();

  EXPECT_EQ(0, pairing_response_count());
  EXPECT_EQ(1, pairing_failed_count());
  ASSERT_TRUE(received_error_code().has_value());
  EXPECT_EQ(received_error_code().value(),
            ErrorCode::kCrossTransportKeyDerivationNotAllowed);
  EXPECT_EQ(0, pairing_complete_count());
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
}

TEST_F(SecurityManagerTest, BrEdrInitiatorLELtkStrongerThanLinkKeyFailure) {
  NewBrEdrSecurityManager(Role::kInitiator);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR);
  // The LE LTK is authenticated, but BR/EDR link key is not.
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kUnauthenticatedCombination256);
  sm::PairingData pairing_data;
  pairing_data.local_ltk = kAuthenticatedSecureKey;
  pairing_data.peer_ltk = kAuthenticatedSecureKey;
  peer().MutLe().SetBondData(pairing_data);

  std::optional<Result<>> ctkd_result;
  auto ctkd_cb = [&ctkd_result](Result<> result) { ctkd_result = result; };
  pairing()->InitiateBrEdrCrossTransportKeyDerivation(std::move(ctkd_cb));
  RunUntilIdle();
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_FALSE(peer().MutBrEdr().is_pairing());
  ASSERT_TRUE(ctkd_result.has_value());
  ASSERT_TRUE(ctkd_result->is_error());
  EXPECT_TRUE(ctkd_result->error_value().is(HostError::kInsufficientSecurity));
}

TEST_F(SecurityManagerTest, BrEdrInitiatorSecurityRequestNotSupported) {
  NewBrEdrSecurityManager(Role::kInitiator);
  ReceiveSecurityRequest();
  RunUntilIdle();
  ASSERT_TRUE(received_error_code().has_value());
  EXPECT_EQ(received_error_code().value(), ErrorCode::kCommandNotSupported);
}

TEST_F(SecurityManagerTest, BrEdrInitiatorPeerDoesNotWantToDoCtkd) {
  NewBrEdrSecurityManager(Role::kInitiator);
  bredr_link()->StartEncryption(
      pw::bluetooth::emboss::EncryptionStatus::ON_WITH_AES_FOR_BREDR);
  bredr_link()->set_link_key(
      hci_spec::LinkKey(kLinkKeyBytes, 0, 0),
      hci_spec::LinkKeyType::kUnauthenticatedCombination256);

  IdentityInfo local_id_info;
  local_id_info.irk = UInt128{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  local_id_info.address = kLocalAddr;
  set_local_id_info(local_id_info);

  // Phase 1
  std::optional<Result<>> ctkd_result;
  auto ctkd_cb = [&ctkd_result](Result<> result) { ctkd_result = result; };
  pairing()->InitiateBrEdrCrossTransportKeyDerivation(std::move(ctkd_cb));
  EXPECT_TRUE(peer().MutBrEdr().is_pairing());
  RunUntilIdle();

  const auto kRequest = StaticByteBuffer(
      0x01,  // code: Pairing Request
      0x00,  // IO cap.: display only
      0x00,  // OOB: not present
      AuthReq::kCT2,
      0x10,  // encr. key size: 16 (default max)
      KeyDistGen::kEncKey | KeyDistGen::kIdKey,  // initiator keys
      KeyDistGen::kEncKey | KeyDistGen::kIdKey   // responder keys
  );
  EXPECT_EQ(1, pairing_request_count());
  EXPECT_EQ(local_pairing_cmd(), kRequest);

  PairingRequestParams preq;
  preq.io_capability = IOCapability::kNoInputNoOutput;
  preq.oob_data_flag = OOBDataFlag::kNotPresent;
  preq.auth_req = AuthReq::kCT2;
  preq.max_encryption_key_size = kMaxEncryptionKeySize;
  preq.initiator_key_dist_gen = 0x00;  // Note: EncKey not set
  preq.responder_key_dist_gen = 0x00;
  ReceivePairingFeatures(preq, /*peer_initiator=*/false);
  RunUntilIdle();

  ASSERT_TRUE(ctkd_result.has_value());
  ASSERT_TRUE(ctkd_result->is_error());
  EXPECT_TRUE(ctkd_result->error_value().is(HostError::kFailed));
  EXPECT_EQ(1, pairing_failed_count());
  ASSERT_TRUE(received_error_code().has_value());
  EXPECT_EQ(received_error_code().value(),
            ErrorCode::kCrossTransportKeyDerivationNotAllowed);
}

TEST_F(ResponderPairingTest,
       BrEdrPairingInProgressErrorInResponseToPairingRequest) {
  InitializePeer(kPeerPublicAddr);
  NewSecurityManager(
      Role::kResponder, IOCapability::kDisplayOnly, BondableMode::Bondable);
  gap::Peer::PairingToken token = peer().MutBrEdr().RegisterPairing();
  ReceivePairingRequest(IOCapability::kDisplayOnly, kSC);
  RunUntilIdle();
  EXPECT_EQ(0, pairing_response_count());
  EXPECT_EQ(1, pairing_failed_count());
  ASSERT_TRUE(received_error_code().has_value());
  EXPECT_EQ(received_error_code().value(), ErrorCode::kBREDRPairingInProgress);
}

TEST_F(ResponderPairingTest, BrEdrPairingInProgressDuringSecurityRequestPhase) {
  InitializePeer(kPeerPublicAddr);
  NewSecurityManager(
      Role::kResponder, IOCapability::kDisplayOnly, BondableMode::Bondable);
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();
  EXPECT_EQ(1, security_request_count());

  gap::Peer::PairingToken token = peer().MutBrEdr().RegisterPairing();

  ReceivePairingRequest(IOCapability::kDisplayOnly, kSC);
  RunUntilIdle();
  EXPECT_EQ(0, pairing_response_count());
  EXPECT_EQ(0, pairing_response_count());
  EXPECT_EQ(1, pairing_failed_count());
  ASSERT_TRUE(received_error_code().has_value());
  EXPECT_EQ(received_error_code().value(), ErrorCode::kBREDRPairingInProgress);
}

TEST_F(InitiatorPairingTest,
       BrEdrPairingInProgressErrorInResponseToSecurityRequest) {
  InitializePeer(kPeerPublicAddr);
  NewSecurityManager(
      Role::kInitiator, IOCapability::kDisplayOnly, BondableMode::Bondable);
  gap::Peer::PairingToken token = peer().MutBrEdr().RegisterPairing();
  ReceiveSecurityRequest(AuthReq::kSC);
  RunUntilIdle();
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(1, pairing_failed_count());
  ASSERT_TRUE(received_error_code().has_value());
  EXPECT_EQ(received_error_code().value(), ErrorCode::kBREDRPairingInProgress);
}

TEST_F(InitiatorPairingTest,
       ReceiveSecurityRequestWhenScPairedDuringBrEdrPairingFails) {
  InitializePeer(kPeerPublicAddr);
  NewSecurityManager(
      Role::kInitiator, IOCapability::kDisplayOnly, BondableMode::Bondable);

  UInt128 stk;
  FastForwardToPhase3(&stk,
                      /*secure_connections=*/true,
                      SecurityLevel::kEncrypted,
                      KeyDistGen::kEncKey);
  EXPECT_EQ(stk, fake_link()->ltk()->value());
  EXPECT_EQ(1, pairing_request_count());

  // Receive EncKey and wait until the link is encrypted with the LTK.
  UInt128 kLTK{{1, 2, 3, 4, 5, 6, 7, 8, 7, 6, 5, 4, 3, 2, 1, 0}};
  uint64_t kRand = 5;
  uint16_t kEDiv = 20;
  ReceiveEncryptionInformation(kLTK);
  ReceiveCentralIdentification(kRand, kEDiv);
  RunUntilIdle();
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(true));
  RunUntilIdle();
  ASSERT_EQ(1, pairing_complete_count());
  ASSERT_EQ(fit::ok(), security_status());
  EXPECT_EQ(1, pairing_request_count());
  ASSERT_EQ(1, fake_link()->start_encryption_count());

  gap::Peer::PairingToken token = peer().MutBrEdr().RegisterPairing();

  ReceiveSecurityRequest(AuthReq::kSC);
  RunUntilIdle();
  EXPECT_EQ(1, pairing_failed_count());
  ASSERT_TRUE(received_error_code().has_value());
  EXPECT_EQ(received_error_code().value(), ErrorCode::kBREDRPairingInProgress);
}

TEST_F(InitiatorPairingTest,
       LegacySecurityRequestWhileWaitingForBrEdrPairingToComplete) {
  InitializePeer(kPeerPublicAddr);
  NewSecurityManager(
      Role::kInitiator, IOCapability::kDisplayOnly, BondableMode::Bondable);

  std::optional<gap::Peer::PairingToken> bredr_pairing_token =
      peer().MutBrEdr().RegisterPairing();

  UpgradeSecurity(SecurityLevel::kEncrypted);
  EXPECT_FALSE(peer().MutLe().is_pairing());
  RunUntilIdle();
  EXPECT_EQ(0, pairing_request_count());

  // Non-SC!
  ReceiveSecurityRequest(/*auth_req=*/0u);

  UInt128 stk;
  FastForwardToSTK(&stk);
  ASSERT_TRUE(fake_link()->ltk());
  EXPECT_EQ(1, fake_link()->start_encryption_count());

  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();
  EXPECT_EQ(0, pairing_failed_count());
  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(security_callback_count(), 2);

  bredr_pairing_token.reset();
  RunUntilIdle();
  EXPECT_EQ(security_callback_count(), 2);
}

TEST_F(ResponderPairingTest,
       EncryptionChangeForBondedPeerWhileWaitingForBrEdrPairing) {
  InitializePeer(kPeerPublicAddr);
  sm::PairingData pairing_data;
  pairing_data.local_ltk = kAuthenticatedSecureKey;
  pairing_data.peer_ltk = kAuthenticatedSecureKey;
  peer().MutLe().SetBondData(pairing_data);
  NewSecurityManager(
      Role::kResponder, IOCapability::kDisplayOnly, BondableMode::Bondable);
  std::optional<gap::Peer::PairingToken> bredr_pairing_token =
      peer().MutBrEdr().RegisterPairing();

  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();
  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(security_callback_count(), 1);
}

TEST_F(InitiatorPairingTest, UpgradeSecurityWaitsForBrEdrPairingToComplete) {
  InitializePeer(kPeerPublicAddr);
  NewSecurityManager(
      Role::kInitiator, IOCapability::kDisplayOnly, BondableMode::Bondable);

  std::optional<gap::Peer::PairingToken> bredr_pairing_token =
      peer().MutBrEdr().RegisterPairing();

  // Queue 2 security upgrade requests to ensure the implementation handles them
  // correctly.
  UpgradeSecurity(SecurityLevel::kEncrypted);
  UpgradeSecurity(SecurityLevel::kEncrypted);
  EXPECT_FALSE(peer().MutLe().is_pairing());
  RunUntilIdle();
  EXPECT_EQ(0, pairing_request_count());

  bredr_pairing_token.reset();
  EXPECT_TRUE(peer().MutLe().is_pairing());
  UInt128 ltk;
  FastForwardToScLtkAfterUpgradeSecurity(&ltk);
  EXPECT_EQ(1, fake_link()->start_encryption_count());
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();
  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(security_callback_count(), 2);
}

TEST_F(InitiatorPairingTest,
       UpgradeSecurityWhileStartingEncryptionQueuesRequest) {
  SecurityProperties sec_props(
      SecurityLevel::kEncrypted, 16, /*secure_connections=*/false);
  LTK ltk(sec_props, hci_spec::LinkKey());
  sm::PairingData bond_data;
  bond_data.peer_ltk = ltk;
  bond_data.local_ltk = ltk;
  peer().MutLe().SetBondData(bond_data);

  NewSecurityManager(
      Role::kInitiator, IOCapability::kDisplayOnly, BondableMode::Bondable);
  RunUntilIdle();
  EXPECT_EQ(1, fake_link()->start_encryption_count());

  UpgradeSecurity(SecurityLevel::kAuthenticated);
  RunUntilIdle();
  EXPECT_EQ(security_callback_count(), 0);
  EXPECT_EQ(0, pairing_request_count());

  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();
  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(1, pairing_request_count());
}

TEST_F(InitiatorPairingTest, SecurityRequestReceivedWhileStartingEncryption) {
  SecurityProperties sec_props(
      SecurityLevel::kEncrypted, 16, /*secure_connections=*/false);
  LTK ltk(sec_props, hci_spec::LinkKey());
  sm::PairingData bond_data;
  bond_data.peer_ltk = ltk;
  bond_data.local_ltk = ltk;
  peer().MutLe().SetBondData(bond_data);

  NewSecurityManager(
      Role::kInitiator, IOCapability::kDisplayOnly, BondableMode::Bondable);
  RunUntilIdle();
  EXPECT_EQ(1, fake_link()->start_encryption_count());

  ReceiveSecurityRequest();
  RunUntilIdle();
  EXPECT_EQ(2, fake_link()->start_encryption_count());

  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();
  EXPECT_EQ(2, fake_link()->start_encryption_count());
  EXPECT_EQ(1, new_sec_props_count());
}

TEST_F(InitiatorPairingTest, BrEdrCtkdHappenedAfterSecurityManagerCreation) {
  InitializePeer(kPeerPublicAddr);
  NewSecurityManager(
      Role::kInitiator, IOCapability::kDisplayOnly, BondableMode::Bondable);

  sm::PairingData pairing_data;
  pairing_data.local_ltk = kAuthenticatedSecureKey;
  pairing_data.peer_ltk = kAuthenticatedSecureKey;
  peer().MutLe().SetBondData(pairing_data);

  // The security upgrade should simply start encryption with the LTK we just
  // set.
  UpgradeSecurity(SecurityLevel::kEncrypted);
  RunUntilIdle();
  EXPECT_EQ(0, pairing_request_count());
  EXPECT_EQ(1, fake_link()->start_encryption_count());
  fake_link()->TriggerEncryptionChangeCallback(fit::ok(/*enabled=*/true));
  RunUntilIdle();
  EXPECT_EQ(1, new_sec_props_count());
  EXPECT_EQ(security_callback_count(), 1);
}

}  // namespace
}  // namespace bt::sm
// inclusive-language: enable
