// Copyright 2026 The Pigweed Authors
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

#include <lib/stdcompat/memory.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include "lib/stdcompat/utility.h"
#include "pw_assert/assert.h"
#include "pw_bytes/span.h"
#include "pw_containers/internal/generic_deque.h"
#include "pw_containers/internal/traits.h"
#include "pw_containers/internal/wrap.h"
#include "pw_containers/storage.h"
#include "pw_numeric/checked_arithmetic.h"

namespace pw::containers::internal {

template <typename T>
constexpr bool kPodType = std::conjunction_v<std::is_trivially_copyable<T>,
                                             std::is_trivially_destructible<T>>;

template <typename Derived, size_t kAlignment, typename SizeType>
class PodDequeImpl
    : public pw::containers::internal::GenericDequeBase<SizeType> {
 public:
  using Base = pw::containers::internal::GenericDequeBase<SizeType>;
  using Base::capacity;
  using Base::empty;
  using Base::size;
  using size_type = SizeType;

  constexpr size_type max_size() const noexcept { return capacity(); }

  void push_back(const std::byte* data) {
    std::memcpy(&derived().data()[tail() * pod_size_], data, pod_size_);
    Base::PushBack();
  }

  void pop_back() { Base::PopBack(); }

  void push_front(const std::byte* data) {
    Base::PushFront();
    std::memcpy(&derived().data()[head() * pod_size_], data, pod_size_);
  }

  void pop_front() { Base::PopFront(); }

  std::byte* front() {
    PW_DASSERT(!empty());
    return &derived().data()[head() * pod_size_];
  }
  const std::byte* front() const {
    PW_DASSERT(!empty());
    return &derived().data()[head() * pod_size_];
  }
  std::byte* back() {
    PW_DASSERT(!empty());
    return &derived().data()[Base::AbsoluteIndex(size() - 1) * pod_size_];
  }
  const std::byte* back() const {
    PW_DASSERT(!empty());
    return &derived().data()[Base::AbsoluteIndex(size() - 1) * pod_size_];
  }

  std::byte* operator[](size_type index) {
    PW_DASSERT(index < size());
    return &derived().data()[Base::AbsoluteIndex(index) * pod_size_];
  }
  const std::byte* operator[](size_type index) const {
    PW_DASSERT(index < size());
    return &derived().data()[Base::AbsoluteIndex(index) * pod_size_];
  }
  std::byte* at(size_type index) {
    return &derived().data()[Base::AbsoluteIndexChecked(index) * pod_size_];
  }
  const std::byte* at(size_type index) const {
    return &derived().data()[Base::AbsoluteIndexChecked(index) * pod_size_];
  }

  std::pair<ConstByteSpan, ConstByteSpan> contiguous_data() const {
    if (empty()) {
      return {{}, {}};
    }
    if (tail() > head()) {
      return {ConstByteSpan(&data()[head() * pod_size_], size() * pod_size_),
              {}};
    } else {
      return {ConstByteSpan(&data()[head() * pod_size_],
                            (capacity() - head()) * pod_size_),
              ConstByteSpan(&data()[0], tail() * pod_size_)};
    }
  }

  std::pair<ByteSpan, ByteSpan> contiguous_data() {
    if (empty()) {
      return {{}, {}};
    }
    if (tail() > head()) {
      return {ByteSpan(&data()[head() * pod_size_], size() * pod_size_), {}};
    } else {
      return {ByteSpan(&data()[head() * pod_size_],
                       (capacity() - head()) * pod_size_),
              ByteSpan(&data()[0], tail() * pod_size_)};
    }
  }

  constexpr void clear() { Base::ClearIndices(); }

  void resize(size_type new_size) { PW_ASSERT(try_resize(new_size)); }

  bool try_resize(size_type new_size) {
    if (size() < new_size) {
      if (!CheckCapacity(new_size)) {
        return false;
      }
      const size_type new_items = new_size - size();
      for (size_type i = 0; i < new_items; ++i) {
        Base::PushBack();  // just need to modify indices
      }
    } else {
      while (size() > new_size) {
        pop_back();
      }
    }
    return true;
  }

  bool try_resize(size_type new_size, const std::byte* value_data) {
    if (size() < new_size) {
      if (!CheckCapacity(new_size)) {
        return false;
      }
      const size_type new_items = new_size - size();
      for (size_type i = 0; i < new_items; ++i) {
        push_back(value_data);
      }
    } else {
      while (size() > new_size) {
        pop_back();
      }
    }
    return true;
  }

  // Ensures that the container can hold at least this many elements.
  constexpr bool CheckCapacity(size_type new_size) {
    if constexpr (Derived::kFixedCapacity) {
      return new_size <= capacity();
    } else {
      return derived().try_reserve(new_size);
    }
  }

  void assign(const std::byte* start, const std::byte* finish) {
    clear();
    while (start != finish) {
      push_back(start);
      start += pod_size_;
    }
  }

  bool try_assign(size_type count, const std::byte* value_data) {
    if (!CheckCapacity(count)) {
      return false;
    }
    clear();
    Base::PushBack(count);
    for (size_type i = 0; i < count; ++i) {
      std::memcpy(&derived().data()[i * pod_size_], value_data, pod_size_);
    }
    return true;
  }

  void push_back_overwrite(const std::byte* data) {
    PW_ASSERT(capacity() > 0);
    std::memcpy(&derived().data()[Base::tail_ * pod_size_], data, pod_size_);
    Base::PushBackOverwrite();
  }

  void push_front_overwrite(const std::byte* data) {
    PW_ASSERT(capacity() > 0);
    Base::PushFrontOverwrite();
    std::memcpy(&derived().data()[Base::head_ * pod_size_], data, pod_size_);
  }

 protected:
  explicit constexpr PodDequeImpl(size_type initial_capacity,
                                  size_t pod_size) noexcept
      : pw::containers::internal::GenericDequeBase<SizeType>(initial_capacity),
        pod_size_(pod_size) {}

  constexpr size_type head() const { return Base::head_; }
  constexpr size_type tail() const { return Base::tail_; }

  void MoveItemsFrom(PodDequeImpl& other) {
    // Doesn't work when other's size exceeds this->capacity()
    PW_ASSERT(other.size() <= capacity());
    std::memcpy(data(), other.data(), capacity() * pod_size_);
    Base::MoveAssignIndices(other);
  }

  void SwapValuesWith(PodDequeImpl& other) {
    // Doesn't work for when one of the container's sizes exceeds the other's
    // capacity
    PW_ASSERT(size() <= other.capacity() && other.size() <= capacity());
    size_t total_bytes = std::min(capacity(), other.capacity()) * pod_size_;

    std::byte* a = data();
    std::byte* b = other.data();

    if constexpr (kAlignment >= 8) {
      SwapInChunks<uint64_t>(a, b, total_bytes);
    } else if constexpr (kAlignment >= 4) {
      SwapInChunks<uint32_t>(a, b, total_bytes);
    } else if constexpr (kAlignment >= 2) {
      SwapInChunks<uint16_t>(a, b, total_bytes);
    } else {
      SwapInChunks<uint8_t>(a, b, total_bytes);
    }

    std::swap(Base::count_, other.count_);
    std::swap(Base::head_, other.head_);
    std::swap(Base::tail_, other.tail_);
  }

  bool ShiftForInsert(size_type insert_index, size_type new_items) {
    if (!CheckCapacityAdd(new_items)) {
      return false;
    }
    if (insert_index < size() / 2) {
      ShiftLeft(insert_index, new_items);
    } else {
      ShiftRight(insert_index, new_items);
    }
    return true;
  }

  void erase(size_type first_pos, size_type last_pos) {
    PW_DASSERT(first_pos <= last_pos);
    const size_type items_to_erase = last_pos - first_pos;
    if (items_to_erase == 0) {
      return;
    }

    const size_type items_after = size() - last_pos;
    if (first_pos < items_after) {  // fewer before than after
      MoveElementsUnchecked(items_to_erase, 0, first_pos);
      Base::head_ = Base::AbsoluteIndex(items_to_erase);
    } else {  // fewer after than before
      MoveElementsUnchecked(first_pos, last_pos, items_after);
      Base::tail_ = Base::AbsoluteIndex(first_pos + items_after);
    }
    Base::count_ = size() - items_to_erase;
  }

  bool CheckCapacityAdd(size_type count) {
    size_type new_size;
    return CheckedAdd(size(), count, new_size) && CheckCapacity(new_size);
  }

 private:
  template <typename Word>
  static void SwapInChunks(std::byte* a, std::byte* b, size_t total_bytes) {
    std::byte* a_end = a + total_bytes;
    while (a < a_end) {
      Word temp;
      std::memcpy(&temp, a, sizeof(Word));
      std::memcpy(a, b, sizeof(Word));
      std::memcpy(b, &temp, sizeof(Word));
      a += sizeof(Word);
      b += sizeof(Word);
    }
  }

  void MoveElementsUnchecked(size_type dst, size_type src, size_type count) {
    if (count == 0) {
      return;
    }
    if (dst < src) {
      for (size_type i = 0; i < count; ++i) {
        std::memcpy((*this)[dst + i], (*this)[src + i], pod_size_);
      }
    } else if (dst > src) {
      for (size_type i = count; i > 0; --i) {
        std::memcpy((*this)[dst + i - 1], (*this)[src + i - 1], pod_size_);
      }
    }
  }

  void ShiftLeft(size_type insert_index, size_type new_items) {
    Base::PushFront(new_items);
    MoveElementsUnchecked(0, new_items, insert_index);
  }

  void ShiftRight(size_type insert_index, size_type new_items) {
    size_type old_size = size();
    Base::PushBack(new_items);
    MoveElementsUnchecked(
        insert_index + new_items, insert_index, old_size - insert_index);
  }

  constexpr Derived& derived() { return static_cast<Derived&>(*this); }
  constexpr const Derived& derived() const {
    return static_cast<const Derived&>(*this);
  }
  std::byte* data() { return derived().data(); }
  const std::byte* data() const { return derived().data(); }
  const size_t pod_size_;
};

// does type erasure by reinterpreting data as byte ptr such that
// PodDequeImpl does not receive T in its template params
template <size_t kAlignment, typename SizeType>
class TypeErasedDequeStorage
    : public PodDequeImpl<TypeErasedDequeStorage<kAlignment, SizeType>,
                          kAlignment,
                          SizeType> {
 public:
  using Base = PodDequeImpl<TypeErasedDequeStorage<kAlignment, SizeType>,
                            kAlignment,
                            SizeType>;
  using Base::empty;
  using Base::size;
  using size_type = SizeType;

  static constexpr bool kFixedCapacity = true;

 protected:
  explicit constexpr TypeErasedDequeStorage(std::byte* storage,
                                            size_type initial_capacity,
                                            size_t pod_size) noexcept
      : PodDequeImpl<TypeErasedDequeStorage<kAlignment, SizeType>,
                     kAlignment,
                     SizeType>(initial_capacity, pod_size),
        buffer_(storage) {}

  constexpr size_type head() const { return Base::head(); }
  constexpr size_type tail() const { return Base::tail(); }

 private:
  // For PodDequeImpl to use TypeErasedDequeStorage's data()
  template <typename D, size_t A, typename ST>
  friend class PodDequeImpl;

  std::byte* buffer_;
  constexpr std::byte* data() { return buffer_; }
  constexpr const std::byte* data() const { return buffer_; }
};

template <typename ValueType, typename SizeType = uint16_t>
class PodDeque : public TypeErasedDequeStorage<alignof(ValueType), SizeType> {
 public:
  using size_type = SizeType;
  using value_type = ValueType;
  using difference_type = std::ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;
  using Base = TypeErasedDequeStorage<alignof(ValueType), SizeType>;

