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
"""Tests for pw_cc_compile_commands_aspect."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":pw_cc_compile_commands_aspect.bzl", "compile_commands_aspect_testing")

def _remap_arg_test_impl(ctx):
    env = unittest.begin(ctx)
    remap_arg = compile_commands_aspect_testing.remap_arg

    virtual_path_map = {
        "v/p": "r/p",
        "virtual/path": "real/path",
    }

    # Exact match
    asserts.equals(env, "real/path", remap_arg("virtual/path", virtual_path_map))

    # Substring match (flags)
    asserts.equals(env, "-Ireal/path", remap_arg("-Ivirtual/path", virtual_path_map))
    asserts.equals(env, "-isystemreal/path", remap_arg("-isystemvirtual/path", virtual_path_map))

    # No match
    asserts.equals(env, "other/path", remap_arg("other/path", virtual_path_map))
    asserts.equals(env, "-Iother/path", remap_arg("-Iother/path", virtual_path_map))

    # Partial match that shouldn't replace if it's not a valid virtual path?
    # Actually, the logic is "if v_path in arg". So "avirtual/pathb" -> "areal/pathb".
    # This matches the implementation behavior.
    asserts.equals(env, "areal/pathb", remap_arg("avirtual/pathb", virtual_path_map))

    return unittest.end(env)

remap_arg_test = unittest.make(_remap_arg_test_impl)

def _remap_target_infos_test_impl(ctx):
    env = unittest.begin(ctx)
    remap_target_infos = compile_commands_aspect_testing.remap_target_infos

    virtual_path_map = {
        "gen/src/b.cc": "src/b.cc",
        "virtual/include/a.h": "real/include/a.h",
    }

    input_infos = [{
        "deps": ["//foo:bar"],
        "hdrs": ["virtual/include/a.h", "normal/c.h"],
        "label": "//foo:baz",
        "srcs": ["gen/src/b.cc", "normal/d.cc", "-Ivirtual/include/a.h"],
    }]

    expected_infos = [{
        "deps": ["//foo:bar"],
        "hdrs": ["real/include/a.h", "normal/c.h"],
        "label": "//foo:baz",
        "srcs": ["src/b.cc", "normal/d.cc", "-Ireal/include/a.h"],
    }]

    asserts.equals(env, expected_infos, remap_target_infos(input_infos, virtual_path_map))

    return unittest.end(env)

remap_target_infos_test = unittest.make(_remap_target_infos_test_impl)

def _get_cpp_compile_commands_ignored_rule_test_impl(ctx):
    env = unittest.begin(ctx)
    get_cpp_compile_commands = compile_commands_aspect_testing.get_cpp_compile_commands

    # Mock ctx with rule.kind
    mock_ctx = struct(
        rule = struct(
            kind = "_load_phase_test",
        ),
    )

    result = get_cpp_compile_commands(mock_ctx, None)

    # Check that providers list is not empty
    asserts.true(env, len(result.providers) > 0, "providers list should not be empty")

    # Check that one of the providers has a 'mappings' field (characteristic of VirtualIncludesInfo)
    found_virtual_includes = False
    for provider in result.providers:
        if hasattr(provider, "mappings"):
            found_virtual_includes = True
            break

    asserts.true(env, found_virtual_includes, "VirtualIncludesInfo provider not found in result")

    return unittest.end(env)

get_cpp_compile_commands_ignored_rule_test = unittest.make(_get_cpp_compile_commands_ignored_rule_test_impl)

def pw_cc_compile_commands_aspect_test_suite(name):
    unittest.suite(
        name,
        remap_arg_test,
        remap_target_infos_test,
        get_cpp_compile_commands_ignored_rule_test,
    )
