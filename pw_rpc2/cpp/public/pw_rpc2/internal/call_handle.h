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
#include <utility>

#include "pw_assert/assert.h"
#include "pw_intrusive_ptr/intrusive_ptr.h"
#include "pw_result/result.h"
#include "pw_rpc2/internal/call.h"
#include "pw_status/status.h"
#include "pw_transport/socket.h"

namespace pw::rpc2::internal {

/// Common base for the public RPC call handles (`Reader`, `Writer`,
/// `UnaryWriter`, and `ReadFuture`).
///
/// `CallHandle` owns the `IntrusivePtr<Call>` that every handle wraps and
/// provides the constructors and accessors they all share. Handles may be
/// default constructed or moved from, in which case they refer to no call;
/// `call()` performs the validity check for every operation that dereferences
/// it, so derived classes do not repeat it.
///
/// Moving a `CallHandle` transfers the call pointer and leaves the source
/// handle empty; it does not touch the call itself. Derived classes are
/// responsible for any end-of-call behavior in their own move assignment and
/// destructor.
class CallHandle {
 public:
  CallHandle(const CallHandle&) = delete;
  CallHandle& operator=(const CallHandle&) = delete;

 protected:
  // Call IDs are an internal detail, so they are exposed through `CallAccess`
  // rather than on the handles themselves.
  friend struct CallAccess;

  constexpr CallHandle() = default;

  explicit CallHandle(const IntrusivePtr<Call>& call) : call_(call) {}
  explicit CallHandle(IntrusivePtr<Call>&& call) : call_(std::move(call)) {}

  CallHandle(CallHandle&&) noexcept = default;
  CallHandle& operator=(CallHandle&&) noexcept = default;

  ~CallHandle() = default;

  /// Returns true if this handle still refers to a call. Use this for
  /// operations that tolerate an empty handle, such as destructors.
  [[nodiscard]] bool has_call() const { return call_ != nullptr; }

  /// Returns the call this handle refers to.
  ///
  /// This is the single place where handle validity is checked: using a handle
  /// that was default constructed or moved from is a programming error and
  /// crashes here.
  [[nodiscard]] Call& call() const {
    PW_DASSERT(call_ != nullptr);
    return *call_;
  }

  /// Returns a reference to the underlying `IntrusivePtr<Call>`.
  ///
  /// Use this when constructing another handle or future from this one, so
  /// that the new object keeps the call alive without creating a temporary
  /// `IntrusivePtr<Call>`.
  [[nodiscard]] const IntrusivePtr<Call>& share_call() const { return call_; }

  /// The inputs every outbound packet needs: a reservation attempt, the ID
  /// of the call the packet belongs to, and a reference to the call (used by
  /// terminal packets to close the write side upon commit or fall back on
  /// abandonment).
  struct Reservation {
    Result<transport::ReserveWriteFuture> future;
    uint32_t call_id;
    EndpointRole role;
    IntrusivePtr<Call> call;
  };

  /// Begins an egress reservation for a packet with a `header_size`-byte
  /// header and up to `max_payload_size` bytes of payload on this handle's
  /// call.
  ///
  /// Handles the empty-handle case, so callers can pass the result straight to
  /// a `ReserveWriteFuture` factory: writing through a handle that was
  /// default constructed or moved from yields a future resolved to
  /// `FAILED_PRECONDITION` rather than dereferencing nothing.
  [[nodiscard]] Reservation ReserveOutbound(size_t header_size,
                                            size_t max_payload_size) const {
    if (!has_call()) {
      return {Status::FailedPrecondition(), 0, EndpointRole::kClient, nullptr};
    }
    return {call().ReserveWrite(header_size, max_payload_size),
            call().call_id(),
            call().role(),
            share_call()};
  }

 private:
  IntrusivePtr<Call> call_;
};

}  // namespace pw::rpc2::internal
