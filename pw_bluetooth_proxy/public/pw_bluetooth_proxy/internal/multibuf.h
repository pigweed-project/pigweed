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

#include <cstddef>
#include <cstdint>
#include <optional>

#include "pw_bluetooth_proxy/config.h"
#include "pw_bytes/span.h"
#include "pw_multibuf/allocator.h"
#include "pw_multibuf/multibuf.h"
#include "pw_span/span.h"

#if PW_MULTIBUF_VERSION == 1

namespace pw::bluetooth::proxy {

using FlatConstMultiBuf = multibuf::v1::MultiBuf;
using FlatConstMultiBufInstance = multibuf::v1::MultiBuf;
using FlatMultiBuf = multibuf::v1::MultiBuf;
using FlatMultiBufInstance = multibuf::v1::MultiBuf;
using MultiBuf = multibuf::v1::MultiBuf;
using MultiBufInstance = multibuf::v1::MultiBuf;
using MultiBufAllocator = multibuf::v1::MultiBufAllocator;

}  // namespace pw::bluetooth::proxy

#elif PW_MULTIBUF_VERSION == 2

namespace pw::bluetooth::proxy {

using FlatConstMultiBufInstance = multibuf::v2::FlatConstMultiBuf::Instance;
using FlatMultiBufInstance = multibuf::v2::FlatMultiBuf::Instance;
using MultiBuf = multibuf::v2::MultiBuf;
using MultiBufInstance = multibuf::v2::MultiBuf::Instance;

struct MultiBufAllocator {
  Allocator& data_alloc;
  Allocator& metadata_alloc;
};

}  // namespace pw::bluetooth::proxy

#else  // PW_MULTIBUF_VERSION

#error "Unsupported PW_MULTIBUF_VERSION"

#endif  // PW_MULTIBUF_VERSION

namespace pw::bluetooth::proxy {

/// A helper type that encapsulates the details of which version of MultiBuf is
/// in use, and provides a common abstraction to the rest of the proxy code.
struct MultiBufAdapter {
  /// Creates an unlayered MultiBuf and adds one chunk of `payload` bytes.
  static std::optional<FlatMultiBufInstance> Create(
      MultiBufAllocator& allocator, size_t payload = 0);

  /// Creates an layered MultiBuf and adds one chunk with some bytes reserved
  /// for a `header`, and others available for a `payload`.
  static std::optional<MultiBufInstance> Create(MultiBufAllocator& allocator,
                                                size_t header,
                                                size_t payload);

  /// Copies up to `src_length` bytes starting at `src_offset` from `src` to
  /// `dst`. May copy less if `dst` is shorter than `src_length`, but `src` must
  /// have enough data to fill `dst`.
  template <typename EmbossView>
  static void Copy(EmbossView dst,
                   const FlatConstMultiBuf& src,
                   size_t src_offset = 0,
                   size_t src_length = dynamic_extent) {
    span<uint8_t> backing_storage(dst.BackingStorage().data(),
                                  dst.SizeInBytes());
    if (src_length != dynamic_extent && src_length != backing_storage.size()) {
      PW_ASSERT(src_length < backing_storage.size());
      backing_storage = backing_storage.subspan(0, src_length);
    }
    MultiBufAdapter::Copy(as_writable_bytes(backing_storage), src, src_offset);
  }

  /// Copies bytes starting at `src_offset` from `src` to `dst`. `src` must have
  /// enough data to fill `dst`.
  static void Copy(ByteSpan dst,
                   const FlatConstMultiBuf& src,
                   size_t src_offset = 0);

  /// Copies bytes from `src` to `dst_offset` within `dst`. `dst` must have
  /// enough room to hold `src`.
  static size_t Copy(FlatMultiBuf& dst, size_t dst_offset, ConstByteSpan src);

  /// @name Unwrap
  /// Returns a MultiBuf reference from a MultiBuf instance.
  /// @{
  static MultiBuf& Unwrap(MultiBufInstance& wrapped);
#if PW_MULTIBUF_VERSION == 2
  static FlatMultiBuf& Unwrap(FlatMultiBufInstance& wrapped);
  static FlatConstMultiBuf& Unwrap(FlatConstMultiBufInstance& wrapped);
#endif  // PW_MULTIBUF_VERSION
  /// @}

  /// Takes back `header_size` bytes previously reserved for a header and makes
  /// them available again. The MultiBuf must have `header_size` bytes reserved.
  static void Claim(MultiBuf& mbuf, size_t header_size);

  /// @name AsSpan
  /// Returns a view to the memory backing the MultiBuf. The MultiBuf must
  /// represent a contiguous span of memory.
  /// @{
  static span<uint8_t> AsSpan(MultiBuf& mbuf);
  static span<const uint8_t> AsSpan(const MultiBuf& mbuf);
#if PW_MULTIBUF_VERSION == 2
  static span<uint8_t> AsSpan(FlatMultiBuf& mbuf);
  static span<const uint8_t> AsSpan(const FlatMultiBuf& mbuf);
  static span<const uint8_t> AsSpan(const FlatConstMultiBuf& mbuf);
#endif  // PW_MULTIBUF_VERSION
  /// @}
};

}  // namespace pw::bluetooth::proxy
