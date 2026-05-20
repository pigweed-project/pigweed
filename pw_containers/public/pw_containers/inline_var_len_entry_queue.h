// Copyright 2023 The Pigweed Authors
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

// TODO: https://pwbug.dev/426012010 - Find a way to single-source this
// content on the Sphinx site without Breathe.

/// @file pw_containers/inline_var_len_entry_queue.h
///
/// A `InlineVarLenEntryQueue` is a queue of inline variable-length binary
/// entries. It is implemented as a ring (circular) buffer and supports
/// operations to append entries and overwrite if necessary. Entries may be zero
/// bytes up to the maximum size supported by the queue.
///
/// The `InlineVarLenEntryQueue` has a few interesting properties.
///
/// - Data and metadata are stored inline in a contiguous block of
///   `uint32_t`-aligned memory.
/// - The data structure is trivially copyable.
/// - All state changes are accomplished with a single update to a `uint32_t`.
///   The memory is always in a valid state and may be parsed offline.
///
/// This data structure is a much simpler version of
/// @cpp_class{pw::ring_buffer::PrefixedEntryRingBuffer}. Prefer this
/// sized-entry ring buffer to `PrefixedEntryRingBuffer` when:
/// - A simple ring buffer of variable-length entries is needed. Advanced
///   features like multiple readers and a user-defined preamble are not
///   required.
/// - A consistent, parsable, in-memory representation is required (e.g. to
///   decode the buffer from a block of memory).
/// - C support is required.
///
/// `InlineVarLenEntryQueue` provides complete C and C++ APIs. The
/// `InlineVarLenEntryQueue` C++ class is structured similarly to
/// `pw::InlineQueue` and `pw::Vector`.

#include "pw_containers/internal/generic_var_len_entry_queue.h"
#include "pw_containers/internal/var_len_entry_queue_iterator.h"
#include "pw_preprocessor/util.h"

#ifndef __cplusplus

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#else  // __cplusplus

#include <cstddef>
#include <cstdint>

#include "pw_bytes/span.h"
#include "pw_containers/internal/raw_storage.h"
#include "pw_span/span.h"
#include "pw_toolchain/constexpr_tag.h"

#endif  // __cplusplus

/// @module{pw_containers}

/// The size of the `InlineVarLenEntryQueue` header, in `uint32_t` elements.
/// This header stores the buffer length and head and tail offsets.
///
/// The underlying `uint32_t` array of a `InlineVarLenEntryQueue` must be larger
/// than this size.
#define PW_VARIABLE_LENGTH_ENTRY_QUEUE_HEADER_SIZE_UINT32 (3)

#define _PW_VAR_QUEUE_SIZE_UINT32(max_size_bytes)      \
  (PW_VARIABLE_LENGTH_ENTRY_QUEUE_HEADER_SIZE_UINT32 + \
   ((PW_VAR_QUEUE_DATA_SIZE_BYTES(max_size_bytes) + 3 /* round up */) / 4))

