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

#include <cstddef>
#include <cstdint>

// These host link functions should be implemented by the system integrator.
namespace pw::chre {

/// @module{pw_chre}

/// This is a token representing a message that CHRE allocated.
/// It must be passed to `FreeMessageToAp` when the message is finished.
using MessageToApContext = const void*;

/// This is a message that should be sent to the AP.
/// It was allocated by CHRE, so pw::chre::FreeMessageToAp should be called
/// in order to free it.
struct MessageToAp {
  /// The id of the nanoapp sending the message.
  uint64_t nanoapp_id;

  /// The type of the message.
  uint32_t message_type;

  uint32_t app_permissions;
  uint32_t message_permissions;

  /// The id of the client that this message should be delivered to on the host.
  uint16_t host_endpoint;

  /// Whether CHRE is responsible for waking the AP.
  /// If this is true, then the client must wake the AP in
  /// `SendMessageToAp` before sending this message.
  bool woke_host;

  /// The underlying data of the message. This is owned by `chre_context` and
  /// should not be accessed after the message has been freed.
  const uint8_t* data;

  /// The length of `data` in bytes.
  size_t length;

  /// The context of the message, used to free the message when the client is
  /// finished sending it.
  MessageToApContext chre_context;
};

/// CHRE calls this method to send a message to the Application Processor (AP).
/// The client must implement this method, and the client is responsible for
/// calling `FreeMessageToAp` once they are finished with the message.
/// @param[in] message The message to be sent.
/// @returns Whether this method was successful.
bool SendMessageToAp(MessageToAp message);

}  // namespace pw::chre
