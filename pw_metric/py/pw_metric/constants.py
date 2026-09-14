# Copyright 2026 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Metric types and masks from pw_metric/public/pw_metric/metric.h."""

# LINT.IfChange(metric_types)
# Metric types from pw_metric/public/pw_metric/metric.h
TYPE_UINT32 = 0x00000000
TYPE_FLOAT = 0x10000000
TYPE_UINT64 = 0x20000000
TYPE_INT64 = 0x30000000
TYPE_DOUBLE = 0x40000000
TYPE_INT32 = 0x50000000
TYPE_BOOL = 0x60000000
TYPE_TOKEN = 0x70000000
# LINT.ThenChange(//pw_metric/public/pw_metric/metric.h:metric_types)

# LINT.IfChange(token_mask_macro)
TOKEN_MASK = 0x0FFFFFFF
# LINT.ThenChange(//pw_metric/public/pw_metric/metric.h:token_mask_macro)
# LINT.IfChange(metric_masks)
TYPE_MASK = 0xF0000000
# LINT.ThenChange(//pw_metric/public/pw_metric/metric.h:metric_masks)
