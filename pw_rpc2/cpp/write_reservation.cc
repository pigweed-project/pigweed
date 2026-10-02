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

#include "pw_rpc2/write_reservation.h"

#include <cstddef>

#include "pw_assert/check.h"
#include "pw_async2/await.h"
#include "pw_bytes/span.h"
#include "pw_rpc2/writer.h"
#include "pw_status/status.h"

// These members are defined out of line so that their preconditions can be
// checked with `PW_CHECK`/`PW_DCHECK`, which carry a message but may not be
// used in headers.

namespace pw::rpc2 {
namespace internal {

async2::Poll<Status> WriteFutureBase::PendWrite(async2::Context& cx,
                                                const void* payload,
                                                SerializeFn serialize) {
  PW_CHECK(is_pendable());

  PW_AWAIT(auto res_result, res_fut_, cx);

  if (!res_result.ok()) {
    return async2::Ready(res_result.status());
  }

  WriteReservation res = std::move(*res_result);

  size_t size_bytes = 0;
  if (serialize != nullptr) {
    StatusWithSize serialized = serialize(payload, res);
    if (!serialized.ok()) {
      return async2::Ready(serialized.status());
    }
    size_bytes = serialized.size();
  } else {
    // `serialize` is null only for header-only packets (`WriteFuture<void>`),
    // which are reserved with a payload size of 0.
    PW_DASSERT(res.size() == 0);
  }

  return async2::Ready(res.Commit(size_bytes));
}

}  // namespace internal

Status WriteReservation::Commit(size_t size_bytes) {
  if (!is_active()) {
    return Status::FailedPrecondition();
  }
  PW_CHECK_UINT_LE(size_bytes, size());

  // The call ended while this reservation was held: its terminal packet was
  // sent first, or it was cancelled, completed by the peer, or retired. The
  // peer has forgotten the call, so this packet would only arrive as a stray.
  if (call_ != nullptr && call_->is_write_closed()) {
    reservation_.Cancel();
    const Status status =
        call_->is_completed() && !call_->completion_status().ok()
            ? call_->completion_status()
            : Status::FailedPrecondition();
    ReleaseCall(/*committed=*/false);
    return status;
  }

  auto encode_res = packet_.EncodeHeader(reservation_, size_bytes);
  // The header always fits: the reservation was sized to hold it, and
  // `size_bytes` is bounded by `size()`.
  PW_CHECK_OK(encode_res.status(),
              "Failed to encode the header of an RPC packet that was reserved "
              "with room for it");
  if (!reservation_.Commit(*encode_res)) {
    ReleaseCall(/*committed=*/false);
    return Status::Unavailable();
  }
  ReleaseCall(/*committed=*/true);
  return OkStatus();
}

void WriteReservation::ReleaseCall(bool committed) {
  if (call_ == nullptr) {
    return;
  }
  if (committed && packet_.type().is_start()) {
    call_->MarkStarted();
  }
  if (packet_.closes_stream()) {
    if (committed) {
      call_->CommitTerminalWrite();
    } else {
      call_->AbandonTerminalWrite();
    }
  }
  call_ = nullptr;
}

ConstByteSpan WriteReservation::PayloadSpan() const {
  PW_DCHECK(is_active(),
            "WriteReservation accessed after Commit(), Drop(), or move");
  if (!packet_.type().has_payload()) {
    return {};
  }
  return ConstByteSpan(reservation_).subspan(packet_.payload_offset());
}

}  // namespace pw::rpc2
