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

#ifdef __cplusplus

#include <array>
#include <cstdint>
#include <cstring>

#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_containers/internal/var_len_entry.h"
#include "pw_containers/internal/var_len_entry_queue_iterator.h"
#include "pw_containers/internal/wrap.h"
#include "pw_preprocessor/compiler.h"
#include "pw_span/cast.h"
#include "pw_span/span.h"
#include "pw_varint/varint.h"

#endif  // __cplusplus

/// Returns the minimum number of data bytes needed to hold an entry of
/// `max_size_bytes`.
#define PW_VAR_QUEUE_DATA_SIZE_BYTES(max_size_bytes)               \
  (PW_VARINT_ENCODED_SIZE_BYTES(max_size_bytes) + max_size_bytes + \
   1 /*end byte*/)

#ifdef __cplusplus
namespace pw::containers::internal {

// Forward declarations for friending.
class GenericVarLenEntryQueueBase;

template <typename Derived, typename T>
class GenericVarLenEntryQueue;

template <typename D1, typename T1, typename D2, typename T2>
constexpr void CopyVarLenEntriesImpl(const GenericVarLenEntryQueue<D1, T1>& src,
                                     GenericVarLenEntryQueue<D2, T2>& dst,
                                     bool overwrite);

template <typename D1, typename T1, typename D2, typename T2>
constexpr void MoveVarLenEntriesImpl(GenericVarLenEntryQueue<D1, T1>& src,
                                     GenericVarLenEntryQueue<D2, T2>& dst,
                                     bool overwrite);

/// Helper class that reduces code size by providing type-erased static methods
/// for the functionality of the templated GenericVarLenEntryQueue types.
class GenericVarLenEntryQueueBase {
 protected:
  struct Info {
    size_t num_entries;
    size_t total_data_size;
  };

  /// Returns the number, total data, and total size of the variable-length
  /// entries in the buffer. This is at most O(n) in the number of entries in
  /// the queue.
  static constexpr Info GetInfo(ConstByteSpan bytes, size_t head, size_t tail);

  /// Returns the number of bytes not being used to store entries.
  static constexpr size_t AvailableBytes(ConstByteSpan bytes,
                                         size_t head,
                                         size_t tail);

  /// Add an entry to the tail of the queue.
  static constexpr bool Push(ConstByteSpan data,
                             bool overwrite,
                             ByteSpan bytes,
                             size_t& head,
                             size_t& tail);

  /// Removes an entry from the head of the queue. Must not be empty.
  static constexpr size_t PopNonEmpty(ConstByteSpan bytes, size_t& head);

  /// Copies entries from another queue to this one.
  ///
  /// This will discard existing entries as needed to make room if allowed to
  /// overwrite.
  static constexpr void CopyEntries(ConstByteSpan src_bytes,
                                    size_t& src_head,
                                    size_t src_tail,
                                    ByteSpan dst_bytes,
                                    size_t& dst_head,
                                    size_t& dst_tail,
                                    bool overwrite);

  /// Copies data to the buffer, wrapping around the end if needed.
  static constexpr void CopyAndWrap(ConstByteSpan data,
                                    ByteSpan bytes,
                                    size_t& tail);

  /// Returns views of the contiguous raw storage backing this queue. If empty,
  /// both returned spans will be empty. If the data does not wrap, only the
  /// first returned span will be non-empty. If the data wraps around, both
  /// contiguous regions will be returned, from the head to the tail.
  static constexpr std::pair<ConstByteSpan, ConstByteSpan> ContiguousRawStorage(
      ConstByteSpan bytes, size_t head, size_t tail);

  /// Moves entries to be contiguous and start from the beginning of the buffer.
  static void Dering(ByteSpan bytes, size_t head) {
    std::rotate(
        bytes.begin(),
        bytes.begin() + static_cast<span<std::byte>::difference_type>(head),
        bytes.end());
  }

