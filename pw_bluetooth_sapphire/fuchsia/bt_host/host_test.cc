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

#include "host.h"

#include <lib/fidl/cpp/binding.h>

#include <string>

#include "pw_bluetooth_sapphire/fuchsia/host/fidl/fake_hci_transport_server.h"
#include "pw_bluetooth_sapphire/fuchsia/host/fidl/fake_vendor_server.h"
#include "pw_bluetooth_sapphire/internal/host/testing/loop_fixture.h"

namespace bthost::testing {

using TestingBase = ::bt::testing::TestLoopFixture;

const std::string DEFAULT_DEV_PATH = "/dev/class/bt-hci/000";
class HostComponentTest : public TestingBase {
 public:
  HostComponentTest() = default;
  ~HostComponentTest() override = default;

  void SetUp() override {
    host_ = BtHostComponent::CreateForTesting(dispatcher(), DEFAULT_DEV_PATH);

    auto [vendor_client_end, vendor_server_end] =
        fidl::Endpoints<fuchsia_hardware_bluetooth::Vendor>::Create();

    auto [hci_client_end, hci_server_end] =
        fidl::Endpoints<fuchsia_hardware_bluetooth::HciTransport>::Create();

    vendor_ = std::move(vendor_client_end);
    hci_ = std::move(hci_client_end);

    fake_hci_server_.emplace(std::move(hci_server_end), dispatcher());
    fake_vendor_server_.emplace(std::move(vendor_server_end), dispatcher());
  }

  void TearDown() override {
    if (host_) {
      host_->ShutDown();
    }
    host_ = nullptr;
    TestingBase::TearDown();
  }

  fidl::ClientEnd<fuchsia_hardware_bluetooth::HciTransport> hci() {
    return std::move(hci_);
  }

  fidl::ClientEnd<fuchsia_hardware_bluetooth::Vendor> vendor() {
    return std::move(vendor_);
  }

 protected:
  BtHostComponent* host() const { return host_.get(); }

  void DestroyHost() { host_ = nullptr; }

 private:
  std::unique_ptr<BtHostComponent> host_;

  fidl::ClientEnd<fuchsia_hardware_bluetooth::HciTransport> hci_;

  fidl::ClientEnd<fuchsia_hardware_bluetooth::Vendor> vendor_;

  std::optional<bt::fidl::testing::FakeHciTransportServer> fake_hci_server_;

  std::optional<bt::fidl::testing::FakeVendorServer> fake_vendor_server_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(HostComponentTest);
};

TEST_F(HostComponentTest, InitializeFailsWhenCommandTimesOut) {
  std::optional<bool> init_cb_result;
  bool error_cb_called = false;
  BtHostComponent::Config host_config = {
      .legacy_pairing_enabled = false,
      .override_vendor_capabilites_version = 0,
      .le_slow_adv_interval_min = 0,
      .le_slow_adv_interval_max = 0,
      .le_fast_adv_interval_min = 0,
      .le_fast_adv_interval_max = 0,
      .le_very_fast_adv_interval_min = 0,
      .le_very_fast_adv_interval_max = 0,
      .le_slow_adv_max_tx_power = 127,
      .le_fast_adv_max_tx_power = 127,
      .le_very_fast_adv_max_tx_power = 127,
      .le_active_scan_interval = 0,
      .le_active_scan_window = 0,
      .le_batched_scanning_enabled = false,
      .le_scan_batch_max_read_delay_seconds = 3,
      .le_scan_offload_filters_enabled = false,
      .hci_command_timeout_seconds = 14,
  };
  bool init_result = host()->Initialize(
      vendor(),
      [&](bool success) {
        init_cb_result = success;
        if (!success) {
          host()->ShutDown();
        }
      },
      [&]() { error_cb_called = true; },
      host_config);
  EXPECT_EQ(init_result, true);

  constexpr zx::duration kCommandTimeout = zx::sec(15);
  RunLoopFor(kCommandTimeout);
  ASSERT_TRUE(init_cb_result.has_value());
  EXPECT_FALSE(*init_cb_result);
  EXPECT_FALSE(error_cb_called);
}

}  // namespace bthost::testing
