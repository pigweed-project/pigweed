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
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>

#include "pw_allocator/allocator.h"
#include "pw_allocator/unique_ptr.h"  // IWYU pragma: export
#include "pw_assert/assert.h"
#include "pw_containers/dynamic_vector.h"

namespace pw {

/// @module{pw_containers}
/// @defgroup pw_containers_vectors Vectors
/// @{

/// A `std::vector`-like container that holds pointers to objects of type `T`.
///
/// `DynamicPtrVector` allocates objects using a `pw::Allocator` and stores
/// pointers to them. This allows storing objects that are not movable or
/// copyable, or objects of polymorphic types, while still providing a
/// vector-like interface.
///
/// The interface mirrors `std::vector<T>`, but the underlying storage is
/// `DynamicVector<T*>`.
///
/// @warning The container's allocator MUST outlive the container, unless one of
/// the following steps is taken:
/// - Call `reset()` to free all memory, including the underlying buffer, and
///   make no further modifications to the container.
/// - Assign from a container with a different allocator (e.g. `NullAllocator`)
///   before the first allocator goes out of scope.
///
/// @warning Calling `clear()` is NOT sufficient; an empty container may still
/// hold a pointer to an allocated buffer.
///
template <typename T, typename SizeType = uint16_t>
class DynamicPtrVector {
 public:
  using value_type = T;
  using size_type = SizeType;
  using difference_type = std::ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;
  using allocator_type = Allocator;

 private:
  // Iterator that automatically dereferences the pointer.
  template <bool kIsConst>
  class Iterator {
   public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = std::conditional_t<kIsConst, const T*, T*>;
    using reference = std::conditional_t<kIsConst, const T&, T&>;

    constexpr Iterator() = default;

    // Allow conversion from non-const to const iterator.
    template <bool kOtherConst,
              typename = std::enable_if_t<kIsConst && !kOtherConst>>
    constexpr Iterator(const Iterator<kOtherConst>& other) : it_(other.it_) {}

    reference operator*() const { return **it_; }
    pointer operator->() const { return *it_; }

    Iterator& operator++() {
      ++it_;
      return *this;
    }
    Iterator operator++(int) { return Iterator(it_++); }
    Iterator& operator--() {
      --it_;
      return *this;
    }
    Iterator operator--(int) { return Iterator(it_--); }

    Iterator& operator+=(difference_type n) {
      it_ += n;
      return *this;
    }
    Iterator& operator-=(difference_type n) {
      it_ -= n;
      return *this;
    }
    friend Iterator operator+(Iterator it, difference_type n) {
      return Iterator(it.it_ + n);
    }
    friend Iterator operator+(difference_type n, Iterator it) {
      return Iterator(it.it_ + n);
    }
    friend Iterator operator-(Iterator it, difference_type n) {
      return Iterator(it.it_ - n);
    }
    friend difference_type operator-(Iterator lhs, Iterator rhs) {
      return lhs.it_ - rhs.it_;
    }

    friend bool operator==(Iterator lhs, Iterator rhs) {
      return lhs.it_ == rhs.it_;
    }
    friend bool operator!=(Iterator lhs, Iterator rhs) {
      return lhs.it_ != rhs.it_;
    }
    friend bool operator<(Iterator lhs, Iterator rhs) {
      return lhs.it_ < rhs.it_;
    }
    friend bool operator<=(Iterator lhs, Iterator rhs) {
      return lhs.it_ <= rhs.it_;
    }
    friend bool operator>(Iterator lhs, Iterator rhs) {
      return lhs.it_ > rhs.it_;
    }
    friend bool operator>=(Iterator lhs, Iterator rhs) {
      return lhs.it_ >= rhs.it_;
    }

    reference operator[](difference_type n) const { return *(*this + n); }

   private:
    using BaseIterator =
        std::conditional_t<kIsConst,
                           typename DynamicVector<T*>::const_iterator,
                           typename DynamicVector<T*>::iterator>;

    explicit constexpr Iterator(BaseIterator it) : it_(it) {}

    friend class DynamicPtrVector;
    BaseIterator it_;
  };

 public:
  using iterator = Iterator<false>;
  using const_iterator = Iterator<true>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  /// Constructs an empty `DynamicPtrVector` using the provided allocator.
  explicit constexpr DynamicPtrVector(Allocator& allocator)
      : vector_(allocator) {}

  ~DynamicPtrVector() { clear(); }

  DynamicPtrVector(const DynamicPtrVector&) = delete;
  DynamicPtrVector& operator=(const DynamicPtrVector&) = delete;

  DynamicPtrVector(DynamicPtrVector&& other) noexcept = default;
  DynamicPtrVector& operator=(DynamicPtrVector&& other) noexcept {
    clear();
    vector_ = std::move(other.vector_);
    return *this;
  }

