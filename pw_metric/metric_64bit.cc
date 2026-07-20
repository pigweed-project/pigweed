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

#include "pw_metric/metric.h"

#if PW_METRIC_CONFIG_ENABLE_64BIT

#include <atomic>
#include <limits>

#include "pw_assert/check.h"
#include "pw_numeric/checked_arithmetic.h"

namespace pw::metric {

void TypedMetric<uint64_t>::Increment(uint64_t amount) {
  PW_DCHECK(is_uint64());
  internal::SaturatedIncrement(value_, amount);
}

void TypedMetric<uint64_t>::Decrement(uint64_t amount) {
  PW_DCHECK(is_uint64());
  internal::SaturatedDecrement(value_, amount);
}

void TypedMetric<int64_t>::Increment(int64_t amount) {
  PW_DCHECK(is_int64());
  internal::SaturatedIncrement(value_, amount);
}

void TypedMetric<int64_t>::Decrement(int64_t amount) {
  PW_DCHECK(is_int64());
  internal::SaturatedDecrement(value_, amount);
}

}  // namespace pw::metric

#endif  // PW_METRIC_CONFIG_ENABLE_64BIT
