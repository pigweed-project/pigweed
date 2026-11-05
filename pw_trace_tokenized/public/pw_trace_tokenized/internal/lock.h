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

#include "pw_trace_tokenized_backend/lock_native.h"

namespace pw::trace::internal {

class PW_LOCKABLE("pw::trace::internal::Lock") Lock {
 public:
  void lock() PW_EXCLUSIVE_LOCK_FUNCTION() { native_.lock(); }
  void unlock() PW_UNLOCK_FUNCTION() { native_.unlock(); }

  [[nodiscard]] bool try_lock() PW_EXCLUSIVE_TRYLOCK_FUNCTION(true) {
    return native_.try_lock();
  }

 private:
  backend::NativeLock native_;
};

}  // namespace pw::trace::internal
