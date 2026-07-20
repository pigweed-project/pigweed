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

#include "pw_containers/intrusive_forward_list.h"
#include "pw_tokenizer/tokenize.h"

namespace pw::metric {

class UntypedMetric;
class Group;
class ResumableMetricWalker;
class MetricWalker;
class MetricService;

namespace internal {

/// Wraps a IntrusiveForwardList to limit the API for use with metrics.
template <typename T>
class ListWrapper {
 public:
  using element_type = typename IntrusiveForwardList<T>::element_type;
  using value_type = typename IntrusiveForwardList<T>::value_type;
  using size_type = typename IntrusiveForwardList<T>::size_type;
  using difference_type = typename IntrusiveForwardList<T>::difference_type;
  using reference = typename IntrusiveForwardList<T>::reference;
  using const_reference = typename IntrusiveForwardList<T>::const_reference;
  using pointer = typename IntrusiveForwardList<T>::pointer;
  using const_pointer = typename IntrusiveForwardList<T>::const_pointer;

  constexpr ListWrapper() = default;

  ~ListWrapper() { list_.clear(); }

  // Temporarily provide for backwards compatibility. Use push_front() since
  // order is not significant.
  void push_back(reference value) { list_.push_front(value); }
  void push_front(reference value) { list_.push_front(value); }

  // Expose specific methods required for metrics.
  [[nodiscard]] bool empty() const { return list_.empty(); }

  [[nodiscard]] size_t size() const {
    return static_cast<size_t>(std::distance(list_.begin(), list_.end()));
  }

  /// Iterates over the list and calls the functor `f` with a reference to each
  /// item.
  template <typename F>
  F for_each(F f) {
    for (auto& item : list_) {
      f(item);
    }
    return f;
  }

  /// Iterates over the list and calls the functor `f` with a const reference to
  /// each item.
  template <typename F>
  F for_each(F f) const {
    for (const auto& item : list_) {
      f(item);
    }
    return f;
  }

  /// Finds an item by name token.
  ///
  /// @returns A pointer to the first matching item, or nullptr if no match is
  /// found.
  pointer find(pw::tokenizer::Token token) {
    for (auto& item : list_) {
      if (item.name() == token) {
        return &item;
      }
    }
    return nullptr;
  }

  /// Finds an item by name token.
  ///
  /// @returns A const pointer to the first matching item, or nullptr if no
  /// match is found.
  const_pointer find(pw::tokenizer::Token token) const {
    for (const auto& item : list_) {
      if (item.name() == token) {
        return &item;
      }
    }
    return nullptr;
  }

 private:
  // Allow pw_metric classes to modify and directly access the list.
  friend UntypedMetric;
  friend Group;

  friend ResumableMetricWalker;
  friend MetricWalker;
  friend MetricService;

  using Item = typename IntrusiveForwardList<T>::Item;

  IntrusiveForwardList<T>& list() { return list_; }
  const IntrusiveForwardList<T>& list() const { return list_; }

  IntrusiveForwardList<T> list_;
};

}  // namespace internal

/// A list of metrics.
using MetricList = internal::ListWrapper<UntypedMetric>;

/// A list of metrics groups.
using GroupList = internal::ListWrapper<Group>;

}  // namespace pw::metric
