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

#include "pw_preprocessor/util.h"

#ifndef __cplusplus

#include <stddef.h>
#include <stdint.h>

#else  // __cplusplus

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_containers/algorithm.h"
#include "pw_containers/internal/var_len_entry_iterator.h"
#include "pw_containers/internal/wrap.h"
#include "pw_span/span.h"
#include "pw_varint/varint.h"

#endif  // __cplusplus

// Forward declarations of C structs.
typedef struct pw_InlineVarLenEntryQueue_Entry pw_InlineVarLenEntryQueue_Entry;
typedef struct pw_InlineVarLenEntryQueue_ConstEntry
    pw_InlineVarLenEntryQueue_ConstEntry;

#ifdef __cplusplus
namespace pw::containers::internal {

/// Returns the total encoded size of an entry.
///
/// It is an error to call this method if `&bytes[offset]` does not point to the
/// start of entry.
template <typename T>
constexpr uint32_t ReadVarLenEntryEncodedSize(span<T> bytes, size_t offset);

/// Returns the size of an entry, including both prefix and data size.
///
/// It is an error to call this method if `&bytes[offset]` does not point to the
/// start of entry.
template <typename T>
constexpr std::pair<uint32_t, uint32_t> ReadVarLenEntrySize(span<T> bytes,
                                                            size_t offset);

/// Refers to an entry in-place in the queue. Entries may be discontiguous.
template <typename T>
class VarLenEntry {
 public:
  using element_type = T;
  using value_type = std::remove_cv_t<T>;
  using size_type = uint32_t;
  using pointer = T*;
  using const_pointer = const T*;
  using reference = T&;
  using const_reference = const T&;

  /// Iterator for the bytes in an Entry. Entries may be discontiguous, so a
  /// pointer cannot serve as an iterator.
  using iterator = VarLenEntryIterator<T>;
  using const_iterator = VarLenEntryIterator<const T>;

  constexpr VarLenEntry() = default;
  constexpr VarLenEntry(const VarLenEntry&) = default;
  constexpr VarLenEntry& operator=(const VarLenEntry&) = default;

  constexpr operator VarLenEntry<const T>() const {
    return VarLenEntry<const T>(data1_, data2_);
  }

  [[nodiscard]] constexpr bool empty() const {
    return data1_.empty() && data2_.empty();
  }

  constexpr size_t size() const { return data1_.size() + data2_.size(); }

  /// Returns a reference to the byte at the specified index.
  ///
  /// @param index  The index of the byte to return.
  constexpr reference at(size_t index) const;

  constexpr reference operator[](size_t index) const { return at(index); }
  constexpr reference front() const { return at(0); }
  constexpr reference back() const { return at(size() - 1); }

  /// Entries may be stored in up to two segments, so this returns spans
  /// referring to both portions of the entry. If the entry is contiguous, the
  /// second span is empty.
  constexpr std::pair<span<T>, span<T>> contiguous_data() const {
    return std::make_pair(data1_, data2_);
  }

  /// Copies the contents of the entry to the provided buffer. The entry may be
  /// split into two regions; this serializes it into one buffer.
  ///
  /// Truncates the data if the buffer is smaller than the `dest` buffer.
  /// Returns the size of the entry; if this is larger than `dest.size()`, the
  /// data was truncated.
  ///
  /// @param dest   Buffer into which to copy the serialized entry
  /// @param count  Length of the buffer, in bytes.
  constexpr size_t copy(value_type* dest, size_t count) const;

  constexpr iterator begin() const { return iterator(*this, 0); }
  constexpr const_iterator cbegin() const { return const_iterator(*this, 0); }

  constexpr iterator end() const { return iterator(*this, size()); }
  constexpr const_iterator cend() const {
    return const_iterator(*this, size());
  }

  [[nodiscard]] friend constexpr bool operator==(const VarLenEntry& lhs,
                                                 const VarLenEntry& rhs) {
    return lhs.Equals(rhs);
  }
  [[nodiscard]] friend constexpr bool operator!=(const VarLenEntry& lhs,
                                                 const VarLenEntry& rhs) {
    return !(lhs == rhs);
  }

 private:
  template <typename>
  friend class VarLenEntry;

  friend class VarLenEntryC;

  template <typename>
  friend class VarLenEntryQueueIterator;

  constexpr VarLenEntry(span<T> data1, span<T> data2)
      : data1_(data1), data2_(data2) {}

