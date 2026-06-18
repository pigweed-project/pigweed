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
"""Custom wrappers for Bazel Skylib unit testing library."""

load("@bazel_skylib//lib:partial.bzl", "partial")
load("@bazel_skylib//lib:unittest.bzl", "unittest")
load("//pw_build:compatibility.bzl", "incompatible_with_mcu")

def pw_unittest_suite(name, *test_rules):
    """A wrapper for unittest.suite that marks all tests as incompatible with MCU targets by default.

    Args:
      name: The name of the test_suite target.
      *test_rules: A list of test rules defined by unittest.make.
    """
    updated_test_rules = []
    for test_rule in test_rules:
        updated_test_rules.append(partial.make(test_rule, target_compatible_with = incompatible_with_mcu()))

    unittest.suite(name, *updated_test_rules)
