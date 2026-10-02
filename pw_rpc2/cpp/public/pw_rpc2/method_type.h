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

#include <cstdint>

namespace pw::rpc2 {

/// The type of an RPC method.
///
/// Bit 0 is set if the server streams responses, and bit 1 if the client
/// streams requests.
enum class MethodType : uint8_t {
  kUnary = 0,
  kServerStreaming = 1,
  kClientStreaming = 2,
  kBidirectionalStreaming = 3,
};

/// True if the server streams responses: server and bidirectional streaming.
constexpr bool HasServerStream(MethodType type) {
  return (static_cast<uint8_t>(type) & 0b01u) != 0u;
}

/// True if the client streams requests: client and bidirectional streaming.
constexpr bool HasClientStream(MethodType type) {
  return (static_cast<uint8_t>(type) & 0b10u) != 0u;
}

}  // namespace pw::rpc2
