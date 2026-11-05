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

// IWYU pragma: private, include "pw_containers/inline_var_len_entry_queue.h"

#include <cstddef>
#include <iterator>
#include <type_traits>

namespace pw::containers::internal {

// Forward declarations.
template <typename T>
class VarLenEntry;

/// An iterator for accessing the contents of a VarLenEntry<T>.
///
/// @tparam   T   Element type of the entry.
template <typename T>
class VarLenEntryIterator {
 public:
  using difference_type = std::ptrdiff_t;
  using value_type = std::remove_cv_t<T>;
  using pointer = T*;
  using reference = T&;
  using iterator_category = std::forward_iterator_tag;

  constexpr VarLenEntryIterator() = default;

  constexpr VarLenEntryIterator(const VarLenEntryIterator&) = default;
  constexpr VarLenEntryIterator& operator=(const VarLenEntryIterator&) =
      default;

  constexpr VarLenEntryIterator& operator++() {
    index_ += 1;
    return *this;
  }
  constexpr VarLenEntryIterator operator++(int) {
    VarLenEntryIterator previous_value(*this);
    operator++();
    return previous_value;
  }

  constexpr VarLenEntryIterator& operator+=(difference_type n) {
    index_ += static_cast<size_t>(n);
    return *this;
  }

  constexpr reference operator*() const { return entry_->at(index_); }

  constexpr pointer operator->() const { return &entry_->at(index_); }

  friend constexpr VarLenEntryIterator operator+(const VarLenEntryIterator& it,
                                                 difference_type n) {
    return VarLenEntryIterator(*it.entry_, it.index_ + static_cast<size_t>(n));
  }

  friend constexpr VarLenEntryIterator operator+(
      difference_type n, const VarLenEntryIterator& it) {
    return VarLenEntryIterator(*it.entry_, it.index_ + static_cast<size_t>(n));
  }

  [[nodiscard]] friend constexpr bool operator==(
      const VarLenEntryIterator& lhs, const VarLenEntryIterator& rhs) {
    if (lhs.entry_ != nullptr && rhs.entry_ != nullptr) {
      return *lhs.entry_ == *rhs.entry_ && lhs.index_ == rhs.index_;
    }
    return lhs.entry_ == rhs.entry_;
  }
  [[nodiscard]] friend constexpr bool operator!=(
      const VarLenEntryIterator& lhs, const VarLenEntryIterator& rhs) {
    return !(lhs == rhs);
  }

 private:
  template <typename>
  friend class VarLenEntry;

  constexpr VarLenEntryIterator(const VarLenEntry<T>& entry, size_t index)
      : entry_(&entry), index_(index) {}

  const VarLenEntry<T>* entry_ = nullptr;
  size_t index_ = 0;
};

}  // namespace pw::containers::internal
