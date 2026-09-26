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

#include "pw_bluetooth_sapphire/internal/host/gap/types.h"

#include "pw_unit_test/framework.h"

namespace bt::gap {

TEST(TypesTest, SecurityPropertiesMeetRequirements) {
  std::array<hci_spec::LinkKeyType, 5> kUnauthenticatedNoScKeyTypes = {
      hci_spec::LinkKeyType::kCombination,
      hci_spec::LinkKeyType::kLocalUnit,
      hci_spec::LinkKeyType::kRemoteUnit,
      hci_spec::LinkKeyType::kDebugCombination,
      hci_spec::LinkKeyType::kUnauthenticatedCombination192};
  for (size_t i = 0; i < kUnauthenticatedNoScKeyTypes.size(); i++) {
    SCOPED_TRACE(i);
    sm::SecurityProperties props(kUnauthenticatedNoScKeyTypes[i],
                                 sm::kMaxEncryptionKeySize);
    EXPECT_TRUE(SecurityPropertiesMeetRequirements(
        props,
        BrEdrSecurityRequirements{.authentication = false,
                                  .secure_connections = false}));
    EXPECT_FALSE(SecurityPropertiesMeetRequirements(
        props,
        BrEdrSecurityRequirements{.authentication = false,
                                  .secure_connections = true}));
    EXPECT_FALSE(SecurityPropertiesMeetRequirements(
        props,
        BrEdrSecurityRequirements{.authentication = true,
                                  .secure_connections = false}));
    EXPECT_FALSE(SecurityPropertiesMeetRequirements(
        props,
        BrEdrSecurityRequirements{.authentication = true,
                                  .secure_connections = true}));
  }

  sm::SecurityProperties props(
      hci_spec::LinkKeyType::kAuthenticatedCombination192,
      sm::kMaxEncryptionKeySize);
  EXPECT_TRUE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = false,
                                .secure_connections = false}));
  EXPECT_FALSE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = false,
                                .secure_connections = true}));
  EXPECT_TRUE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = true,
                                .secure_connections = false}));
  EXPECT_FALSE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = true,
                                .secure_connections = true}));

  props = sm::SecurityProperties(
      hci_spec::LinkKeyType::kUnauthenticatedCombination256,
      sm::kMaxEncryptionKeySize);
  EXPECT_TRUE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = false,
                                .secure_connections = false}));
  EXPECT_TRUE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = false,
                                .secure_connections = true}));
  EXPECT_FALSE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = true,
                                .secure_connections = false}));
  EXPECT_FALSE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = true,
                                .secure_connections = true}));

  props = sm::SecurityProperties(
      hci_spec::LinkKeyType::kAuthenticatedCombination256,
      sm::kMaxEncryptionKeySize);
  EXPECT_TRUE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = false,
                                .secure_connections = false}));
  EXPECT_TRUE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = false,
                                .secure_connections = true}));
  EXPECT_TRUE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = true,
                                .secure_connections = false}));
  EXPECT_TRUE(SecurityPropertiesMeetRequirements(
      props,
      BrEdrSecurityRequirements{.authentication = true,
                                .secure_connections = true}));
}

TEST(TypesTest,
     SecurityPropertiesMeetRequirementsEnforcesKeySizeAndEncryption) {
  sm::SecurityProperties unencrypted_props(
      /*encrypted=*/false,
      /*authenticated=*/false,
      /*secure_connections=*/false,
      sm::kMaxEncryptionKeySize);

  EXPECT_TRUE(SecurityPropertiesMeetRequirements(
      unencrypted_props,
      BrEdrSecurityRequirements{.authentication = false,
                                .secure_connections = false}));

  EXPECT_FALSE(SecurityPropertiesMeetRequirements(
      unencrypted_props,
      BrEdrSecurityRequirements{.authentication = true,
                                .secure_connections = false}));
  EXPECT_FALSE(SecurityPropertiesMeetRequirements(
      unencrypted_props,
      BrEdrSecurityRequirements{.authentication = false,
                                .secure_connections = true}));

  sm::SecurityProperties weak_sc_props(
      /*encrypted=*/true,
      /*authenticated=*/true,
      /*secure_connections=*/true,
      7);

  EXPECT_FALSE(SecurityPropertiesMeetRequirements(
      weak_sc_props,
      BrEdrSecurityRequirements{.authentication = false,
                                .secure_connections = true}));

  EXPECT_TRUE(SecurityPropertiesMeetRequirements(
      weak_sc_props,
      BrEdrSecurityRequirements{.authentication = true,
                                .secure_connections = false}));
}

}  // namespace bt::gap
