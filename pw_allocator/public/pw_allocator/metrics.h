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

#include "pw_allocator/allocator.h"
#include "pw_metric/metric.h"
#include "pw_status/status_with_size.h"

namespace pw::allocator {

/// @submodule{pw_allocator,util}

/// Applies the given macro to each metric.
///
/// This macro helps ensure consistency between definitions that involve every
/// metric.
///
/// The recognized pw_allocator metrics are:
///
/// - Metrics to track the current, peak, and cumulative number of bytes
///   requested to be allocated, respectively.
///   - requested_bytes
///   - peak_requested_bytes
///   - cumulative_requested_bytes
///
/// - Metrics to track the current, peak, and cumulative number of bytes
///   actually allocated, respectively.
///   - allocated_bytes
///   - peak_allocated_bytes
///   - cumulative_allocated_bytes
///
/// - Metrics to track the number of successful calls to each interface method.
///   - num_allocations
///   - num_deallocations
///   - num_resizes
///   - num_reallocations
///
/// - Metrics to track block-related numbers, including the number of free
///   blocks, as well as the sizes of the smallest and largest blocks.
///   - num_free_blocks
///   - smallest_free_block_size
///   - largest_free_block_size
///
/// - Metrics to tracks the number of interface calls that failed, and the
///   number of bytes requested in those calls.
///   - num_failures
///   - unfulfilled_bytes
#define PW_ALLOCATOR_METRICS_FOREACH(fn) \
  fn(requested_bytes);                   \
  fn(peak_requested_bytes);              \
  fn(cumulative_requested_bytes);        \
  fn(allocated_bytes);                   \
  fn(peak_allocated_bytes);              \
  fn(cumulative_allocated_bytes);        \
  fn(num_allocations);                   \
  fn(num_deallocations);                 \
  fn(num_resizes);                       \
  fn(num_reallocations);                 \
  fn(num_free_blocks);                   \
  fn(smallest_free_block_size);          \
  fn(largest_free_block_size);           \
  fn(num_failures);                      \
  fn(unfulfilled_bytes)

#define PW_ALLOCATOR_ABSORB_SEMICOLON() static_assert(true)

/// Declares the names of metrics used by `pw::allocator::Metrics`.
///
/// Only the names of declared metrics may be passed to
/// ``PW_ALLOCATOR_METRICS_ENABLE`` as part of a metrics struct definition.
///
/// This macro generates trait types that are used by ``Metrics`` to
/// conditionally include metric-related code.
///
/// Note: if enabling ``peak_allocated_bytes` or `cumulative_allocated_bytes`,
/// `allocated_bytes` should also be enabled.
#define PW_ALLOCATOR_METRICS_DECLARE(metric_name)                 \
  static constexpr bool has_##metric_name() { return false; }     \
  ::pw::StatusWithSize get_##metric_name() const {                \
    return ::pw::StatusWithSize::NotFound();                      \
  }                                                               \
  static constexpr bool metric_name##_enabled() { return false; } \
  PW_ALLOCATOR_ABSORB_SEMICOLON()

/// A predefined metric struct that enables no allocator metrics.
///
/// This struct declares all valid metrics, but enables none of them. Other
/// metrics struct MUST derive from this type, and should use
/// `PW_ALLOCATOR_METRICS_INCLUDE` and/or `PW_ALLOCATOR_METRICS_ENABLE`.
struct NoMetrics {
  PW_ALLOCATOR_METRICS_FOREACH(PW_ALLOCATOR_METRICS_DECLARE);

  /// Updates metrics by querying an allocator directly.
  ///
  /// Metrics are typically updated by an allocator when the pw::Allocator API
  /// is invoked. In some cases, there may be metrics that cannot be determined
  /// at the interface or are too expensive to do so, e.g. determining the
  /// smallest free block. This method provides a way for metrics structs to
  /// request these values on-demand.
  ///
  /// For this empty base struct, this is simply a no-op.
  void UpdateDeferred(const pw::Allocator&) {}
};

#undef PW_ALLOCATOR_METRICS_DECLARE

