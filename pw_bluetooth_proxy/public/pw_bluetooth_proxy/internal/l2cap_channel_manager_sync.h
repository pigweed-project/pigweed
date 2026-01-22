// Copyright 2025 The Pigweed Authors
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

#include "pw_bluetooth_proxy/config.h"

#if PW_BLUETOOTH_PROXY_ASYNC == 0

#include <array>
#include <cstdint>

#include "pw_allocator/allocator.h"
#include "pw_allocator/best_fit.h"
#include "pw_allocator/synchronized_allocator.h"
#include "pw_bluetooth_proxy/internal/l2cap_channel.h"
#include "pw_containers/intrusive_map.h"
#include "pw_sync/lock_annotations.h"
#include "pw_sync/mutex.h"

namespace pw::bluetooth::proxy {

class L2capChannelManager;

namespace internal {

class L2capChannelManagerImpl;

/// Implementation detail class for L2capChannelManager.
///
/// This type is used when `PW_BLUETOOTH_PROXY_ASYNC` is false; that is, when
/// the proxy is using a purely synchronous execution model using one or more
/// threads and using mutexes to protect shared state from concurrent
/// modification.
class L2capChannelManagerImpl {
 public:
  using L2capChannelMap = IntrusiveMap<uint32_t, L2capChannel::Handle>;
  using L2capChannelIterator = L2capChannelMap::iterator;

  L2capChannelManagerImpl(L2capChannelManager& manager, Allocator& allocator);
  ~L2capChannelManagerImpl();

  constexpr L2capChannelManager& manager() { return manager_; }

  constexpr Allocator& allocator() { return allocator_; }

  /// Update iterators on channel registration.
  void OnRegister() PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex_);

  /// Update iterators on channel deregistration.
  void OnDeregister(const L2capChannel& channel)
      PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex_);

  /// Update iterators on channel deletion.
  void OnDeletion() PW_EXCLUSIVE_LOCKS_REQUIRED(channels_mutex_);

  // See L2capChannelManager::ReportNewTxPacketsOrCredits
  void ReportNewTxPacketsOrCredits() PW_LOCKS_EXCLUDED(drain_status_mutex_);

  // See L2capChannelManager::DrainChannelQueuesIfNewTx
  void DrainChannelQueuesIfNewTx() PW_LOCKS_EXCLUDED(channels_mutex_,
                                                     drain_status_mutex_,
                                                     L2capChannelImpl::mutex());

 private:
  friend class pw::bluetooth::proxy::L2capChannelManager;

  static constexpr sync::Mutex& channels_mutex()
      PW_LOCK_RETURNED(channels_mutex_) {
    return channels_mutex_;
  }

  // Enforce mutual exclusion of all operations on channels.
  // This is ACQUIRED_BEFORE AclDataChannel::credit_mutex_.
  static sync::Mutex channels_mutex_;

  L2capChannelManager& manager_;

  Allocator& allocator_;

  // Iterator to "least recently drained" channel.
  L2capChannelIterator lrd_channel_ PW_GUARDED_BY(channels_mutex_);

  // Iterator to final channel to be visited in ongoing round robin.
  L2capChannelIterator round_robin_terminus_ PW_GUARDED_BY(channels_mutex_);

  // Guard access to tx related state flags.
  sync::Mutex drain_status_mutex_ PW_ACQUIRED_AFTER(channels_mutex_);

  // True if new tx packets are queued or new tx resources have become
  // available.
  bool drain_needed_ PW_GUARDED_BY(drain_status_mutex_) = false;

  // True if a drain is running.
  bool drain_running_ PW_GUARDED_BY(drain_status_mutex_) = false;
};

}  // namespace internal
}  // namespace pw::bluetooth::proxy

#endif  // PW_BLUETOOTH_PROXY_ASYNC
