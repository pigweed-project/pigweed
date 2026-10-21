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

#include "pw_containers/internal/generic_var_len_entry_queue.h"  // IWYU pragma: export
#include "pw_containers/internal/raw_storage.h"  // IWYU pragma: export
#include "pw_span/span.h"

namespace pw {

/// Queue of variable-length entries.
///
/// This queue holds entries that are variable-length byte sequences. It is
/// implemented as a ring (circular) buffer and supports operations to append
/// entries and overwrite if necessary. Entries may be zero bytes up to the
/// maximum size supported by the queue.
///
/// This type is similar to `BasicInlineVarLenEntryQueue`, with two notable
/// exceptions:
///
///   * The memory used to back the queue is referenced but not owned by the
///     object. The span must be valid for the lifetime of this object.
///   * The metadata is not stored inline within the backing memory. This object
///     has fields to hold the metadata.
///
/// @tparam   T   Element type of the queue entries.
template <typename T>
class BasicVarLenEntryQueue
    : public containers::internal::
          GenericVarLenEntryQueue<BasicVarLenEntryQueue<T>, T> {
 public:
  constexpr explicit BasicVarLenEntryQueue(span<T> buffer) : buffer_(buffer) {}

 private:
  template <typename, typename>
  friend class containers::internal::GenericVarLenEntryQueue;

  constexpr size_t buffer_size() const { return buffer_.size(); }

  constexpr size_t head() const { return head_; }
  constexpr void set_head(size_t head) { head_ = head; }

  constexpr size_t tail() const { return tail_; }
  constexpr void set_tail(size_t tail) { tail_ = tail; }

  constexpr span<T> buffer() { return buffer_; }
  constexpr span<const T> buffer() const { return buffer_; }

  span<T> buffer_;
  size_t head_ = 0;
  size_t tail_ = 0;
};

/// Variable-length entry queue that uses ``std::byte`` for the byte type.
using VarLenEntryQueue = BasicVarLenEntryQueue<std::byte>;

}  // namespace pw