/// Includes a metric in a metrics struct.
///
/// The ``pw::allocator::TrackingAllocator`` template takes a struct that
/// includes zero or more of the metrics enumerated by
/// ``PW_ALLOCATOR_METRICS_DECLARE```.
///
/// This struct may be one of ``AllMetrics`` or ``NoMetrics``, or may be a
/// custom struct that selects a subset of metrics.
///
/// A metric that is only included, and not enabled, is expected to only be
/// updated when `UpdatedDeferred` is invoked.
#define PW_ALLOCATOR_METRICS_INCLUDE(metric_name)                  \
  static_assert(!::pw::allocator::NoMetrics::has_##metric_name()); \
  static constexpr bool has_##metric_name() { return true; }       \
  inline ::pw::StatusWithSize get_##metric_name() const {          \
    return ::pw::StatusWithSize(metric_name.value());              \
  }                                                                \
  PW_METRIC(metric_name, #metric_name, 0U)

/// Enables a metric for in a metrics struct.
///
/// The ``pw::allocator::TrackingAllocator`` template takes a struct that
/// enables zero or more of the metrics enumerated by
/// ``PW_ALLOCATOR_METRICS_DECLARE```.
///
/// This struct may be one of ``AllMetrics`` or ``NoMetrics``, or may be a
/// custom struct that selects a subset of metrics.
///
/// A metric that is enabled is expected to be updated automatically by using a
/// tracking allocator's API.
///
/// Example:
/// @code{.cpp}
///   struct MyMetrics {
///     PW_ALLOCATOR_METRICS_ENABLE(allocated_bytes);
///     PW_ALLOCATOR_METRICS_ENABLE(peak_allocated_bytes);
///     PW_ALLOCATOR_METRICS_INCLUDE(num_free_blocks);
///
///     void UpdateDeferred(const Allocator& allocator);
///   };
/// @endcode
#define PW_ALLOCATOR_METRICS_ENABLE(metric_name)                 \
  static constexpr bool metric_name##_enabled() { return true; } \
  PW_ALLOCATOR_METRICS_INCLUDE(metric_name)

namespace internal {

/// A metrics type that enables all metrics for testing.
///
/// Warning! Do not use in production code. As metrics are added to it later,
/// code using this struct may unexpectedly grow in code size, memory usage,
/// and/or performance overhead.
struct AllMetrics : public NoMetrics {
  PW_ALLOCATOR_METRICS_FOREACH(PW_ALLOCATOR_METRICS_ENABLE);
};

/// Returns whether any metric is enabled. If not, metric collection can be
/// skipped.
template <typename MetricsType>
constexpr bool AnyEnabled();

/// Copies the values enabled for a given `MetricsType` from `src` to `dst`.
template <typename MetricsType>
void CopyMetrics(const MetricsType& src, MetricsType& dst);

/// Non-templated base class for tracking allocator metrics encapsulation.
class GenericMetrics {
 public:
  ~GenericMetrics() = default;

  const metric::Group& group() const { return group_; }
  metric::Group& group() { return group_; }

  [[nodiscard]] constexpr bool is_reallocating() const { return reallocating_; }
  constexpr void set_reallocating(bool reallocating) {
    reallocating_ = reallocating;
  }

 protected:
  explicit GenericMetrics(metric::Token token) : group_(token) {}

 private:
  metric::Group group_;

  bool reallocating_ = false;
};

/// Encapsulates the metrics struct for ``pw::allocator::TrackingAllocator``.
///
/// This class uses the type traits from ``PW_ALLOCATOR_METRICS_DECLARE`` to
/// conditionally include or exclude code to update metrics based on calls to
/// the ``pw::Allocator`` API. This minimizes code size without adding
/// additional conditions to be evaluated at runtime.
///
/// @tparam   MetricsType   The struct defining which metrics are enabled.
template <typename MetricsType>
class Metrics final : public internal::GenericMetrics {
 public:
  constexpr Metrics(metric::Token token);
  ~Metrics() = default;

  const MetricsType& metrics() const { return metrics_; }

  /// Updates how much memory was requested and successfully allocated.
  ///
  /// This will update the current, peak, and cumulative amounts of memory
  /// requests that were satisfied by an allocator.
  ///
  /// @param  increase         How much memory was requested to be allocated.
  /// @param  decrease         How much memory was requested to be freed.
  void ModifyRequested(size_t increase, size_t decrease);

  /// Updates how much memory is allocated.
  ///
  /// This will update the current, peak, and cumulative amounts of memory that
  /// has been actually allocated or freed. This method acts as if it frees
  /// memory before allocating. If a routine suchas `Reallocate` allocates
  /// before freeing, the update should be separated into two calls, e.g.
  ///
  /// @code{.cpp}
  /// ModifyAllocated(increase, 0);
  /// ModifyAllocated(0, decrease);
  /// @endcode
  ///
  /// The given `std::optional` parameters be dereferenced if and only if the
  /// "allocated_bytes" metric is enabled.
  ///
  /// @param  increase         How much memory was allocated.
  /// @param  decrease         How much memory was freed.
  void ModifyAllocated(size_t increase, size_t decrease);

  /// Records that a call to `Allocate` was made.
  void IncrementAllocations();

  /// Records that a call to `Deallocate` was made.
  void IncrementDeallocations();

  /// Records that a call to `Resize` was made.
  void IncrementResizes();

  /// Records that a call to `Reallocate` was made.
  void IncrementReallocations();

  /// Records that a call to `Allocate`, `Resize`, or `Reallocate` failed.
  ///
  /// This may indicated that memory becoming exhausted and/or highly
  /// fragmented.
  ///
  /// @param  requested         How much memory was requested in the failed
  ///                           call.
  void RecordFailure(size_t requested);

  /// Updates metrics by querying an allocator directly.
  ///
  /// See also `NoMetrics::UpdateDeferred`.
  void UpdateDeferred(const pw::Allocator& allocator) {
    metrics_.UpdateDeferred(allocator);
  }

 private:
  MetricsType metrics_;
};

/// @}

// Template and inline method implementations.

/// Helper method for converting `size_t`s to `uint32_t`s.
inline uint32_t ClampU32(size_t size) {
  return static_cast<uint32_t>(std::min(
      size, static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
}

template <typename MetricsType>
constexpr bool AnyEnabled() {
#define PW_ALLOCATOR_RETURN_IF_ENABLED(metric_name) \
  if (MetricsType::metric_name##_enabled()) {       \
    return true;                                    \
  }                                                 \
  PW_ALLOCATOR_ABSORB_SEMICOLON()

  PW_ALLOCATOR_METRICS_FOREACH(PW_ALLOCATOR_RETURN_IF_ENABLED);
  return false;

#undef PW_ALLOCATOR_RETURN_IF_ENABLED
}

template <typename MetricsType>
void CopyMetrics(const MetricsType& src, MetricsType& dst) {
#define PW_ALLOCATOR_COPY_METRIC(metric_name)       \
  if constexpr (MetricsType::has_##metric_name()) { \
    dst.metric_name.Set(src.metric_name.value());   \
  }                                                 \
  PW_ALLOCATOR_ABSORB_SEMICOLON()

  PW_ALLOCATOR_METRICS_FOREACH(PW_ALLOCATOR_COPY_METRIC);

#undef PW_ALLOCATOR_COPY_METRIC
}

template <typename MetricsType>
constexpr Metrics<MetricsType>::Metrics(metric::Token token)
    : GenericMetrics(token) {
#define PW_ALLOCATOR_ADD_TO_GROUP(metric_name)      \
  if constexpr (MetricsType::has_##metric_name()) { \
    group().Add(metrics_.metric_name);              \
  }                                                 \
  PW_ALLOCATOR_ABSORB_SEMICOLON()

  PW_ALLOCATOR_METRICS_FOREACH(PW_ALLOCATOR_ADD_TO_GROUP);

#undef PW_ALLOCATOR_ADD_TO_GROUP
}

template <typename MetricsType>
void Metrics<MetricsType>::ModifyRequested(size_t increase, size_t decrease) {
  if constexpr (MetricsType::requested_bytes_enabled()) {
    metrics_.requested_bytes.Increment(internal::ClampU32(increase));
    metrics_.requested_bytes.Decrement(internal::ClampU32(decrease));
    if constexpr (MetricsType::peak_requested_bytes_enabled()) {
      uint32_t requested_bytes = metrics_.requested_bytes.value();
      if (metrics_.peak_requested_bytes.value() < requested_bytes) {
        metrics_.peak_requested_bytes.Set(requested_bytes);
      }
    }
    if constexpr (MetricsType::cumulative_requested_bytes_enabled()) {
      if (increase > decrease) {
        metrics_.cumulative_requested_bytes.Increment(
            internal::ClampU32(increase - decrease));
      }
    }
  }
}

template <typename MetricsType>
void Metrics<MetricsType>::ModifyAllocated(size_t increase, size_t decrease) {
  if constexpr (MetricsType::allocated_bytes_enabled()) {
    metrics_.allocated_bytes.Increment(internal::ClampU32(increase));
    metrics_.allocated_bytes.Decrement(internal::ClampU32(decrease));
    if constexpr (MetricsType::peak_allocated_bytes_enabled()) {
      uint32_t allocated_bytes = metrics_.allocated_bytes.value();
      if (metrics_.peak_allocated_bytes.value() < allocated_bytes) {
        metrics_.peak_allocated_bytes.Set(allocated_bytes);
      }
    }
    if constexpr (MetricsType::cumulative_allocated_bytes_enabled()) {
      if (increase > decrease) {
        metrics_.cumulative_allocated_bytes.Increment(
            internal::ClampU32(increase - decrease));
      }
    }
  }
}

template <typename MetricsType>
void Metrics<MetricsType>::IncrementAllocations() {
  if constexpr (MetricsType::num_allocations_enabled()) {
    if (!is_reallocating()) {
      metrics_.num_allocations.Increment();
    }
  }
}

template <typename MetricsType>
void Metrics<MetricsType>::IncrementDeallocations() {
  if constexpr (MetricsType::num_deallocations_enabled()) {
    if (!is_reallocating()) {
      metrics_.num_deallocations.Increment();
    }
  }
}

template <typename MetricsType>
void Metrics<MetricsType>::IncrementResizes() {
  if constexpr (MetricsType::num_resizes_enabled()) {
    if (!is_reallocating()) {
      metrics_.num_resizes.Increment();
    }
  }
}

template <typename MetricsType>
void Metrics<MetricsType>::IncrementReallocations() {
  if constexpr (MetricsType::num_reallocations_enabled()) {
    metrics_.num_reallocations.Increment();
  }
}

template <typename MetricsType>
void Metrics<MetricsType>::RecordFailure(size_t requested) {
  if constexpr (MetricsType::num_failures_enabled()) {
    if (!is_reallocating()) {
      metrics_.num_failures.Increment();
    }
  }
  if constexpr (MetricsType::unfulfilled_bytes_enabled()) {
    if (!is_reallocating()) {
      metrics_.unfulfilled_bytes.Increment(internal::ClampU32(requested));
    }
  }
}

#undef PW_ALLOCATOR_ABSORB_SEMICOLON

}  // namespace internal
}  // namespace pw::allocator