#ifdef __cplusplus
namespace pw {

// A`BasicInlineVarLenEntryQueue` with a known maximum size of a single entry.
// The member functions are immplemented in the generic-capacity base.
// TODO: b/303056683 - Add helper for calculating kMaxSizeBytes for N entries of
//     a particular size.
template <typename T,
          size_t kMaxSizeBytes = containers::internal::kGenericSized>
class BasicInlineVarLenEntryQueue
    // TODO: b/513051956 - Fix `recursive class relation` error
    /// @cond
    : public BasicInlineVarLenEntryQueue<T, containers::internal::kGenericSized>
/// @endcond
{
 private:
  using Base =
      BasicInlineVarLenEntryQueue<T, containers::internal::kGenericSized>;

 public:
  using value_type = typename Base::value_type;
  using const_value_type = typename Base::const_value_type;
  using size_type = typename Base::size_type;
  using pointer = typename Base::pointer;
  using const_pointer = typename Base::const_pointer;
  using reference = typename Base::reference;
  using const_reference = typename Base::const_reference;

  BasicInlineVarLenEntryQueue()
      : array_{PW_VAR_QUEUE_DATA_SIZE_BYTES(kMaxSizeBytes), 0, 0} {}

  // Explicit zero element constexpr constructor. Using this constructor will
  // place the entire object in .data, which will increase ROM size. Use with
  // caution if working with large capacity sizes.
  constexpr BasicInlineVarLenEntryQueue(ConstexprTag)
      : array_{PW_VAR_QUEUE_DATA_SIZE_BYTES(kMaxSizeBytes), 0, 0} {}

  BasicInlineVarLenEntryQueue(const BasicInlineVarLenEntryQueue<T>& other) {
    *this = other;
  }
  BasicInlineVarLenEntryQueue(BasicInlineVarLenEntryQueue<T>&& other) {
    *this = std::move(other);
  }
  BasicInlineVarLenEntryQueue& operator=(
      const BasicInlineVarLenEntryQueue<T>& other);
  BasicInlineVarLenEntryQueue& operator=(
      BasicInlineVarLenEntryQueue<T>&& other);

 private:
  template <typename, size_t>
  friend class BasicInlineVarLenEntryQueue;

  constexpr span<uint32_t> array() { return array_; }
  constexpr span<const uint32_t> array() const { return array_; }

  // This class does not derive from RawStorage in order to allow this array to
  // be initialized as a constant expression.
  std::array<uint32_t, _PW_VAR_QUEUE_SIZE_UINT32(kMaxSizeBytes)> array_;
};

/// @addtogroup pw_containers_queues
/// @{

/// @defgroup inline_var_len_entry_queue_cpp_api C++ API
/// @{

/// Variable-length entry queue class template for any byte type (e.g.
/// ``std::byte`` or ``uint8_t``).
///
/// ``BasicInlineVarLenEntryQueue`` instances are declared with their capacity
/// / max single entry size (``BasicInlineVarLenEntryQueue<char, 64>``), but
/// may be referred to without the size
/// (``BasicInlineVarLenEntryQueue<char>&``).
template <typename T>
class BasicInlineVarLenEntryQueue<T, containers::internal::kGenericSized>
    : public containers::internal::GenericVarLenEntryQueue<
          BasicInlineVarLenEntryQueue<T, containers::internal::kGenericSized>,
          T> {
 private:
  using Base = containers::internal::GenericVarLenEntryQueue<
      BasicInlineVarLenEntryQueue<T, containers::internal::kGenericSized>,
      T>;

 public:
  using value_type = containers::internal::VarLenEntry<T>;
  using const_value_type = containers::internal::VarLenEntry<const T>;
  using size_type = std::uint32_t;
  using pointer = const value_type*;
  using const_pointer = pointer;
  using reference = const value_type&;
  using const_reference = reference;

  /// Initializes a `BasicInlineVarLenEntryQueue` in place within a `uint32_t`
  /// array. The array MUST be larger than
  /// @c_macro{PW_VARIABLE_LENGTH_ENTRY_QUEUE_HEADER_SIZE_UINT32} (3) elements.
  template <size_t kArraySize>
  static BasicInlineVarLenEntryQueue& Init(uint32_t (&array)[kArraySize]);
  static BasicInlineVarLenEntryQueue& Init(uint32_t array[],
                                           size_t array_size_uint32);

  /// Underlying storage of the variable-length entry queue. May be used to
  /// memcpy the queue.
  span<const T> raw_storage() const;

 protected:
  constexpr BasicInlineVarLenEntryQueue() = default;
  ~BasicInlineVarLenEntryQueue() = default;

 private:
  static constexpr size_t kNumFields = 3;
  static constexpr size_t kBufferSize = 0;
  static constexpr size_t kHead = 1;
  static constexpr size_t kTail = 2;

  // GenericVarLenEntryQueue uses CRTP to encapsulate
  template <typename, typename>
  friend class containers::internal::GenericVarLenEntryQueue;

  size_t buffer_size() const { return array()[kBufferSize]; }

  size_t head() const { return array()[kHead]; }
  void set_head(size_t head) { array()[kHead] = static_cast<uint32_t>(head); }

  size_t tail() const { return array()[kTail]; }
  void set_tail(size_t tail) { array()[kTail] = static_cast<uint32_t>(tail); }

  span<T> buffer() {
    return span<T>(reinterpret_cast<T*>(array().subspan(kNumFields).data()),
                   buffer_size());
  }

  span<const T> buffer() const {
    return span<const T>(
        reinterpret_cast<const T*>(array().subspan(kNumFields).data()),
        buffer_size());
  }

  // The underlying data is not part of the generic-length queue class. It is
  // provided in the derived class from which this instance was constructed. To
  // access the data, downcast this to a BasicInlineVarLenEntryQueue with a
  // known max size, and return a pointer to the start of the array, which is
  // the same for all queues with explicit max size.
  span<uint32_t> array() {
    return static_cast<BasicInlineVarLenEntryQueue<T, 0>*>(this)->array();
  }
  span<const uint32_t> array() const {
    return static_cast<const BasicInlineVarLenEntryQueue<T, 0>*>(this)->array();
  }
};

/// Variable-length entry queue that uses ``std::byte`` for the byte type.
template <size_t kMaxSizeBytes = containers::internal::kGenericSized>
using InlineVarLenEntryQueue =
    BasicInlineVarLenEntryQueue<std::byte, kMaxSizeBytes>;

////////////////////////////////////////////////////////////////////////////////
// Template method implementation.

// BasicInlineVarLenEntryQueue<T, kMaxSizeBytes> methods.

template <typename T, size_t kMaxSizeBytes>
BasicInlineVarLenEntryQueue<T, kMaxSizeBytes>&
BasicInlineVarLenEntryQueue<T, kMaxSizeBytes>::operator=(
    const BasicInlineVarLenEntryQueue<T>& other) {
  PW_ASSERT(this->GetBufferSize() >= other.GetBufferSize());
  size_t array_size =
      (sizeof(uint32_t) *
       BasicInlineVarLenEntryQueue<T, kMaxSizeBytes>::kNumFields) +
      other.GetBufferSize();
  std::memcpy(this->array(), other.array(), array_size);
  return *this;
}

template <typename T, size_t kMaxSizeBytes>
BasicInlineVarLenEntryQueue<T, kMaxSizeBytes>&
BasicInlineVarLenEntryQueue<T, kMaxSizeBytes>::operator=(
    BasicInlineVarLenEntryQueue<T>&& other) {
  *this = other;
  other.clear();
  return *this;
}

// BasicInlineVarLenEntryQueue<T> methods.

/// @cond
template <typename T>
template <size_t kArraySize>
BasicInlineVarLenEntryQueue<T>& BasicInlineVarLenEntryQueue<T>::Init(
    uint32_t (&array)[kArraySize]) {
  static_assert(
      kArraySize > PW_VARIABLE_LENGTH_ENTRY_QUEUE_HEADER_SIZE_UINT32,
      "InlineVarLenEntryQueue must be backed by an array with more than "
      "PW_VARIABLE_LENGTH_ENTRY_QUEUE_HEADER_SIZE_UINT32 (3) elements");
  return Init(array, kArraySize);
}

template <typename T>
BasicInlineVarLenEntryQueue<T>& BasicInlineVarLenEntryQueue<T>::Init(
    uint32_t array[], size_t array_size_uint32) {
  auto array_size = static_cast<uint32_t>(array_size_uint32);
  array_size -= PW_VARIABLE_LENGTH_ENTRY_QUEUE_HEADER_SIZE_UINT32;
  array[0] = array_size * sizeof(uint32_t);
  array[1] = 0;  // head
  array[2] = 0;  // tail
  return *(std::launder(reinterpret_cast<BasicInlineVarLenEntryQueue*>(array)));
}

template <typename T>
span<const T> BasicInlineVarLenEntryQueue<T>::raw_storage() const {
  size_t raw_size = (kNumFields * sizeof(uint32_t)) + buffer_size();
  return span<const T>(reinterpret_cast<const T*>(array().data()), raw_size);
}
/// @endcond

}  // namespace pw