 private:
  template <typename D1, typename T1, typename D2, typename T2>
  friend constexpr void CopyVarLenEntriesImpl(
      const GenericVarLenEntryQueue<D1, T1>& src,
      GenericVarLenEntryQueue<D2, T2>& dst,
      bool overwrite);

  template <typename D1, typename T1, typename D2, typename T2>
  friend constexpr void MoveVarLenEntriesImpl(
      GenericVarLenEntryQueue<D1, T1>& src,
      GenericVarLenEntryQueue<D2, T2>& dst,
      bool overwrite);
};

/// A queue of variable length entries.
///
/// This type use CRTP to allow a derived class to specify access to the backing
/// storage and indicate where the queue head and tail are within it.
template <typename Derived, typename T>
class GenericVarLenEntryQueue : private GenericVarLenEntryQueueBase {
 private:
  using Base = GenericVarLenEntryQueueBase;

 public:
  using value_type = containers::internal::VarLenEntry<T>;
  using const_value_type = containers::internal::VarLenEntry<const T>;
  using iterator = VarLenEntryQueueIterator<T>;
  using const_iterator = VarLenEntryQueueIterator<const T>;

  /// @copydoc PW_VAR_QUEUE_DATA_SIZE_BYTES
  static constexpr size_t DataSizeBytes(size_t max_size_bytes) {
    return PW_VAR_QUEUE_DATA_SIZE_BYTES(max_size_bytes);
  }

  /// Returns an iterator to the start of the `queue`.
  constexpr iterator begin() { return iterator(GetBuffer(), GetHead()); }
  constexpr const_iterator begin() const {
    return const_iterator(GetBuffer(), GetHead());
  }
  constexpr const_iterator cbegin() const { return begin(); }

  /// Returns an iterator to the end of the `queue`.
  constexpr iterator end() { return iterator(GetBuffer(), GetTail()); }
  constexpr const_iterator end() const {
    return const_iterator(GetBuffer(), GetTail());
  }
  constexpr const_iterator cend() const { return end(); }

  /// Returns true if the queue is empty, false if it has at least one entry.
  [[nodiscard]] constexpr bool empty() const { return GetHead() == GetTail(); }

  /// Returns the number of variable-length entries in the queue. This is at
  /// most O(n) in the number of entries in the queue.
  constexpr size_t size() const {
    return Base::GetInfo(GetBytes(), GetHead(), GetTail()).num_entries;
  }

  /// Returns the maximum number of entries in the queue. This is only
  /// attainable if all entries are empty.
  constexpr size_t max_size() const { return GetBuffer().size() - 1; }

  /// Returns the combined size in bytes of all entries in the queue, excluding
  /// metadata. This is at most O(n) in the number of entries in the queue.
  constexpr size_t size_bytes() const {
    return Base::GetInfo(GetBytes(), GetHead(), GetTail()).total_data_size;
  }

  /// Returns the combined size in bytes of all entries in the queue, including
  /// metadata. This is at most O(n) in the number of entries in the queue.
  constexpr size_t encoded_size_bytes() const {
    return max_size() - Base::AvailableBytes(GetBytes(), GetHead(), GetTail());
  }

  /// Returns the maximum number of bytes that can be stored in the queue.
  /// This is largest possible value of `size_bytes()`, and the size of the
  /// largest single entry that can be stored in this queue. Attempting to store
  /// a larger entry is invalid and results in a crash.
  constexpr size_t max_size_bytes() const {
    return max_size() - varint::EncodedSize(max_size());
  }

  /// Returns the first entry in the queue.
  /// @pre The queue must NOT be empty (`empty()` is false).
  constexpr value_type front();
  constexpr const_value_type front() const;

  /// Appends an entry to the end of the queue.
  ///
  /// @pre The entry MUST NOT be larger than `max_size_bytes()`.
  /// @pre There must be sufficient space in the queue for this entry.
  constexpr void push(span<const T> data) { PW_ASSERT(try_push(data)); }

