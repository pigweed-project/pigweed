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

#include "pw_memory/no_destructor.h"
#include "pw_metric/list.h"
#include "pw_metric/metric.h"
#include "pw_tokenizer/tokenize.h"

namespace pw::metric {

/// @module{pw_metric}

/// Global list of registered metric groups.
// TODO(keir): Add protection to IntrusiveList to detect the unitialized case,
// which can happen with the global static constructors in the -O0 case.
extern GroupList global_groups;

/// Global list of registered metrics.
extern MetricList global_metrics;

/// Declare a `pw::metric::TypedMetric<T>` with name `metric_name`, and register
/// it in the global metrics list `pw::metric::global_metrics`.
///
/// This is useful for cases where uncoordinated instrumentation with metrics is
/// needed; for example, when instrumenting legacy code where plumbing a metric
/// group around by dependency injection is infeasible.
///
/// Example:
/// @code{.cpp}
/// #include "pw_metric/global.h"
/// #include "pw_metric/metric.h"
///
/// // No need to coordinate collection of foo and bar; they're autoregistered.
/// PW_METRIC_GLOBAL(foo, "foo", 0.2f);
/// PW_METRIC_GLOBAL(bar, "bar", 44000u);
/// @endcode
///
/// Note that metrics defined with `PW_METRIC_GLOBAL` should never be added to
/// groups defined with `PW_METRIC_GROUP_GLOBAL`. Each metric can only belong
/// to one group, and metrics defined with `PW_METRIC_GLOBAL` are
/// pre-registered with the global metrics list.
///
/// @warning Do not create `PW_METRIC_GLOBAL` instances anywhere other than
/// global scope. Putting these on an instance (member context) would lead to
/// dangling pointers and misery. Metrics are never deleted or unregistered!
#define PW_METRIC_GLOBAL(variable_name, metric_name, init)                   \
  static constexpr uint32_t variable_name##_token =                          \
      PW_METRIC_TOKEN(metric_name);                                          \
  ::pw::NoDestructor<                                                        \
      ::pw::metric::TypedMetric<_PW_METRIC_FLOAT_OR_UINT32(init)>>           \
      variable_name##_storage{                                               \
          variable_name##_token, init, ::pw::metric::global_metrics};        \
  ::pw::metric::TypedMetric<_PW_METRIC_FLOAT_OR_UINT32(init)>& variable_name \
      [[maybe_unused]] = *variable_name##_storage

/// Declare a `pw::metric::TypedMetric<T>` with name `metric_name` and explicit
/// type `type`, and register it in the global metrics list
/// `pw::metric::global_metrics`.
///
/// This macro works identically to `PW_METRIC_GLOBAL`, but requires explicitly
/// specifying the metric type (e.g. `float`, `uint32_t`, or `uint64_t`).
///
/// Example:
/// @code{.cpp}
/// #include "pw_metric/global.h"
/// #include "pw_metric/metric.h"
///
/// PW_METRIC_GLOBAL_TYPED(my_global_64bit, "my_global_64bit", uint64_t, 0ULL);
/// @endcode
#define PW_METRIC_GLOBAL_TYPED(variable_name, metric_name, type, init)         \
  static constexpr uint32_t variable_name##_token =                            \
      PW_METRIC_TOKEN(metric_name);                                            \
  ::pw::NoDestructor<::pw::metric::TypedMetric<type>> variable_name##_storage{ \
      variable_name##_token, init, ::pw::metric::global_metrics};              \
  ::pw::metric::TypedMetric<type>& variable_name [[maybe_unused]] =            \
      *variable_name##_storage

/// Declare a `pw::metric::Group` with name `group_name`, and register it in the
/// global metric groups list `pw::metric::global_groups`.
///
/// Note that metrics created with `PW_METRIC_GLOBAL` should never be added to
/// groups! Instead, just create a freestanding metric and register it into the
/// global group (like in the example below).
///
/// Example:
/// @code{.cpp}
/// #include "pw_metric/global.h"
/// #include "pw_metric/metric.h"
///
/// // No need to coordinate collection of this group; it's globally registered.
/// PW_METRIC_GROUP_GLOBAL(legacy_system, "legacy_system");
/// PW_METRIC(legacy_system, foo, "foo", 0.2f);
/// PW_METRIC(legacy_system, bar, "bar", 44000u);
/// @endcode
///
/// @warning Do not create `PW_METRIC_GROUP_GLOBAL` instances anywhere other
/// than global scope. Putting these on an instance (member context) would lead
/// to dangling pointers and misery. Metrics are never deleted or unregistered!
#define PW_METRIC_GROUP_GLOBAL(variable_name, group_name)          \
  static constexpr uint32_t variable_name##_token =                \
      PW_TOKENIZE_STRING_DOMAIN("metrics", group_name);            \
  ::pw::NoDestructor<::pw::metric::Group> variable_name##_storage{ \
      variable_name##_token, ::pw::metric::global_groups};         \
  ::pw::metric::Group& variable_name [[maybe_unused]] = *variable_name##_storage

/// @endmodule

}  // namespace pw::metric
