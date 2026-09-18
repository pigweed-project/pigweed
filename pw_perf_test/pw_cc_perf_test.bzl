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
"""Rules for declaring performance tests."""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

def pw_cc_perf_test(**kwargs):
    """A Pigweed performance test.

    This macro produces a cc_binary and,

    *  Adds a dep on the pw_assert backend.
    *  Adds a dep on //pw_perf_test:logging_main

    Args:
      **kwargs: Passed to cc_binary.
    """
    kwargs["deps"] = kwargs.get("deps", []) + \
                     [str(Label("//pw_perf_test:main"))]
    kwargs["deps"] = kwargs["deps"] + [str(Label("//pw_assert:assert_backend_impl"))]
    kwargs["deps"] = kwargs["deps"] + [str(Label("//pw_assert:check_backend_impl"))]
    kwargs["testonly"] = True
    cc_binary(**kwargs)