  using iterator = containers::internal::DequeIterator<PodDeque>;
  using const_iterator = containers::internal::DequeIterator<const PodDeque>;

  template <size_t kAlignment, size_t kSizeBytes>
  explicit constexpr PodDeque(
      pw::containers::Storage<kAlignment, kSizeBytes>& buffer) noexcept
      : PodDeque(Aligned(buffer.data(), buffer.size())) {
    static_assert(kAlignment >= alignof(value_type));
  }

  explicit constexpr PodDeque(ByteSpan unaligned_buffer) noexcept
      : PodDeque(Aligned::Align(unaligned_buffer)) {}

  // Iterators
  constexpr iterator begin() noexcept {
    if (empty()) {
      return end();
    }
    return iterator(*this, 0);
  }
  constexpr const_iterator begin() const noexcept { return cbegin(); }
  constexpr const_iterator cbegin() const noexcept {
    if (empty()) {
      return cend();
    }
    return const_iterator(*this, 0);
  }

  constexpr iterator end() noexcept { return iterator(*this, size()); }
  constexpr const_iterator end() const noexcept { return cend(); }
  constexpr const_iterator cend() const noexcept {
    return const_iterator(*this, size());
  }

  using Base::empty;
  using Base::size;

  void push_back(const ValueType& value) {
    Base::push_back(reinterpret_cast<const std::byte*>(&value));
  }

