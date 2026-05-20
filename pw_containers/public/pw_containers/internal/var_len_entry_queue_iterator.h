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

#include "pw_containers/internal/var_len_entry.h"
#include "pw_preprocessor/util.h"

#ifndef __cplusplus

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#else  // __cplusplus

#include <cstddef>
#include <cstdint>
#include <iterator>

#include "pw_containers/internal/wrap.h"
#include "pw_span/span.h"

#endif  // __cplusplus

// Forward declarations of C structs.
typedef struct pw_InlineVarLenEntryQueue_Iterator
    pw_InlineVarLenEntryQueue_Iterator;
typedef struct pw_InlineVarLenEntryQueue_ConstIterator
    pw_InlineVarLenEntryQueue_ConstIterator;

#ifdef __cplusplus
namespace pw::containers::internal {

template <typename T>
class VarLenEntryQueueIterator {
 public:
  using difference_type = std::ptrdiff_t;
  using value_type = VarLenEntry<T>;
  using size_type = uint32_t;
  using pointer = const value_type*;
  using reference = const value_type&;
  using iterator_category = std::forward_iterator_tag;

  static_assert(std::is_same_v<size_type, typename VarLenEntry<T>::size_type>);

  constexpr VarLenEntryQueueIterator() {}

  constexpr VarLenEntryQueueIterator(const VarLenEntryQueueIterator&) = default;
  constexpr VarLenEntryQueueIterator& operator=(
      const VarLenEntryQueueIterator&) = default;

  /// Advances the iterator to the next entry in the queue.
  ///
  /// @returns A reference to this iterator after advancing.
  constexpr VarLenEntryQueueIterator& operator++() {
    size_type entry_size = ReadVarLenEntryEncodedSize(data_, offset_);
    IncrementWithWrap(
        offset_, entry_size, static_cast<size_type>(data_.size()));
    entry_ = value_type{};  // mark the entry as unloaded
    return *this;
  }

  constexpr VarLenEntryQueueIterator operator++(int) {
    VarLenEntryQueueIterator previous_value(*this);
    operator++();
    return previous_value;
  }

  /// Returns the entry pointed to by the iterator.
  ///
  /// @returns The entry.
  constexpr reference operator*() const {
    LoadEntry();
    return entry_;
  }
  constexpr pointer operator->() const {
    LoadEntry();
    return &entry_;
  }

  /// Compares two iterators for equality.
  ///
  /// @returns true if they refer to the same position in the same queue.
  [[nodiscard]] friend constexpr bool operator==(
      const VarLenEntryQueueIterator& lhs,
      const VarLenEntryQueueIterator& rhs) {
    return lhs.data_.data() == rhs.data_.data() &&
           lhs.data_.size() == rhs.data_.size() && lhs.offset_ == rhs.offset_;
  }
  [[nodiscard]] friend constexpr bool operator!=(
      const VarLenEntryQueueIterator& lhs,
      const VarLenEntryQueueIterator& rhs) {
    return !(lhs == rhs);
  }

 private:
  using SpanType = span<T>;

  template <typename, typename>
  friend class GenericVarLenEntryQueue;

  friend class VarLenEntryQueueIteratorC;

  constexpr VarLenEntryQueueIterator(SpanType data, size_t offset)
      : data_(data), offset_(static_cast<size_type>(offset)) {}

  constexpr void LoadEntry() const;

  SpanType data_;
  size_type offset_ = 0;
  mutable value_type entry_;
};

////////////////////////////////////////////////////////////////////////////////
// Template method implementations.

template <typename T>
constexpr void VarLenEntryQueueIterator<T>::LoadEntry() const {
  if (!entry_.empty()) {
    return;
  }
  auto [prefix_size, data_size] = ReadVarLenEntrySize(data_, offset_);
  size_type offset = offset_;
  IncrementWithWrap(offset, prefix_size, static_cast<size_type>(data_.size()));
  size_t first_chunk = data_.size() - offset;
  if (data_size <= first_chunk) {
    entry_ = VarLenEntry<T>(data_.subspan(offset, data_size), SpanType());
  } else {
    entry_ = VarLenEntry<T>(data_.subspan(offset, first_chunk),
                            data_.subspan(0, data_size - first_chunk));
  }
}

// Helper class with methods to convert between the C++ object and C structs.
class VarLenEntryQueueIteratorC {
 public:
  static VarLenEntryQueueIterator<std::byte> From(
      const pw_InlineVarLenEntryQueue_Iterator& iterator);
  static VarLenEntryQueueIterator<const std::byte> From(
      const pw_InlineVarLenEntryQueue_ConstIterator& iterator);
  static pw_InlineVarLenEntryQueue_Iterator To(
      const VarLenEntryQueueIterator<std::byte>& cxx_iterator);
  static pw_InlineVarLenEntryQueue_ConstIterator To(
      const VarLenEntryQueueIterator<const std::byte>& cxx_iterator);
};

}  // namespace pw::containers::internal
#endif  // __cplusplus

////////////////////////////////////////////////////////////////////////////////
// C API.

PW_EXTERN_C_START

struct pw_InlineVarLenEntryQueue_Iterator {
  uint8_t* internal_data;
  uint32_t internal_size;
  uint32_t internal_offset;
};

struct pw_InlineVarLenEntryQueue_ConstIterator {
  const uint8_t* internal_data;
  uint32_t internal_size;
  uint32_t internal_offset;
};

/// @copydoc pw::containers::internal::VarLenEntryQueueIterator::operator++
void pw_InlineVarLenEntryQueue_Iterator_Advance(
    pw_InlineVarLenEntryQueue_Iterator* iterator);
void pw_InlineVarLenEntryQueue_ConstIterator_Advance(
    pw_InlineVarLenEntryQueue_ConstIterator* iterator);

/// @copydoc pw::containers::internal::VarLenEntryQueueIterator::operator==
bool pw_InlineVarLenEntryQueue_Iterator_Equal(
    const pw_InlineVarLenEntryQueue_Iterator* lhs,
    const pw_InlineVarLenEntryQueue_Iterator* rhs);
bool pw_InlineVarLenEntryQueue_ConstIterator_Equal(
    const pw_InlineVarLenEntryQueue_ConstIterator* lhs,
    const pw_InlineVarLenEntryQueue_ConstIterator* rhs);

/// @copydoc pw::containers::internal::VarLenEntryQueueIterator::operator*
pw_InlineVarLenEntryQueue_Entry pw_InlineVarLenEntryQueue_GetEntry(
    const pw_InlineVarLenEntryQueue_Iterator* iterator);
pw_InlineVarLenEntryQueue_ConstEntry pw_InlineVarLenEntryQueue_GetConstEntry(
    const pw_InlineVarLenEntryQueue_ConstIterator* iterator);

PW_EXTERN_C_END
