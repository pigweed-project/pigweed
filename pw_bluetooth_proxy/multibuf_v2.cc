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

#if PW_MULTIBUF_VERSION == 2

namespace pw::bluetooth::proxy {

std::optional<FlatMultiBufInstance> MultiBufAdapter::Create(
    MultiBufAllocator& allocator, size_t payload) {
  FlatMultiBufInstance mbuf(allocator.metadata_alloc);
  if (payload == 0) {
    return mbuf;
  }
  auto chunk = allocator.data_alloc.MakeUnique<std::byte[]>(payload);
  if (chunk == nullptr || !mbuf->TryReserveForPushBack()) {
    return std::nullopt;
  }
  mbuf->PushBack(std::move(chunk));
  return mbuf;
}

std::optional<MultiBufInstance> MultiBufAdapter::Create(
    MultiBufAllocator& allocator, size_t header, size_t payload) {
  MultiBufInstance mbuf(allocator.metadata_alloc);
  if (header + payload == 0) {
    return mbuf;
  }
  auto chunk = allocator.data_alloc.MakeUnique<std::byte[]>(header + payload);
  if (chunk == nullptr || !mbuf->TryReserveLayers(2)) {
    return std::nullopt;
  }
  mbuf->PushBack(std::move(chunk));
  PW_CHECK(mbuf->AddLayer(header));
  return mbuf;
}

void MultiBufAdapter::Copy(ByteSpan dst,
                           const FlatConstMultiBuf& src,
                           size_t src_offset) {
  size_t copied = src.CopyTo(dst, src_offset);
  PW_CHECK_UINT_EQ(copied, dst.size());
}

size_t MultiBufAdapter::Copy(FlatMultiBuf& dst,
                             size_t dst_offset,
                             ConstByteSpan src) {
  return dst.CopyFrom(src, dst_offset);
}

MultiBuf& MultiBufAdapter::Unwrap(MultiBufInstance& wrapped) {
  return *wrapped;
}

FlatMultiBuf& MultiBufAdapter::Unwrap(FlatMultiBufInstance& wrapped) {
  return *wrapped;
}

FlatConstMultiBuf& MultiBufAdapter::Unwrap(FlatConstMultiBufInstance& wrapped) {
  return *wrapped;
}

void MultiBufAdapter::Claim(MultiBuf& mbuf, size_t) { mbuf.PopLayer(); }

span<uint8_t> MultiBufAdapter::AsSpan(MultiBuf& mbuf) {
  return span_cast<uint8_t>(*mbuf.Chunks().begin());
}

span<const uint8_t> MultiBufAdapter::AsSpan(const MultiBuf& mbuf) {
  return span_cast<const uint8_t>(*mbuf.ConstChunks().begin());
}

span<uint8_t> MultiBufAdapter::AsSpan(FlatMultiBuf& mbuf) {
  return span_cast<uint8_t>(*mbuf.Chunks().begin());
}

span<const uint8_t> MultiBufAdapter::AsSpan(const FlatMultiBuf& mbuf) {
  return span_cast<const uint8_t>(*mbuf.ConstChunks().begin());
}

span<const uint8_t> MultiBufAdapter::AsSpan(const FlatConstMultiBuf& mbuf) {
  return span_cast<const uint8_t>(*mbuf.ConstChunks().begin());
}

}  // namespace pw::bluetooth::proxy

#endif  // PW_MULTIBUF_VERSION
