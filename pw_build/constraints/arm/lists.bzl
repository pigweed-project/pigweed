# Copyright 2024 The Pigweed Authors
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
"""A programatic list of all known Cortex-A and Cortex-M CPUs."""

ALL_CORTEX_A_CPUS = (
    "@pigweed//pw_build/constraints/arm:cortex-a32",
    "@pigweed//pw_build/constraints/arm:cortex-a35",
)

ALL_CORTEX_M_CPUS = (
    "@platforms//cpu:armv6-m",
    "@platforms//cpu:armv7-m",
    "@platforms//cpu:armv7e-m",
    # TODO: https://github.com/bazelbuild/platforms/issues/100 - Kept for legacy
    # reasons.
    "@platforms//cpu:armv7e-mf",
    # TODO: https://github.com/bazelbuild/platforms/issues/104 - Kept for legacy
    # reasons.
    "@platforms//cpu:armv8-m",
)
