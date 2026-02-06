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

"""Test coverage for pw_pylint.bzl"""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts", "unittest")
load("@rules_python//python:defs.bzl", "py_library")
load("//pw_build:pw_py_importable_runfile.bzl", "pw_py_importable_runfile")
load("//pw_build:pw_pylint.bzl", "PylintAspectForTestInfo", "is_rules_python_generated_source", "map_import_path_for_python_path", "pylint_aspect")

def _is_rules_python_generated_source_test_impl(ctx):
    env = unittest.begin(ctx)
    asserts.true(env, is_rules_python_generated_source("rules_python_entry_point_foo.py", "foo"))
    asserts.false(env, is_rules_python_generated_source("foo.py", "foo"))
    asserts.false(env, is_rules_python_generated_source("rules_python_entry_point_bar.py", "foo"))
    return unittest.end(env)

is_rules_python_generated_source_test = unittest.make(_is_rules_python_generated_source_test_impl)

def _map_import_path_for_python_path_test_impl(ctx):
    env = unittest.begin(ctx)

    # https://pwbug.dev/248343713#comment11 - py_library in this repo
    # Expect _main/pw_build/py -> pw_build/py
    asserts.equals(
        env,
        "pw_build/py",
        map_import_path_for_python_path(ctx, "_main/pw_build/py"),
    )

    # https://pwbug.dev/248343713#comment11 - py_library in external repo
    # Expect project+/path -> external/project+/path
    asserts.equals(
        env,
        "external/pigweed+/pw_build/py",
        map_import_path_for_python_path(ctx, "pigweed+/pw_build/py"),
    )

    # https://pwbug.dev/248343713#comment11 - Proto in this repo
    # _main/pw_protobuf/_virtual_imports/common_proto -> $(BINDIR)/pw_protobuf/_virtual_imports/common_proto
    asserts.equals(
        env,
        ctx.var["BINDIR"] + "/pw_protobuf/_virtual_imports/common_proto",
        map_import_path_for_python_path(ctx, "_main/pw_protobuf/_virtual_imports/common_proto"),
    )

    # https://pwbug.dev/248343713#comment11 - Proto in external repo
    # _main/external/pigweed+/pw_snapshot/_virtual_imports/snapshot_proto -> $(BINDIR)/external/pigweed+/pw_snapshot/_virtual_imports/snapshot_proto
    asserts.equals(
        env,
        ctx.var["BINDIR"] + "/external/pigweed+/pw_snapshot/_virtual_imports/snapshot_proto",  # buildifier: disable=external-path
        map_import_path_for_python_path(ctx, "_main/external/pigweed+/pw_snapshot/_virtual_imports/snapshot_proto"),  # buildifier: disable=external-path
    )

    # https://pwbug.dev/248343713#comment11 - Importable runfile in this repo
    # _main/pw_build/_test_runfile_virtual_imports -> $(BINDIR)/pw_build/_test_runfile_virtual_imports
    asserts.equals(
        env,
        ctx.var["BINDIR"] + "/pw_build/_test_runfile_virtual_imports",
        map_import_path_for_python_path(ctx, "_main/pw_build/_test_runfile_virtual_imports"),
    )

    # https://pwbug.dev/248343713#comment11 - Importable runfile in this external repo
    # Note: This mapping was not implemented this way, and may not be correct.
    # +_repo_rules7+pw_build_external_runfile_resource/_black_virtual_imports -> $(BINDIR)/external/_black_virtual_imports
    # asserts.equals(
    #     env,
    #     ctx.var["BINDIR"] + "/external/_black_virtual_imports",
    #     map_import_path_for_python_path(ctx, "+_repo_rules7+pw_build_external_runfile_resource/_black_virtual_imports"),
    # )

    # This example is not from https://pwbug.dev/248343713#comment11, but was
    # is a real example to be supported.
    #
    # Importable runfile in external repo
    # pigweed+/pw_presubmit/py/_black_runfiles_virtual_imports -> $(BINDIR)/external/pigweed+/pw_presubmit/py/_black_runfiles_virtual_imports
    asserts.equals(
        env,
        ctx.var["BINDIR"] + "/external/pigweed+/pw_presubmit/py/_black_runfiles_virtual_imports",  # buildifier: disable=external-path
        map_import_path_for_python_path(ctx, "pigweed+/pw_presubmit/py/_black_runfiles_virtual_imports"),
    )

    return unittest.end(env)

