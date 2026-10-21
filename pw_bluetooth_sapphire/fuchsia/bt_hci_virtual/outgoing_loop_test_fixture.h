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
#pragma once

#include <fidl/fuchsia.hardware.bluetooth/cpp/fidl.h>
#include <fidl/fuchsia.io/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/incoming/cpp/service.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/zx/time.h>
#include <zircon/types.h>

#include <string>
#include <utility>

#include "gtest/gtest.h"

namespace bt_hci_virtual::testing {

// Serves a component::OutgoingDirectory on a single-threaded async::Loop and
// probes the fuchsia.hardware.bluetooth service instances published to it, as
// the devices and the VirtualController publish them.
class OutgoingLoopTestFixture : public ::testing::Test {
 public:
  void SetUp() override {
    zx::result endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    ASSERT_TRUE(endpoints.is_ok()) << endpoints.status_string();
    zx::result serve_result = outgoing_.Serve(std::move(endpoints->server));
    ASSERT_TRUE(serve_result.is_ok()) << serve_result.status_string();
    outgoing_client_ = std::move(endpoints->client);
  }

 protected:
  async_dispatcher_t* dispatcher() { return loop_.dispatcher(); }

  void RunUntilIdle() { loop_.RunUntilIdle(); }

  // Shuts down the dispatcher, as happens when the component or driver hosting
  // the devices stops.
  void ShutdownLoop() { loop_.Shutdown(); }

  component::OutgoingDirectory* outgoing() { return &outgoing_; }

  fidl::UnownedClientEnd<fuchsia_io::Directory> outgoing_client() const {
    return outgoing_client_.borrow();
  }

  // Returns true if a Vendor client connected through the
  // fuchsia.hardware.bluetooth.Service instance `name` can complete a request.
  bool VendorServiceIsServed(const std::string& name) {
    std::string path =
        "svc/" + component::MakeServiceMemberPath<
                     fuchsia_hardware_bluetooth::Service::Vendor>(name);
    zx::result client_end =
        component::ConnectAt<fuchsia_hardware_bluetooth::Vendor>(
            outgoing_client(), path);
    if (client_end.is_error()) {
      return false;
    }
    fidl::WireClient<fuchsia_hardware_bluetooth::Vendor> client(
        std::move(*client_end), dispatcher());
    bool ok = false;
    client->GetFeatures().ThenExactlyOnce(
        [&ok](fidl::WireUnownedResult<
              fuchsia_hardware_bluetooth::Vendor::GetFeatures>& res) {
          ok = res.ok();
        });
    RunUntilIdle();
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

 private:
  async::Loop loop_{&kAsyncLoopConfigAttachToCurrentThread};
  component::OutgoingDirectory outgoing_{loop_.dispatcher()};
  fidl::ClientEnd<fuchsia_io::Directory> outgoing_client_;
};

}  // namespace bt_hci_virtual::testing
