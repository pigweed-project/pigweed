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

#include "pw_bluetooth_sapphire/internal/host/gap/fake_pairing_delegate.h"

#include "pw_bluetooth_sapphire/internal/host/testing/test_helpers.h"
#include "pw_unit_test/framework.h"

namespace bt::gap {
namespace {

TEST(FakePairingDelegateTest, io_capability) {
  FakePairingDelegate delegate(sm::IOCapability::kDisplayYesNo);
  EXPECT_EQ(sm::IOCapability::kDisplayYesNo, delegate.io_capability());
  delegate.set_io_capability(sm::IOCapability::kNoInputNoOutput);
  EXPECT_EQ(sm::IOCapability::kNoInputNoOutput, delegate.io_capability());
}

TEST(FakePairingDelegateTest, CompletePairing) {
  FakePairingDelegate delegate(sm::IOCapability::kDisplayYesNo);

  bool cb_called = false;
  auto cb = [&cb_called](auto id, auto status) {
    cb_called = true;
    EXPECT_EQ(PeerId(5), id);
    EXPECT_EQ(ToResult(HostError::kFailed), status);
  };
  delegate.SetCompletePairingCallback(std::move(cb));
  delegate.CompletePairing(PeerId(5), ToResult(HostError::kFailed));
  EXPECT_TRUE(cb_called);
}

TEST(FakePairingDelegateTest, ConfirmPairing) {
  FakePairingDelegate delegate(sm::IOCapability::kDisplayYesNo);

  bool cb_called = false;
  auto cb = [&cb_called](auto id, auto confirm) {
    cb_called = true;
    EXPECT_EQ(PeerId(5), id);
    ASSERT_TRUE(confirm);
    confirm(true);
  };
  delegate.SetConfirmPairingCallback(std::move(cb));
  delegate.ConfirmPairing(PeerId(5), [](bool) {});
  EXPECT_TRUE(cb_called);
}

TEST(FakePairingDelegateTest, DisplayPasskey) {
  FakePairingDelegate delegate(sm::IOCapability::kDisplayYesNo);

  bool cb_called = false;
  auto cb = [&cb_called](auto id, auto passkey, auto method, auto confirm) {
    cb_called = true;
    EXPECT_EQ(PeerId(5), id);
    EXPECT_EQ(123456U, passkey);
    EXPECT_EQ(PairingDelegate::DisplayMethod::kComparison, method);
    ASSERT_TRUE(confirm);
    confirm(true);
  };
  delegate.SetDisplayPasskeyCallback(std::move(cb));
  delegate.DisplayPasskey(
      PeerId(5), 123456, PairingDelegate::DisplayMethod::kComparison, [](bool) {
      });
  EXPECT_TRUE(cb_called);
}

TEST(FakePairingDelegateTest, RequestPasskey) {
  FakePairingDelegate delegate(sm::IOCapability::kDisplayYesNo);

  bool cb_called = false;
  auto cb = [&cb_called](auto id, auto respond) {
    cb_called = true;
    EXPECT_EQ(PeerId(5), id);
    ASSERT_TRUE(respond);
    respond(-1);
  };
  delegate.SetRequestPasskeyCallback(std::move(cb));
  delegate.RequestPasskey(PeerId(5), [](int64_t) {});
  EXPECT_TRUE(cb_called);
}

TEST(FakePairingDelegateTest, UnexpectedCalls) {
  FakePairingDelegate delegate(sm::IOCapability::kDisplayYesNo);

  // Each of the following calls should generate failure(s).
  // delegate.CompletePairing(PeerId(5), ToResult(HostError::kFailed));
  // delegate.ConfirmPairing(PeerId(5), [](bool) {});
  // delegate.DisplayPasskey(PeerId(5), 123456,
  // PairingDelegate::DisplayMethod::kComparison,
  //                         [](bool) {});
  // delegate.RequestPasskey(PeerId(5), [](uint64_t) {});
}

TEST(FakePairingDelegateTest, ExpectCallNotCalled) {
  FakePairingDelegate delegate(sm::IOCapability::kDisplayYesNo);

  // delegate.SetCompletePairingCallback([](auto, auto) {});
  // delegate.SetConfirmPairingCallback([](auto, auto) {});
  // delegate.SetDisplayPasskeyCallback([](auto, auto, auto, auto) {});
  // delegate.SetRequestPasskeyCallback([](auto, auto) {});
  // Each of the preceding calls should generate failure(s) when |delegate| goes
  // out of scope.
}

}  // namespace
}  // namespace bt::gap
