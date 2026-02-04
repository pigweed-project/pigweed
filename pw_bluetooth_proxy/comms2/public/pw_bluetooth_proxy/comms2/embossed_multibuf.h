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
#include <type_traits>

#include "pw_bytes/span.h"
#include "pw_multibuf/v2/multibuf.h"
#include "pw_span/cast.h"

namespace pw::bluetooth::proxy {

using ::pw::multibuf::v2::MultiBuf;

class EmbossedMultiBuf {
 public:
  template <typename T, size_t kSize, int&... kExplicitGuard, typename Visitor>
  Status Visit(Visitor visitor, size_t offset = 0);

  std::byte& operator[](size_t index) { return (*multibuf_)[index]; }
  const std::byte& operator[](size_t index) const {
    return (*multibuf_)[index];
  }

  ConstByteSpan Get(ByteSpan copy, size_t offset = 0) const {
    return multibuf_->Get(copy, offset);
  }

  bool empty() const { return multibuf_->empty(); }

 protected:
  EmbossedMultiBuf(Allocator& allocator) : multibuf_(allocator) {}

  MultiBuf& multibuf() { return multibuf_; }
  const MultiBuf& multibuf() const { return multibuf_; }

  bool AddLayer(size_t offset, size_t length = dynamic_extent) {
    return multibuf_->AddLayer(offset, length);
  }

 private:
  MultiBuf::Instance multibuf_;
};

// Template method implementations.

template <typename T, size_t kSize, int&... kExplicitGuard, typename Visitor>
Status EmbossedMultiBuf::Visit(Visitor visitor, size_t offset) {
  using VisitorReturnType = std::invoke_result_t<Visitor, T>;
  static_assert(std::is_same_v<VisitorReturnType, Status> ||
                std::is_void_v<VisitorReturnType>);
  std::array<std::byte, kSize> tmp;
  return multibuf_->Visit(
      [v = std::move(visitor)](ConstByteSpan bytes) {
        if (bytes.size() < kSize) {
          return Status::Unavailable();
        }
        span<const uint8_t> payload = span_cast<const uint8_t>(bytes);
        auto t = T(payload.data(), payload.size());
        if (!t.Ok()) {
          return Status::DataLoss();
        }
        if constexpr (std::is_void_v<VisitorReturnType>) {
          v(t);
          return OkStatus();
        } else {
          return v(t);
        }
      },
      tmp,
      offset);
}

}  // namespace pw::bluetooth::proxy
