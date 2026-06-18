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

"""Unit tests for classify.bzl."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("//pw_build:pw_starlark_unittest.bzl", "pw_unittest_suite")
load("//pw_env_setup/bazel/cipd/private/versions:classify.bzl", "is_immutable_version", "is_instance_id", "is_key_value_tag", "is_ref_tag")

def _is_key_value_tag_test_impl(ctx):
    env = unittest.begin(ctx)

    def check(input, expect):
        asserts.equals(
            env,
            expect,
            is_key_value_tag(input),
            "is_keyvalue_tag(\"{}\"): ".format(input),
        )

    check("git_revision:abcdef", expect = True)
    check("version:2@1.2.3", expect = True)

    check("0" * 64, expect = False)
    check("latest", expect = False)
    check("", expect = False)
    check(None, expect = False)

    return unittest.end(env)

is_key_value_tag_test = unittest.make(_is_key_value_tag_test_impl)

def _is_instance_id_test_impl(ctx):
    env = unittest.begin(ctx)

    def check(input, expect):
        asserts.equals(
            env,
            expect,
            is_instance_id(input),
            "is_instance_id(\"{}\"): ".format(input),
        )

    check("a" * 64, expect = True)
    check("b" * 63, expect = False)
    check("c" * 65, expect = False)
    check("z" * 64, expect = False)

    check("git_revision:abcdef", expect = False)
    check("version:2@1.2.3", expect = False)
    check("latest", expect = False)
    check("", expect = False)
    check(None, expect = False)

    return unittest.end(env)

is_instance_id_test = unittest.make(_is_instance_id_test_impl)

def _is_ref_tag_test_impl(ctx):
    env = unittest.begin(ctx)

    def check(input, expect):
        asserts.equals(
            env,
            expect,
            is_ref_tag(input),
            "is_ref_tag(\"{}\"): ".format(input),
        )

    check("latest", expect = True)
    check("prod", expect = True)
    check("experimental", expect = True)

    check("git_revision:abcdef", expect = False)
    check("version:2@1.2.3", expect = False)
    check("0" * 64, expect = False)
    check("", expect = False)
    check(None, expect = False)

    return unittest.end(env)

is_ref_tag_test = unittest.make(_is_ref_tag_test_impl)

def _is_immutable_test_impl(ctx):
    env = unittest.begin(ctx)

    def check(input, expect):
        asserts.equals(
            env,
            expect,
            is_immutable_version(input),
            "is_immutable_version(\"{}\"): ".format(input),
        )

    # Key-value tags
    check("git_revision:abcdef", expect = True)
    check("version:2@1.2.3", expect = True)

    # Instance IDs (64 hex chars)
    check("0" * 64, expect = True)

    # Ref tags
    check("latest", expect = False)
    check("prod", expect = False)
    check("experimental", expect = False)

    return unittest.end(env)

is_immutable_test = unittest.make(_is_immutable_test_impl)

def test_suite(name):
    pw_unittest_suite(
        name,
        is_key_value_tag_test,
        is_instance_id_test,
        is_ref_tag_test,
        is_immutable_test,
    )