#define _PW_VAR_QUEUE_CHECK_SIZE(max_size_bytes)                      \
  static_assert(sizeof(pw::InlineVarLenEntryQueue<max_size_bytes>) == \
                _PW_VAR_QUEUE_SIZE_UINT32(max_size_bytes) * sizeof(uint32_t));

#else  // !defined(__cplusplus)

#define _PW_VAR_QUEUE_CHECK_SIZE(max_size_bytes)

#endif  // __cplusplus

PW_EXTERN_C_START

/// @defgroup inline_var_len_entry_queue_c_api C API
/// @{

/// Declares and initializes an array that can back an `InlineVarLenEntryQueue`
/// that can hold up to `max_size_bytes` bytes when using the C API.
///`max_size_bytes` is the largest supported size for a single entry; attempting
/// to store larger entries is invalid and will fail an assertion.
///
/// See also `pw_InlineVarLenEntryQueue_Init`.
///
/// @param variable variable name for the queue
/// @param max_size_bytes the capacity of the queue
#define PW_VARIABLE_LENGTH_ENTRY_QUEUE_DECLARE(variable, max_size_bytes) \
  _PW_VAR_QUEUE_CHECK_SIZE(max_size_bytes)                               \
  uint32_t variable[_PW_VAR_QUEUE_SIZE_UINT32(max_size_bytes)] = {       \
      PW_VAR_QUEUE_DATA_SIZE_BYTES(max_size_bytes), /*head=*/0u, /*tail=*/0u}

