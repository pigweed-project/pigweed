// Copyright 2024 The Pigweed Authors
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

// TODO: b/385440717 - Delete this backend when generic thread creation is
// implemented for all pw_thread backends.

#include <cstddef>

namespace pw::thread::backend {

// Signal to pw_thread that the thread creation backend is not implemented.
#define _PW_THREAD_GENERIC_CREATION_UNSUPPORTED

class NativeContext {
 public:
  // The generic thread creation backend has not been implemented.
  template <bool kGenericThreadCreationIsSupported = false>
  constexpr NativeContext() {
    static_assert(
        kGenericThreadCreationIsSupported,
        "Generic thread creation is not yet supported for this pw_thread "
        "backend. Implement the generic thread creation backend to use "
        "pw::ThreadContext or pw::ThreadContextFor. See "
        "pigweed.dev/pw_thread.");
  }
};

template <size_t kStackSizeBytes>
using NativeContextWithStack = NativeContext;

}  // namespace pw::thread::backend
