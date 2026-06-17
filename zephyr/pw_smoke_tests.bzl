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

"""Defines the list of tests to run as Zephyr smoke tests

The tests are run out of zephyr-bazel
(https://pigweed.googlesource.com/zephyr/zephyr-bazel/), out of the
tests/pw_smoke_tests subdirectory.

They are run there instead of here as the zephyr-bazel is not yet stable, and
the setup is evolving.
"""

# Note: If not specified, the default timeout is 5 seconds.
ZEPHYR_PW_SMOKE_TESTS = {
    "pw_allocator_zephyr_test": {
        "deps": ["//pw_allocator_zephyr:heap_allocator_test"],
        "timeout": 5,
    },
    "pw_chrono_zephyr_test": {
        "deps": ["//pw_chrono_zephyr:system_timer_test"],
        "timeout": 10,
    },
}
