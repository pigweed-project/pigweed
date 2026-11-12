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

#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/virtual_controller.h"

#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/component/incoming/cpp/service.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/zx/channel.h>
#include <lib/zx/result.h>
#include <zircon/types.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"
#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/outgoing_loop_test_fixture.h"

namespace bt_hci_virtual {
namespace {

namespace fhb = fuchsia_hardware_bluetooth;

// Drives a `VirtualController` through its FIDL surface on a plain async loop.
class VirtualControllerTest : public testing::OutgoingLoopTestFixture {
 public:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(OutgoingLoopTestFixture::SetUp());
    controller_.emplace(dispatcher(), outgoing());
    client_.Bind(ConnectController(), dispatcher());
    RunUntilIdle();
  }

  void TearDown() override { DestroyController(); }

 protected:
  VirtualController& controller() { return *controller_; }

  void DestroyController() {
    client_ = {};
    controller_.reset();
    RunUntilIdle();
  }

  // Connects a new client directly to the controller.
  fidl::ClientEnd<fhb::VirtualController> ConnectController() {
    zx::result endpoints = fidl::CreateEndpoints<fhb::VirtualController>();
    if (endpoints.is_error()) {
      ADD_FAILURE() << "CreateEndpoints failed: " << endpoints.status_string();
      return {};
    }
    controller_->Connect(std::move(endpoints->server));
    return std::move(endpoints->client);
  }

  // Returns true if the instance `PublishService()` publishes is present.
  bool VirtualControllerServiceIsPublished() {
    return ServiceInstanceIsPublished(
        fhb::VirtualControllerService::Name,
        component::OutgoingDirectory::kDefaultServiceInstance);
  }

