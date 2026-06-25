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

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/adapter_test_fixture.h"

namespace bthost::testing {

using bt::testing::FakeController;
using TestingBase = bt::testing::ControllerTest<bt::testing::FakeController>;

void AdapterTestFixture::SetUp() {
  FakeController::Settings settings;
  settings.ApplyDualModeDefaults();
  SetUp(settings);
}

void AdapterTestFixture::SetUp(
    FakeController::Settings settings,
    pw::bluetooth::Controller::FeaturesBits features) {
  TestingBase::Initialize(features, /*initialize_transport=*/false);

  auto l2cap = std::make_unique<bt::l2cap::testing::FakeL2cap>(pw_dispatcher());
  l2cap_ = l2cap.get();
  gatt_ = std::make_unique<bt::gatt::testing::FakeLayer>(pw_dispatcher());
  bt::gap::Adapter::Config config = {
      .legacy_pairing_enabled = false,
  };
  adapter_ = bt::gap::Adapter::Create(pw_dispatcher(),
                                      transport()->GetWeakPtr(),
                                      gatt_->GetWeakPtr(),
                                      config,
                                      lease_provider_,
                                      std::nullopt,
                                      std::move(l2cap));

  test_device()->set_settings(settings);

  bool success = false;
  adapter_->Initialize([&](bool result) { success = result; }, [] {});
  RunLoopUntilIdle();
  ASSERT_TRUE(success);
  ASSERT_TRUE(adapter_->le());
  ASSERT_TRUE(adapter_->bredr());
}

void AdapterTestFixture::TearDown() {
  // Drain all scheduled tasks.
  RunLoopUntilIdle();

  // Cleanly shut down the stack.
  l2cap_ = nullptr;
  adapter_ = nullptr;
  RunLoopUntilIdle();

  gatt_ = nullptr;
}

}  // namespace bthost::testing
