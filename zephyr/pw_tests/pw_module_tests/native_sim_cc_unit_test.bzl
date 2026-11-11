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

"""Defines the native_sim_cc_unit_test rule."""

load("@rules_platform//platform_data:defs.bzl", "platform_data")
load("@rules_python//python:py_test.bzl", "py_test")
load("@zephyr//:cc.bzl", "zephyr_app")

def native_sim_cc_unit_test(
        name,
        deps = [],
        sim_timeout = 5,
        target_compatible_with = None,
        tags = [],
        visibility = None,
        timeout = None,
        size = None):
    """Creates a Zephyr unit test binary and runs it on native_sim.

    Args:
        name: The name of the test target.
        deps: Dependencies for the test.
        sim_timeout: Simulator execution timeout in seconds.
        target_compatible_with: Additional compatibility constraints.
        tags: Bazel tags for the test target.
        visibility: Visibility for the test target.
        timeout: Bazel test timeout.
        size: Bazel test size.
    """
    binary_name = name + "_bin"
    data_name = name + "_data"
    compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ] + (target_compatible_with or [])

    zephyr_app(
        name = binary_name,
        deps = deps + [
            ":main",
            "@pigweed//pw_unit_test",
        ],
        target_compatible_with = compatible_with,
        testonly = True,
    )

    platform_data(
        name = data_name,
        target = ":" + binary_name + ".elf",
        platform = "@zephyr//boards/native/native_sim:native_64",
        target_compatible_with = compatible_with,
        testonly = True,
    )

    py_test(
        name = name,
        srcs = [":runner.py"],
        main = ":runner.py",
        args = [
            "--binary",
            "$(rlocationpath :" + data_name + ")",
            "--stop-at",
            str(sim_timeout),
        ],
        data = [":" + data_name],
        deps = ["@zephyr//bazel/tools/runners:ztest_runner"],
        target_compatible_with = compatible_with,
        tags = tags,
        visibility = visibility,
        timeout = timeout,
        size = size,
    )

def generate_module_tests(tests_dict):
    """Generates native_sim_cc_unit_test targets for a dictionary of tests.

    Args:
        tests_dict: Mapping of target names to test configuration dicts.

    Returns:
        A list of generated target labels.
    """
    targets = []
    for name, config in tests_dict.items():
        deps = [
            "@pigweed" + dep if dep.startswith("//") else dep
            for dep in config.get("deps", [])
        ]
        native_sim_cc_unit_test(
            name = name,
            deps = deps,
            sim_timeout = config.get("timeout", 5),
        )
        targets.append(":" + name)
    return targets
