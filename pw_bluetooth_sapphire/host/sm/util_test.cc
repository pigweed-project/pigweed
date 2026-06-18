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

#include "pw_bluetooth_sapphire/internal/host/sm/util.h"

#include <array>
#include <cstdint>

#include "pw_bluetooth_sapphire/internal/host/common/byte_buffer.h"
#include "pw_bluetooth_sapphire/internal/host/common/device_address.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint128.h"
#include "pw_bluetooth_sapphire/internal/host/common/uint256.h"
#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"
#include "pw_bluetooth_sapphire/internal/host/sm/error.h"
#include "pw_bluetooth_sapphire/internal/host/sm/smp.h"
#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

// inclusive-language: disable

namespace bt::sm::util {
namespace {

TEST(UtilTest, ConvertSmIoCapabilityToHci) {
  EXPECT_EQ(pw::bluetooth::emboss::IoCapability::DISPLAY_ONLY,
            IOCapabilityForHci(IOCapability::kDisplayOnly));
  EXPECT_EQ(pw::bluetooth::emboss::IoCapability::DISPLAY_YES_NO,
            IOCapabilityForHci(IOCapability::kDisplayYesNo));
  EXPECT_EQ(pw::bluetooth::emboss::IoCapability::KEYBOARD_ONLY,
            IOCapabilityForHci(IOCapability::kKeyboardOnly));
  EXPECT_EQ(pw::bluetooth::emboss::IoCapability::NO_INPUT_NO_OUTPUT,
            IOCapabilityForHci(IOCapability::kNoInputNoOutput));
  EXPECT_EQ(pw::bluetooth::emboss::IoCapability::DISPLAY_YES_NO,
            IOCapabilityForHci(IOCapability::kKeyboardDisplay));

  // Test remaining invalid values for sm::IOCapability.
  for (int i = 0x05; i < 0xff; i++) {
    EXPECT_EQ(pw::bluetooth::emboss::IoCapability::NO_INPUT_NO_OUTPUT,
              IOCapabilityForHci(static_cast<IOCapability>(i)));
  }
}

TEST(UtilTest, MapToRolesCorrectly) {
  UInt128 local_val = {1}, peer_val = {2};
  auto [initiator_val, responder_val] =
      MapToRoles(local_val, peer_val, Role::kInitiator);
  EXPECT_EQ(local_val, initiator_val);
  EXPECT_EQ(peer_val, responder_val);

  std::tie(initiator_val, responder_val) =
      MapToRoles(local_val, peer_val, Role::kResponder);
  EXPECT_EQ(local_val, responder_val);
  EXPECT_EQ(peer_val, initiator_val);
}

TEST(UtilTest, SelectPairingMethodOOB) {
  // In SC OOB is selected if either device has OOB data.
  EXPECT_EQ(PairingMethod::kOutOfBand,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/true,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kOutOfBand,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/true,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));
  EXPECT_NE(PairingMethod::kOutOfBand,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));

  // In legacy OOB is selected if both devices have OOB data.
  EXPECT_EQ(PairingMethod::kOutOfBand,
            SelectPairingMethod(/*secure_connections=*/false,
                                /*local_oob=*/true,
                                /*peer_oob=*/true,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));
  EXPECT_NE(PairingMethod::kOutOfBand,
            SelectPairingMethod(/*secure_connections=*/false,
                                /*local_oob=*/false,
                                /*peer_oob=*/true,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));
  EXPECT_NE(PairingMethod::kOutOfBand,
            SelectPairingMethod(/*secure_connections=*/false,
                                /*local_oob=*/true,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));
  EXPECT_NE(PairingMethod::kOutOfBand,
            SelectPairingMethod(/*secure_connections=*/false,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));
}

TEST(UtilTest, SelectPairingMethodNoMITM) {
  // The pairing method should be "Just Works" if neither device requires MITM
  // protection, regardless of other parameters.
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/false,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));

  // Shouldn't default to "Just Works" if at least one device requires MITM
  // protection.
  EXPECT_NE(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/false,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));
}