  /// Objects of this type are equal if the refer to the same data.
  [[nodiscard]] constexpr bool Equals(const VarLenEntry& other) const;

  /// An entry in the queue. Entries may be stored in up to two segments.
  span<T> data1_;
  span<T> data2_;
};

////////////////////////////////////////////////////////////////////////////////
// Template method implementations.

template <typename T>
constexpr uint32_t ReadVarLenEntryEncodedSize(span<T> bytes, size_t offset) {
  std::pair<uint32_t, uint32_t> entry_size = ReadVarLenEntrySize(bytes, offset);
  return entry_size.first + entry_size.second;
}

template <typename T>
constexpr std::pair<uint32_t, uint32_t> ReadVarLenEntrySize(span<T> bytes,
                                                            size_t offset) {
  uint32_t prefix_size = 0;
  uint32_t data_size = 0;
  while (true) {
    PW_DASSERT(prefix_size < varint::kMaxVarint32SizeBytes);
    if (!varint::DecodeOneByte(
            std::byte(bytes[offset]), prefix_size++, &data_size)) {
      return std::make_pair(prefix_size, data_size);
    }
    IncrementWithWrap(offset, size_t(1), bytes.size());
  }
}

template <typename T>
constexpr size_t VarLenEntry<T>::copy(value_type* dest, size_t count) const {
  using difference_type = typename span<T>::difference_type;
  size_t size1 = std::min(count, data1_.size());
  auto end = data1_.begin() + static_cast<difference_type>(size1);
  pw::copy(data1_.begin(), end, dest);
  dest = &dest[size1];
  count -= size1;
  size_t size2 = std::min(count, data2_.size());
  if (size2 != 0) {
    end = data2_.begin() + static_cast<difference_type>(size2);
    pw::copy(data2_.begin(), end, dest);
  }
  return data1_.size() + data2_.size();
}

template <typename T>
constexpr T& VarLenEntry<T>::at(size_t index) const {
  if (index < data1_.size()) {
    return data1_[index];
  }
  index -= data1_.size();
  PW_ASSERT(index < data2_.size());
  return data2_[index];
}

template <typename T>
constexpr bool VarLenEntry<T>::Equals(const VarLenEntry& other) const {
  return data1_.data() == other.data1_.data() &&
         data1_.size() == other.data1_.size() &&
         data2_.data() == other.data2_.data() &&
         data2_.size() == other.data2_.size();
}

////////////////////////////////////////////////////////////////////////////////
// Helper class with methods to convert between the C++ object and C structs.
class VarLenEntryC {
 public:
  static VarLenEntry<std::byte> From(
      const pw_InlineVarLenEntryQueue_Entry& entry);
  static VarLenEntry<const std::byte> From(
      const pw_InlineVarLenEntryQueue_ConstEntry& entry);
  static pw_InlineVarLenEntryQueue_Entry To(
      const VarLenEntry<std::byte>& cxx_entry);
  static pw_InlineVarLenEntryQueue_ConstEntry To(
      const VarLenEntry<const std::byte>& cxx_entry);
};

}  // namespace pw::containers::internal
#endif  // __cplusplus

////////////////////////////////////////////////////////////////////////////////
// C API.

PW_EXTERN_C_START

struct pw_InlineVarLenEntryQueue_Entry {
  uint8_t* data_1;
  uint32_t size_1;
  uint8_t* data_2;
  uint32_t size_2;
};

struct pw_InlineVarLenEntryQueue_ConstEntry {
  const uint8_t* data_1;
  uint32_t size_1;
  const uint8_t* data_2;
  uint32_t size_2;
};

/// @copydoc pw::containers::internal::VarLenEntry::copy
uint32_t pw_InlineVarLenEntryQueue_Entry_Copy(
    const pw_InlineVarLenEntryQueue_Entry* entry, void* dest, uint32_t count);
uint32_t pw_InlineVarLenEntryQueue_ConstEntry_Copy(
    const pw_InlineVarLenEntryQueue_ConstEntry* entry,
    void* dest,
    uint32_t count);

/// @copydoc pw::containers::internal::VarLenEntry::at
uint8_t pw_InlineVarLenEntryQueue_Entry_At(
    const pw_InlineVarLenEntryQueue_Entry* entry, size_t index);
uint8_t pw_InlineVarLenEntryQueue_ConstEntry_At(
    const pw_InlineVarLenEntryQueue_ConstEntry* entry, size_t index);

PW_EXTERN_C_END