map_import_path_for_python_path_test = unittest.make(_map_import_path_for_python_path_test_impl)

def _pylint_aspect_test_impl(ctx):
    env = analysistest.begin(ctx)

    target_under_test = analysistest.target_under_test(env)
    info = target_under_test[PylintAspectForTestInfo]

    asserts.equals(
        env,
        sorted(ctx.attr.expected_sources),
        sorted(info.sources),
        "expected sources do not match.",
    )

    info = target_under_test[PylintAspectForTestInfo]
    asserts.equals(
        env,
        [path.replace("$(BINDIR)", ctx.var["BINDIR"]) for path in ctx.attr.expected_python_path],
        info.python_path,
    )

    return analysistest.end(env)

pylint_aspect_test = analysistest.make(
    _pylint_aspect_test_impl,
    extra_target_under_test_aspects = [pylint_aspect],
    attrs = {
        "expected_python_path": attr.string_list(mandatory = True),
        "expected_sources": attr.string_list(mandatory = True),
    },
)

def pw_pylint_test_suite(name):
    """ Tests for pw_pylint.bzl

    Args:
        name: Name for the suite
    """

    is_rules_python_generated_source_test(
        name = name + "_is_rules_python_generated_source_test",
    )

    map_import_path_for_python_path_test(
        name = name + "_map_import_path_for_python_path_test",
    )

    py_library(
        name = name + "_py_lib_dep",
        srcs = ["pw_pylint_test_example.py"],
        tags = ["manual"],
        visibility = ["//visibility:private"],
    )

    pw_py_importable_runfile(
        name = name + "_py_lib_dep_runfiles",
        src = name + "_py_lib_dep",
        tags = ["manual"],
        visibility = ["//visibility:private"],
    )

    py_library(
        name = name + "_py_lib",
        srcs = ["pw_pylint_test_example.py"],

        # All these deps are for testing the generated python path and not
        # actual dependencies. Feel free to update these and
        # expected_python_path in the test below as required.
        deps = [
            # Internal py_library dep
            name + "_py_lib_dep",
            # External py_library dep
            "@com_google_protobuf//:protobuf_python",
            # Internal protobuf dep
            "//pw_protobuf:common_py_pb2",
            # External protobuf dep
            "@com_google_protobuf//:well_known_types_py_pb2",
            # Internal runfiles dep
            name + "_py_lib_dep_runfiles",
            # External runfiles dep
            "@rules_python//python/runfiles",
        ],
        tags = ["manual"],
        visibility = ["//visibility:private"],
    )

    pylint_aspect_test(
        name = name + "_aspect_test",
        target_under_test = ":" + name + "_py_lib",
        # There should be one file, matching the .py source file above.
        expected_sources = [
            "pw_build/pw_pylint_test_example.py",
        ],
        # There should be some base entries, plus entries for all the deps above.
        expected_python_path = [
            ".",
            "$(BINDIR)",
            "external/protobuf+/python/python",
            "external/protobuf+/python",
            "$(BINDIR)/pw_protobuf/_virtual_imports/common_proto",
            "pw_build/py",
            "external/rules_python+",
            "$(BINDIR)/pw_build/_pw_pylint_test_py_lib_dep_runfiles_virtual_imports",
            "external/protobuf+",
            "$(BINDIR)/external/protobuf+",  # buildifier: disable=external-path
            "external/rules_python+",
            "$(BINDIR)/external/rules_python+",  # buildifier: disable=external-path
        ],
        visibility = ["//visibility:private"],
    )

    native.test_suite(
        name = name,
        tests = [
            ":" + name + "_is_rules_python_generated_source_test",
            ":" + name + "_map_import_path_for_python_path_test",
            ":" + name + "_aspect_test",
        ],
        visibility = ["//visibility:private"],
    )
