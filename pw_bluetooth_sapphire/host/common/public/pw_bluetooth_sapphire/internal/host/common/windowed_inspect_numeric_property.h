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
#include <pw_assert/check.h>

#include <algorithm>
#include <optional>
#include <queue>

#include "pw_bluetooth_sapphire/internal/host/common/inspect.h"
#include "pw_bluetooth_sapphire/internal/host/common/smart_task.h"

namespace bt {

// This class is a utility for wrapping a numeric Inspect property, such as
// IntProperty or UintProperty, such that value updates are reversed after
// |expiry_duration|. This is useful for creating properties like "disconnects
// in the past 10 minutes". Note that this is not very space efficient and
// should not be used for properties that get updated extremely frequently.
//
// |NumericPropertyT| is an inspect property like inspect::IntProperty, and
// |ValueT| is the internal value like int64_t. Use the convenience types below
// to simplify type declaration.
template <typename NumericPropertyT, typename ValueT>
class WindowedInspectNumericProperty {
 public:
  // Maximum number of time-bucketed entries retained in the expiry queue.
  // 60 buckets (e.g. 10-second intervals over the default 10-minute window)
  // provides sufficient expiry granularity for rolling diagnostic counters
  // while keeping the queue within a single std::deque allocation block.
  static constexpr size_t kMaxPendingValues = 60;

  // |expiry_duration| is the time duration after which changes should be
  // reversed. |min_resolution| is the smallest duration between changes such
  // that they are reversed independently. Changes closer to this interval may
  // be batched together for expiry, biased towards earlier expiry than
  // |expiry_duration|. If 0 (default) or smaller than
  // |expiry_duration| / |kMaxPendingValues|, |min_resolution| is clamped to
  // bound queue growth.
  explicit WindowedInspectNumericProperty(
      pw::async::Dispatcher& pw_dispatcher,
      pw::chrono::SystemClock::duration expiry_duration =
          std::chrono::minutes(10),
      pw::chrono::SystemClock::duration min_resolution =
          pw::chrono::SystemClock::duration())
      : expiry_duration_(expiry_duration),
        min_resolution_(
            std::max({min_resolution,
                      expiry_duration / static_cast<int64_t>(kMaxPendingValues),
                      pw::chrono::SystemClock::duration(1)})),
        expiry_task_(pw_dispatcher,
                     [this](pw::async::Context /*ctx*/, pw::Status status) {
                       if (!status.ok()) {
                         return;
                       }

                       PW_CHECK(!values_.empty());
                       auto oldest_value = values_.front();
                       // Undo expiring value.
                       property_.Subtract(oldest_value.second);
                       values_.pop();
                       StartExpiryTimeout();
                     }),
        pw_dispatcher_(pw_dispatcher) {}
  virtual ~WindowedInspectNumericProperty() = default;

  // Allow moving, disallow copying.
  WindowedInspectNumericProperty(const WindowedInspectNumericProperty& other) =
      delete;
  WindowedInspectNumericProperty(
      WindowedInspectNumericProperty&& other) noexcept = default;
  WindowedInspectNumericProperty& operator=(
      const WindowedInspectNumericProperty& other) = delete;
  WindowedInspectNumericProperty& operator=(
      WindowedInspectNumericProperty&& other) noexcept = default;

  // Set the underlying inspect property, and reset the expiry timer. This is
  // used by the convenience types that implement AttachInspect() below.
  void SetProperty(NumericPropertyT property) {
    property_ = std::move(property);
    expiry_task_.Cancel();
    // Clear queue without running pop() in a loop.
    values_ = decltype(values_)();
  }

  // Create an inspect property named "name" as a child of "node".
  //
  // AttachInspect is only supported for the convenience types declared below.
  virtual void AttachInspect(::inspect::Node& /*node*/, std::string /*name*/) {
    PW_CHECK(false, "AttachInspect not implemented for NumericPropertyT");
  }

  // Add the given value to the value of this numeric metric.
  void Add(ValueT value) {
    property_.Add(value);
    pw::chrono::SystemClock::time_point now = pw_dispatcher_.now();
    if (!values_.empty()) {
      // If the most recent change's age is less than |min_resolution_|, or the
      // queue has reached its capacity limit, merge this change to it.
      if (now < values_.back().first + min_resolution_ ||
          values_.size() >= kMaxPendingValues) {
        values_.back().second += value;
        return;
      }
    }
    values_.push({now, value});
    StartExpiryTimeout();
  }

  // Return true if property is valid.
  explicit operator bool() { return property_; }

 private:
  void StartExpiryTimeout() {
    if (values_.empty() || expiry_task_.is_pending()) {
      return;
    }

    auto oldest_value = values_.front();
    pw::chrono::SystemClock::time_point oldest_value_time = oldest_value.first;
    pw::chrono::SystemClock::time_point expiry_time =
        expiry_duration_ + oldest_value_time;
    expiry_task_.PostAt(expiry_time);
  }

  // This is not very space efficient, requiring a node for every value during
  // the expiry_duration_.
  std::queue<std::pair<pw::chrono::SystemClock::time_point, ValueT>> values_;

  NumericPropertyT property_;
  pw::chrono::SystemClock::duration expiry_duration_;
  pw::chrono::SystemClock::duration min_resolution_;

  using SelfT = WindowedInspectNumericProperty<NumericPropertyT, ValueT>;
  SmartTask expiry_task_;
  pw::async::Dispatcher& pw_dispatcher_;
};

// Convenience WindowedInspectNumericProperty types:

#define CREATE_WINDOWED_TYPE(property_t, inner_t)                              \
  class WindowedInspect##property_t##Property final                            \
      : public WindowedInspectNumericProperty<::inspect::property_t##Property, \
                                              inner_t> {                       \
   public:                                                                     \
    using WindowedInspectNumericProperty<                                      \
        ::inspect::property_t##Property,                                       \
        inner_t>::WindowedInspectNumericProperty;                              \
    void AttachInspect(::inspect::Node& node, std::string name) override {     \
      this->SetProperty(node.Create##property_t(name, inner_t()));             \
    }                                                                          \
  };

// WindowedInspectIntProperty
CREATE_WINDOWED_TYPE(Int, int64_t)
// WindowedInspectUintProperty
CREATE_WINDOWED_TYPE(Uint, uint64_t)

#undef CREATE_WINDOWED_TYPE

}  // namespace bt
