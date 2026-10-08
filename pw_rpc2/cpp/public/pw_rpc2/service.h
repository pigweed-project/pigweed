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

#include "pw_containers/intrusive_forward_list.h"
#include "pw_rpc2/internal/method.h"
#include "pw_span/span.h"

namespace pw::rpc2 {

namespace internal {

struct ServiceAccess;

}  // namespace internal

/// A collection of RPC methods.
///
/// @note **Method lifetime.** A method's future or coroutine is owned by its
/// call and is destroyed as soon as the call ends (when the final response
/// packet is committed, the client cancels, the connection closes, or the
/// server shuts down). Perform all per-call work before committing the final
/// write. Conversely, if a method's future or coroutine finishes before its
/// `Writer` or `UnaryWriter` has completed the call, the call is terminated
/// and any remaining handles are detached.
///
/// @note **Per-call state.** All of a service's calls share the service
/// object. Keep per-call state in the method's future or coroutine.
class Service : public IntrusiveForwardList<Service>::Item {
 protected:
  constexpr Service(uint32_t service_id, span<const internal::Method> methods)
      : methods_(methods), service_id_(service_id) {}

  /// @pre The service must not still be registered with a `Server`, and any
  /// call dispatched to it must have finished.
  ///
  /// A service cannot safely unlink itself: the registry belongs to the
  /// server's dispatcher, which is not reachable from here. Destroying a
  /// registered service is a bug in any case, because only closing the server
  /// guarantees no new call is dispatched to it.
  ///
  /// `Server::Close()` unregisters everything, so a service may be destroyed
  /// once the close has completed --- that is, once the `ControlFuture` from
  /// `Close()` has resolved or `CloseBlocking()` has returned.
  ~Service();

 private:
  friend struct internal::ServiceAccess;

  uint32_t service_id() const { return service_id_; }

  span<const internal::Method> methods() const { return methods_; }

  const internal::Method* FindMethod(uint32_t method_id) const {
    for (const auto& method : methods_) {
      if (method.id() == method_id) {
        return &method;
      }
    }
    return nullptr;
  }

  span<const internal::Method> methods_;
  uint32_t service_id_;
};

}  // namespace pw::rpc2
