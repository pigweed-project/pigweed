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
"""Unit tests for client/digest_parser.bzl."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("//pw_build:pw_starlark_unittest.bzl", "pw_unittest_suite")
load("//pw_env_setup/bazel/cipd/private/client:digest_parser.bzl", "get_digest")

def _get_digest_test_impl(ctx):
    env = unittest.begin(ctx)

    digest_contents = """
# one
one   sha256  1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef
two   md5     abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890
three sha256  bcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890
four sha256 1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef
"""

    expected = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"

    digest, err = get_digest("test", digest_contents, "one")
    asserts.equals(env, expected, digest)
    asserts.equals(env, None, err)

    digest, err = get_digest("test", digest_contents, "two")
    asserts.equals(env, None, digest)
    asserts.equals(env, True, "not a sha256 digest" in err)

    digest, err = get_digest("test", digest_contents, "three")
    asserts.equals(env, None, digest)
    asserts.equals(env, True, "invalid sha256 digest" in err)

    digest, err = get_digest("test", digest_contents, "four-oh-four")
    asserts.equals(env, None, digest)
    asserts.equals(env, True, "no entry" in err)

    digest, err = get_digest("test", digest_contents, "four")
    asserts.equals(env, expected, digest)
    asserts.equals(env, None, err)

    digest, err = get_digest("test", "", "empty-file")
    asserts.equals(env, None, digest)
    asserts.equals(env, True, "no entry" in err)

    return unittest.end(env)

_get_digest_test = unittest.make(_get_digest_test_impl)

def test_suite(name):
    pw_unittest_suite(
        name,
        _get_digest_test,
    )