  void pop_back() { Base::pop_back(); }

  void push_front(const ValueType& value) {
    Base::push_front(reinterpret_cast<const std::byte*>(&value));
  }

  void pop_front() { Base::pop_front(); }

  constexpr ValueType& front() {
    return reinterpret_cast<ValueType&>(*Base::front());
  }
  constexpr const ValueType& front() const {
    return reinterpret_cast<const ValueType&>(*Base::front());
  }

  constexpr ValueType& back() {
    return reinterpret_cast<ValueType&>(*Base::back());
  }
  constexpr const ValueType& back() const {
    return reinterpret_cast<const ValueType&>(*Base::back());
  }

  constexpr ValueType& operator[](size_type index) {
    return reinterpret_cast<ValueType&>(*Base::operator[](index));
  }
  constexpr const ValueType& operator[](size_type index) const {
    return reinterpret_cast<const ValueType&>(*Base::operator[](index));
  }
  constexpr ValueType& at(size_type index) {
    return reinterpret_cast<ValueType&>(*Base::at(index));
  }
  constexpr const ValueType& at(size_type index) const {
    return reinterpret_cast<const ValueType&>(*Base::at(index));
  }

  std::pair<span<const ValueType>, span<const ValueType>> contiguous_data()
      const {
    auto [first, second] = Base::contiguous_data();
    return {
        span<const ValueType>(reinterpret_cast<const ValueType*>(first.data()),
                              first.size() / sizeof(ValueType)),
        span<const ValueType>(reinterpret_cast<const ValueType*>(second.data()),
                              second.size() / sizeof(ValueType))};
  }

