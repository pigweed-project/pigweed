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

#include "pw_assert/check.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_span/cast.h"

#if PW_MULTIBUF_VERSION == 1

namespace pw::bluetooth::proxy {

std::optional<FlatMultiBufInstance> MultiBufAdapter::Create(
    MultiBufAllocator& allocator, size_t payload) {
  return allocator.AllocateContiguous(payload);
}

std::optional<MultiBufInstance> MultiBufAdapter::Create(
    MultiBufAllocator& allocator, size_t header, size_t payload) {
  auto maybe_mbuf = MultiBufAdapter::Create(allocator, header + payload);
  if (maybe_mbuf.has_value()) {
    maybe_mbuf.value().DiscardPrefix(header);
  }
  return maybe_mbuf;
}

void MultiBufAdapter::Copy(ByteSpan dst,
                           const FlatConstMultiBuf& src,
                           size_t src_offset) {
  auto result = src.CopyTo(dst, src_offset);
  PW_CHECK_UINT_EQ(result.size(), dst.size());
}

size_t MultiBufAdapter::Copy(FlatMultiBuf& dst,
                             size_t dst_offset,
                             ConstByteSpan src) {
  return dst.CopyFrom(src, dst_offset).size();
}

MultiBuf& MultiBufAdapter::Unwrap(MultiBufInstance& wrapped) { return wrapped; }

void MultiBufAdapter::Claim(MultiBuf& mbuf, size_t header_size) {
  PW_CHECK(mbuf.ClaimPrefix(header_size));
}

span<uint8_t> MultiBufAdapter::AsSpan(MultiBuf& mbuf) {
  return span_cast<uint8_t>(mbuf.ContiguousSpan().value());
}

span<const uint8_t> MultiBufAdapter::AsSpan(const MultiBuf& mbuf) {
  return span_cast<const uint8_t>(mbuf.ContiguousSpan().value());
}

}  // namespace pw::bluetooth::proxy

#endif  // PW_MULTIBUF_VERSION
