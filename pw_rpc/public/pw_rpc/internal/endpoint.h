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
#pragma once

#include <cstddef>
#include <iterator>
#include <tuple>

#include "pw_assert/assert.h"
#include "pw_containers/intrusive_list.h"
#include "pw_result/result.h"
#include "pw_rpc/channel.h"
#include "pw_rpc/internal/call.h"
#include "pw_rpc/internal/channel_list.h"
#include "pw_rpc/internal/lock.h"
#include "pw_rpc/internal/packet.h"
#include "pw_span/span.h"
#include "pw_sync/lock_annotations.h"

namespace pw::rpc::internal {

class LockedEndpoint;

// Manages a list of channels and a list of ongoing calls for either a server or
// client.
//
// For clients, calls start when they send a REQUEST packet to a server. For
// servers, calls start when the REQUEST packet is received. In either case,
// calls add themselves to the Endpoint's list when they're started and
// remove themselves when they complete. Calls do this through their associated
// Server or Client object, which derive from Endpoint.
class Endpoint {
 public:
  // If an endpoint is deleted, all calls using it are closed without notifying
  // the other endpoint.
  ~Endpoint() PW_LOCKS_EXCLUDED(rpc_lock()) { RemoveAllCalls(); }

  // Public functions

  // Creates a channel with the provided ID and ChannelOutput, if a channel slot
  // is available or can be allocated (if PW_RPC_DYNAMIC_ALLOCATION is enabled).
  // Returns:
  //
  //   OK - the channel was opened successfully
  //   ALREADY_EXISTS - a channel with this ID is already present; remove it
  //       first
  //   RESOURCE_EXHAUSTED - no unassigned channels are available and
  //       PW_RPC_DYNAMIC_ALLOCATION is disabled
  //
  Status OpenChannel(uint32_t id, ChannelOutput& interface)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    return channels_.Add(id, interface);
  }

  // Closes a channel and terminates any pending calls on that channel.
  // If the calls are client requests, their on_error callback will be
  // called with the ABORTED status.
  Status CloseChannel(uint32_t channel_id) PW_LOCKS_EXCLUDED(rpc_lock());

  // Internal functions, hidden by the Client and Server classes