typedef uint32_t* pw_InlineVarLenEntryQueue_Handle;
typedef const uint32_t* pw_InlineVarLenEntryQueue_ConstHandle;

/// Initializes a `BasicInlineVarLenEntryQueue` in place within a `uint32_t`
/// array. The array MUST be larger than
/// `PW_VARIABLE_LENGTH_ENTRY_QUEUE_HEADER_SIZE_UINT32` (3) elements.
void pw_InlineVarLenEntryQueue_Init(uint32_t array[], size_t array_size_uint32);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::begin
pw_InlineVarLenEntryQueue_Iterator pw_InlineVarLenEntryQueue_Begin(
    pw_InlineVarLenEntryQueue_Handle queue);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::cbegin
pw_InlineVarLenEntryQueue_ConstIterator pw_InlineVarLenEntryQueue_ConstBegin(
    pw_InlineVarLenEntryQueue_ConstHandle queue);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::end
pw_InlineVarLenEntryQueue_Iterator pw_InlineVarLenEntryQueue_End(
    pw_InlineVarLenEntryQueue_Handle queue);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::cend
pw_InlineVarLenEntryQueue_ConstIterator pw_InlineVarLenEntryQueue_ConstEnd(
    pw_InlineVarLenEntryQueue_ConstHandle queue);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::empty
bool pw_InlineVarLenEntryQueue_Empty(
    pw_InlineVarLenEntryQueue_ConstHandle queue);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::size
uint32_t pw_InlineVarLenEntryQueue_Size(
    pw_InlineVarLenEntryQueue_ConstHandle queue);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::max_size
uint32_t pw_InlineVarLenEntryQueue_MaxSize(
    pw_InlineVarLenEntryQueue_ConstHandle queue);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::size_bytes
uint32_t pw_InlineVarLenEntryQueue_SizeBytes(
    pw_InlineVarLenEntryQueue_ConstHandle queue);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::max_size_bytes
uint32_t pw_InlineVarLenEntryQueue_MaxSizeBytes(
    pw_InlineVarLenEntryQueue_ConstHandle queue);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::push
void pw_InlineVarLenEntryQueue_Push(pw_InlineVarLenEntryQueue_Handle queue,
                                    const void* data,
                                    uint32_t data_size_bytes);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::try_push
bool pw_InlineVarLenEntryQueue_TryPush(pw_InlineVarLenEntryQueue_Handle queue,
                                       const void* data,
                                       const uint32_t data_size_bytes);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::push_overwrite
void pw_InlineVarLenEntryQueue_PushOverwrite(
    pw_InlineVarLenEntryQueue_Handle queue,
    const void* data,
    uint32_t data_size_bytes);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::pop
void pw_InlineVarLenEntryQueue_Pop(pw_InlineVarLenEntryQueue_Handle queue);

/// @copydoc pw::containers::internal::GenericVarLenEntryQueue::clear
void pw_InlineVarLenEntryQueue_Clear(pw_InlineVarLenEntryQueue_Handle queue);

/// Underlying storage of the variable-length entry queue. May be used to
/// memcpy the queue.
uint32_t pw_InlineVarLenEntryQueue_RawStorageSizeBytes(
    pw_InlineVarLenEntryQueue_ConstHandle queue);

/// @copydoc pw::containers::internal::CopyVarLenEntries
void pw_InlineVarLenEntryQueue_CopyEntries(
    pw_InlineVarLenEntryQueue_ConstHandle from,
    pw_InlineVarLenEntryQueue_Handle to);

/// @copydoc pw::containers::internal::CopyVarLenEntriesOverwrite
void pw_InlineVarLenEntryQueue_CopyEntriesOverwrite(
    pw_InlineVarLenEntryQueue_ConstHandle from,
    pw_InlineVarLenEntryQueue_Handle to);

/// @copydoc pw::containers::internal::MoveVarLenEntries
void pw_InlineVarLenEntryQueue_MoveEntries(
    pw_InlineVarLenEntryQueue_Handle from, pw_InlineVarLenEntryQueue_Handle to);

/// @copydoc pw::containers::internal::MoveVarLenEntriesOverwrite
void pw_InlineVarLenEntryQueue_MoveEntriesOverwrite(
    pw_InlineVarLenEntryQueue_Handle from, pw_InlineVarLenEntryQueue_Handle to);

/// @}

PW_EXTERN_C_END

/// @}
