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

#pragma once
#include <fuzzer/FuzzedDataProvider.h>

#include <functional>

#include "pw_bluetooth_sapphire/internal/host/gap/peer.h"

namespace bt {
namespace testing {

inline DeviceAddress MakePublicDeviceAddress(FuzzedDataProvider& fdp) {
  DeviceAddressBytes device_address_bytes;
  fdp.ConsumeData(&device_address_bytes, sizeof(device_address_bytes));
  return DeviceAddress(fdp.PickValueInArray({DeviceAddress::Type::kBREDR,
                                             DeviceAddress::Type::kLEPublic}),
                       device_address_bytes);
}

}  // namespace testing

namespace gap::testing {

class PeerFuzzer final {
 public:
  // Core Spec v5.2, Vol 6, Part B, Section 2.3.4.9
  static constexpr size_t kMaxLeAdvDataLength = 1650;

  // Create a PeerFuzzer that mutates |peer| using |fuzzed_data_provider|. Both
  // arguments must outlive this object.
  PeerFuzzer(FuzzedDataProvider& fuzzed_data_provider, Peer& peer)
      : fuzzed_data_provider_(fuzzed_data_provider), peer_(peer) {}

  // Use the FuzzedDataProvider with which this object was constructed to choose
  // a member function that is then used to mutate the corresponding Peer field.
  void FuzzOneField() {
    // The decltype is easier to read than void (&PeerFuzzer::*)(), but this
    // function isn't special and should not pick itself
    using FuzzMemberFunction = decltype(&PeerFuzzer::FuzzOneField);
    constexpr FuzzMemberFunction kFuzzFunctions[] = {
        &PeerFuzzer::LEDataSetAdvertisingData,
        &PeerFuzzer::LEDataRegisterInitializingConnection,
        &PeerFuzzer::LEDataRegisterConnection,
        &PeerFuzzer::LEDataSetConnectionParameters,
        &PeerFuzzer::LEDataSetPreferredConnectionParameters,
        &PeerFuzzer::LEDataSetBondData,
        &PeerFuzzer::LEDataClearBondData,
        &PeerFuzzer::LEDataSetFeatures,
        &PeerFuzzer::LEDataSetServiceChangedGattData,
        &PeerFuzzer::LEDataSetAutoConnectBehavior,
        &PeerFuzzer::BrEdrDataSetInquiryData,
        &PeerFuzzer::BrEdrDataSetInquiryDataWithRssi,
        &PeerFuzzer::BrEdrDataSetInquiryDataFromExtendedInquiryResult,
        &PeerFuzzer::BrEdrDataRegisterInitializingConnection,
        &PeerFuzzer::BrEdrDataUnregisterInitializingConnection,
        &PeerFuzzer::BrEdrDataRegisterConnection,
        &PeerFuzzer::BrEdrUnregisterConnection,
        &PeerFuzzer::BrEdrDataSetBondData,
        &PeerFuzzer::BrEdrDataClearBondData,
        &PeerFuzzer::BrEdrDataAddService,
        &PeerFuzzer::RegisterName,
        &PeerFuzzer::SetFeaturePage,
        &PeerFuzzer::set_last_page_number,
        &PeerFuzzer::set_version,
        &PeerFuzzer::set_connectable,
    };
    std::invoke(fdp().PickValueInArray(kFuzzFunctions), this);
  }

  void LEDataSetAdvertisingData() {
    peer_.MutLe().SetAdvertisingData(
        fdp().ConsumeIntegral<uint8_t>(),
        DynamicByteBuffer(
            BufferView(fdp().ConsumeBytes<uint8_t>(kMaxLeAdvDataLength))),
        pw::chrono::SystemClock::time_point());
  }

  void LEDataRegisterInitializingConnection() {
    if (peer_.connectable() && fdp().ConsumeBool()) {
      le_init_conn_tokens_.emplace_back(
          peer_.MutLe().RegisterInitializingConnection());
    } else if (!le_init_conn_tokens_.empty()) {
      le_init_conn_tokens_.pop_back();
    }
  }

