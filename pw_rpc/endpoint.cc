// Copyright 2021 The Pigweed Authors
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

// clang-format off
#include "pw_rpc/internal/log_config.h"  // PW_LOG_* macros must be first.

#include "pw_rpc/internal/endpoint.h"
// clang-format on

#include "pw_log/log.h"
#include "pw_rpc/internal/lock.h"

#if PW_RPC_YIELD_MODE == PW_RPC_YIELD_MODE_BUSY_LOOP

static_assert(
    PW_RPC_USE_GLOBAL_MUTEX == 0,
    "The RPC global mutex is enabled, but no pw_rpc yield mode is selected! "
    "Because the global mutex is in use, pw_rpc may be used from multiple "
    "threads. This could result in thread starvation. To fix this, set "
    "PW_RPC_YIELD to PW_RPC_YIELD_MODE_SLEEP and add a dependency on "
    "pw_thread:sleep.");

#elif PW_RPC_YIELD_MODE == PW_RPC_YIELD_MODE_SLEEP

#include <chrono>

#if !__has_include("pw_thread/sleep.h")

static_assert(false,
              "PW_RPC_YIELD_MODE is PW_RPC_YIELD_MODE_SLEEP "
              "(pw::this_thread::sleep_for()), but no backend is set for "
              "pw_thread:sleep. Set a pw_thread:sleep backend or use a "
              "different PW_RPC_YIELD_MODE setting.");

#endif  // !__has_include("pw_thread/sleep.h")

#include "pw_thread/sleep.h"

#elif PW_RPC_YIELD_MODE == PW_RPC_YIELD_MODE_YIELD

#if !__has_include("pw_thread/yield.h")

static_assert(false,
              "PW_RPC_YIELD_MODE is PW_RPC_YIELD_MODE_YIELD "
              "(pw::this_thread::yield()), but no backend is set for "
              "pw_thread:yield. Set a pw_thread:yield backend or use a "
              "different PW_RPC_YIELD_MODE setting.");

#endif  // !__has_include("pw_thread/yield.h")

#include "pw_thread/yield.h"

#else

static_assert(
    false,
    "PW_RPC_YIELD_MODE macro must be set to PW_RPC_YIELD_MODE_BUSY_LOOP, "
    "PW_RPC_YIELD_MODE_SLEEP (pw::this_thread::sleep_for()), or "
    "PW_RPC_YIELD_MODE_YIELD (pw::this_thread::yield())");

#endif  // PW_RPC_YIELD_MODE

namespace pw::rpc::internal {

void YieldRpcLock() {
  rpc_lock().unlock();
#if PW_RPC_YIELD_MODE == PW_RPC_YIELD_MODE_SLEEP
  static constexpr chrono::SystemClock::duration kSleepDuration =
      PW_RPC_YIELD_SLEEP_DURATION;
  this_thread::sleep_for(kSleepDuration);
#elif PW_RPC_YIELD_MODE == PW_RPC_YIELD_MODE_YIELD
  this_thread::yield();
#endif  // PW_RPC_YIELD_MODE
  rpc_lock().lock();
}

Result<Packet> Endpoint::ProcessPacket(span<const std::byte> data,
                                       Packet::Destination destination) {
  Result<Packet> result = Packet::FromBuffer(data);

  if (!result.ok()) {
    PW_LOG_WARN("Failed to decode pw_rpc packet");
    return Status::DataLoss();
  }

  Packet& packet = *result;

  if (packet.channel_id() == Channel::kUnassignedChannelId ||
      packet.service_id() == 0 || packet.method_id() == 0) {
    PW_LOG_WARN("Received malformed pw_rpc packet");
    return Status::DataLoss();
  }

  if (packet.destination() != destination) {
    return Status::InvalidArgument();
  }

  return result;
}

void Endpoint::RegisterCall(Call& new_call) {
  // Mark any exisitng duplicate calls as cancelled.
  auto [before_call, call] = FindIteratorsForCall(new_call);
  if (call != calls_.end()) {
    CloseCallAndMarkForCleanup(before_call, call, Status::Cancelled());
  }

  // Register the new call.
  calls_.push_front(new_call);
}

std::tuple<IntrusiveForwardList<Call>::iterator,
           IntrusiveForwardList<Call>::iterator>
Endpoint::FindIteratorsForCall(uint32_t channel_id,
                               uint32_t service_id,
                               uint32_t method_id,
                               uint32_t call_id) {
  auto previous = calls_.before_begin();
  auto call = calls_.begin();

  while (call != calls_.end()) {
    if (channel_id == call->channel_id_locked() &&
        service_id == call->service_id() && method_id == call->method_id()) {
      if (call_id == call->id() || call_id == kOpenCallId ||
          call_id == kLegacyOpenCallId) {
        break;
      }
      if (call->id() == kOpenCallId || call->id() == kLegacyOpenCallId) {
        // Calls with ID of `kOpenCallId` were unrequested, and
        // are updated to have the call ID of the first matching request.
        //
        // kLegacyOpenCallId is used for compatibility with old servers
        // which do not specify a Call ID but expect to be able to send
        // unrequested responses.
        call->set_id(call_id);
        break;
      }
    }
    previous = call;
    ++call;
  }

  return {previous, call};
}

Status Endpoint::CloseChannel(uint32_t channel_id) {
  rpc_lock().lock();

  Channel* channel = channels_.Get(channel_id);
  if (channel == nullptr) {
    rpc_lock().unlock();
    return Status::NotFound();
  }
  static_cast<internal::ChannelBase*>(channel)->Close();

  // Close pending calls on the channel that's going away.
  AbortCalls(AbortIdType::kChannel, channel_id);

  CleanUpCalls();

  return OkStatus();
}

void Endpoint::AbortCalls(AbortIdType type, uint32_t id) {
  auto previous = calls_.before_begin();
  auto current = calls_.begin();

  while (current != calls_.end()) {
    if (id == (type == AbortIdType::kChannel ? current->channel_id_locked()
                                             : current->service_id())) {
      current =
          CloseCallAndMarkForCleanup(previous, current, Status::Aborted());
    } else {
      previous = current;
      ++current;
    }
  }
}

void Endpoint::CleanUpCalls() {
  if (to_cleanup_.empty()) {
    rpc_lock().unlock();
    return;
  }

  // Drain the to_cleanup_ list. This while loop is structured to avoid
  // unnecessarily acquiring the lock after popping the last call in the common
  // case, while re-checking to_cleanup_.empty() if a concurrent cleanup or
  // DynamicCallPtr::reset() drained remaining calls while the lock was
  // released.
  while (!to_cleanup_.empty()) {
    Call& call = to_cleanup_.front();
    to_cleanup_.pop_front();

    const bool done = to_cleanup_.empty();

    call.CleanUpFromEndpoint();

    if (done) {
      return;
    }

    rpc_lock().lock();
  }
  rpc_lock().unlock();
}

void Endpoint::RemoveAllCalls() {
  RpcLockGuard lock;

  // Close all calls without invoking on_error callbacks, since the calls should
  // have been closed before the Endpoint was deleted.
  while (!calls_.empty()) {
    calls_.front().CloseFromDeletedEndpoint();
    calls_.pop_front();
  }
  while (!to_cleanup_.empty()) {
    to_cleanup_.front().CloseFromDeletedEndpoint();
    to_cleanup_.pop_front();
  }
}

}  // namespace pw::rpc::internal
