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

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>

#include "pw_allocator/synchronized_allocator.h"
#include "pw_allocator/testing.h"
#include "pw_assert/assert.h"
#include "pw_multibuf/config.h"
#include "pw_multibuf/v1/simple_allocator.h"

namespace pw::multibuf::v1::test {

/// @submodule{pw_multibuf,v1_test}

/// Simple, self-contained `pw::multibuf::MultiBufAllocator` for test use.
template <size_t kDataSizeBytes = 1024, size_t kMetaSizeBytes = kDataSizeBytes>
class PW_MULTIBUF_DEPRECATED SimpleAllocatorForTest : public SimpleAllocator {
 public:
  /// Size of the data area.
  static constexpr size_t data_size_bytes() { return kDataSizeBytes; }

  SimpleAllocatorForTest()
      : SimpleAllocator(data_area_, meta_alloc_), meta_alloc_(alloc_) {}

  /// Allocates a `MultiBuf` and initializes its contents to the provided data.
  MultiBuf BufWith(std::initializer_list<std::byte> data) {
    std::optional<MultiBuf> buffer = Allocate(data.size());
    PW_ASSERT(buffer.has_value());
    std::copy(data.begin(), data.end(), buffer->begin());
    return *std::move(buffer);
  }

 private:
  std::byte data_area_[kDataSizeBytes];
  allocator::test::AllocatorForTest<kMetaSizeBytes> alloc_;
  allocator::SynchronizedAllocator<sync::Mutex> meta_alloc_;
};

SimpleAllocatorForTest() -> SimpleAllocatorForTest<>;

/// @}

}  // namespace pw::multibuf::v1::test