  /// Appends an entry to the end of the queue, but only if there is sufficient
  /// space for it.
  ///
  /// @returns true if the data was added to the queue; false if it did not fit
  /// @pre The entry MUST NOT be larger than `max_size_bytes()`.
  [[nodiscard]] constexpr bool try_push(span<const T> data) {
    return Push(data, /* overwrite: */ false);
  }

  /// Appends an entry to the end of the queue, removing entries with `Pop`
  /// as necessary to make room. Calling this function drops old entries to make
  /// room for new; call `try_push()` to drop new entries instead.
  ///
  /// @pre The entry MUST NOT be larger than `max_size_bytes()`.
  constexpr void push_overwrite(span<const T> data) {
    Push(data, /* overwrite: */ true);
  }

  /// Removes the first entry from queue.
  ///
  /// @pre The queue MUST have at least one entry.
  constexpr void pop();

  /// Empties the queue.
  constexpr void clear();

  /// @copydoc GenericVarLenEntryQueueBase::ContiguousRawStorage
  constexpr std::pair<span<const T>, span<const T>> contiguous_raw_storage()
      const;

  /// @copydoc GenericVarLenEntryQueueBase::Dering
  ///
  /// Returns a view to the contiguous data.
  span<const T> dering();

 protected:
  constexpr GenericVarLenEntryQueue() = default;

  // The queue metadata and data is part of the derived type, and can be
  // accessed and modified by downcasting to that type.
  constexpr size_t GetHead() const { return derived().head(); }
  constexpr void SetHead(size_t head) { return derived().set_head(head); }
  constexpr size_t GetTail() const { return derived().tail(); }
  constexpr void SetTail(size_t tail) { return derived().set_tail(tail); }
  constexpr span<T> GetBuffer() { return derived().buffer(); }
  constexpr span<const T> GetBuffer() const { return derived().buffer(); }

  /// @name GetBytes
  /// Returns the buffer as a span of bytes. Only constexpr if T is std::byte.
  /// @{
  constexpr ByteSpan GetBytes() { return AsWritableBytes(GetBuffer()); }
  constexpr ConstByteSpan GetBytes() const { return AsBytes(GetBuffer()); }
  /// @}

 private:
  template <typename D1, typename T1, typename D2, typename T2>
  friend constexpr void CopyVarLenEntriesImpl(
      const GenericVarLenEntryQueue<D1, T1>& src,
      GenericVarLenEntryQueue<D2, T2>& dst,
      bool overwrite);

  template <typename D1, typename T1, typename D2, typename T2>
  friend constexpr void MoveVarLenEntriesImpl(
      GenericVarLenEntryQueue<D1, T1>& src,
      GenericVarLenEntryQueue<D2, T2>& dst,
      bool overwrite);

  static_assert(std::is_integral_v<T> || std::is_same_v<T, std::byte>);
  static_assert(sizeof(T) == sizeof(std::byte));

  constexpr Derived& derived() { return *static_cast<Derived*>(this); }
  constexpr const Derived& derived() const {
    return *static_cast<const Derived*>(this);
  }

  /// Return the given span as a span of writable bytes. This is constexpr if
  /// `T` is `[const] std::byte`.
  static constexpr ByteSpan AsWritableBytes(span<T> data);

  /// Return the given span as a span of bytes. This is constexpr if T is
  /// `[const] std::byte`.
  static constexpr ConstByteSpan AsBytes(span<const T> data);

