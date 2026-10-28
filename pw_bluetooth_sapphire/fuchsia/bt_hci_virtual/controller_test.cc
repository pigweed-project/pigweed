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

#include "controller.h"

#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <lib/driver/testing/cpp/driver_test.h>
#include <lib/driver/testing/cpp/test_node.h>
#include <lib/fdf/cpp/dispatcher.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>
#include <lib/zx/time.h>
#include <zircon/types.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace bt_hci_virtual {
namespace {

namespace fhb = fuchsia_hardware_bluetooth;

// Name of the first emulator a fresh driver creates. The regression test needs
// it before calling CreateEmulator, to occupy it on the fake framework node.
const std::string kFirstEmulatorName =
    std::string(VirtualController::kEmulatorNamePrefix) + "0";

// Upper bound on how long a test waits for the driver and the fake framework
// node (which runs on another thread) to converge. Generous because the tests
// run on emulators.
constexpr zx::duration kWaitTimeout = zx::sec(30);

// VirtualController needs nothing from its incoming directory.
class Environment : public fdf_testing::Environment {
 public:
  zx::result<> Serve(fdf::OutgoingDirectory& to_driver_vfs) override {
    return zx::ok();
  }
};

class Config final {
 public:
  using DriverType = VirtualController;
  using EnvironmentType = Environment;
};

// Drives a real VirtualController through its FIDL surface, with the SDK's
// fake driver-framework Node standing in for driver_manager.
class VirtualControllerTest : public ::testing::Test {
 public:
  void SetUp() override {
    zx::result start = driver_test_.StartDriver();
    ASSERT_TRUE(start.is_ok()) << start.status_string();

    zx::result client_end =
        driver_test_.ConnectThroughDevfs<fhb::VirtualController>(
            std::string(VirtualController::kNodeName));
    ASSERT_TRUE(client_end.is_ok()) << client_end.status_string();
    controller_.Bind(std::move(*client_end), dispatcher());
  }

  void TearDown() override {
    zx::result stop = driver_test_.StopDriver();
    EXPECT_TRUE(stop.is_ok()) << stop.status_string();
    driver_test_.ShutdownAndDestroyDriver();
  }

 protected:
  async_dispatcher_t* dispatcher() {
    return fdf::Dispatcher::GetCurrent()->async_dispatcher();
  }

  fdf_testing::DriverRuntime& runtime() { return driver_test_.runtime(); }

  // Pumps the driver's dispatcher until `condition` holds. Returns false on
  // timeout. `condition` may observe state owned by the fake framework node's
  // thread, so this must poll (the default step) rather than block.
  [[nodiscard]] bool WaitUntil(fit::function<bool()> condition) {
    return runtime().RunWithTimeoutOrUntil(std::move(condition), kWaitTimeout);
  }

  // Calls VirtualController.CreateEmulator and returns the reply.
  zx::result<std::string> CreateEmulator() {
    std::optional<zx::result<std::string>> result;
    controller_->CreateEmulator().ThenExactlyOnce(
        [&result](
            fidl::WireUnownedResult<fhb::VirtualController::CreateEmulator>&
                reply) {
          if (!reply.ok()) {
            result = zx::error(reply.status());
          } else if (reply->is_error()) {
            result = zx::error(reply->error_value());
          } else if (reply->value()->name.is_null()) {
            result = zx::error(ZX_ERR_BAD_STATE);
          } else {
            result = zx::ok(std::string(reply->value()->name.get()));
          }
        });
    if (!WaitUntil([&result] { return result.has_value(); })) {
      return zx::error(ZX_ERR_TIMED_OUT);
    }
    return *result;
  }

  // Calls VirtualController.CreateLoopbackDevice with a fresh UART channel and
  // returns the other end of that channel.
  zx::channel CreateLoopbackDevice() {
    zx::channel host_end, device_end;
    EXPECT_EQ(zx::channel::create(0, &host_end, &device_end), ZX_OK);
    fidl::Arena arena;
    auto request =
        fhb::wire::VirtualControllerCreateLoopbackDeviceRequest::Builder(arena)
            .uart_channel(std::move(device_end))
            .Build();
    fidl::OneWayStatus status = controller_->CreateLoopbackDevice(request);
    EXPECT_TRUE(status.ok()) << status.status_string();
    runtime().RunUntilIdle();
    return host_end;
  }

