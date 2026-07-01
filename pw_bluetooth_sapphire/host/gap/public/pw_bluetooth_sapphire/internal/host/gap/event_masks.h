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

#pragma once
#include <cstdint>

#include "pw_bluetooth_sapphire/internal/host/hci-spec/constants.h"

namespace bt::gap {

// Builds and returns the HCI event mask based on our supported host side
// features and controller capabilities. This is used to mask events that we
// do not know how to handle.
constexpr uint64_t BuildEventMask() {
  uint64_t event_mask = 0;

#define ENABLE_EVT(event) \
  event_mask |= static_cast<uint64_t>(hci_spec::EventMask::event)

  // Enable events that are needed for basic functionality. (alphabetic)
  ENABLE_EVT(kAuthenticationCompleteEvent);
  ENABLE_EVT(kConnectionCompleteEvent);
  ENABLE_EVT(kConnectionPacketTypeChangedEvent);
  ENABLE_EVT(kConnectionRequestEvent);
  ENABLE_EVT(kDataBufferOverflowEvent);
  ENABLE_EVT(kDisconnectionCompleteEvent);
  ENABLE_EVT(kEncryptionChangeEvent);
  ENABLE_EVT(kEncryptionKeyRefreshCompleteEvent);
  ENABLE_EVT(kExtendedInquiryResultEvent);
  ENABLE_EVT(kHardwareErrorEvent);
  ENABLE_EVT(kInquiryCompleteEvent);
  ENABLE_EVT(kInquiryResultEvent);
  ENABLE_EVT(kInquiryResultWithRSSIEvent);
  ENABLE_EVT(kIOCapabilityRequestEvent);
  ENABLE_EVT(kIOCapabilityResponseEvent);
  ENABLE_EVT(kLEMetaEvent);
  ENABLE_EVT(kLinkKeyRequestEvent);
  ENABLE_EVT(kLinkKeyNotificationEvent);
  ENABLE_EVT(kModeChangeEvent);
  ENABLE_EVT(kPINCodeRequestEvent);
  ENABLE_EVT(kRemoteOOBDataRequestEvent);
  ENABLE_EVT(kRemoteNameRequestCompleteEvent);
  ENABLE_EVT(kReadRemoteSupportedFeaturesCompleteEvent);
  ENABLE_EVT(kReadRemoteVersionInformationCompleteEvent);
  ENABLE_EVT(kReadRemoteExtendedFeaturesCompleteEvent);
  ENABLE_EVT(kRoleChangeEvent);
  ENABLE_EVT(kSimplePairingCompleteEvent);
  ENABLE_EVT(kSynchronousConnectionCompleteEvent);
  ENABLE_EVT(kUserConfirmationRequestEvent);
  ENABLE_EVT(kUserPasskeyRequestEvent);
  ENABLE_EVT(kUserPasskeyNotificationEvent);

#undef ENABLE_EVT

  return event_mask;
}

// Builds and returns the LE event mask based on our supported host side
// features and controller capabilities. This is used to mask LE events that
// we do not know how to handle.
constexpr uint64_t BuildLEEventMask() {
  uint64_t event_mask = 0;

#define ENABLE_EVT(event) \
  event_mask |= static_cast<uint64_t>(hci_spec::LEEventMask::event)

  ENABLE_EVT(kLEAdvertisingReport);
  ENABLE_EVT(kLEConnectionComplete);
  ENABLE_EVT(kLEConnectionUpdateComplete);
  ENABLE_EVT(kLEEnhancedConnectionComplete);
  ENABLE_EVT(kLEExtendedAdvertisingReport);
  ENABLE_EVT(kLEExtendedAdvertisingSetTerminated);
  ENABLE_EVT(kLECISRequest);
  ENABLE_EVT(kLECISEstablished);
  ENABLE_EVT(kLELongTermKeyRequest);
  ENABLE_EVT(kLEReadRemoteFeaturesComplete);
  ENABLE_EVT(kLERequestPeerSCAComplete);

#undef ENABLE_EVT

  return event_mask;
}

}  // namespace bt::gap