// Tests all combinations that result in the "Just Works" pairing method.
TEST(UtilTest, SelectPairingMethodJustWorks) {
  // Local: DisplayOnly
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayOnly,
                                /*peer_ioc=*/IOCapability::kDisplayOnly,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayOnly,
                                /*peer_ioc=*/IOCapability::kDisplayYesNo,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayOnly,
                                /*peer_ioc=*/IOCapability::kNoInputNoOutput,
                                /*local_initiator=*/true));

  // Local: DisplayYesNo
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayYesNo,
                                /*peer_ioc=*/IOCapability::kDisplayOnly,
                                /*local_initiator=*/true));
  // If both devices are DisplayYesNo, then "Just Works" is selected for LE
  // legacy pairing (i.e. at least one device doesn't support Secure
  // Connections).
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/false,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayYesNo,
                                /*peer_ioc=*/IOCapability::kDisplayYesNo,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayYesNo,
                                /*peer_ioc=*/IOCapability::kNoInputNoOutput,
                                /*local_initiator=*/true));

  // Local: KeyboardOnly
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardOnly,
                                /*peer_ioc=*/IOCapability::kNoInputNoOutput,
                                /*local_initiator=*/true));

  // Local: NoInputNoOutput. Always "Just Works".
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kNoInputNoOutput,
                                /*peer_ioc=*/IOCapability::kDisplayOnly,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kNoInputNoOutput,
                                /*peer_ioc=*/IOCapability::kDisplayYesNo,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kNoInputNoOutput,
                                /*peer_ioc=*/IOCapability::kKeyboardOnly,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kNoInputNoOutput,
                                /*peer_ioc=*/IOCapability::kNoInputNoOutput,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kNoInputNoOutput,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));

  // Local: KeyboardDisplay
  EXPECT_EQ(PairingMethod::kJustWorks,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kNoInputNoOutput,
                                /*local_initiator=*/true));
}

// Tests all combinations that result in the "Passkey Entry (input)" pairing
// method.
TEST(UtilTest, SelectPairingMethodPasskeyEntryInput) {
  // Local: KeyboardOnly
  EXPECT_EQ(PairingMethod::kPasskeyEntryInput,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardOnly,
                                /*peer_ioc=*/IOCapability::kDisplayOnly,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kPasskeyEntryInput,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardOnly,
                                /*peer_ioc=*/IOCapability::kDisplayYesNo,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kPasskeyEntryInput,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardOnly,
                                /*peer_ioc=*/IOCapability::kKeyboardOnly,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kPasskeyEntryInput,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardOnly,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));

  // Local: KeyboardDisplay
  EXPECT_EQ(PairingMethod::kPasskeyEntryInput,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kDisplayOnly,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kPasskeyEntryInput,
            SelectPairingMethod(/*secure_connections=*/false,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kDisplayYesNo,
                                /*local_initiator=*/true));

  // If both devices have the KeyboardDisplay capability then the responder
  // inputs.
  EXPECT_EQ(PairingMethod::kPasskeyEntryInput,
            SelectPairingMethod(/*secure_connections=*/false,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/false));
}

// Tests all combinations that result in the "Passkey Entry (display)" pairing
// method.
TEST(UtilTest, SelectPairingMethodPasskeyEntryDisplay) {
  // Local: DisplayOnly
  EXPECT_EQ(PairingMethod::kPasskeyEntryDisplay,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayOnly,
                                /*peer_ioc=*/IOCapability::kKeyboardOnly,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kPasskeyEntryDisplay,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayOnly,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));

  // Local: DisplayYesNo
  EXPECT_EQ(PairingMethod::kPasskeyEntryDisplay,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayYesNo,
                                /*peer_ioc=*/IOCapability::kKeyboardOnly,
                                /*local_initiator=*/true));
  // If the peer has a display then use "Passkey Entry" only for LE Legacy
  // pairing.
  EXPECT_EQ(PairingMethod::kPasskeyEntryDisplay,
            SelectPairingMethod(/*secure_connections=*/false,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayYesNo,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));

  // Local: KeyboardDisplay
  EXPECT_EQ(PairingMethod::kPasskeyEntryDisplay,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardOnly,
                                /*local_initiator=*/true));

  // If both devices have the KeyboardDisplay capability then the initiator
  // displays.
  EXPECT_EQ(PairingMethod::kPasskeyEntryDisplay,
            SelectPairingMethod(/*secure_connections=*/false,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));
}

// Tests all combinations that result in the "Numeric Comparison" pairing
// method. This will be selected in certain I/O capability combinations only if
// both devices support Secure Connections.
TEST(UtilTest, SelectPairingMethodNumericComparison) {
  // Local: DisplayYesNo
  EXPECT_EQ(PairingMethod::kNumericComparison,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayYesNo,
                                /*peer_ioc=*/IOCapability::kDisplayYesNo,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kNumericComparison,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kDisplayYesNo,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));

  // Local: KeyboardDisplay
  EXPECT_EQ(PairingMethod::kNumericComparison,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kDisplayYesNo,
                                /*local_initiator=*/true));
  EXPECT_EQ(PairingMethod::kNumericComparison,
            SelectPairingMethod(/*secure_connections=*/true,
                                /*local_oob=*/false,
                                /*peer_oob=*/false,
                                /*mitm_required=*/true,
                                /*local_ioc=*/IOCapability::kKeyboardDisplay,
                                /*peer_ioc=*/IOCapability::kKeyboardDisplay,
                                /*local_initiator=*/true));
}

