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

#include "emulator.h"

#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <lib/component/incoming/cpp/service.h>

#include <memory>
#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/outgoing_loop_test_fixture.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"

namespace bt_hci_virtual {
namespace {

namespace fhb = fuchsia_hardware_bluetooth;

constexpr char kEmulatorName[] = "emulator";

class EmulatorTest : public testing::OutgoingLoopTestFixture {
 public:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(OutgoingLoopTestFixture::SetUp());

    device_ = std::make_unique<EmulatorDevice>(dispatcher());
    // Mirrors VirtualController: the shutdown callback releases the device.
    auto shutdown_cb = [this]() {
      shutdown_count_++;
      device_.reset();
    };
    zx_status_t status =
        device_->Initialize(kEmulatorName, std::move(shutdown_cb), outgoing());
    ASSERT_EQ(status, ZX_OK);
    RunUntilIdle();
  }

  void TearDown() override {
    device_.reset();
    RunUntilIdle();
  }

  EmulatorDevice* device() { return device_.get(); }

  int shutdown_count() const { return shutdown_count_; }

  // Connects a new Emulator client to the device and returns its client end.
  fidl::ClientEnd<fhb::Emulator> ConnectEmulator() {
    zx::result endpoints = fidl::CreateEndpoints<fhb::Emulator>();
    if (endpoints.is_error()) {
      ADD_FAILURE() << "CreateEndpoints failed: " << endpoints.status_string();
      return {};
    }
    device_->ConnectEmulator(std::move(endpoints->server));
    RunUntilIdle();
    return std::move(endpoints->client);
  }

  // Publishes the HCI device (i.e. the vendor service) via `emulator`.
  void Publish(fidl::Client<fhb::Emulator>& emulator) {
    bool done = false;
    emulator->Publish(fhb::EmulatorSettings())
        .ThenExactlyOnce([&done](fidl::Result<fhb::Emulator::Publish>& result) {
          EXPECT_TRUE(result.is_ok()) << result.error_value();
          done = true;
        });
    RunUntilIdle();
    ASSERT_TRUE(done);
  }

  bool VendorServiceIsServed() {
    return OutgoingLoopTestFixture::VendorServiceIsServed(kEmulatorName);
  }

  void ReleaseDevice() { device_.reset(); }

