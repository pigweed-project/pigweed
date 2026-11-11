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
  /// Constructs a tracking allocator without setting its wrapped allocator.
  ///
  /// `Init()` must be called with the wrapped allocator before any allocation
  /// methods are invoked. Can be used with `PW_CONSTINIT` for two-phase
  /// initialization regardless of which metrics `MetricsType` enables.
  ///
  /// @param[in] token Tokenized name of this allocator's metric group.
  /// @param[in] capabilities Capabilities exposed by this allocator.
  constexpr explicit TrackingAllocator(
      metric::Token token, const Capabilities& capabilities) noexcept
      : Base(capabilities), metrics_(token) {}

  /// Constructs a tracking allocator wrapping `allocator` with explicit
  /// `capabilities`.
  ///
  /// Unlike `TrackingAllocator(metric::Token, Allocator&)`, this constructor
  /// does not read `allocator.capabilities()`, so it can be used with
  /// `PW_CONSTINIT` for single-phase constant initialization when wrapping a
  /// separate static allocator and `MetricsType` only enables counter metrics
  /// (i.e. neither `requested_bytes` nor `allocated_bytes` is enabled). When
  /// `requested_bytes` or `allocated_bytes` is enabled, use two-phase
  /// initialization via `TrackingAllocator(token, capabilities)` and `Init()`.
  ///
  /// @param[in] token Tokenized name of this allocator's metric group.
  /// @param[in,out] allocator Wrapped allocator to forward requests to. Must
  /// remain valid for the lifetime of this object.
  /// @param[in] capabilities Capabilities exposed by this allocator.
  constexpr TrackingAllocator(metric::Token token,
                              Allocator& allocator,
                              const Capabilities& capabilities)
      : TrackingAllocator(token, capabilities) {
    Init(allocator);
  }

  /// Constructs a tracking allocator wrapping `allocator` and inheriting
  /// `allocator.capabilities()`.
  ///
  /// @param[in] token Tokenized name of this allocator's metric group.
  /// @param[in,out] allocator Wrapped allocator to forward requests to. Must
  /// remain valid for the lifetime of this object.
  constexpr TrackingAllocator(metric::Token token,
                              Allocator& allocator) noexcept
      : TrackingAllocator(token, allocator, allocator.capabilities()) {}

  /// Constructs a tracking allocator without setting its wrapped allocator and
  /// registers its `metric_group()` in `groups` (for example
  /// `pw::metric::global_groups` or `parent_group.children()`).
  ///
  /// `Init()` must be called with the wrapped allocator before any allocation
  /// methods are invoked. Can be used with `PW_CONSTINIT` inside a composite
  /// struct containing `groups` regardless of which metrics `MetricsType`
  /// enables.
  ///
  /// @param[in] token Tokenized name of this allocator's metric group.
  /// @param[in] capabilities Capabilities exposed by this allocator.
  /// @param[in,out] groups Metric group list to register `metric_group()` in.
  ///
  /// @pre `groups` must already be initialized, and access to `groups` during
  /// construction and destruction must be externally synchronized.
  constexpr TrackingAllocator(metric::Token token,
                              const Capabilities& capabilities,
                              metric::GroupList& groups) noexcept
      : Base(capabilities), metrics_(token, groups) {}

  /// Constructs a tracking allocator wrapping `allocator` with explicit
  /// `capabilities` and registers its `metric_group()` in `groups`.
  ///
  /// Does not read `allocator.capabilities()`, so it can be used with
  /// `PW_CONSTINIT` inside a composite struct containing `groups` when wrapping
  /// a separate static allocator and `MetricsType` only enables counter
  /// metrics.
  ///
  /// @param[in] token Tokenized name of this allocator's metric group.
  /// @param[in,out] allocator Wrapped allocator to forward requests to. Must
  /// remain valid for the lifetime of this object.
  /// @param[in] capabilities Capabilities exposed by this allocator.
  /// @param[in,out] groups Metric group list to register `metric_group()` in.
  ///
  /// @pre `groups` must already be initialized, and access to `groups` during
  /// construction and destruction must be externally synchronized.
  constexpr TrackingAllocator(metric::Token token,
                              Allocator& allocator,
                              const Capabilities& capabilities,
                              metric::GroupList& groups)
      : TrackingAllocator(token, capabilities, groups) {
    Init(allocator);
  }

  /// Constructs a tracking allocator wrapping `allocator` and registers its
  /// `metric_group()` in `groups` (for example `pw::metric::global_groups` or
  /// `parent_group.children()`).
  ///
  /// @param[in] token Tokenized name of this allocator's metric group.
  /// @param[in,out] allocator Wrapped allocator to forward requests to. Must
  /// remain valid for the lifetime of this object.
  /// @param[in,out] groups Metric group list to register `metric_group()` in.
  ///
  /// @pre `groups` must already be initialized (`pw::metric::global_groups` is
  /// `PW_CONSTINIT` and safe during dynamic static initialization; avoid
  /// passing `children()` of a dynamically initialized global `Group` in
  /// another translation unit), and access to `groups` during construction and
  /// destruction must be externally synchronized.
  constexpr TrackingAllocator(metric::Token token,
                              Allocator& allocator,
                              metric::GroupList& groups)
      : TrackingAllocator(token, allocator, allocator.capabilities(), groups) {}

  /// Constructs a tracking allocator wrapping `parent` and adds its
  /// `metric_group()` as a child of `parent.metric_group()`.
  ///
  /// @param[in] token Tokenized name of this allocator's metric group.
  /// @param[in,out] parent Parent tracking allocator to wrap and register
  /// under. Must remain valid for the lifetime of this object.
  template <typename OtherMetrics>
  constexpr TrackingAllocator(metric::Token token,
                              TrackingAllocator<OtherMetrics>& parent,
                              const AddTrackingAllocatorAsChild&)
      : TrackingAllocator(token, parent, parent.metric_group().children()) {}

  /// Sets the wrapped allocator after two-phase construction and verifies that
  /// `allocator` supports all metrics enabled by `MetricsType`.
  ///
  /// @param[in,out] allocator Wrapped allocator to forward requests to. Must
  /// remain valid for the lifetime of this object.
  ///
  /// @pre No wrapped allocator has been set yet, and if `allocator` itself uses
  /// two-phase initialization (such as `SynchronizedAllocator`),
  /// `allocator.Init(...)` must be called first when `requested_bytes` or
  /// `allocated_bytes` is enabled.
  constexpr void Init(Allocator& allocator);

  /// Returns this tracking allocator's metric group.
  constexpr const metric::Group& metric_group() const {
    return metrics_.group();
  }
  /// Returns this tracking allocator's mutable metric group.
  constexpr metric::Group& metric_group() { return metrics_.group(); }

  /// Returns the metrics struct tracked by this allocator.
  constexpr const MetricsType& metrics() const { return metrics_.metrics(); }

  /// Requests to update out-of-band metrics, if any.
  ///
  /// See also `NoMetrics::UpdateDeferred`.
  void UpdateDeferred() { metrics_.UpdateDeferred(allocator()); }

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
  internal::Metrics<MetricsType> metrics_;
};

// Template method implementation.

template <typename MetricsType>
constexpr void TrackingAllocator<MetricsType>::Init(Allocator& allocator) {
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
