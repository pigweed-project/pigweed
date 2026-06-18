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
"""Unit tests for packages/expand_vars.bzl."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("//pw_build:pw_starlark_unittest.bzl", "pw_unittest_suite")
load("//pw_env_setup/bazel/cipd/private/packages:expand_vars.bzl", "expand_vars")

def _os(name, arch):
    return struct(name = name, arch = arch)

_LINUX_AMD64 = _os("linux", "amd64")

def _expand_vars_test_impl(ctx):
    env = unittest.begin(ctx)

    def expand(input, expect):
        actual = expand_vars(input, _LINUX_AMD64)

        # Substitute the expected error for the actual error before using
        # `asserts.equals`, but only if it is a substring of the actual error.
        # This allows the expected error to not be the full error message.
        if expect[1] and actual[1] and expect[1] in actual[1]:
            expect = (expect[0], actual[1])

        asserts.equals(
            env,
            actual,
            expect,
            "expand_vars(\"{}\"): ".format(input),
        )

    # https://chromium.googlesource.com/infra/luci/luci-go/+/29e9a814821a9816b0489f9efe03cdb92ed8a6d1/cipd/client/cipd/ensure/doc.go#113
    #
    # > All of these parameters also support the syntax ${var=possible,values}.

    expand("a/b/c", expect = ("a/b/c", None))

    expand("a/b/${platform}", expect = ("a/b/linux-amd64", None))
    expand("a/b/${platform=linux-amd64,linux-arm64}", expect = ("a/b/linux-amd64", None))
    expand("a/b/${platform=linux-arm64}", expect = ("", None))

    expand("a/b/${os}-${arch}", expect = ("a/b/linux-amd64", None))
    expand("a/b/${os=linux}-${arch}", expect = ("a/b/linux-amd64", None))
    expand("a/b/${os=mac,linux}-${arch}", expect = ("a/b/linux-amd64", None))
    expand("a/b/${os=windows,mac}-${arch}", expect = ("", None))

    expand("a/b/linux-${arch}", expect = ("a/b/linux-amd64", None))
    expand("a/b/linux-${arch=amd64}", expect = ("a/b/linux-amd64", None))
    expand("a/b/linux-${arch=arm64}", expect = ("", None))

    expand("a/${arch}/${os}/x-${os}", expect = ("a/amd64/linux/x-linux", None))

    # https://chromium.googlesource.com/infra/luci/luci-go/+/29e9a814821a9816b0489f9efe03cdb92ed8a6d1/cipd/client/cipd/ensure/doc.go#75
    #
    # > These placeholders can appear anywhere in the package template except
    # > for the first letter

    expand("${platform}", expect = (None, "Cannot start package path"))
    expand("x${platform}", expect = ("xlinux-amd64", None))

    expand("a/${unknown", expect = (None, "Unterminated \"${\""))
    expand("a/${unknown}", expect = (None, "Unknown variable"))

    return unittest.end(env)

expand_vars_test = unittest.make(_expand_vars_test_impl)

def test_suite(name):
    pw_unittest_suite(
        name,
        expand_vars_test,
    )