  // Calls `VirtualController.CreateEmulator` through `client` and returns the
  // reply.
  zx::result<std::string> CreateEmulator(
      fidl::WireClient<fhb::VirtualController>& client) {
    std::optional<zx::result<std::string>> result;
    client->CreateEmulator().ThenExactlyOnce(
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
    RunUntilIdle();
    if (!result.has_value()) {
      return zx::error(ZX_ERR_TIMED_OUT);
    }
    return *result;
  }
  zx::result<std::string> CreateEmulator() { return CreateEmulator(client_); }

  // Sends `VirtualController.CreateLoopbackDevice` through `client`, with
  // `uart_channel` if set, and runs the loop.
  template <typename Client>
  void SendCreateLoopbackDevice(Client& client,
                                std::optional<zx::channel> uart_channel) {
    fidl::Arena arena;
    auto builder =
        fhb::wire::VirtualControllerCreateLoopbackDeviceRequest::Builder(arena);
    if (uart_channel.has_value()) {
      builder.uart_channel(std::move(*uart_channel));
    }
    auto status = client->CreateLoopbackDevice(builder.Build());
    EXPECT_TRUE(status.ok()) << status.status_string();
    RunUntilIdle();
  }

  // Calls `VirtualController.CreateLoopbackDevice` with a fresh UART channel
  // and returns the other end of that channel.
  zx::channel CreateLoopbackDevice() {
    zx::channel host_end, device_end;
    EXPECT_EQ(zx::channel::create(0, &host_end, &device_end), ZX_OK);
    SendCreateLoopbackDevice(client_, std::move(device_end));
    return host_end;
  }

  // Publishes an `EmulatorService` instance `name` that nothing serves, so that
  // an emulator of that name cannot publish its own. The emulator's failed
  // `AddService` removes this instance too.
  void OccupyEmulatorServiceInstance(const std::string& name) {
    zx::result result = outgoing()->AddService<fhb::EmulatorService>(
        fhb::EmulatorService::InstanceHandler(
            {.device = [](fidl::ServerEnd<fhb::Emulator>) {}}),
        name);
    ASSERT_TRUE(result.is_ok()) << result.status_string();
  }

  // Publishes a vendor Service instance `name` that nothing serves, so that a
  // loopback device of that name cannot publish its own. The device's failed
  // `AddService` removes this instance too.
  void OccupyVendorServiceInstance(std::string_view name) {
    zx::result result = outgoing()->AddService<fhb::Service>(
        fhb::Service::InstanceHandler(
            {.vendor = [](fidl::ServerEnd<fhb::Vendor>) {}}),
        name);
    ASSERT_TRUE(result.is_ok()) << result.status_string();
  }

 private:
  std::optional<VirtualController> controller_;
  fidl::WireClient<fhb::VirtualController> client_;
};

// `PublishService()` makes the `VirtualController` protocol reachable at the
// `VirtualControllerService` instance path.
TEST_F(VirtualControllerTest, PublishServiceServesVirtualController) {
  zx::result publish = controller().PublishService();
  ASSERT_TRUE(publish.is_ok()) << publish.status_string();
  EXPECT_EQ(controller().PublishService().status_value(),
            ZX_ERR_ALREADY_EXISTS);

  std::string path =
      "svc/" +
      component::MakeServiceMemberPath<fhb::VirtualControllerService::Device>(
          component::OutgoingDirectory::kDefaultServiceInstance);
  zx::result client_end =
      component::ConnectAt<fhb::VirtualController>(outgoing_client(), path);
  ASSERT_TRUE(client_end.is_ok()) << client_end.status_string();
  fidl::WireClient<fhb::VirtualController> service_client(
      std::move(*client_end), dispatcher());

  zx::result name = CreateEmulator(service_client);
  ASSERT_TRUE(name.is_ok()) << name.status_string();
  EXPECT_EQ(name->rfind(VirtualController::kEmulatorNamePrefix, 0), 0u);
}

// `PublishService()` fails without an outgoing directory.
TEST_F(VirtualControllerTest, PublishServiceWithoutOutgoingFails) {
  VirtualController controller(dispatcher(), nullptr);
  EXPECT_EQ(controller.PublishService().status_value(), ZX_ERR_BAD_STATE);
  EXPECT_EQ(controller.CreateDeviceServiceDirectories().status_value(),
            ZX_ERR_BAD_STATE);
}

// CreateDeviceServiceDirectories() makes the EmulatorService and Service
// directories openable before any device publishes an instance, and devices
// still publish into them afterwards.
TEST_F(VirtualControllerTest, CreateDeviceServiceDirectoriesPrecedesDevices) {
  EXPECT_FALSE(ServiceDirectoryIsPublished(fhb::EmulatorService::Name));
  EXPECT_FALSE(ServiceDirectoryIsPublished(fhb::Service::Name));

  ASSERT_TRUE(controller().CreateDeviceServiceDirectories().is_ok());
  EXPECT_TRUE(ServiceDirectoryIsPublished(fhb::EmulatorService::Name));
  EXPECT_TRUE(ServiceDirectoryIsPublished(fhb::Service::Name));
  EXPECT_FALSE(EmulatorServiceIsServed(VirtualController::FirstEmulatorName()));

  zx::result name = CreateEmulator();
  ASSERT_TRUE(name.is_ok()) << name.status_string();
  EXPECT_TRUE(EmulatorServiceIsServed(*name));
}

// Destroying the controller removes the VirtualControllerService instance it
// published.
TEST_F(VirtualControllerTest, DestroyingControllerUnpublishesService) {
  ASSERT_TRUE(controller().PublishService().is_ok());
  ASSERT_TRUE(VirtualControllerServiceIsPublished());

  DestroyController();
  EXPECT_FALSE(VirtualControllerServiceIsPublished());
}

// Destroying the controller destroys its devices and unpublishes their
// services.
TEST_F(VirtualControllerTest, DestroyingControllerDestroysDevices) {
  zx::result name = CreateEmulator();
  ASSERT_TRUE(name.is_ok()) << name.status_string();
  fidl::ClientEnd<fhb::Emulator> owner = ConnectEmulatorService(*name);
  zx::channel uart = CreateLoopbackDevice();

  DestroyController();
  EXPECT_TRUE(PeerClosed(owner));
  EXPECT_TRUE(PeerClosed(uart));
  EXPECT_FALSE(EmulatorServiceIsServed(*name));
  EXPECT_FALSE(VendorServiceIsServed(VirtualController::kLoopbackName));
}

// Each `CreateEmulator` call produces a distinct emulator with its own
// `EmulatorService` instance, and closing one emulator's owner tears down only
// that emulator.
TEST_F(VirtualControllerTest, CreateEmulatorTwiceYieldsIndependentEmulators) {
  zx::result first = CreateEmulator();
  ASSERT_TRUE(first.is_ok()) << first.status_string();
  zx::result second = CreateEmulator();
  ASSERT_TRUE(second.is_ok()) << second.status_string();
  EXPECT_NE(*first, *second);

  fidl::ClientEnd<fhb::Emulator> first_owner = ConnectEmulatorService(*first);
  fidl::ClientEnd<fhb::Emulator> second_owner = ConnectEmulatorService(*second);
  EXPECT_TRUE(EmulatorServiceIsServed(*first));
  EXPECT_TRUE(EmulatorServiceIsServed(*second));

  // Closing one owner tears down only that emulator.
  first_owner.reset();
  RunUntilIdle();
  EXPECT_FALSE(EmulatorServiceIsServed(*first));
  EXPECT_TRUE(EmulatorServiceIsServed(*second));
}

// If the emulator fails to initialize, the caller gets the error and later
// calls still succeed.
TEST_F(VirtualControllerTest, EmulatorInitializeFailureIsReportedToCaller) {
  const std::string first_name = VirtualController::FirstEmulatorName();
  ASSERT_NO_FATAL_FAILURE(OccupyEmulatorServiceInstance(first_name));

  zx::result first = CreateEmulator();
  ASSERT_TRUE(first.is_error());
  EXPECT_EQ(first.status_value(), ZX_ERR_ALREADY_EXISTS);

  zx::result second = CreateEmulator();
  ASSERT_TRUE(second.is_ok()) << second.status_string();
  EXPECT_NE(*second, first_name);
  EXPECT_TRUE(EmulatorServiceIsServed(*second));
}

// `CreateLoopbackDevice` publishes the fixed-name vendor service instance, and
// calling it again replaces the previous device and re-publishes the instance.
TEST_F(VirtualControllerTest, CreateLoopbackDeviceTwiceRepublishesService) {
  zx::channel first_uart = CreateLoopbackDevice();
  EXPECT_TRUE(VendorServiceIsServed(VirtualController::kLoopbackName));

  zx::channel second_uart = CreateLoopbackDevice();
  EXPECT_TRUE(VendorServiceIsServed(VirtualController::kLoopbackName));
  // The first device is gone, so its end of the UART channel is closed.
  EXPECT_TRUE(PeerClosed(first_uart));
  EXPECT_FALSE(PeerClosed(second_uart));
}

// If the loopback device fails to initialize, the device is destroyed and a
// later call still works.
TEST_F(VirtualControllerTest, LoopbackInitializeFailureDestroysDevice) {
  ASSERT_NO_FATAL_FAILURE(
      OccupyVendorServiceInstance(VirtualController::kLoopbackName));

  zx::channel first_uart = CreateLoopbackDevice();
  EXPECT_TRUE(PeerClosed(first_uart));
  EXPECT_FALSE(VendorServiceIsServed(VirtualController::kLoopbackName));

  zx::channel second_uart = CreateLoopbackDevice();
  EXPECT_TRUE(VendorServiceIsServed(VirtualController::kLoopbackName));
  EXPECT_FALSE(PeerClosed(second_uart));
}

// A `CreateLoopbackDevice` request without a UART channel closes that
// connection and creates nothing.
TEST_F(VirtualControllerTest, CreateLoopbackDeviceWithoutUartClosesConnection) {
  fidl::WireSyncClient<fhb::VirtualController> client(ConnectController());
  SendCreateLoopbackDevice(client, std::nullopt);

  EXPECT_TRUE(PeerClosed(client.client_end()));
  EXPECT_FALSE(VendorServiceIsServed(VirtualController::kLoopbackName));
}

}  // namespace
}  // namespace bt_hci_virtual