  std::pair<span<ValueType>, span<ValueType>> contiguous_data() {
    auto [first, second] = Base::contiguous_data();
    return {span<ValueType>(reinterpret_cast<ValueType*>(first.data()),
                            first.size() / sizeof(ValueType)),
            span<ValueType>(reinterpret_cast<ValueType*>(second.data()),
                            second.size() / sizeof(ValueType))};
  }

  template <typename It>
  void assign(It start, It finish) {
    Base::clear();
    while (start != finish) {
      push_back(*start);
      ++start;
    }
  }

  void assign(size_type count, const ValueType& value) {
    PW_ASSERT(
        Base::try_assign(count, reinterpret_cast<const std::byte*>(&value)));
  }

  void assign(std::initializer_list<ValueType> list) {
    assign(list.begin(), list.end());
  }

  void resize(size_type new_size) { Base::resize(new_size); }

  void resize(size_type new_size, const ValueType& value) {
    PW_ASSERT(
        Base::try_resize(new_size, reinterpret_cast<const std::byte*>(&value)));
  }

  void push_back_overwrite(const ValueType& value) {
    Base::push_back_overwrite(reinterpret_cast<const std::byte*>(&value));
  }
  void push_front_overwrite(const ValueType& value) {
    Base::push_front_overwrite(reinterpret_cast<const std::byte*>(&value));
  }

  // Insert/Erase/Emplace
  template <typename... Args>
  iterator emplace(const_iterator pos, Args&&... args) {
    ValueType temp(std::forward<Args>(args)...);
    return insert(pos, temp);
  }

  iterator insert(const_iterator pos, const ValueType& value) {
    PW_ASSERT(Base::ShiftForInsert(pos.pos_, 1));
    iterator it(*this, pos.pos_);
    std::memcpy(std::addressof(*it), &value, sizeof(ValueType));
    return it;
  }

  iterator insert(const_iterator pos, size_type count, const ValueType& value) {
    iterator it(*this, pos.pos_);
    if (count == 0) {
      return it;
    }
    PW_ASSERT(Base::ShiftForInsert(pos.pos_, count));
    for (size_type i = 0; i < count; ++i) {
      std::memcpy(std::addressof(*(it + i)), &value, sizeof(ValueType));
    }
    return it;
  }

  template <typename It,
            typename = containers::internal::EnableIfInputIterator<It>>
  iterator insert(const_iterator pos, It first, It last) {
    if constexpr (std::is_same_v<
                      typename std::iterator_traits<It>::iterator_category,
                      std::input_iterator_tag>) {
      iterator insert_pos(*this, pos.pos_);
      while (first != last) {
        insert_pos = emplace(insert_pos, *first);
        ++first;
        ++insert_pos;
      }
      return iterator(*this, pos.pos_);
    } else {
      const auto distance = std::distance(first, last);
      const size_type count = static_cast<size_type>(distance);
      iterator it(*this, pos.pos_);
      if (count == 0) {
        return it;
      }
      PW_ASSERT(Base::ShiftForInsert(pos.pos_, count));
      for (size_type i = 0; i < count; ++i) {
        std::memcpy(std::addressof(*(it + i)),
                    std::addressof(*first),
                    sizeof(ValueType));
        ++first;
      }
      return it;
    }
  }

  iterator insert(const_iterator pos, std::initializer_list<ValueType> ilist) {
    return insert(pos, ilist.begin(), ilist.end());
  }

  iterator erase(const_iterator pos) { return erase(pos, pos + 1); }
  iterator erase(const_iterator first, const_iterator last) {
    Base::erase(first.pos_, last.pos_);
    return iterator(*this, first.pos_);
  }

 private:
  // Allow DequeIterator to use at() and other private/protected methods.
  friend class containers::internal::DequeIterator<PodDeque>;
  friend class containers::internal::DequeIterator<const PodDeque>;

  using Aligned = containers::internal::Aligned<value_type>;

  explicit constexpr PodDeque(Aligned buffer) noexcept
      : PodDeque(buffer.data(), static_cast<size_type>(buffer.capacity())) {}

  explicit constexpr PodDeque(std::byte* storage, size_type kCapacity)
      : TypeErasedDequeStorage<alignof(ValueType), SizeType>(
            storage, kCapacity, sizeof(ValueType)) {}
};

}  // namespace pw::containers::internal