 private:
  std::unique_ptr<EmulatorDevice> device_;
  int shutdown_count_ = 0;
};

TEST_F(EmulatorTest, OwnerDisconnectShutsDownAndClosesObservers) {
  fidl::ClientEnd<fhb::Emulator> owner = ConnectEmulator();
  fidl::ClientEnd<fhb::Emulator> observer = ConnectEmulator();
  EXPECT_EQ(shutdown_count(), 0);
  EXPECT_FALSE(PeerClosed(owner));
  EXPECT_FALSE(PeerClosed(observer));

  owner.reset();
  RunUntilIdle();

  EXPECT_EQ(shutdown_count(), 1);
  EXPECT_EQ(device(), nullptr);
  EXPECT_TRUE(PeerClosed(observer));
}

TEST_F(EmulatorTest, OwnerDisconnectUnpublishesVendorService) {
  std::optional<fidl::Client<fhb::Emulator>> owner;
  owner.emplace(ConnectEmulator(), dispatcher());
  ASSERT_NO_FATAL_FAILURE(Publish(*owner));
  EXPECT_TRUE(VendorServiceIsServed());

  // Destroying the client closes the owning connection.
  owner.reset();
  RunUntilIdle();

  EXPECT_EQ(shutdown_count(), 1);
  EXPECT_FALSE(VendorServiceIsServed());
}

TEST_F(EmulatorTest, ObserverDisconnectDoesNotShutDown) {
  fidl::ClientEnd<fhb::Emulator> owner = ConnectEmulator();
  fidl::ClientEnd<fhb::Emulator> observer = ConnectEmulator();

  observer.reset();
  RunUntilIdle();

  EXPECT_EQ(shutdown_count(), 0);
  ASSERT_NE(device(), nullptr);
  EXPECT_FALSE(PeerClosed(owner));

  // A new observer can still connect after a previous one has gone away.
  fidl::ClientEnd<fhb::Emulator> observer2 = ConnectEmulator();
  EXPECT_FALSE(PeerClosed(observer2));
  EXPECT_EQ(shutdown_count(), 0);
}

// Ownership is inferred from connection order, so a first connection whose
// client end is already gone still counts as the owner and tears the emulator
// down. This documents the hazard described on `ConnectEmulator`.
TEST_F(EmulatorTest, AlreadyClosedFirstConnectionShutsDown) {
  zx::result endpoints = fidl::CreateEndpoints<fhb::Emulator>();
  ASSERT_TRUE(endpoints.is_ok());
  endpoints->client.reset();

  device()->ConnectEmulator(std::move(endpoints->server));
  RunUntilIdle();

  EXPECT_EQ(shutdown_count(), 1);
  EXPECT_EQ(device(), nullptr);
}

// If the device is shut down (and destroyed) by some other path first, e.g.
// the framework removing its node, the owner's binding is torn down with it
// and its close handler must never fire.
TEST_F(EmulatorTest, ShutdownBeforeOwnerDisconnectOnlyShutsDownOnce) {
  fidl::ClientEnd<fhb::Emulator> owner = ConnectEmulator();

  device()->Shutdown();
  EXPECT_EQ(shutdown_count(), 1);
  EXPECT_EQ(device(), nullptr);
  RunUntilIdle();
  EXPECT_TRUE(PeerClosed(owner));

  owner.reset();
  RunUntilIdle();
  EXPECT_EQ(shutdown_count(), 1);
}

// Stopping the host shuts the dispatcher down while the owner is still
// connected. That tears the owner's binding down and runs its close handler,
// which must not post the deferred shutdown to the shutting-down dispatcher
// (doing so asserts). The host owns the device's lifetime in this case.
TEST_F(EmulatorTest, DispatcherShutdownWithLiveOwnerDoesNotShutDown) {
  fidl::ClientEnd<fhb::Emulator> owner = ConnectEmulator();

  ShutdownLoop();

  EXPECT_TRUE(PeerClosed(owner));
  EXPECT_EQ(shutdown_count(), 0);
  EXPECT_NE(device(), nullptr);
}

TEST_F(EmulatorTest, OwnerDisconnectClosesVendorHciTransportAndHangingGets) {
  std::optional<fidl::Client<fhb::Emulator>> owner;
  owner.emplace(ConnectEmulator(), dispatcher());
  ASSERT_NO_FATAL_FAILURE(Publish(*owner));

  // Queue pending WatchControllerParameters and WatchLegacyAdvertisingStates
  // hanging-get completers on the server before disconnecting.
  (*owner)->WatchControllerParameters().ThenExactlyOnce(
      [](fidl::Result<fhb::Emulator::WatchControllerParameters>&) {});
  (*owner)->WatchLegacyAdvertisingStates().ThenExactlyOnce(
      [](fidl::Result<fhb::Emulator::WatchLegacyAdvertisingStates>&) {});
  RunUntilIdle();

  // Connect a Vendor client and open an HciTransport connection through it.
  std::string path =
      "svc/" +
      component::MakeServiceMemberPath<fhb::Service::Vendor>(kEmulatorName);
  zx::result vendor_end =
      component::ConnectAt<fhb::Vendor>(outgoing_client(), path);
  ASSERT_TRUE(vendor_end.is_ok());

  fidl::ClientEnd<fhb::HciTransport> hci_transport_end;
  {
    fidl::WireClient<fhb::Vendor> vendor_client(std::move(*vendor_end),
                                                dispatcher());
    vendor_client->OpenHciTransport().ThenExactlyOnce(
        [&](fidl::WireUnownedResult<fhb::Vendor::OpenHciTransport>& res) {
          ASSERT_TRUE(res.ok());
          ASSERT_TRUE(res->is_ok());
          hci_transport_end = std::move(res->value()->channel);
        });
    RunUntilIdle();
    ASSERT_TRUE(hci_transport_end.is_valid());
  }

  // Connect a second raw Vendor channel so we can observe its peer closure
  // directly alongside `hci_transport_end`.
  zx::result raw_vendor_end =
      component::ConnectAt<fhb::Vendor>(outgoing_client(), path);
  ASSERT_TRUE(raw_vendor_end.is_ok());
  RunUntilIdle();
  EXPECT_FALSE(PeerClosed(*raw_vendor_end));
  EXPECT_FALSE(PeerClosed(hci_transport_end));

  owner.reset();
  RunUntilIdle();

  EXPECT_EQ(shutdown_count(), 1);
  EXPECT_TRUE(PeerClosed(*raw_vendor_end));
  EXPECT_TRUE(PeerClosed(hci_transport_end));
}

TEST_F(EmulatorTest, ShutdownIsIdempotentWhenDeviceRemainsAlive) {
  ReleaseDevice();
  RunUntilIdle();

  auto standalone = std::make_unique<EmulatorDevice>(dispatcher());
  int callbacks = 0;
  zx_status_t status =
      standalone->Initialize(kEmulatorName, [&callbacks]() { callbacks++; });
  ASSERT_EQ(status, ZX_OK);

  zx::result endpoints = fidl::CreateEndpoints<fhb::Emulator>();
  ASSERT_TRUE(endpoints.is_ok());
  standalone->ConnectEmulator(std::move(endpoints->server));
  RunUntilIdle();

  // Owner disconnect triggers Shutdown() via the deferred task, without
  // destroying `standalone`.
  endpoints->client.reset();
  RunUntilIdle();
  EXPECT_EQ(callbacks, 1);

  // A subsequent explicit Shutdown() call is a no-op because `shut_down_` is
  // already set.
  standalone->Shutdown();
  EXPECT_EQ(callbacks, 1);
}

TEST_F(EmulatorTest, RandomGeneratorInstalledOnceAcrossMultipleEmulators) {
  ReleaseDevice();
  RunUntilIdle();

  struct RestoreHarnessRng {
    pw::random::RandomGenerator* saved = bt::random_generator();
    ~RestoreHarnessRng() {
      bt::set_random_generator(nullptr);
      bt::set_random_generator(saved);
    }
  } restore_rng;
  bt::set_random_generator(nullptr);
  ASSERT_EQ(bt::random_generator(), nullptr);

  // First EmulatorDevice installs the process-wide generator on Initialize().
  auto first = std::make_unique<EmulatorDevice>(dispatcher());
  ASSERT_EQ(first->Initialize(kEmulatorName, []() {}), ZX_OK);
  pw::random::RandomGenerator* installed = bt::random_generator();
  EXPECT_NE(installed, nullptr);

  // A second EmulatorDevice can Initialize() (concurrently or sequentially)
  // without tripping the single-registration check in
  // bt::set_random_generator(), and destroying the first leaves the generator
  // valid.
  auto second = std::make_unique<EmulatorDevice>(dispatcher());
  ASSERT_EQ(second->Initialize(kEmulatorName, []() {}), ZX_OK);
  first.reset();
  EXPECT_EQ(bt::random_generator(), installed);
}

}  // namespace
}  // namespace bt_hci_virtual