  // Returns the number calls in the RPC calls list.
  size_t active_call_count() const PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    return static_cast<size_t>(std::distance(calls_.begin(), calls_.end()));
  }

  // Claims that `rpc_lock()` is held, returning a wrapped endpoint.
  //
  // This function should only be called in contexts in which it is clear that
  // `rpc_lock()` is held. When calling this function from a constructor, the
  // lock annotation will not result in errors, so care should be taken to
  // ensure that `rpc_lock()` is held.
  LockedEndpoint& ClaimLocked() PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());

  // Finds an internal::ChannelBase with this ID or nullptr if none matches.
  ChannelBase* GetInternalChannel(uint32_t channel_id)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    return channels_.Get(channel_id);
  }

  // Loops until the list of calls to clean up is empty. Releases the RPC lock.
  //
  // This must be called after operations that potentially put calls in the
  // awaiting cleanup state:
  //
  // - Creating a new call object, either from handling a request on the server
  //   or starting a new call on the client.
  // - Processing a stream message, since decoding to Nanopb or pwpb could fail,
  //   and the RPC mutex should not be released yet.
  // - Calls to CloseChannel() or UnregisterService(), which may need to cancel
  //   multiple calls before the mutex is released.
  //
  void CleanUpCalls() PW_UNLOCK_FUNCTION(rpc_lock());

  // Sets the default channel output.
  //
  // If a default channel output is already set, this will return AlreadyExists.
  Status SetDefaultChannelOutput(ChannelOutput& output)
      PW_LOCKS_EXCLUDED(rpc_lock()) {
    RpcLockGuard lock;
    return channels_.SetDefaultChannelOutput(output);
  }

 protected:
  _PW_RPC_CONSTEXPR Endpoint() = default;

  // Initializes the endpoint from a span of channels.
  _PW_RPC_CONSTEXPR Endpoint(span<Channel> channels) : channels_(channels) {}

  // Parses an RPC packet and sets ongoing_call to the matching call, if any.
  // Returns the parsed packet or an error.
  Result<Packet> ProcessPacket(span<const std::byte> data,
                               Packet::Destination destination)
      PW_LOCKS_EXCLUDED(rpc_lock());

  // Finds a call object for an ongoing call associated with this packet, if
  // any. The iterator will be calls_end() if no match was found.
  IntrusiveForwardList<Call>::iterator FindCall(const Packet& packet)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    return std::get<1>(FindIteratorsForCall(packet.channel_id(),
                                            packet.service_id(),
                                            packet.method_id(),
                                            packet.call_id()));
  }

  // Used to check if a call iterator is valid or not.
  IntrusiveForwardList<Call>::const_iterator calls_end() const
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    return calls_.end();
  }

  // Aborts calls associated with a particular service. Calls to
  // AbortCallsForService() must be followed by a call to CleanUpCalls().
  void AbortCallsForService(const Service& service)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    AbortCalls(AbortIdType::kService, UnwrapServiceId(service.service_id()));
  }

  // Marks an active call as awaiting cleanup, moving it from the active calls_
  // list to the to_cleanup_ list.
  //
  // This method is protected so it can be exposed in tests.
  void CloseCallAndMarkForCleanup(Call& call, Status error)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    call.CloseAndMarkForCleanupFromEndpoint(error);
    calls_.remove(call);
    to_cleanup_.push_front(call);
  }

  // Iterator version of CloseCallAndMarkForCleanup. Returns the iterator to the
  // item after the closed call.
  IntrusiveForwardList<Call>::iterator CloseCallAndMarkForCleanup(
      IntrusiveForwardList<Call>::iterator before_call,
      IntrusiveForwardList<Call>::iterator call_iterator,
      Status error) PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    Call& call = *call_iterator;
    call.CloseAndMarkForCleanupFromEndpoint(error);
    auto next = calls_.erase_after(before_call);
    to_cleanup_.push_front(call);
    return next;
  }

 private:
  // Give Call access to the register/unregister functions.
  friend class Call;

  enum class AbortIdType : bool { kChannel, kService };

  // Aborts calls for a particular channel or service and enqueues them for
  // cleanup. AbortCalls() must be followed by a call to CleanUpCalls().
  void AbortCalls(AbortIdType type, uint32_t id)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());

  // Returns an ID that can be assigned to a new call.
  uint32_t NewCallId() PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    // Call IDs are varint encoded. Limit the varint size to 2 bytes (14 usable
    // bits).
    constexpr uint32_t kMaxCallId = 1 << 14;
    auto call_id = next_call_id_;
    next_call_id_ = (next_call_id_ + 1) % kMaxCallId;

    // Skip call_id `0` to avoid confusion with legacy servers which use
    // call_id `0` as `kOpenCallId` or which do not provide call_id at all.
    if (next_call_id_ == 0) {
      next_call_id_ = 1;
    }

    return call_id;
  }

  // Adds a call to the internal call registry. If a matching call already
  // exists, it is cancelled. CleanUpCalls() must be called after RegisterCall.
  void RegisterCall(Call& call) PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());

  // Registers a call that is known to be unique. The calls list is NOT checked
  // for existing calls.
  void RegisterUniqueCall(Call& call) PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    calls_.push_front(call);
  }

  void CleanUpCall(Call& call) PW_UNLOCK_FUNCTION(rpc_lock()) {
    const bool removed_call_to_cleanup = to_cleanup_.remove(call);
    PW_DASSERT(removed_call_to_cleanup);  // Should have been awaiting cleanup
    call.CleanUpFromEndpoint();
  }

  void CancelCleanup(Call& call) PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    const bool removed_call_to_cleanup = to_cleanup_.remove(call);
    PW_DASSERT(removed_call_to_cleanup);  // Should have been awaiting cleanup
  }

  // Removes the provided call from the call registry.
  void UnregisterCall(const Call& call)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    bool closed_call_was_in_list = calls_.remove(call);
    PW_DASSERT(closed_call_was_in_list);
  }

  std::tuple<IntrusiveForwardList<Call>::iterator,
             IntrusiveForwardList<Call>::iterator>
  FindIteratorsForCall(uint32_t channel_id,
                       uint32_t service_id,
                       uint32_t method_id,
                       uint32_t call_id)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());

  std::tuple<IntrusiveForwardList<Call>::iterator,
             IntrusiveForwardList<Call>::iterator>
  FindIteratorsForCall(const Call& call)
      PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock()) {
    return FindIteratorsForCall(call.channel_id_locked(),
                                call.service_id(),
                                call.method_id(),
                                call.id());
  }

  // Silently closes all calls. Called by the destructor. This is a
  // non-destructor function so that Clang's lock safety analysis applies.
  //
  // Endpoints are not deleted in normal RPC use, and especially would not be
  // deleted before the calls that use them. To handle this unusual case, all
  // calls are closed without invoking on_error callbacks. If cleanup tasks are
  // required, users should perform them before deleting the Endpoint. Cleanup
  // could be done individually for each call or by closing channels with
  // CloseChannel.
  void RemoveAllCalls() PW_LOCKS_EXCLUDED(rpc_lock());

  ChannelList channels_ PW_GUARDED_BY(rpc_lock());

  // List of all active calls associated with this endpoint. Calls are added to
  // this list when they start and removed from it when they finish.
  IntrusiveForwardList<Call> calls_ PW_GUARDED_BY(rpc_lock());

  // List of all inactive calls that need to have their on_error callbacks
  // called. Calling on_error requires releasing the RPC lock, so calls are
  // added to this list in situations where releasing the mutex could be
  // problematic.
  IntrusiveForwardList<Call> to_cleanup_ PW_GUARDED_BY(rpc_lock());

  // Skip call_id `0` to avoid confusion with legacy servers which use
  // call_id `0` as `kOpenCallId` or which do not provide call_id at all.
  uint32_t next_call_id_ PW_GUARDED_BY(rpc_lock()) = 1;
};

// An `Endpoint` indicating that `rpc_lock()` is held.
//
// This is used as a constructor argument to supplement
// `PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock())`. Current compilers do not enforce
// lock annotations on constructors; no warnings or errors are produced when
// calling an annotated constructor without holding `rpc_lock()`.
class LockedEndpoint : public Endpoint {
 public:
  friend class Endpoint;
  // No public constructor: this is created only via the `ClaimLocked` method on
  // `Endpoint`.
  constexpr LockedEndpoint() = delete;
};

inline LockedEndpoint& Endpoint::ClaimLocked() {
  return *static_cast<LockedEndpoint*>(this);
}

}  // namespace pw::rpc::internal
