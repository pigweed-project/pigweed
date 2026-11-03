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

#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <fidl/fuchsia.io/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/incoming/cpp/constants.h>
#include <lib/component/incoming/cpp/directory.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/component/incoming/cpp/service.h>
#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/sys/component/cpp/testing/realm_builder_types.h>
#include <lib/zx/result.h>
#include <lib/zx/time.h>
#include <zircon/types.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"
#include "pw_bluetooth_sapphire/fuchsia/bt_hci_virtual/virtual_controller.h"

namespace bt_hci_virtual {
namespace {

namespace fhb = fuchsia_hardware_bluetooth;

constexpr char kChildName[] = "bt-hci-virtual";
// Resolves within the test package; keep in sync with the manifest's basename
// in BUILD.bazel.
constexpr char kChildUrl[] = "#meta/bt-hci-virtual.cm";
// Bounds the component's deferred emulator shutdown; generous because the
// tests run on emulators.
constexpr zx::duration kWaitTimeout = zx::sec(30);

// Runs bt-hci-virtual.cm in a test realm and reaches it only through the
// services the component exposes, as its parent on a Fuchsia product would.
class ComponentTest : public ::testing::Test {
 public:
  void SetUp() override {
    component_testing::RealmBuilder builder =
        component_testing::RealmBuilder::Create();
    builder.AddChild(kChildName, kChildUrl);
    builder.AddRoute(component_testing::Route{
        .capabilities = {component_testing::Protocol{"fuchsia.logger.LogSink"}},
        .source = component_testing::ParentRef(),
        .targets = {component_testing::ChildRef{kChildName}}});
    builder.AddRoute(component_testing::Route{
        .capabilities = {component_testing::Service{fhb::EmulatorService::Name},
                         component_testing::Service{fhb::Service::Name},
                         component_testing::Service{
                             VirtualController::kServiceName}},
        .source = component_testing::ChildRef{kChildName},
        .targets = {component_testing::ParentRef()}});
    realm_.emplace(builder.Build(loop_.dispatcher()));
    exposed_ = fidl::ClientEnd<fuchsia_io::Directory>(
        realm_->component().CloneExposedDir().TakeChannel());
  }

 protected:
  // The realm's exposed directory, which holds the exposed services at its
  // root.
  fidl::UnownedClientEnd<fuchsia_io::Directory> exposed() const {
    return exposed_.borrow();
  }

  // Returns true if `instance` of `service` is exposed, by opening its
  // directory.
  bool ServiceInstanceIsExposed(std::string_view service,
                                std::string_view instance) {
    return DirectoryIsExposed(
        std::string(service).append("/").append(instance));
  }

  // Returns true if the directory of `service` is exposed, whether or not it
  // holds any instance.
  bool ServiceDirectoryIsExposed(std::string_view service) {
    return DirectoryIsExposed(std::string(service));
  }

  // Returns true if `path` is a directory in the realm's exposed directory, by
  // opening it.
  bool DirectoryIsExposed(const std::string& path) {
    zx::result dir = component::OpenDirectoryAt(exposed(), path);
    if (dir.is_error()) {
      return false;
    }
    return fidl::WireCall(*dir)->Query().ok();
  }

  // Calls VirtualController.CreateEmulator through the exposed
  // VirtualControllerService instance and returns the emulator's name.
  zx::result<std::string> CreateEmulator() {
    // Spelled out by path: the pinned SDK has no bindings for
    // VirtualControllerService yet.
    zx::result controller = component::ConnectAt<fhb::VirtualController>(
        exposed(),
        std::string(VirtualController::kServiceName)
            .append("/")
            .append(component::kDefaultInstance)
            .append("/")
            .append(VirtualController::kServiceMemberName));
    if (controller.is_error()) {
      return controller.take_error();
    }
    fidl::WireResult result = fidl::WireCall(*controller)->CreateEmulator();
    if (!result.ok()) {
      return zx::error(result.status());
    }
    if (result->is_error()) {
      return zx::error(result->error_value());
    }
    if (result->value()->name.is_null()) {
      return zx::error(ZX_ERR_BAD_STATE);
    }
    return zx::ok(std::string(result->value()->name.get()));
  }

 private:
  // Never run: Build() needs a dispatcher only for local components (this
  // realm has none) and RealmRoot tears the realm down synchronously, so all
  // traffic here is synchronous FIDL. Declared before `realm_`, which refers
  // to this dispatcher.
  async::Loop loop_{&kAsyncLoopConfigAttachToCurrentThread};
  std::optional<component_testing::RealmRoot> realm_;
  fidl::ClientEnd<fuchsia_io::Directory> exposed_;
};

// The device service directories can be opened, and so watched, before the
// first emulator or loopback device exists.
TEST_F(ComponentTest, ExposesDeviceServiceDirectoriesFromTheStart) {
  EXPECT_TRUE(ServiceDirectoryIsExposed(fhb::EmulatorService::Name));
  EXPECT_TRUE(ServiceDirectoryIsExposed(fhb::Service::Name));
  EXPECT_FALSE(ServiceInstanceIsExposed(
      fhb::EmulatorService::Name, VirtualController::FirstEmulatorName()));
}

// The component exposes VirtualControllerService/default, through which a
// client creates emulators; a fresh component numbers them from the start.
TEST_F(ComponentTest, ExposesVirtualControllerService) {
  zx::result name = CreateEmulator();
  ASSERT_TRUE(name.is_ok()) << name.status_string();
  EXPECT_EQ(*name, VirtualController::FirstEmulatorName());
}

// An emulator's EmulatorService instance appears when it is created, its
// vendor Service instance when it is published, and both go away when its
// owner disconnects.
TEST_F(ComponentTest, EmulatorServicesFollowTheEmulatorsLifetime) {
  zx::result name = CreateEmulator();
  ASSERT_TRUE(name.is_ok()) << name.status_string();
  EXPECT_TRUE(ServiceInstanceIsExposed(fhb::EmulatorService::Name, *name));
  EXPECT_FALSE(ServiceInstanceIsExposed(fhb::Service::Name, *name));

  // The first Emulator connection owns the emulator.
  zx::result owner_end =
      component::ConnectAtMember<fhb::EmulatorService::Device>(exposed(),
                                                               *name);
  ASSERT_TRUE(owner_end.is_ok()) << owner_end.status_string();
  std::optional<fidl::SyncClient<fhb::Emulator>> owner(std::move(*owner_end));
  fidl::Result publish = (*owner)->Publish(fhb::EmulatorSettings());
  ASSERT_TRUE(publish.is_ok()) << publish.error_value().FormatDescription();

  EXPECT_TRUE(ServiceInstanceIsExposed(fhb::Service::Name, *name));
  zx::result vendor =
      component::ConnectAtMember<fhb::Service::Vendor>(exposed(), *name);
  ASSERT_TRUE(vendor.is_ok()) << vendor.status_string();
  EXPECT_TRUE(fidl::WireCall(*vendor)->GetFeatures().ok());

  // Shutdown() removes both service instances and closes the Vendor
  // connections in one task, so the latter marks the former as done.
  owner.reset();
  zx_signals_t observed = 0;
  ASSERT_EQ(
      vendor->channel().wait_one(
          ZX_CHANNEL_PEER_CLOSED, zx::deadline_after(kWaitTimeout), &observed),
      ZX_OK);
  EXPECT_FALSE(ServiceInstanceIsExposed(fhb::EmulatorService::Name, *name));
  EXPECT_FALSE(ServiceInstanceIsExposed(fhb::Service::Name, *name));
}

}  // namespace
}  // namespace bt_hci_virtual