  // Names of the child nodes the driver has added under its own node.
  std::vector<std::string> ChildNodeNames() {
    return driver_test_.RunInNodeContext<std::vector<std::string>>(
        [](fdf_testing::TestNode& node) {
          std::vector<std::string> names;
          for (auto& [name, child] : node.children()) {
            names.push_back(name);
          }
          std::sort(names.begin(), names.end());
          return names;
        });
  }

  bool HasChildNode(const std::string& name) {
    std::vector<std::string> names = ChildNodeNames();
    return std::find(names.begin(), names.end(), name) != names.end();
  }

  // Connects to the Emulator protocol of the emulator node `name` through its
  // devfs connector. The first connection becomes the emulator's owner.
  fidl::ClientEnd<fhb::Emulator> ConnectEmulator(const std::string& name) {
    zx::result client_end =
        driver_test_.ConnectThroughDevfs<fhb::Emulator>(name);
    if (client_end.is_error()) {
      ADD_FAILURE() << "ConnectThroughDevfs(" << name
                    << ") failed: " << client_end.status_string();
      return {};
    }
    // Let the driver accept the connection before anything else happens.
    runtime().RunUntilIdle();
    return std::move(*client_end);
  }

  // Publishes the HCI device (i.e. the vendor service instance) of the emulator
  // node `name` through a fresh, non-owning Emulator connection.
  void Publish(const std::string& name) {
    zx::result client_end =
        driver_test_.ConnectThroughDevfs<fhb::Emulator>(name);
    ASSERT_TRUE(client_end.is_ok()) << client_end.status_string();
    fidl::Client<fhb::Emulator> client(std::move(*client_end), dispatcher());
    bool done = false;
    client->Publish(fhb::EmulatorSettings())
        .ThenExactlyOnce([&done](fidl::Result<fhb::Emulator::Publish>& reply) {
          EXPECT_TRUE(reply.is_ok()) << reply.error_value();
          done = true;
        });
    ASSERT_TRUE(WaitUntil([&done] { return done; }));
  }

  // Returns true if a Vendor client connected through the published
  // fuchsia.hardware.bluetooth.Service instance `name` can complete a request.
  bool VendorServiceIsServed(const std::string& name) {
    zx::result client_end = driver_test_.Connect<fhb::Service::Vendor>(name);
    if (client_end.is_error()) {
      return false;
    }
    fidl::WireClient<fhb::Vendor> client(std::move(*client_end), dispatcher());
    std::optional<bool> ok;
    client->GetFeatures().ThenExactlyOnce(
        [&ok](fidl::WireUnownedResult<fhb::Vendor::GetFeatures>& reply) {
          ok = reply.ok();
        });
    if (!WaitUntil([&ok] { return ok.has_value(); })) {
      ADD_FAILURE() << "timed out waiting for Vendor reply";
      return false;
    }
    return *ok;
  }

  // Returns true if an Emulator client connected through the
  // fuchsia.hardware.bluetooth.EmulatorService instance `name` gets a reply.
  // The connection is closed again before returning, so this must not be the
  // emulator's first connection (that would make it the owner). The request is
  // Publish, so call this only after `Publish(name)`: the reply is then the
  // HCI_ALREADY_PUBLISHED domain error and nothing changes.
  bool EmulatorServiceIsServed(const std::string& name) {
    zx::result client_end =
        driver_test_.Connect<fhb::EmulatorService::Device>(name);
    if (client_end.is_error()) {
      return false;
    }
    fidl::Client<fhb::Emulator> client(std::move(*client_end), dispatcher());
    std::optional<bool> served;
    client->Publish(fhb::EmulatorSettings())
        .ThenExactlyOnce([&served](fidl::Result<fhb::Emulator::Publish>& res) {
          served = res.is_ok() || res.error_value().is_domain_error();
        });
    if (!WaitUntil([&served] { return served.has_value(); })) {
      ADD_FAILURE() << "timed out waiting for Emulator reply";
      return false;
    }
    return *served;
  }

  // Pre-populates a child node named `name` on the fake framework node so that
  // the driver's next AddChild with that name fails with NAME_ALREADY_EXISTS.
  void OccupyChildNodeName(const std::string& name) {
    driver_test_.RunInNodeContext([name](fdf_testing::TestNode& node) {
      node.children().try_emplace(name, name, node.dispatcher());
    });
  }

