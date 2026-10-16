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

#include <fidl/fuchsia.driver.framework/cpp/fidl.h>
#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <fidl/fuchsia.io/cpp/fidl.h>
#include <lib/component/incoming/cpp/service.h>
#include <lib/driver/outgoing/cpp/outgoing_directory.h>
#include <lib/driver/testing/cpp/driver_runtime.h>
#include <lib/driver/testing/cpp/scoped_global_logger.h>
#include <lib/fdf/cpp/dispatcher.h>
#include <lib/zx/channel.h>
#include <lib/zx/time.h>

#include <memory>
#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "pw_bluetooth_sapphire/internal/host/common/random.h"

namespace bt_hci_virtual {
namespace {

namespace fhb = fuchsia_hardware_bluetooth;

class EmulatorTest : public ::testing::Test {
 public:
  void SetUp() override {
    fdf::OutgoingDirectory& outgoing =
        outgoing_.emplace(fdf::Dispatcher::GetCurrent()->get());
    auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    ASSERT_TRUE(endpoints.is_ok());
    zx::result serve_result = outgoing.Serve(std::move(endpoints->server));
    ASSERT_TRUE(serve_result.is_ok());
    outgoing_client_ = std::move(endpoints->client);

    device_ = std::make_unique<EmulatorDevice>();
    int add_child_cb_count = 0;
    auto add_child_cb = [&](fuchsia_driver_framework::wire::NodeAddArgs) {
      add_child_cb_count++;
    };
    // Mirrors VirtualController: the shutdown callback releases the device.
    auto shutdown_cb = [this]() {
      shutdown_count_++;
      device_.reset();
    };
    zx_status_t status = device_->Initialize(
        "emulator", std::move(add_child_cb), std::move(shutdown_cb), &outgoing);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(add_child_cb_count, 1);
    fdf_testing_run_until_idle();
  }

  void TearDown() override {
    device_.reset();
    fdf_testing_run_until_idle();
    runtime_.ShutdownAllDispatchers(fdf::Dispatcher::GetCurrent()->get());
  }

  async_dispatcher_t* dispatcher() {
    return fdf::Dispatcher::GetCurrent()->async_dispatcher();
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
    fdf_testing_run_until_idle();
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
    fdf_testing_run_until_idle();
    ASSERT_TRUE(done);
  }

  // Returns true if a Vendor client connected through the published
  // fuchsia.hardware.bluetooth.Service instance can complete a request.
  bool VendorServiceIsServed() {
    std::string path =
        "svc/" + component::MakeServiceMemberPath<fhb::Service::Vendor>(
                     component::kDefaultInstance);
    zx::result client_end =
        component::ConnectAt<fhb::Vendor>(outgoing_client_.borrow(), path);
    if (client_end.is_error()) {
      return false;
    }
    fidl::WireClient<fhb::Vendor> client(std::move(*client_end), dispatcher());
    bool ok = false;
    client->GetFeatures().ThenExactlyOnce(
        [&ok](fidl::WireUnownedResult<fhb::Vendor::GetFeatures>& res) {
          ok = res.ok();
        });
    fdf_testing_run_until_idle();
    return ok;
  }

  // Returns true if the peer of `client_end` has been closed.
  template <typename Protocol>
  static bool PeerClosed(const fidl::ClientEnd<Protocol>& client_end) {
    zx_signals_t observed = 0;
    zx_status_t status = client_end.channel().wait_one(
        ZX_CHANNEL_PEER_CLOSED, zx::time::infinite_past(), &observed);
    return status == ZX_OK && (observed & ZX_CHANNEL_PEER_CLOSED);
  }

  fidl::UnownedClientEnd<fuchsia_io::Directory> outgoing_client() const {
    return outgoing_client_.borrow();
  }

  void ReleaseDevice() { device_.reset(); }

  // Shuts down every driver dispatcher, as the framework does when the driver
  // stops. Idempotent, so TearDown may call it again.
  void ShutdownDispatchers() {
    runtime_.ShutdownAllDispatchers(fdf::Dispatcher::GetCurrent()->get());
  }

 private:
  // Attaches a foreground dispatcher for us automatically.
  fdf_testing::DriverRuntime runtime_;
  // EmulatorDevice logs through the global fdf::Logger.
  fdf_testing::ScopedGlobalLogger logger_;

  std::optional<fdf::OutgoingDirectory> outgoing_;
  fidl::ClientEnd<fuchsia_io::Directory> outgoing_client_;

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
  fdf_testing_run_until_idle();

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
  fdf_testing_run_until_idle();

