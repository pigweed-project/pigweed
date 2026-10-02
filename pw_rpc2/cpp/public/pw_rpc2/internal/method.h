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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_buf/buf.h"
#include "pw_rpc2/internal/protocol_status.h"
#include "pw_rpc2/method_type.h"

namespace pw::rpc2 {
class Service;
}  // namespace pw::rpc2

namespace pw::rpc2::internal {

class ServerCall;

/// Descriptor for a registered RPC method in a service's method table.
class Method {
 public:
  using InvokeFn = ProtocolStatus (*)(Service& service,
                                      ServerCall& call,
                                      ConstBuf&& request_payload);

  /// Maximum supported size in bytes for a method's future storage.
  ///
  /// This storage is allocated per in-flight call and lives inline in the
  /// call's `ServerCall`, so it is a direct multiplier on the memory a
  /// server needs to serve concurrent requests.
  static constexpr size_t kMaxSizeBytes = std::numeric_limits<uint16_t>::max();

  /// Describes a method of `type`.
  ///
  /// `invoke` receives the request message for unary and server-streaming
  /// methods. For client-streaming and bidirectional-streaming methods, any
  /// message in the packet that opened the call is instead queued to the
  /// call's reader, and `invoke` receives an empty buffer.
  constexpr Method(uint32_t id,
                   MethodType type,
                   size_t future_storage_size,
                   InvokeFn invoke)
      : id_(id),
        future_storage_size_(PackSize(future_storage_size)),
        type_(type),
        invoke_(invoke) {}

  constexpr uint32_t id() const { return id_; }
  constexpr MethodType type() const { return type_; }
  constexpr size_t future_storage_size() const { return future_storage_size_; }

  ProtocolStatus Invoke(Service& service,
                        ServerCall& call,
                        ConstBuf&& request_payload) const {
    return invoke_(service, call, std::move(request_payload));
  }

 private:
  static constexpr uint16_t PackSize(size_t size) {
    PW_ASSERT(size <= kMaxSizeBytes);
    return static_cast<uint16_t>(size);
  }

  uint32_t id_;
  uint16_t future_storage_size_;
  MethodType type_;
  InvokeFn invoke_;
};

// `Method` is intended to pack into two 32-bit words --- the ID, then the
// 16-bit size and 8-bit type --- followed by a function pointer, i.e. 16 bytes
// on 64-bit targets and 12 bytes on 32-bit targets. The exact size is not part
// of the ABI, so only a ceiling is enforced here.
static_assert(sizeof(Method) <= 2 * sizeof(uint32_t) + 2 * sizeof(void*),
              "Method must not grow beyond its intended packing");

}  // namespace pw::rpc2::internal
