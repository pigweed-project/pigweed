// Copyright 2024 The Pigweed Authors
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

#include <utility>

#include "pw_containers/internal/intrusive_item.h"

namespace pw::containers::internal {

// Forward declaration for friending.
template <typename>
class GenericIntrusiveList;

class GenericIntrusiveQueue;

/// Base class for items that can be included in intrusive lists.
///
/// This class provides a pointer to the next item in a list and provides a
/// common interface for lists, including a way to get the previous item. When
/// not part of a list, an item must point to itself for its next and previous
/// items.
template <typename Derived>
class IntrusiveListItemBase {
 public:
  /// Destructor.
  ///
  /// Subclasses MUST call `unlist` in their destructors, since unlisting may
  /// require calling derived class methods.
  ~IntrusiveListItemBase() {
    if (!unlisted()) {
      CrashIntrusiveItemIsContained();
    }
  }

  /// Items are not copyable.
  IntrusiveListItemBase(const IntrusiveListItemBase&) = delete;
  IntrusiveListItemBase& operator=(const IntrusiveListItemBase&) = delete;

 protected:
  constexpr IntrusiveListItemBase() : next_(derived()) {}

  /// Returns whether this object is not part of a list.
  ///
  /// This is O(1) whether the object is in a list or not.
  bool unlisted() const { return this == next_; }

  /// Unlink this from the list it is apart of, if any.
  ///
  /// Specifying prev saves calling previous(), which may require looping around
  /// the cycle. This is always O(1) with `previous`, and may be O(n) without.
  void unlist(Derived* prev = nullptr) {
    if (prev == nullptr) {
      prev = previous();
    }
    prev->next_ = next_;
    next_->set_previous(prev);
    set_previous(derived());
    next_ = derived();
  }

  /// Replace this item with another.
  ///
  /// After this call, other will be unlisted and this item will have taken its
  /// place in its list, if any.
  ///
  /// Example: Given items A though H and lists L1=[A, B, C] and L2=[D, E, F]
  /// with G and H unlisted, then the result of calling `B.replace(E)` and
  ///`G.replace(C)` would be L1=[A, G] and L2=[D, B, F] with C, E and H
  /// unlisted.
  void replace(Derived& other) {
    // Remove `this` object from its current list.
    if (!unlisted()) {
      unlist();
    }
    // If `other` is listed, remove it from its list and put `this` in its
    // place.
    if (!other.unlisted()) {
      Derived* prev = other.previous();
      other.unlist(prev);
      next_ = prev->next_;
      prev->next_ = derived();
      set_previous(prev);
      next_->set_previous(derived());
    }
  }

 private:
  friend Derived;

  template <typename>
  friend class GenericIntrusiveList;

  friend class GenericIntrusiveQueue;

  template <typename, typename>
  friend class Incrementable;

  template <typename, typename>
  friend class Decrementable;

  constexpr Derived* derived() { return static_cast<Derived*>(this); }
  constexpr const Derived* derived() const {
    return static_cast<const Derived*>(this);
  }

  /// Return the next item in the list.
  Derived* next() const { return next_; }

  /// Return the previous item in the list.
  ///
  /// Derived classes that do not store a pointer to the previous item may find
  /// it by looping around the cycle. This approach is O(n), where "n" is the
  /// number of items in this object's list.
  ///
  /// Unlisted items are self-cycles, i.e. this method will return `this`.
  Derived* previous() const { return derived()->DoGetPrevious(); }

  /// Stores the pointer to the previous item, if supported.
  void set_previous(Derived* prev) {
    static_cast<Derived*>(this)->DoSetPrevious(prev);
  }

  /// Next item in list. Unlisted items are self-cycles, i.e. next_ == `this`.
  Derived* next_;
};

/// Base class for items in singly-linked lists.
class IntrusiveForwardListItem
    : public IntrusiveListItemBase<IntrusiveForwardListItem> {
 protected:
  constexpr IntrusiveForwardListItem() = default;

 private:
  friend IntrusiveListItemBase<IntrusiveForwardListItem>;

  template <typename>
  friend class GenericIntrusiveList;

  IntrusiveForwardListItem* DoGetPrevious() const {
    IntrusiveForwardListItem* prev = next_;
    while (prev->next_ != this) {
      prev = prev->next_;
    }
    return prev;
  }

  void DoSetPrevious(IntrusiveForwardListItem*) {}
};

/// Base class for items in doubly-linked lists.
class IntrusiveListItem : public IntrusiveListItemBase<IntrusiveListItem> {
 protected:
  constexpr IntrusiveListItem() = default;

 private:
  friend IntrusiveListItemBase<IntrusiveListItem>;

  template <typename>
  friend class GenericIntrusiveList;

  IntrusiveListItem* DoGetPrevious() const { return prev_; }
  void DoSetPrevious(IntrusiveListItem* prev) { prev_ = prev; }

  IntrusiveListItem* prev_ = this;
};

}  // namespace pw::containers::internal