  /// Add `data` to the tail of queue. If allowed to `overwrite`, remove entries
  /// from the head to make space as needed. Returns whether enough space was
  /// found or made and data was added.
  constexpr bool Push(span<const T> data, bool overwrite);
};

/// Copies as many consecutive entries as will fit in the destination queue
/// from a source queue to another, and returns the number of bytes copied.
template <typename D1, typename T1, typename D2, typename T2>
constexpr void CopyVarLenEntries(const GenericVarLenEntryQueue<D1, T1>& src,
                                 GenericVarLenEntryQueue<D2, T2>& dst) {
  CopyVarLenEntriesImpl(src, dst, /*overwrite=*/false);
}

/// Copies consecutive entries from one queue to another, discarding entries
/// from the destination queue as needed to make room. Some entries may not be
/// copied if the data in the source queue is larger the destination queue's
/// capacity. Returns the number of bytes copied.
template <typename D1, typename T1, typename D2, typename T2>
constexpr void CopyVarLenEntriesOverwrite(
    const GenericVarLenEntryQueue<D1, T1>& src,
    GenericVarLenEntryQueue<D2, T2>& dst) {
  CopyVarLenEntriesImpl(src, dst, /*overwrite=*/true);
}

/// Moves as many consecutive entries as will fit in the destination queue
/// from a source queue to another, and returns the number of bytes moved.
template <typename D1, typename T1, typename D2, typename T2>
constexpr void MoveVarLenEntries(GenericVarLenEntryQueue<D1, T1>& src,
                                 GenericVarLenEntryQueue<D2, T2>& dst) {
  MoveVarLenEntriesImpl(src, dst, /*overwrite=*/false);
}

/// Moves consecutive entries from one queue to another, discarding entries
/// from the destination queue as needed to make room. Some entries may not be
/// moved if the data in the source queue is larger the destination queue's
/// capacity. Returns the number of bytes moved.
template <typename D1, typename T1, typename D2, typename T2>
constexpr void MoveVarLenEntriesOverwrite(
    GenericVarLenEntryQueue<D1, T1>& src,
    GenericVarLenEntryQueue<D2, T2>& dst) {
  MoveVarLenEntriesImpl(src, dst, /*overwrite=*/true);
}

////////////////////////////////////////////////////////////////////////////////
// Constexpr method implementations.

constexpr GenericVarLenEntryQueueBase::Info
GenericVarLenEntryQueueBase::GetInfo(ConstByteSpan bytes,
                                     size_t head,
                                     size_t tail) {
  Info info = {0, 0};
  while (head != tail) {
    auto [prefix_size, data_size] = ReadVarLenEntrySize(bytes, head);
    size_t entry_size = ReadVarLenEntryEncodedSize(bytes, head);
    IncrementWithWrap(head, entry_size, bytes.size());
    ++info.num_entries;
    info.total_data_size += data_size;
  }
  return info;
}

constexpr size_t GenericVarLenEntryQueueBase::AvailableBytes(
    ConstByteSpan bytes, size_t head, size_t tail) {
  PW_ASSERT(head < bytes.size());
  PW_ASSERT(tail < bytes.size());
  if (head <= tail) {
    head += bytes.size();
  }
  return head - tail - 1;
}

constexpr bool GenericVarLenEntryQueueBase::Push(ConstByteSpan data,
                                                 bool overwrite,
                                                 ByteSpan bytes,
                                                 size_t& head,
                                                 size_t& tail) {
  // Encode the data size in a temporary buffer.
  std::byte tmp[varint::kMaxVarint32SizeBytes] = {};
  auto size_u32 = static_cast<uint32_t>(data.size());
  size_t encoded = varint::Encode(size_u32, ByteSpan(tmp, sizeof(tmp)));
  ConstByteSpan prefix(tmp, encoded);

  // Calculate how much space is neededvs how much is available.
  size_t needed_bytes = prefix.size() + data.size();
  PW_ASSERT(needed_bytes < bytes.size());
  size_t available_bytes = AvailableBytes(bytes, head, tail);

  // Check if sufficient space is available, or can be made available.
  if (overwrite) {
    while (needed_bytes > available_bytes) {
      available_bytes += PopNonEmpty(bytes, head);
    }
  } else {
    if (needed_bytes > available_bytes) {
      return false;
    }
  }

  // Write out the prefix and data.
  CopyAndWrap(prefix, bytes, tail);
  CopyAndWrap(data, bytes, tail);
  return true;
}

constexpr size_t GenericVarLenEntryQueueBase::PopNonEmpty(ConstByteSpan bytes,
                                                          size_t& head) {
  size_t entry_size = ReadVarLenEntryEncodedSize(bytes, head);
  IncrementWithWrap(head, entry_size, bytes.size());
  return entry_size;
}

constexpr void GenericVarLenEntryQueueBase::CopyEntries(ConstByteSpan src_bytes,
                                                        size_t& src_head,
                                                        size_t src_tail,
                                                        ByteSpan dst_bytes,
                                                        size_t& dst_head,
                                                        size_t& dst_tail,
                                                        bool overwrite) {
  size_t available = AvailableBytes(dst_bytes, dst_head, dst_tail);
  size_t to_copy = 0;
  size_t offset = src_head;
  while (src_head != src_tail) {
    size_t src_entry_size = ReadVarLenEntryEncodedSize(src_bytes, src_head);
    if (src_entry_size <= available) {
      // Sufficient room; no-op.

    } else if (!overwrite) {
      // Not enough room, and cannot make more.
      break;

    } else if ((dst_bytes.size() - 1) < src_entry_size ||
               (dst_bytes.size() - 1 - src_entry_size) < to_copy) {
      // The next entry is too big to fit even if the destination were cleared.
      break;

    } else {
      // Pop just enough elements to make room.
      while (dst_head != dst_tail && available < src_entry_size) {
        size_t dst_entry_size = ReadVarLenEntryEncodedSize(dst_bytes, dst_head);
        IncrementWithWrap(dst_head, dst_entry_size, dst_bytes.size());
        available += dst_entry_size;
      }
      // If dst_head == dst_tail, then available is dst_bytes.size() - 1, which
      // is guaranteed to be greater than or equal to src_entry_size.
    }
    to_copy += src_entry_size;
    available -= src_entry_size;
    IncrementWithWrap(src_head, src_entry_size, src_bytes.size());
  }

  if (to_copy == 0) {
    return;
  }
  size_t src_chunk = src_bytes.size() - offset;
  if (src_chunk >= to_copy) {
    CopyAndWrap(src_bytes.subspan(offset, to_copy), dst_bytes, dst_tail);
  } else {
    CopyAndWrap(src_bytes.subspan(offset), dst_bytes, dst_tail);
    CopyAndWrap(src_bytes.subspan(0, to_copy - src_chunk), dst_bytes, dst_tail);
  }
}

constexpr void GenericVarLenEntryQueueBase::CopyAndWrap(ConstByteSpan data,
                                                        ByteSpan bytes,
                                                        size_t& tail) {
  if (data.empty()) {
    return;
  }
  // Copy the new data in one or two chunks. The first chunk is copied to the
  // byte after the tail, the second from the beginning of the buffer. Both may
  // be zero bytes.
  size_t first_chunk = bytes.size() - tail;
  if (first_chunk >= data.size()) {
    first_chunk = data.size();
  } else {  // Copy 2nd chunk from the beginning of the buffer (may be 0 bytes).
    pw::copy(data.begin() +
                 static_cast<span<std::byte>::difference_type>(first_chunk),
             data.end(),
             bytes.begin());
  }
  pw::copy(
      data.begin(),
      data.begin() + static_cast<span<std::byte>::difference_type>(first_chunk),
      bytes.begin() + static_cast<span<std::byte>::difference_type>(tail));
  IncrementWithWrap(tail, data.size(), bytes.size());
}

constexpr std::pair<ConstByteSpan, ConstByteSpan>
GenericVarLenEntryQueueBase::ContiguousRawStorage(ConstByteSpan bytes,
                                                  size_t head,
                                                  size_t tail) {
  if (head == tail) {
    return std::make_pair(ConstByteSpan(), ConstByteSpan());
  }
  if (head < tail) {
    return std::make_pair(bytes.subspan(head, tail - head), ConstByteSpan());
  }
  return std::make_pair(bytes.subspan(head), bytes.subspan(0, tail));
}

////////////////////////////////////////////////////////////////////////////////
// Template method implementations.

template <typename Derived, typename T>
constexpr typename GenericVarLenEntryQueue<Derived, T>::value_type
GenericVarLenEntryQueue<Derived, T>::front() {
  PW_ASSERT(!empty());
  return *begin();
}

template <typename Derived, typename T>
constexpr typename GenericVarLenEntryQueue<Derived, T>::const_value_type
GenericVarLenEntryQueue<Derived, T>::front() const {
  PW_ASSERT(!empty());
  return *cbegin();
}

template <typename Derived, typename T>
constexpr void GenericVarLenEntryQueue<Derived, T>::pop() {
  PW_ASSERT(!empty());
  size_t head = GetHead();
  PopNonEmpty(GetBytes(), head);
  SetHead(head);
}

template <typename Derived, typename T>
constexpr void GenericVarLenEntryQueue<Derived, T>::clear() {
  SetHead(0);
  SetTail(0);
}

template <typename Derived, typename T>
constexpr std::pair<span<const T>, span<const T>>
GenericVarLenEntryQueue<Derived, T>::contiguous_raw_storage() const {
  auto [first, second] =
      Base::ContiguousRawStorage(GetBytes(), GetHead(), GetTail());
  return std::make_pair(span_cast<const T>(first), span_cast<const T>(second));
}

template <typename Derived, typename T>
span<const T> GenericVarLenEntryQueue<Derived, T>::dering() {
  ByteSpan bytes = GetBytes();
  size_t tail = encoded_size_bytes();
  Base::Dering(bytes, GetHead());
  SetHead(0);
  SetTail(tail);
  return span_cast<const T>(bytes.subspan(0, tail));
}

template <typename Derived, typename T>
constexpr ByteSpan GenericVarLenEntryQueue<Derived, T>::AsWritableBytes(
    span<T> data) {
  if constexpr (std::is_same_v<T, std::byte>) {
    return data;
  } else {
    return as_writable_bytes(data);
  }
}

template <typename Derived, typename T>
constexpr ConstByteSpan GenericVarLenEntryQueue<Derived, T>::AsBytes(
    span<const T> data) {
  if constexpr (std::is_same_v<T, std::byte>) {
    return data;
  } else {
    return as_bytes(data);
  }
}

template <typename Derived, typename T>
constexpr bool GenericVarLenEntryQueue<Derived, T>::Push(span<const T> data,
                                                         bool overwrite) {
  size_t head = GetHead();
  size_t tail = GetTail();
  bool result = Base::Push(AsBytes(data), overwrite, GetBytes(), head, tail);
  SetHead(head);
  SetTail(tail);
  return result;
}

template <typename D1, typename T1, typename D2, typename T2>
constexpr void CopyVarLenEntriesImpl(const GenericVarLenEntryQueue<D1, T1>& src,
                                     GenericVarLenEntryQueue<D2, T2>& dst,
                                     bool overwrite) {
  size_t src_head = src.GetHead();
  size_t dst_head = dst.GetHead();
  size_t dst_tail = dst.GetTail();
  GenericVarLenEntryQueueBase::CopyEntries(src.GetBytes(),
                                           src_head,
                                           src.GetTail(),
                                           dst.GetBytes(),
                                           dst_head,
                                           dst_tail,
                                           overwrite);
  dst.SetHead(dst_head);
  dst.SetTail(dst_tail);
}

template <typename D1, typename T1, typename D2, typename T2>
constexpr void MoveVarLenEntriesImpl(GenericVarLenEntryQueue<D1, T1>& src,
                                     GenericVarLenEntryQueue<D2, T2>& dst,
                                     bool overwrite) {
  size_t src_head = src.GetHead();
  size_t dst_head = dst.GetHead();
  size_t dst_tail = dst.GetTail();
  GenericVarLenEntryQueueBase::CopyEntries(src.GetBytes(),
                                           src_head,
                                           src.GetTail(),
                                           dst.GetBytes(),
                                           dst_head,
                                           dst_tail,
                                           overwrite);
  src.SetHead(src_head);
  dst.SetHead(dst_head);
  dst.SetTail(dst_tail);
}

}  // namespace pw::containers::internal
#endif  // __cplusplus