  void LEDataRegisterConnection() {
    if (peer_.connectable() && fdp().ConsumeBool()) {
      le_conn_tokens_.emplace_back(peer_.MutLe().RegisterConnection());
    } else if (!le_conn_tokens_.empty()) {
      le_conn_tokens_.pop_back();
    }
  }

  void LEDataSetConnectionParameters() {
    if (!peer_.connectable()) {
      return;
    }
    const hci_spec::LEConnectionParameters conn_params(
        fdp().ConsumeIntegral<uint16_t>(),
        fdp().ConsumeIntegral<uint16_t>(),
        fdp().ConsumeIntegral<uint16_t>());
    peer_.MutLe().SetConnectionParameters(conn_params);
  }

  void LEDataSetPreferredConnectionParameters() {
    if (!peer_.connectable()) {
      return;
    }
    const hci_spec::LEPreferredConnectionParameters conn_params(
        fdp().ConsumeIntegral<uint16_t>(),
        fdp().ConsumeIntegral<uint16_t>(),
        fdp().ConsumeIntegral<uint16_t>(),
        fdp().ConsumeIntegral<uint16_t>());
    peer_.MutLe().SetPreferredConnectionParameters(conn_params);
  }

  void LEDataSetBondData() {
    if (!peer_.connectable()) {
      return;
    }
    sm::PairingData data;
    auto do_if_fdp = [&](auto action) {
      if (fdp().ConsumeBool()) {
        action();
      }
    };
    do_if_fdp([&] {
      data.identity_address = bt::testing::MakePublicDeviceAddress(fdp());
    });
    do_if_fdp([&] { data.local_ltk = MakeLtk(); });
    do_if_fdp([&] { data.peer_ltk = MakeLtk(); });
    do_if_fdp([&] { data.cross_transport_key = MakeLtk(); });
    do_if_fdp([&] { data.irk = MakeKey(); });
    do_if_fdp([&] { data.csrk = MakeKey(); });
    (void)peer_.MutLe().SetBondData(data);
  }

  void LEDataClearBondData() {
    if (!peer_.le() || !peer_.le()->bonded()) {
      return;
    }
    peer_.MutLe().ClearBondData();
  }

  void LEDataSetFeatures() {
    hci_spec::LESupportedFeatures features = {};
    fdp().ConsumeData(&features, sizeof(features));
    peer_.MutLe().SetFeatures(features);
  }

  void LEDataSetServiceChangedGattData() {
    peer_.MutLe().set_service_changed_gatt_data(
        {fdp().ConsumeBool(), fdp().ConsumeBool()});
  }

  void LEDataSetAutoConnectBehavior() {
    peer_.MutLe().set_auto_connect_behavior(fdp().PickValueInArray(
        {Peer::AutoConnectBehavior::kAlways,
         Peer::AutoConnectBehavior::kSkipUntilNextConnection}));
  }

  void BrEdrDataSetInquiryData() {
    if (!peer_.identity_known()) {
      return;
    }
    StaticPacket<pw::bluetooth::emboss::InquiryResultWriter> inquiry_data;
    fdp().ConsumeData(inquiry_data.mutable_data().mutable_data(),
                      inquiry_data.data().size());
    inquiry_data.view().bd_addr().CopyFrom(peer_.address().value().view());
    peer_.MutBrEdr().SetInquiryData(inquiry_data.view());
  }

  void BrEdrDataSetInquiryDataWithRssi() {
    if (!peer_.identity_known()) {
      return;
    }
    StaticPacket<pw::bluetooth::emboss::InquiryResultWithRssiWriter>
        inquiry_data;
    fdp().ConsumeData(inquiry_data.mutable_data().mutable_data(),
                      inquiry_data.data().size());
    inquiry_data.view().bd_addr().CopyFrom(peer_.address().value().view());
    peer_.MutBrEdr().SetInquiryData(inquiry_data.view());
  }