 private:
  fdf_testing::ForegroundDriverTest<Config> driver_test_;
  fidl::WireClient<fhb::VirtualController> controller_;
};

// Start() adds the devfs node through which clients reach VirtualController,
// and nothing else.
TEST_F(VirtualControllerTest, StartAddsControllerNode) {
  EXPECT_EQ(
      ChildNodeNames(),
      std::vector<std::string>{std::string(VirtualController::kNodeName)});
}

// Each CreateEmulator call produces a distinct emulator with its own node,
// EmulatorService instance and vendor service instance, and closing one
// emulator's owner tears down only that emulator.
TEST_F(VirtualControllerTest, CreateEmulatorTwiceYieldsIndependentEmulators) {
  zx::result first = CreateEmulator();
  ASSERT_TRUE(first.is_ok()) << first.status_string();
  zx::result second = CreateEmulator();
  ASSERT_TRUE(second.is_ok()) << second.status_string();
  EXPECT_NE(*first, *second);
  EXPECT_TRUE(HasChildNode(*first));
  EXPECT_TRUE(HasChildNode(*second));

  fidl::ClientEnd<fhb::Emulator> first_owner = ConnectEmulator(*first);
  fidl::ClientEnd<fhb::Emulator> second_owner = ConnectEmulator(*second);
  ASSERT_NO_FATAL_FAILURE(Publish(*first));
  ASSERT_NO_FATAL_FAILURE(Publish(*second));
  EXPECT_TRUE(EmulatorServiceIsServed(*first));
  EXPECT_TRUE(EmulatorServiceIsServed(*second));
  EXPECT_TRUE(VendorServiceIsServed(*first));
  EXPECT_TRUE(VendorServiceIsServed(*second));

  // Closing one owner tears down only that emulator.
  first_owner.reset();
  ASSERT_TRUE(WaitUntil([&] { return !HasChildNode(*first); }));
  EXPECT_FALSE(EmulatorServiceIsServed(*first));
  EXPECT_FALSE(VendorServiceIsServed(*first));
  EXPECT_TRUE(HasChildNode(*second));
  EXPECT_TRUE(EmulatorServiceIsServed(*second));
  EXPECT_TRUE(VendorServiceIsServed(*second));
}

// An emulator created after a previous one was torn down gets a new name, so
// it cannot collide with the node being removed.
TEST_F(VirtualControllerTest, RecreateAfterTeardownGetsFreshName) {
  zx::result first = CreateEmulator();
  ASSERT_TRUE(first.is_ok()) << first.status_string();
  {
    fidl::ClientEnd<fhb::Emulator> owner = ConnectEmulator(*first);
    ASSERT_TRUE(owner.is_valid());
  }
  ASSERT_TRUE(WaitUntil([&] { return !HasChildNode(*first); }));

  zx::result second = CreateEmulator();
  ASSERT_TRUE(second.is_ok()) << second.status_string();
  EXPECT_NE(*first, *second);
  EXPECT_TRUE(HasChildNode(*second));
}

// Regression test: if the framework refuses the emulator's node, the caller
// must get an error rather than a name that will never appear, and the driver
// must still be able to create emulators afterwards.
TEST_F(VirtualControllerTest, CreateEmulatorFailsWhenNodeCannotBeAdded) {
  OccupyChildNodeName(kFirstEmulatorName);

  zx::result first = CreateEmulator();
  EXPECT_TRUE(first.is_error());
  // Only the controller's own node and the occupied name exist; the failed
  // attempt added nothing.
  EXPECT_EQ(ChildNodeNames(),
            (std::vector<std::string>{std::string(VirtualController::kNodeName),
                                      kFirstEmulatorName}));

  zx::result second = CreateEmulator();
  ASSERT_TRUE(second.is_ok()) << second.status_string();
  EXPECT_NE(*second, kFirstEmulatorName);
  EXPECT_TRUE(HasChildNode(*second));
}

// Calling CreateLoopbackDevice again replaces the previous loopback device and
// its child node.
TEST_F(VirtualControllerTest, CreateLoopbackDeviceTwiceReplacesDevice) {
  const std::string name(VirtualController::kLoopbackName);

  zx::channel first_uart = CreateLoopbackDevice();
  EXPECT_TRUE(HasChildNode(name));
  EXPECT_TRUE(VendorServiceIsServed(name));

  zx::channel second_uart = CreateLoopbackDevice();
  zx_signals_t observed = 0;
  EXPECT_EQ(first_uart.wait_one(
                ZX_CHANNEL_PEER_CLOSED, zx::time::infinite_past(), &observed),
            ZX_OK);
  EXPECT_TRUE(HasChildNode(name));
  EXPECT_TRUE(VendorServiceIsServed(name));
}

}  // namespace
}  // namespace bt_hci_virtual
