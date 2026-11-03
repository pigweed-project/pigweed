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
"""Unit tests for pw_cxx_toolchain.bzl."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("//pw_build:pw_starlark_unittest.bzl", "pw_unittest_suite")
load(":pw_cxx_toolchain.bzl", "resolve_toolchain_tags")

def _tag(cipd_tag):
    return struct(cipd_tag = cipd_tag)

def _mod(name, is_root = False, **flavor_tags):
    tags_dict = {}
    for flavor in ("arm_gcc", "llvm", "zephyr_gcc", "zephyr_llvm"):
        val = flavor_tags.get(flavor, [])
        if type(val) == "string":
            val = [_tag(val)]
        tags_dict[flavor] = val
    return struct(
        name = name,
        is_root = is_root,
        tags = struct(**tags_dict),
    )

def _pigweed_mod(is_root = False):
    return _mod(
        "pigweed",
        is_root = is_root,
        arm_gcc = "pw-arm-gcc-default",
        llvm = "pw-llvm-default",
        zephyr_gcc = "pw-zephyr-gcc-default",
        zephyr_llvm = "pw-zephyr-llvm-default",
    )

def _resolve_toolchain_tags_test_impl(ctx):
    env = unittest.begin(ctx)

    # 1. Pigweed as the root module uses Pigweed's default tags.
    resolved, err = resolve_toolchain_tags([_pigweed_mod(is_root = True)])
    asserts.equals(env, None, err)
    asserts.equals(
        env,
        {
            "arm_gcc": _tag("pw-arm-gcc-default"),
            "llvm": _tag("pw-llvm-default"),
            "zephyr_gcc": _tag("pw-zephyr-gcc-default"),
            "zephyr_llvm": _tag("pw-zephyr-llvm-default"),
        },
        resolved,
    )

    # 2. Downstream root module overrides a subset of tags (`arm_gcc` and
    #    `zephyr_llvm`) and falls back to Pigweed's defaults for the rest,
    #    while ignoring tags from non-root, non-pigweed modules.
    resolved, err = resolve_toolchain_tags([
        _mod(
            "downstream_root",
            is_root = True,
            arm_gcc = "root-arm-gcc-override",
            zephyr_llvm = "root-zephyr-llvm-override",
        ),
        _mod(
            "transitive_dep",
            is_root = False,
            arm_gcc = "ignored-transitive-arm-gcc",
            llvm = "ignored-transitive-llvm",
        ),
        _pigweed_mod(is_root = False),
    ])
    asserts.equals(env, None, err)
    asserts.equals(
        env,
        {
            "arm_gcc": _tag("root-arm-gcc-override"),
            "llvm": _tag("pw-llvm-default"),
            "zephyr_gcc": _tag("pw-zephyr-gcc-default"),
            "zephyr_llvm": _tag("root-zephyr-llvm-override"),
        },
        resolved,
    )

    # 3. `cipd_tag_allowlist` validation accepts allowed tags and rejects
    #    disallowed tags.
    variants_with_allowlist = {
        "arm_gcc": struct(cipd_tag_allowlist = ["allowed-tag"]),
    }
    resolved, err = resolve_toolchain_tags(
        [
            _mod("downstream_root", is_root = True, arm_gcc = "allowed-tag"),
            _pigweed_mod(is_root = False),
        ],
        variants = variants_with_allowlist,
    )
    asserts.equals(env, None, err)
    asserts.equals(env, {"arm_gcc": _tag("allowed-tag")}, resolved)

    resolved, err = resolve_toolchain_tags(
        [
            _mod("downstream_root", is_root = True, arm_gcc = "disallowed-tag"),
            _pigweed_mod(is_root = False),
        ],
        variants = variants_with_allowlist,
    )
    asserts.equals(env, None, resolved)
    asserts.true(env, err != None and "must be one of the following CIPD tags" in err)

    # 4. Duplicate or empty tag definitions in the root module fail cleanly.
    resolved, err = resolve_toolchain_tags([
        _mod(
            "downstream_root",
            is_root = True,
            arm_gcc = [_tag("tag-1"), _tag("tag-2")],
        ),
        _pigweed_mod(is_root = False),
    ])
    asserts.equals(env, None, resolved)
    asserts.true(env, err != None and "more than one definition" in err)

    resolved, err = resolve_toolchain_tags([
        _mod("downstream_root", is_root = True, arm_gcc = [_tag("")]),
        _pigweed_mod(is_root = False),
    ])
    asserts.equals(env, None, resolved)
    asserts.true(env, err != None and "`cipd_tag` must be set once" in err)

    return unittest.end(env)

resolve_toolchain_tags_test = unittest.make(_resolve_toolchain_tags_test_impl)

def test_suite(name):
    pw_unittest_suite(
        name,
        resolve_toolchain_tags_test,
    )