  EXPECT_EQ(shutdown_count(), 1);
  EXPECT_FALSE(VendorServiceIsServed());
}

TEST_F(EmulatorTest, ObserverDisconnectDoesNotShutDown) {
  fidl::ClientEnd<fhb::Emulator> owner = ConnectEmulator();
  fidl::ClientEnd<fhb::Emulator> observer = ConnectEmulator();

  observer.reset();
  fdf_testing_run_until_idle();

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
  fdf_testing_run_until_idle();

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
  fdf_testing_run_until_idle();
  EXPECT_TRUE(PeerClosed(owner));

  owner.reset();
  fdf_testing_run_until_idle();
  EXPECT_EQ(shutdown_count(), 1);
}

// Driver stop shuts the dispatchers down while the owner is still connected.
// That tears the owner's binding down and runs its close handler, which must
// not post the deferred shutdown to the shutting-down dispatcher (doing so
// asserts). The driver owns the device's lifetime in this case.
TEST_F(EmulatorTest, DispatcherShutdownWithLiveOwnerDoesNotShutDown) {
  fidl::ClientEnd<fhb::Emulator> owner = ConnectEmulator();

  ShutdownDispatchers();

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
  fdf_testing_run_until_idle();

  // Connect a Vendor client and open an HciTransport connection through it.
  std::string path =
      "svc/" + component::MakeServiceMemberPath<fhb::Service::Vendor>(
                   component::kDefaultInstance);
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
    fdf_testing_run_until_idle();
    ASSERT_TRUE(hci_transport_end.is_valid());
  }

  // Connect a second raw Vendor channel so we can observe its peer closure
  // directly alongside `hci_transport_end`.
  zx::result raw_vendor_end =
      component::ConnectAt<fhb::Vendor>(outgoing_client(), path);
  ASSERT_TRUE(raw_vendor_end.is_ok());
  fdf_testing_run_until_idle();
  EXPECT_FALSE(PeerClosed(*raw_vendor_end));
  EXPECT_FALSE(PeerClosed(hci_transport_end));

  owner.reset();
  fdf_testing_run_until_idle();

  EXPECT_EQ(shutdown_count(), 1);
  EXPECT_TRUE(PeerClosed(*raw_vendor_end));
  EXPECT_TRUE(PeerClosed(hci_transport_end));
}

TEST_F(EmulatorTest, ShutdownIsIdempotentWhenDeviceRemainsAlive) {
  ReleaseDevice();
  fdf_testing_run_until_idle();

  auto standalone = std::make_unique<EmulatorDevice>();
  int callbacks = 0;
  zx_status_t status = standalone->Initialize(
      "emulator",
      [](fuchsia_driver_framework::wire::NodeAddArgs) {},
      [&callbacks]() { callbacks++; });
  ASSERT_EQ(status, ZX_OK);

  zx::result endpoints = fidl::CreateEndpoints<fhb::Emulator>();
  ASSERT_TRUE(endpoints.is_ok());
  standalone->ConnectEmulator(std::move(endpoints->server));
  fdf_testing_run_until_idle();

  // Owner disconnect triggers Shutdown() via the deferred task, without
  // destroying `standalone`.
  endpoints->client.reset();
  fdf_testing_run_until_idle();
  EXPECT_EQ(callbacks, 1);

  // A subsequent explicit Shutdown() call is a no-op because `shut_down_` is
  // already set.
  standalone->Shutdown();
  EXPECT_EQ(callbacks, 1);
}

TEST_F(EmulatorTest, RandomGeneratorInstalledOnceAcrossMultipleEmulators) {
  ReleaseDevice();
  fdf_testing_run_until_idle();

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
  auto first = std::make_unique<EmulatorDevice>();
  ASSERT_EQ(first->Initialize(
                "emulator",
                [](fuchsia_driver_framework::wire::NodeAddArgs) {},
                []() {}),
            ZX_OK);
  pw::random::RandomGenerator* installed = bt::random_generator();
  EXPECT_NE(installed, nullptr);

  // A second EmulatorDevice can Initialize() (concurrently or sequentially)
  // without tripping the single-registration check in
  // bt::set_random_generator(), and destroying the first leaves the generator
  // valid.
  auto second = std::make_unique<EmulatorDevice>();
  ASSERT_EQ(second->Initialize(
                "emulator",
                [](fuchsia_driver_framework::wire::NodeAddArgs) {},
                []() {}),
            ZX_OK);
  first.reset();
  EXPECT_EQ(bt::random_generator(), installed);
}

}  // namespace
}  // namespace bt_hci_virtual
