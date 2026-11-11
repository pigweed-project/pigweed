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

"""Defines the list of Pigweed module tests to run on Zephyr.

The tests are run out of the `zephyr/pw_tests/pw_module_tests` directory.
"""

# Note: If not specified, the default timeout is 5 seconds.
ZEPHYR_PW_MODULE_TESTS = {
    "pw_allocator_zephyr_test": {
        "deps": ["//pw_allocator_zephyr:heap_allocator_test"],
        "timeout": 5,
    },
    "pw_chrono_zephyr_test": {
        "deps": ["//pw_chrono_zephyr:system_timer_test"],
        "timeout": 10,
    },
    "pw_sync_zephyr_test": {
        "deps": ["//pw_sync_zephyr:mutex_test"],
        "timeout": 5,
    },
}