  void BrEdrDataSetInquiryDataFromExtendedInquiryResult() {
    if (!peer_.identity_known()) {
      return;
    }
    StaticPacket<pw::bluetooth::emboss::ExtendedInquiryResultEventWriter>
        inquiry_data;
    fdp().ConsumeData(inquiry_data.mutable_data().mutable_data(),
                      inquiry_data.data().size());
    inquiry_data.view().bd_addr().CopyFrom(peer_.address().value().view());
    peer_.MutBrEdr().SetInquiryData(inquiry_data.view());
  }

  void BrEdrDataRegisterInitializingConnection() {
    if (!peer_.identity_known() || !peer_.connectable() ||
        bredr_conn_token_.has_value()) {
      return;
    }
    bredr_init_conn_tokens_.emplace_back(
        peer_.MutBrEdr().RegisterInitializingConnection());
  }

  void BrEdrDataUnregisterInitializingConnection() {
    if (!bredr_init_conn_tokens_.empty()) {
      bredr_init_conn_tokens_.pop_back();
    }
  }

  void BrEdrDataRegisterConnection() {
    if (!peer_.identity_known() || !peer_.connectable()) {
      return;
    }

    // Only 1 BR/EDR connection may be registered at a time.
    bredr_conn_token_.reset();
    bredr_conn_token_ = peer_.MutBrEdr().RegisterConnection();
  }

  void BrEdrUnregisterConnection() { bredr_conn_token_.reset(); }

  void BrEdrDataSetBondData() {
    if (!peer_.identity_known() || !peer_.connectable()) {
      return;
    }
    (void)peer_.MutBrEdr().SetBondData(MakeLtk());
  }

  void BrEdrDataClearBondData() {
    if (!peer_.bredr() || !peer_.bredr()->bonded()) {
      return;
    }
    peer_.MutBrEdr().ClearBondData();
  }

  void BrEdrDataAddService() {
    if (!peer_.identity_known() || !peer_.connectable()) {
      return;
    }
    UUID uuid;
    fdp().ConsumeData(&uuid, sizeof(uuid));
    peer_.MutBrEdr().AddService(uuid);
  }

  void RegisterName() { peer_.RegisterName(fdp().ConsumeRandomLengthString()); }

  void SetFeaturePage() {
    peer_.SetFeaturePage(
        fdp().ConsumeIntegralInRange<size_t>(
            0, bt::hci_spec::LMPFeatureSet::kMaxLastPageNumber),
        fdp().ConsumeIntegral<uint64_t>());
  }

  void set_last_page_number() {
    peer_.set_last_page_number(fdp().ConsumeIntegral<uint8_t>());
  }

  void set_version() {
    peer_.set_version(
        pw::bluetooth::emboss::CoreSpecificationVersion{
            fdp().ConsumeIntegral<uint8_t>()},
        fdp().ConsumeIntegral<uint16_t>(),
        fdp().ConsumeIntegral<uint16_t>());
  }

  void set_connectable() {
    // It doesn't make sense to make a peer unconnectable and it fires lots of
    // asserts.
    peer_.set_connectable(true);
  }

 private:
  FuzzedDataProvider& fdp() { return fuzzed_data_provider_; }

  sm::Key MakeKey() {
    // Actual value of the key is not fuzzed.
    return sm::Key(MakeSecurityProperties(), {});
  }

  sm::LTK MakeLtk() {
    // Actual value of the key is not fuzzed.
    return sm::LTK(MakeSecurityProperties(), {});
  }

  sm::SecurityProperties MakeSecurityProperties() {
    sm::SecurityProperties security(
        fdp().ConsumeBool(),
        fdp().ConsumeBool(),
        fdp().ConsumeBool(),
        fdp().ConsumeIntegralInRange<size_t>(0, sm::kMaxEncryptionKeySize));
    return security;
  }

  FuzzedDataProvider& fuzzed_data_provider_;
  Peer& peer_;
  std::vector<Peer::ConnectionToken> le_conn_tokens_;
  std::vector<Peer::InitializingConnectionToken> le_init_conn_tokens_;
  std::optional<Peer::ConnectionToken> bredr_conn_token_;
  std::vector<Peer::InitializingConnectionToken> bredr_init_conn_tokens_;
};

}  // namespace gap::testing
}  // namespace bt
