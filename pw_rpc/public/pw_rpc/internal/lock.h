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

#include "pw_memory/no_destructor.h"
#include "pw_rpc/internal/config.h"
#include "pw_sync/lock_annotations.h"

#if PW_RPC_USE_GLOBAL_MUTEX

#include "pw_sync/mutex.h"  // nogncheck

#else

#include "pw_sync/no_lock.h"

#endif  // PW_RPC_USE_GLOBAL_MUTEX

namespace pw::rpc::internal {

#if PW_RPC_USE_GLOBAL_MUTEX

using RpcLock = sync::Mutex;

#else

using RpcLock = sync::NoLock;

#endif  // PW_RPC_USE_GLOBAL_MUTEX

inline RpcLock& rpc_lock() {
  static NoDestructor<RpcLock> lock;
  static_assert(PW_RPC_USE_GLOBAL_MUTEX != 0 ||
                std::is_trivially_destructible_v<decltype(lock)>);
  return *lock;
}

class PW_SCOPED_LOCKABLE RpcLockGuard {
 public:
  RpcLockGuard() PW_EXCLUSIVE_LOCK_FUNCTION(rpc_lock()) { rpc_lock().lock(); }

  ~RpcLockGuard() PW_UNLOCK_FUNCTION(rpc_lock()) { rpc_lock().unlock(); }
};

// Releases the RPC lock, yields, and reacquires it.
void YieldRpcLock() PW_EXCLUSIVE_LOCKS_REQUIRED(rpc_lock());

}  // namespace pw::rpc::internal
