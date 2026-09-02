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
"""Rules for declaring unit tests."""

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//pw_build:compatibility.bzl", "incompatible_with_mcu")
load(
    "//pw_compilation_testing:pw_cc_nc_test.bzl",
    _check_and_update_nc_test_deps = "check_and_update_nc_test_deps",
    _pw_cc_nc_test = "pw_cc_nc_test",
)

def pw_cc_test(name, has_nc_test = False, deps = None, **kwargs):
    """Wrapper for cc_test that provides additional features.

    pw_cc_test expands to the following targets:

    * $name: Runs all associated tests
    * $name.cc_test: standard cc_test target with a dep on //pw_unit_test:test_main.
    * $name.nc_test: negative compilation test (if has_nc_test = True).
    * $name.lib: cc_library version of the cc_test.

    pw_cc_test automatically adds necessary link-time dependencies to the cc_test and cc_library.

    The .lib target that is created is a cc_library version of the cc_test.
    These library targets can be used as dependencies of firmware images
    bundling multiple tests. The library target has alwayslink = 1 to support
    dynamic registration. This ensures the tests are baked into the image even
    though there are no references to them, so that they can be found by
    RUN_ALL_TESTS at runtime.

    Args:
        name: base name of the test target.
        has_nc_test: set to True to enable PW_NC_TESTs.
        deps: Dependencies forwarded to the cc_test and cc_library.
        **kwargs: Other arguments forwarded to the cc_test and cc_library.
    """
    deps = [] if deps == None else deps
    deps.append(str(Label("//pw_build:default_link_extra_lib")))
    deps.append(str(Label("//pw_unit_test:pw_unit_test_alias_for_migration_only")))

    # If there are multiple test targets, group them into a test_suite.
    if has_nc_test:
        cc_test_name = name + ".cc_test"
        native.test_suite(
            name = name,
            tests = [cc_test_name, name + ".nc_test"],
            tags = kwargs.get("tags"),
        )
    else:
        cc_test_name = name

        # Use a test_suite to alias $name.cc_test to $name. This ensures $name.cc_test always
        # exists, but avoids adding a new label to Bazel's test output, which an alias would.
        native.test_suite(
            name = name + ".cc_test",
            tests = [name],
            tags = kwargs.get("tags"),
        )

    # Check for incorrectly added NC test deps and add them if needed.
    _check_and_update_nc_test_deps(has_nc_test, deps)

    if has_nc_test:
        if "srcs" not in kwargs:
            fail("pw_cc_test requires a 'srcs' argument if has_nc_test = True")

        _pw_cc_nc_test(
            name = name + ".nc_test",
            base = cc_test_name,
            srcs = kwargs["srcs"],
            tags = kwargs.get("tags"),
            # TODO: https://pwbug.dev/452721904 - These produce shell scripts
            # that emit the results, which are incompatible with device/MCU
            # builds.
            target_compatible_with = incompatible_with_mcu() + kwargs.get("target_compatible_with", []),
            # TODO: https://pwbug.dev/446943440 - Handle more common attributes.
        )

    # Add the cc_test with the unit test main label flag dep.
    test_main = kwargs.pop("test_main", str(Label("//pw_unit_test:main")))
    cc_test(name = cc_test_name, deps = deps + [test_main], **kwargs)

    # Update kwargs for the cc_library version of the cc_test. This must be last.
    kwargs["alwayslink"] = 1

    # pw_cc_test libraries may include testonly targets.
    kwargs["testonly"] = True

    # Remove any kwargs that cc_library would not recognize.
    for arg in (
        "additional_linker_inputs",
        "args",
        "env",
        "env_inherit",
        "flaky",
        "linkopts",
        "local",
        "malloc",
        "shard_count",
        "size",
        "stamp",
        "timeout",
    ):
        kwargs.pop(arg, "")

    cc_library(name = name + ".lib", deps = deps, **kwargs)
