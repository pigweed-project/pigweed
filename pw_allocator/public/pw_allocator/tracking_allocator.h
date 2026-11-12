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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "pw_allocator/capability.h"
#include "pw_allocator/forwarding_allocator.h"
#include "pw_allocator/metrics.h"
#include "pw_assert/assert.h"
#include "pw_metric/metric.h"
#include "pw_preprocessor/compiler.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::allocator {

/// @submodule{pw_allocator,forwarding}

/// This tag type is used to explicitly select the constructor which adds
/// the tracking allocator's metrics group as a child of the info
/// allocator it is wrapping.
static constexpr struct AddTrackingAllocatorAsChild {
} kAddTrackingAllocatorAsChild = {};

/// Wraps an `Allocator` and records details of its usage.
///
/// Metric collection is performed using the provided template parameter type.
/// Callers can not instantiate this class directly, as it lacks a public
/// constructor. Instead, callers should use derived classes which provide the
/// template parameter type, such as `TrackingAllocator` which uses the
/// default metrics implementation, or `TrackingAllocatorForTest` which
/// always uses the real metrics implementation.
///
/// If the underlying allocator does not have the
/// `kImplementsGetAllocatedLayout` capability, the peak allocation metric may
/// be lower than the actual peak allocation value. This is because the
/// tracking allocator cannot account for the overlap in memory usage during
/// reallocation when it occurs as a "move-and-copy" operation.
template <typename MetricsType>
class TrackingAllocator : public ForwardingAllocator {
 private:
  using Base = ForwardingAllocator;

 public:
  explicit TrackingAllocator(metric::Token token,
                             const Capabilities& capabilities) noexcept
      : Base(capabilities), metrics_(token) {}

  TrackingAllocator(metric::Token token, Allocator& allocator) noexcept
      : TrackingAllocator(token, allocator.capabilities()) {
    Init(allocator);
  }

  template <typename OtherMetrics>
  TrackingAllocator(metric::Token token,
                    TrackingAllocator<OtherMetrics>& parent,
                    const AddTrackingAllocatorAsChild&)
      : TrackingAllocator(token, parent) {
    parent.metric_group().Add(metric_group());
  }

  void Init(Allocator& allocator);

  const metric::Group& metric_group() const { return metrics_.group(); }
  metric::Group& metric_group() { return metrics_.group(); }

  const MetricsType& metrics() const { return metrics_.metrics(); }

  /// Requests to update out-of-band metrics, if any.
  ///
  /// See also `NoMetrics::UpdateDeferred`.
  void UpdateDeferred() const { metrics_.UpdateDeferred(allocator()); }

 protected:
  /// @copydoc Allocator::Allocate
  void* DoAllocate(Layout layout) override;

  /// @copydoc Allocator::Deallocate
  void DoDeallocate(void* ptr) override;

  /// @copydoc Allocator::Resize
  bool DoResize(void* ptr, size_t new_size) override;

  /// @copydoc Allocator::DoBeforeReallocate
  bool DoBeforeReallocate(void* ptr, Layout new_layout) override;

  /// @copydoc Allocator::DoAfterReallocateDone
  void DoAfterReallocateDone(Layout new_layout, void* new_ptr) override;

 private:
  mutable internal::Metrics<MetricsType> metrics_;
};

// Template method implementation.

template <typename MetricsType>
void TrackingAllocator<MetricsType>::Init(Allocator& allocator) {
  // Check that allocator supports requested metrics.
  if constexpr (MetricsType::requested_bytes_enabled()) {
    PW_ASSERT(GetRequestedLayout(allocator, nullptr).status() !=
              Status::Unimplemented());
  }
  if constexpr (MetricsType::allocated_bytes_enabled()) {
    PW_ASSERT(allocator.GetAllocated() != kGetAllocatedUnsupported ||
              allocator.GetAllocatedBytes().has_value());
  }
  Base::Init(allocator);
}

template <typename MetricsType>
void* TrackingAllocator<MetricsType>::DoAllocate(Layout layout) {
  std::optional<size_t> pre_allocated;
  if constexpr (MetricsType::allocated_bytes_enabled()) {
    pre_allocated = DoGetAllocatedBytes();
  }
  auto* new_ptr = Base::DoAllocate(layout);
  if (new_ptr == nullptr) {
    metrics_.RecordFailure(layout.size());
    return nullptr;
  }
  metrics_.IncrementAllocations();
  if constexpr (MetricsType::requested_bytes_enabled()) {
    metrics_.ModifyRequested(layout.size(), 0);
  }
  if constexpr (MetricsType::allocated_bytes_enabled()) {
    auto post_allocated = DoGetAllocatedBytes();
    metrics_.ModifyAllocated(*post_allocated, *pre_allocated);
  }
  return new_ptr;
}

template <typename MetricsType>
void TrackingAllocator<MetricsType>::DoDeallocate(void* ptr) {
  std::optional<size_t> requested;
  if constexpr (MetricsType::requested_bytes_enabled()) {
    if (auto result = GetRequestedLayout(ptr); result.ok()) {
      requested = result->size();
    }
  }
  std::optional<size_t> pre_allocated;
  if constexpr (MetricsType::allocated_bytes_enabled()) {
    pre_allocated = DoGetAllocatedBytes();
  }
  Base::DoDeallocate(ptr);
  metrics_.IncrementDeallocations();
  if constexpr (MetricsType::requested_bytes_enabled()) {
    if (requested.has_value()) {
      metrics_.ModifyRequested(0, *requested);
    }
  }
  if constexpr (MetricsType::allocated_bytes_enabled()) {
    auto post_allocated = DoGetAllocatedBytes();
    metrics_.ModifyAllocated(*post_allocated, *pre_allocated);
  }
}

template <typename MetricsType>
bool TrackingAllocator<MetricsType>::DoResize(void* ptr, size_t new_size) {
  std::optional<size_t> old_size;
  if constexpr (MetricsType::requested_bytes_enabled()) {
    if (auto result = GetRequestedLayout(ptr); result.ok()) {
      old_size = result->size();
    }
  }
  std::optional<size_t> pre_allocated;
  if constexpr (MetricsType::allocated_bytes_enabled()) {
    pre_allocated = DoGetAllocatedBytes();
  }
  if (!Base::DoResize(ptr, new_size)) {
    metrics_.RecordFailure(new_size);
    return false;
  }
  metrics_.IncrementResizes();
  if constexpr (MetricsType::requested_bytes_enabled()) {
    if (old_size.has_value()) {
      metrics_.ModifyRequested(new_size, *old_size);
    }
  }
  if constexpr (MetricsType::allocated_bytes_enabled()) {
    auto post_allocated = DoGetAllocatedBytes();
    metrics_.ModifyAllocated(*post_allocated, *pre_allocated);
  }
  return true;
}

template <typename MetricsType>
[[nodiscard]] bool TrackingAllocator<MetricsType>::DoBeforeReallocate(
    void* ptr, Layout new_layout) {
  metrics_.set_reallocating(true);
  return Base::DoBeforeReallocate(ptr, new_layout);
}

template <typename MetricsType>
void TrackingAllocator<MetricsType>::DoAfterReallocateDone(Layout new_layout,
                                                           void* new_ptr) {
  Base::DoAfterReallocateDone(new_layout, new_ptr);
  metrics_.set_reallocating(false);
  if (new_ptr == nullptr) {
    metrics_.RecordFailure(new_layout.size());
  } else {
    metrics_.IncrementReallocations();
  }
}

/// @}

}  // namespace pw::allocator