  // Iterators

  iterator begin() { return iterator(vector_.begin()); }
  const_iterator begin() const { return const_iterator(vector_.begin()); }
  const_iterator cbegin() const { return begin(); }

  iterator end() { return iterator(vector_.end()); }
  const_iterator end() const { return const_iterator(vector_.end()); }
  const_iterator cend() const { return end(); }

  reverse_iterator rbegin() { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const {
    return const_reverse_iterator(end());
  }
  const_reverse_iterator crbegin() const { return rbegin(); }

  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const {
    return const_reverse_iterator(begin());
  }
  const_reverse_iterator crend() const { return rend(); }

  // Capacity

  allocator_type& get_allocator() const { return vector_.get_allocator(); }
  [[nodiscard]] bool empty() const { return vector_.empty(); }
  size_type size() const { return vector_.size(); }
  size_type ptr_capacity() const { return vector_.capacity(); }
  size_type max_size() const { return vector_.max_size(); }

  /// Reserves slots in the pointer vector. Does NOT guarantee that future
  /// operations that add items will succeed, since they require separate
  /// allocations for the objects themselves.
  void reserve_ptr(size_type new_capacity) { vector_.reserve(new_capacity); }

  /// @copydoc reserve_ptr
  void reserve_ptr_exact(size_type new_capacity) {
    vector_.reserve_exact(new_capacity);
  }

  /// Attempts to reserve slots in the pointer vector. Does NOT guarantee that
  /// future operations that add items will succeed, since they require separate
  /// allocations for the objects themselves.
  [[nodiscard]] bool try_reserve_ptr(size_type new_capacity) {
    return vector_.try_reserve(new_capacity);
  }
  /// @copydoc try_reserve_ptr
  [[nodiscard]] bool try_reserve_ptr_exact(size_type new_capacity) {
    return vector_.try_reserve_exact(new_capacity);
  }

  void shrink_to_fit() { vector_.shrink_to_fit(); }

  // Element access

  reference operator[](size_type pos) { return *vector_[pos]; }
  const_reference operator[](size_type pos) const { return *vector_[pos]; }

  reference at(size_type pos) { return *vector_.at(pos); }
  const_reference at(size_type pos) const { return *vector_.at(pos); }

  reference front() { return *vector_.front(); }
  const_reference front() const { return *vector_.front(); }

  reference back() { return *vector_.back(); }
  const_reference back() const { return *vector_.back(); }

  /// Returns a pointer to the underlying array of pointers.
  /// Note: This returns `T**`, not `T*`.
  T* const* data() { return vector_.data(); }
  const T* const* data() const { return vector_.data(); }

  // Modifiers

  /// Appends a new element by copying `value`.
  void push_back(const T& value) { emplace_back(value); }

  /// Appends a new element by moving `value`.
  void push_back(T&& value) { emplace_back(std::move(value)); }

  /// Attempts to append a new element by copying `value`.
  [[nodiscard]] bool try_push_back(const T& value) {
    return try_emplace_back(value);
  }

  /// Appends a new element by taking ownership of the object in `value`.
  ///
  /// If `value` was allocated using this vector's allocator, the pointer is
  /// transferred directly without reallocation. Otherwise, a new object is
  /// move-constructed from `*value`.
  void push_back(UniquePtr<T>&& value) {
    PW_ASSERT(value != nullptr);
    if (value.deallocator()->IsEqual(get_allocator())) {
      vector_.push_back(value.get());
      value.Release();
    } else {
      emplace_back(std::move(*value));
      value.Reset();
    }
  }

  void pop_back() {
    T* ptr = vector_.back();
    vector_.pop_back();
    Delete(ptr);
  }

  /// Removes the element at `pos` and returns it as a `UniquePtr`.
  [[nodiscard]] pw::UniquePtr<T> take(const_iterator pos) {
    T* ptr = const_cast<T*>(pos.operator->());
    // Remove from vector WITHOUT destroying the object.
    vector_.erase(pos.it_);
    return pw::UniquePtr<T>(ptr, vector_.get_allocator());
  }

  /// Constructs an element in-place at the end of the vector.
  ///
  /// This function constructs an object of type `U` (which defaults to `T`)
  /// using the provided arguments and the vector's allocator. The object is
  /// stored as a pointer in the vector.
  ///
  /// To construct a derived class object, specify the type as a template
  /// argument: `vec.emplace_back<Derived>(args...)`. The derived class must be
  /// trivially destructible or the base must have a virtual destructor.
  template <typename U = T, typename... Args>
  void emplace_back(Args&&... args) {
    PW_ASSERT(try_emplace_back<U>(std::forward<Args>(args)...));
  }

  /// Attempts to construct an element in-place at the end of the vector, as in
  /// `DynamicPtrVector<T>::emplace_back`.
  ///
  /// @retval true The item was successfully allocated and added to the vector.
  /// @retval false Allocation for either the item or the vector capacity
  /// failed.
  template <typename U = T, typename... Args>
  [[nodiscard]] bool try_emplace_back(Args&&... args) {
    return try_emplace<U>(end(), std::forward<Args>(args)...).has_value();
  }

  /// Constructs an element in-place at the specified position.
  ///
  /// This function constructs an object of type `U` (which defaults to `T`)
  /// using the provided arguments and the vector's allocator. The object is
  /// stored as a pointer in the vector.
  ///
  /// To construct a derived class object, specify the type as a template
  /// argument: `vec.emplace<Derived>(pos, args...)`. The derived class must be
  /// trivially destructible or the base must have a virtual destructor.
  template <typename U = T, typename... Args>
  iterator emplace(const_iterator pos, Args&&... args) {
    std::optional<iterator> res =
        try_emplace<U>(pos, std::forward<Args>(args)...);
    PW_ASSERT(res.has_value());
    return *res;
  }

  /// Attempts to construct an element in-place at the specified position, as in
  /// `DynamicPtrVector<T>::emplace`.
  ///
  /// @retval iterator to the new element if successful,
  /// @retval std::nullopt Allocation for either the item or the vector capacity
  /// failed.
  template <typename U = T, typename... Args>
  [[nodiscard]] std::optional<iterator> try_emplace(const_iterator pos,
                                                    Args&&... args) {
    static_assert(
        std::is_same_v<T, U> ||
            (std::is_base_of_v<T, U> && (std::is_trivially_destructible_v<U> ||
                                         std::has_virtual_destructor_v<T>)),
        "Objects must inherit from T and be either virtually or trivially "
        "destructible");
    // Get the index since try_reserve() invalidates iterators.
    const auto index = std::distance(vector_.cbegin(), pos.it_);

    if (!vector_.try_reserve(vector_.size() + 1)) {
      return std::nullopt;
    }

    if (T* ptr = vector_.get_allocator().template New<U>(
            std::forward<Args>(args)...);
        ptr != nullptr) {
      // Skip has_value() check; try_insert() will succeed due to try_reserve().
      return iterator(*vector_.try_insert(vector_.begin() + index, ptr));
    }
    return std::nullopt;
  }

  iterator insert(const_iterator pos, const T& value) {
    return emplace(pos, value);
  }
  iterator insert(const_iterator pos, T&& value) {
    return emplace(pos, std::move(value));
  }

  /// Inserts a new element by taking ownership of the object in `value`.
  ///
  /// If `value` was allocated using this vector's allocator, the pointer is
  /// transferred directly without reallocation. Otherwise, a new object is
  /// move-constructed from `*value`.
  iterator insert(const_iterator pos, UniquePtr<T>&& value) {
    PW_ASSERT(value != nullptr);
    if (value.deallocator()->IsEqual(get_allocator())) {
      iterator it = iterator(vector_.insert(pos.it_, value.get()));
      value.Release();
      return it;
    }

    iterator it = emplace(pos, std::move(*value));
    value.Reset();
    return it;
  }

  [[nodiscard]] std::optional<iterator> try_insert(const_iterator pos,
                                                   const T& value) {
    return try_emplace(pos, value);
  }

  iterator erase(const_iterator pos) { return erase(pos, pos + 1); }

  iterator erase(const_iterator first, const_iterator last) {
    // DynamicVector::erase moves elements to fill the gap, so delete first.
    for (auto it = first; it != last; ++it) {
      Delete(const_cast<T*>(it.operator->()));
    }
    return iterator(vector_.erase(first.it_, last.it_));
  }

  void clear() {
    for (T* ptr : vector_) {
      Delete(ptr);
    }
    vector_.clear();
  }

  /// @copydoc DynamicDeque::reset
  void reset() {
    clear();
    vector_.reset();
  }

  /// Swaps the contents of this `DynamicPtrVector` with another one.
  void swap(DynamicPtrVector& other) { vector_.swap(other.vector_); }

  /// Swaps two indices within this vector. This is a trivial swap of two
  /// pointers; no move assignments or allocations occur.
  void swap(size_type first, size_type second) {
    std::swap(vector_[first], vector_[second]);
  }

  /// Swaps two items within this vector. This is a trivial swap of two
  /// pointers; no move assignments or allocations occur.
  void swap(const_iterator first, const_iterator second) {
    std::swap(const_cast<pointer&>(*first.it_),
              const_cast<pointer&>(*second.it_));
  }

 private:
  void Delete(T* ptr) { vector_.get_allocator().Delete(ptr); }

  DynamicVector<T*, SizeType> vector_;
};

/// @}

}  // namespace pw
