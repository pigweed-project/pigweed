// Copyright 2020 The Pigweed Authors
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

#include "pw_containers/intrusive_forward_list.h"

namespace pw::containers::internal {

/// The class named `IntrusiveList<T>` was originally singly-linked and much
/// closer to `IntrusiveForwardList<T>`. This class represents the original
/// behavior of that class in the following ways:
///
/// * Items automatically unlist themselves on destruction.
/// * Items may be made movable.
/// * Lists automatically clear themselves on destruction.
/// * `size`, `back`, and `push_back` methods are provided, despite being O(n).
///
/// TODO(b/362348318): This class is deprecated and not used for new code.
template <typename T>
class LegacyIntrusiveList : public IntrusiveForwardList<T> {
 private:
  using Base = IntrusiveForwardList<T>;

 public:
  class Item : public Base::Item {
   public:
    ~Item() { Base::Item::unlist(); }

   protected:
    constexpr explicit Item() = default;

    Item(Item&& other) : Item() { *this = std::move(other); }
    Item& operator=(Item&& other) {
      Base::replace(other);
      return *this;
    }
  };

  constexpr LegacyIntrusiveList() = default;

  template <typename Iterator>
  LegacyIntrusiveList(Iterator first, Iterator last) : Base(first, last) {}

  LegacyIntrusiveList(std::initializer_list<Item*> items)
      : Base(items.begin(), items.end()) {}

  ~LegacyIntrusiveList() {
    // The legacy intrusive list unlisted the sentinel item on destruction.
    Base::clear();
  }

  /// Returns a reference to the last element in the list.
  ///
  /// Undefined behavior if empty(). Runs in O(n) time.
  T& back() { return static_cast<T&>(*Base::list().before_end()); }

  /// Returns the number of items in the list.
  ///
  /// Runs in O(n) time.
  size_t size() const {
    return static_cast<size_t>(std::distance(Base::begin(), Base::end()));
  }

  /// Inserts an item at the end of the list.
  ///
  /// Runs in O(n) time.
  void push_back(T& item) { Base::list().push_back(item); }
};

}  // namespace pw::containers::internal