// Tests "c1" using the sample data from Vol 3, Part H, 2.2.3.
TEST(UtilTest, C1) {
  const UInt128 tk{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
  const UInt128 r{{0xE0,
                   0x2E,
                   0x70,
                   0xC6,
                   0x4E,
                   0x27,
                   0x88,
                   0x63,
                   0x0E,
                   0x6F,
                   0xAD,
                   0x56,
                   0x21,
                   0xD5,
                   0x83,
                   0x57}};
  const StaticByteBuffer preq(0x01, 0x01, 0x00, 0x00, 0x10, 0x07, 0x07);
  const StaticByteBuffer pres(0x02, 0x03, 0x00, 0x00, 0x08, 0x00, 0x05);
  const DeviceAddress initiator_addr(DeviceAddress::Type::kLERandom,
                                     {0xA6, 0xA5, 0xA4, 0xA3, 0xA2, 0xA1});
  const DeviceAddress responder_addr(DeviceAddress::Type::kLEPublic,
                                     {0xB6, 0xB5, 0xB4, 0xB3, 0xB2, 0xB1});

  const UInt128 kExpected{{0x86,
                           0x3B,
                           0xF1,
                           0xBE,
                           0xC5,
                           0x4D,
                           0xA7,
                           0xD2,
                           0xEA,
                           0x88,
                           0x89,
                           0x87,
                           0xEF,
                           0x3F,
                           0x1E,
                           0x1E}};

  std::optional<UInt128> result =
      C1(tk, r, preq, pres, initiator_addr, responder_addr);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(ContainersEqual(kExpected, *result));

  const DeviceAddress invalid_addr(DeviceAddress::Type::kBREDR,
                                   {0xB6, 0xB5, 0xB4, 0xB3, 0xB2, 0xB1});
  std::optional<UInt128> fail_result =
      C1(tk, r, preq, pres, invalid_addr, responder_addr);
  EXPECT_FALSE(fail_result.has_value());
}

// Tests "s1" using the sample data from Vol 3, Part H, 2.2.4.
TEST(UtilTest, S1) {
  const UInt128 tk{{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
  const UInt128 r1{{0x88,
                    0x77,
                    0x66,
                    0x55,
                    0x44,
                    0x33,
                    0x22,
                    0x11,
                    0x09,
                    0xA0,
                    0xB0,
                    0xC0,
                    0xD0,
                    0xE0,
                    0xF0,
                    0x00}};
  const UInt128 r2{{0x00,
                    0xFF,
                    0xEE,
                    0xDD,
                    0xCC,
                    0xBB,
                    0xAA,
                    0x99,
                    0x08,
                    0x07,
                    0x06,
                    0x05,
                    0x04,
                    0x03,
                    0x02,
                    0x01}};

  const UInt128 kExpected{{0x62,
                           0xA0,
                           0x6D,
                           0x79,
                           0xAE,
                           0x16,
                           0x42,
                           0x5B,
                           0x9B,
                           0xF4,
                           0xB0,
                           0xE8,
                           0xF0,
                           0xE1,
                           0x1F,
                           0x9A}};

  UInt128 result;
  S1(tk, r1, r2, &result);
  EXPECT_TRUE(ContainersEqual(kExpected, result));
}

// Test "ah" using the sample data from Vol 3, Part H, Appendix D.7.
TEST(UtilTest, Ah) {
  const UInt128 irk{{0x9B,
                     0x7D,
                     0x39,
                     0x0A,
                     0xA6,
                     0x10,
                     0x10,
                     0x34,
                     0x05,
                     0xAD,
                     0xC8,
                     0x57,
                     0xA3,
                     0x34,
                     0x02,
                     0xEC}};
  const uint32_t prand = 0x708194;
  const uint32_t kExpected = 0x0DFBAA;

  EXPECT_EQ(kExpected, Ah(irk, prand));
}

TEST(UtilTest, IrkCanResolveRpa) {
  // Using the sample data from Vol 3, Part H, Appendix D.7.
  const UInt128 kIRK{{0x9B,
                      0x7D,
                      0x39,
                      0x0A,
                      0xA6,
                      0x10,
                      0x10,
                      0x34,
                      0x05,
                      0xAD,
                      0xC8,
                      0x57,
                      0xA3,
                      0x34,
                      0x02,
                      0xEC}};
  const DeviceAddress kStaticRandom(DeviceAddress::Type::kLERandom,
                                    {0xA9, 0xFB, 0x0D, 0x94, 0x81, 0xF0});
  const DeviceAddress kNonResolvable(DeviceAddress::Type::kLERandom,
                                     {0xA9, 0xFB, 0x0D, 0x94, 0x81, 0x00});
  const DeviceAddress kNonMatchingResolvable(
      DeviceAddress::Type::kLERandom, {0xA9, 0xFB, 0x0D, 0x94, 0x81, 0x70});
  const DeviceAddress kMatchingResolvable(DeviceAddress::Type::kLERandom,
                                          {0xAA, 0xFB, 0x0D, 0x94, 0x81, 0x70});

  ASSERT_FALSE(kStaticRandom.IsResolvablePrivate());
  ASSERT_FALSE(kNonResolvable.IsResolvablePrivate());
  ASSERT_TRUE(kNonMatchingResolvable.IsResolvablePrivate());
  ASSERT_TRUE(kMatchingResolvable.IsResolvablePrivate());

  EXPECT_FALSE(IrkCanResolveRpa(kIRK, kStaticRandom));
  EXPECT_FALSE(IrkCanResolveRpa(kIRK, kNonResolvable));
  EXPECT_FALSE(IrkCanResolveRpa(kIRK, kNonMatchingResolvable));
  EXPECT_TRUE(IrkCanResolveRpa(kIRK, kMatchingResolvable));
}

TEST(UtilTest, GenerateRpa) {
  const UInt128 irk{{'s',
                     'o',
                     'm',
                     'e',
                     ' ',
                     'r',
                     'a',
                     'n',
                     'd',
                     'o',
                     'm',
                     ' ',
                     'd',
                     'a',
                     't',
                     'a'}};

  DeviceAddress rpa = GenerateRpa(irk);

  EXPECT_EQ(DeviceAddress::Type::kLERandom, rpa.type());
  EXPECT_TRUE(rpa.IsResolvablePrivate());

  // It should be possible to resolve the RPA with the IRK used to generate it.
  EXPECT_TRUE(IrkCanResolveRpa(irk, rpa));
}

TEST(UtilTest, GenerateRandomAddress) {
  DeviceAddress addr = GenerateRandomAddress(false);
  EXPECT_EQ(DeviceAddress::Type::kLERandom, addr.type());
  EXPECT_TRUE(addr.IsNonResolvablePrivate());

  addr = GenerateRandomAddress(true);
  EXPECT_EQ(DeviceAddress::Type::kLERandom, addr.type());
  EXPECT_TRUE(addr.IsStaticRandom());
}

// Using the sample data from Vol 3, Part H, Appendix D.1.
TEST(UtilTest, AesCmac) {
  const UInt128 key{0x3C,
                    0x4F,
                    0xCF,
                    0x09,
                    0x88,
                    0x15,
                    0xF7,
                    0xAB,
                    0xA6,
                    0xD2,
                    0xAE,
                    0x28,
                    0x16,
                    0x15,
                    0x7E,
                    0x2B};

  // D.1.1 Example 1: Len = 0
  const BufferView kMsg0;
  const UInt128 kMsg0ExpectedCmac = {0x46,
                                     0x67,
                                     0x75,
                                     0x9B,
                                     0x12,
                                     0x7D,
                                     0xA3,
                                     0x7F,
                                     0x28,
                                     0x37,
                                     0x59,
                                     0xE9,
                                     0x29,
                                     0x69,
                                     0x1D,
                                     0xBB};

  // D.1.2 Example 2: Len = 16
  const StaticByteBuffer<16> kMsg16{0x2A,
                                    0x17,
                                    0x93,
                                    0x73,
                                    0x11,
                                    0x7E,
                                    0x3D,
                                    0xE9,
                                    0x96,
                                    0x9F,
                                    0x40,
                                    0x2E,
                                    0xE2,
                                    0xBE,
                                    0xC1,
                                    0x6B};
  const UInt128 kMsg16ExpectedCmac{0x7C,
                                   0x28,
                                   0x4A,
                                   0xD0,
                                   0x9D,
                                   0xDD,
                                   0x9B,
                                   0xF7,
                                   0x44,
                                   0x41,
                                   0x4D,
                                   0x6B,
                                   0xB4,
                                   0x16,
                                   0x0A,
                                   0x07};

  // D.1.3 Example 3: Len = 40
  const StaticByteBuffer<40> kMsg40{
      0x11, 0xE4, 0x5C, 0xA3, 0x46, 0x1C, 0xC8, 0x30, 0x51, 0x8E,
      0xAF, 0x45, 0xAC, 0x6F, 0xB7, 0x9E, 0x9C, 0xAC, 0x03, 0x1E,
      0x57, 0x8A, 0x2D, 0xAE, 0x2A, 0x17, 0x93, 0x73, 0x11, 0x7E,
      0x3D, 0xE9, 0x96, 0x9F, 0x40, 0x2E, 0xE2, 0xBE, 0xC1, 0x6B};
  const UInt128 kMsg40ExpectedCmac{0x27,
                                   0xC8,
                                   0x97,
                                   0x14,
                                   0x61,
                                   0x32,
                                   0xCA,
                                   0x30,
                                   0x30,
                                   0xE6,
                                   0x9A,
                                   0xDE,
                                   0x47,
                                   0x67,
                                   0xA6,
                                   0xDF};

  // D.1.4 Example 4: Len = 64
  const StaticByteBuffer<64> kMsg64{
      0x10, 0x37, 0x6C, 0xE6, 0x7B, 0x41, 0x2B, 0xAD, 0x17, 0x9B, 0x4F,
      0xDF, 0x45, 0x24, 0x9F, 0xF6, 0xEF, 0x52, 0x0A, 0x1A, 0x19, 0xC1,
      0xFB, 0xE5, 0x11, 0xE4, 0x5C, 0xA3, 0x46, 0x1C, 0xC8, 0x30, 0x51,
      0x8E, 0xAF, 0x45, 0xAC, 0x6F, 0xB7, 0x9E, 0x9C, 0xAC, 0x03, 0x1E,
      0x57, 0x8A, 0x2D, 0xAE, 0x2A, 0x17, 0x93, 0x73, 0x11, 0x7E, 0x3D,
      0xE9, 0x96, 0x9F, 0x40, 0x2E, 0xE2, 0xBE, 0xC1, 0x6B};
  const UInt128 kMsg64ExpectedCmac{0xFE,
                                   0x3C,
                                   0x36,
                                   0x79,
                                   0x17,
                                   0x74,
                                   0x49,
                                   0xFC,
                                   0x92,
                                   0x9D,
                                   0x3B,
                                   0x7E,
                                   0xBF,
                                   0xBE,
                                   0xF0,
                                   0x51};

  std::optional<UInt128> cmac_output{};

  cmac_output = AesCmac(key, kMsg0);
  ASSERT_TRUE(cmac_output.has_value());
  EXPECT_EQ(kMsg0ExpectedCmac, *cmac_output);

  cmac_output = AesCmac(key, kMsg16);
  ASSERT_TRUE(cmac_output.has_value());
  EXPECT_EQ(kMsg16ExpectedCmac, *cmac_output);

  cmac_output = AesCmac(key, kMsg40);
  ASSERT_TRUE(cmac_output.has_value());
  EXPECT_EQ(kMsg40ExpectedCmac, *cmac_output);

  cmac_output = AesCmac(key, kMsg64);
  ASSERT_TRUE(cmac_output.has_value());
  EXPECT_EQ(kMsg64ExpectedCmac, *cmac_output);
}

// Using the sample data from Vol 3, Part H, Appendix D.2.
TEST(UtilTest, F4) {
  const UInt256 kU{0xE6, 0x9D, 0x35, 0x0E, 0x48, 0x01, 0x03, 0xCC,
                   0xDB, 0xFD, 0xF4, 0xAC, 0x11, 0x91, 0xF4, 0xEF,
                   0xB9, 0xA5, 0xF9, 0xE9, 0xA7, 0x83, 0x2C, 0x5E,
                   0x2C, 0xBE, 0x97, 0xF2, 0xD2, 0x03, 0xB0, 0x20};
  const UInt256 kV{0xFD, 0xC5, 0x7F, 0xF4, 0x49, 0xDD, 0x4F, 0x6B,
                   0xFB, 0x7C, 0x9D, 0xF1, 0xC2, 0x9A, 0xCB, 0x59,
                   0x2A, 0xE7, 0xD4, 0xEE, 0xFB, 0xFC, 0x0A, 0x90,
                   0x9A, 0xBB, 0xF6, 0x32, 0x3D, 0x8B, 0x18, 0x55};
  const UInt128 kX{0xAB,
                   0xAE,
                   0x2B,
                   0x71,
                   0xEC,
                   0xB2,
                   0xFF,
                   0xFF,
                   0x3E,
                   0x73,
                   0x77,
                   0xD1,
                   0x54,
                   0x84,
                   0xCB,
                   0xD5};
  const uint8_t kZ = 0x00;
  const UInt128 kExpectedF4{0x2D,
                            0x87,
                            0x74,
                            0xA9,
                            0xBE,
                            0xA1,
                            0xED,
                            0xF1,
                            0x1C,
                            0xBD,
                            0xA9,
                            0x07,
                            0xF1,
                            0x16,
                            0xC9,
                            0xF2};

  std::optional<UInt128> f4_out = F4(kU, kV, kX, kZ);
  ASSERT_TRUE(f4_out.has_value());
  EXPECT_EQ(kExpectedF4, *f4_out);
}

// Using the sample data from Vol 3, Part H, Appendix D.3.
TEST(UtilTest, F5) {
  const UInt256 kDhKey{0x98, 0xA6, 0xBF, 0x73, 0xF3, 0x34, 0x8D, 0x86,
                       0xF1, 0x66, 0xF8, 0xB4, 0x13, 0x6B, 0x79, 0x99,
                       0x9B, 0x7D, 0x39, 0x0A, 0xA6, 0x10, 0x10, 0x34,
                       0x05, 0xAD, 0xC8, 0x57, 0xA3, 0x34, 0x02, 0xEC};
  const UInt128 kInitiatorNonce{0xAB,
                                0xAE,
                                0x2B,
                                0x71,
                                0xEC,
                                0xB2,
                                0xFF,
                                0xFF,
                                0x3E,
                                0x73,
                                0x77,
                                0xD1,
                                0x54,
                                0x84,
                                0xCB,
                                0xD5};
  const UInt128 kResponderNonce{0xCF,
                                0xC4,
                                0x3D,
                                0xFF,
                                0xF7,
                                0x83,
                                0x65,
                                0x21,
                                0x6E,
                                0x5F,
                                0xA7,
                                0x25,
                                0xCC,
                                0xE7,
                                0xE8,
                                0xA6};
  const DeviceAddress kInitiatorAddr(DeviceAddress::Type::kLEPublic,
                                     {0xCE, 0xBF, 0x37, 0x37, 0x12, 0x56});
  const DeviceAddress kResponderAddr(DeviceAddress::Type::kLEPublic,
                                     {0xC1, 0xCF, 0x2D, 0x70, 0x13, 0xA7});
  const UInt128 kExpectedMacKey{0x20,
                                0x6E,
                                0x63,
                                0xCE,
                                0x20,
                                0x6A,
                                0x3F,
                                0xFD,
                                0x02,
                                0x4A,
                                0x08,
                                0xA1,
                                0x76,
                                0xF1,
                                0x65,
                                0x29};
  const UInt128 kExpectedLtk{0x38,
                             0x0A,
                             0x75,
                             0x94,
                             0xB5,
                             0x22,
                             0x05,
                             0x98,
                             0x23,
                             0xCD,
                             0xD7,
                             0x69,
                             0x11,
                             0x79,
                             0x86,
                             0x69};

  std::optional<F5Results> results = F5(
      kDhKey, kInitiatorNonce, kResponderNonce, kInitiatorAddr, kResponderAddr);

  ASSERT_TRUE(results.has_value());
  EXPECT_EQ(kExpectedMacKey, results->mac_key);
  EXPECT_EQ(kExpectedLtk, results->ltk);
}

// Using the sample data from Vol 3, Part H, Appendix D.4
TEST(UtilTest, F6) {
  const UInt128 kMacKey{0x20,
                        0x6E,
                        0x63,
                        0xCE,
                        0x20,
                        0x6A,
                        0x3F,
                        0xFD,
                        0x02,
                        0x4A,
                        0x08,
                        0xA1,
                        0x76,
                        0xF1,
                        0x65,
                        0x29};
  const UInt128 kN1{0xAB,
                    0xAE,
                    0x2B,
                    0x71,
                    0xEC,
                    0xB2,
                    0xFF,
                    0xFF,
                    0x3E,
                    0x73,
                    0x77,
                    0xD1,
                    0x54,
                    0x84,
                    0xCB,
                    0xD5};
  const UInt128 kN2{0xCF,
                    0xC4,
                    0x3D,
                    0xFF,
                    0xF7,
                    0x83,
                    0x65,
                    0x21,
                    0x6E,
                    0x5F,
                    0xA7,
                    0x25,
                    0xCC,
                    0xE7,
                    0xE8,
                    0xA6};
  const UInt128 kR{0xC8,
                   0x0F,
                   0x2D,
                   0x0C,
                   0xD2,
                   0x42,
                   0xDA,
                   0x08,
                   0x54,
                   0xBB,
                   0x53,
                   0xB4,
                   0x3B,
                   0x34,
                   0xA3,
                   0x12};
  const AuthReqField auth_req = 0x01;
  const auto oob = static_cast<OOBDataFlag>(0x01);
  const auto io_cap = static_cast<IOCapability>(0x02);
  const DeviceAddress a1(DeviceAddress::Type::kLEPublic,
                         {0xCE, 0xBF, 0x37, 0x37, 0x12, 0x56});
  const DeviceAddress a2(DeviceAddress::Type::kLEPublic,
                         {0xC1, 0xCF, 0x2D, 0x70, 0x13, 0xA7});
  const UInt128 kExpectedF6Out{0x61,
                               0x8F,
                               0x95,
                               0xDA,
                               0x09,
                               0x0B,
                               0x6C,
                               0xD2,
                               0xC5,
                               0xE8,
                               0xD0,
                               0x9C,
                               0x98,
                               0x73,
                               0xC4,
                               0xE3};

  std::optional<UInt128> f6_out =
      F6(kMacKey, kN1, kN2, kR, auth_req, oob, io_cap, a1, a2);

  ASSERT_TRUE(f6_out.has_value());
  EXPECT_EQ(kExpectedF6Out, *f6_out);
}

// Using the sample data from Vol 3, Part H, Appendix D.5
TEST(UtilTest, G2) {
  const UInt256 kInitiatorPubKeyX{
      0xE6, 0x9D, 0x35, 0x0E, 0x48, 0x01, 0x03, 0xCC, 0xDB, 0xFD, 0xF4,
      0xAC, 0x11, 0x91, 0xF4, 0xEF, 0xB9, 0xA5, 0xF9, 0xE9, 0xA7, 0x83,
      0x2C, 0x5E, 0x2C, 0xBE, 0x97, 0xF2, 0xD2, 0x03, 0xB0, 0x20};
  const UInt256 kResponderPubKeyX{
      0xFD, 0xC5, 0x7F, 0xF4, 0x49, 0xDD, 0x4F, 0x6B, 0xFB, 0x7C, 0x9D,
      0xF1, 0xC2, 0x9A, 0xCB, 0x59, 0x2A, 0xE7, 0xD4, 0xEE, 0xFB, 0xFC,
      0x0A, 0x90, 0x9A, 0xBB, 0xF6, 0x32, 0x3D, 0x8B, 0x18, 0x55};
  const UInt128 kInitiatorNonce{0xAB,
                                0xAE,
                                0x2B,
                                0x71,
                                0xEC,
                                0xB2,
                                0xFF,
                                0xFF,
                                0x3E,
                                0x73,
                                0x77,
                                0xD1,
                                0x54,
                                0x84,
                                0xCB,
                                0xD5};
  const UInt128 kResponderNonce{0xCF,
                                0xC4,
                                0x3D,
                                0xFF,
                                0xF7,
                                0x83,
                                0x65,
                                0x21,
                                0x6E,
                                0x5F,
                                0xA7,
                                0x25,
                                0xCC,
                                0xE7,
                                0xE8,
                                0xA6};
  const uint32_t kExpectedG2Out = 0x2f9ed5ba;

  std::optional<uint32_t> g2_out = G2(
      kInitiatorPubKeyX, kResponderPubKeyX, kInitiatorNonce, kResponderNonce);

  ASSERT_TRUE(g2_out.has_value());
  EXPECT_EQ(kExpectedG2Out, *g2_out);
}

// Using the sample data from v5.2 Vol. 3, Part H, Appendix D.6
TEST(UtilTest, H6) {
  const UInt128 kKeyBytes = {0x9B,
                             0x7D,
                             0x39,
                             0x0A,
                             0xA6,
                             0x10,
                             0x10,
                             0x34,
                             0x05,
                             0xAD,
                             0xC8,
                             0x57,
                             0xA3,
                             0x34,
                             0x02,
                             0xEC};
  const uint32_t kKeyId = 0x6c656272;
  const UInt128 kExpectedH6Out = {0x99,
                                  0x63,
                                  0xB1,
                                  0x80,
                                  0xE2,
                                  0xA9,
                                  0xD3,
                                  0xE8,
                                  0x1C,
                                  0xC9,
                                  0x6D,
                                  0xE7,
                                  0x02,
                                  0xE1,
                                  0x9A,
                                  0x2D};

  std::optional<UInt128> h6_out = H6(kKeyBytes, kKeyId);
  ASSERT_TRUE(h6_out.has_value());
  ASSERT_EQ(kExpectedH6Out, *h6_out);
}

// Using the sample data from v5.2 Vol. 3, Part H, Appendix D.8
TEST(UtilTest, H7) {
  const UInt128 kKeyBytes = {0x9B,
                             0x7D,
                             0x39,
                             0x0A,
                             0xA6,
                             0x10,
                             0x10,
                             0x34,
                             0x05,
                             0xAD,
                             0xC8,
                             0x57,
                             0xA3,
                             0x34,
                             0x02,
                             0xEC};
  const UInt128 kSalt = {0x31,
                         0x70,
                         0x6D,
                         0x74,
                         0x00,
                         0x00,
                         0x00,
                         0x00,
                         0x00,
                         0x00,
                         0x00,
                         0x00,
                         0x00,
                         0x00,
                         0x00,
                         0x00};
  const UInt128 kExpectedH7Out = {0x11,
                                  0x70,
                                  0xA5,
                                  0x75,
                                  0x2A,
                                  0x8C,
                                  0x99,
                                  0xD2,
                                  0xEC,
                                  0xC0,
                                  0xA3,
                                  0xC6,
                                  0x97,
                                  0x35,
                                  0x17,
                                  0xFB};

  std::optional<UInt128> h7_out = H7(kSalt, kKeyBytes);
  ASSERT_TRUE(h7_out.has_value());
  ASSERT_EQ(kExpectedH7Out, *h7_out);
}

// Using the sample data from v5.2 Vol. 3, Part H, Appendix D.9
TEST(UtilTest, LtkToLinkKeyUsingH7) {
  const UInt128 kLtkBytes = {0x64,
                             0xBF,
                             0x4F,
                             0x33,
                             0x33,
                             0x6C,
                             0x06,
                             0xBD,
                             0x58,
                             0x4B,
                             0x26,
                             0xE3,
                             0xBC,
                             0xF9,
                             0x8D,
                             0x36};
  const UInt128 kExpectedLinkKeyBytes = {0x35,
                                         0xB8,
                                         0x47,
                                         0x30,
                                         0xF4,
                                         0xF1,
                                         0x39,
                                         0x0A,
                                         0x53,
                                         0x02,
                                         0xA4,
                                         0xDC,
                                         0x79,
                                         0xD3,
                                         0x7A,
                                         0x28};
  std::optional<UInt128> out_link_key =
      LeLtkToBrEdrLinkKey(kLtkBytes, CrossTransportKeyAlgo::kUseH7);

  ASSERT_TRUE(out_link_key.has_value());
  EXPECT_EQ(kExpectedLinkKeyBytes, *out_link_key);
}

// Using the sample data from v5.2 Vol. 3, Part H, Appendix D.10
TEST(UtilTest, LtkToLinkKeyUsingH6) {
  const UInt128 kLtkBytes = {0x64,
                             0xBF,
                             0x4F,
                             0x33,
                             0x33,
                             0x6C,
                             0x06,
                             0xBD,
                             0x58,
                             0x4B,
                             0x26,
                             0xE3,
                             0xBC,
                             0xF9,
                             0x8D,
                             0x36};
  const UInt128 kExpectedLinkKeyBytes = {0xB0,
                                         0x8F,
                                         0x38,
                                         0xEE,
                                         0xAF,
                                         0x30,
                                         0x82,
                                         0x0D,
                                         0xBD,
                                         0xC1,
                                         0x3F,
                                         0x63,
                                         0xEF,
                                         0xA4,
                                         0x1C,
                                         0xBC};
  std::optional<UInt128> out_link_key =
      LeLtkToBrEdrLinkKey(kLtkBytes, CrossTransportKeyAlgo::kUseH6);

  ASSERT_TRUE(out_link_key.has_value());
  EXPECT_EQ(kExpectedLinkKeyBytes, *out_link_key);
}

// Using the sample data from v6.0, Vol. 3, Part H, Appendix D.12
TEST(UtilTest, LinkKeyToLtkUsingH6) {
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
  const UInt128 kExpectedLtkBytes = {0x30,
                                     0x0a,
                                     0x0d,
                                     0xf1,
                                     0x43,
                                     0x9a,
                                     0x2c,
                                     0x8a,
                                     0xa1,
                                     0xdf,
                                     0xa3,
                                     0xf1,
                                     0x72,
                                     0xfb,
                                     0x13,
                                     0xa8};
  std::optional<UInt128> out_ltk =
      BrEdrLinkKeyToLeLtk(kLinkKeyBytes, CrossTransportKeyAlgo::kUseH6);
  ASSERT_TRUE(out_ltk.has_value());
  EXPECT_EQ(kExpectedLtkBytes, *out_ltk);
}

// Using the sample data from v6.0, Vol. 3, Part H, Appendix D.11
TEST(UtilTest, LinkKeyToLtkUsingH7) {
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
  const UInt128 kExpectedLtkBytes = {
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
  std::optional<UInt128> out_ltk =
      BrEdrLinkKeyToLeLtk(kLinkKeyBytes, CrossTransportKeyAlgo::kUseH7);
  ASSERT_TRUE(out_ltk.has_value());
  EXPECT_EQ(kExpectedLtkBytes, *out_ltk);
}

}  // namespace
}  // namespace bt::sm::util
