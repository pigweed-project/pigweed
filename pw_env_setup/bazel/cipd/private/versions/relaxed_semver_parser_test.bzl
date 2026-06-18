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

"""Unit tests for relaxed_semver_parser.bzl."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("//pw_build:pw_starlark_unittest.bzl", "pw_unittest_suite")
load("//pw_env_setup/bazel/cipd/private/versions:relaxed_semver_parser.bzl", "parse")

def _checked_parse(env, input, expect):
    result = parse(input)
    asserts.equals(
        env,
        expect,
        result,
        "parse(\"{}\")".format(input),
    )
    return result

def _check_cmp(env, v1, v2, expect):
    r1 = parse(v1)
    r2 = parse(v2)

    cmp = "???"
    actual = False
    if expect == "lt":
        actual = r1 < r2
        cmp = "<"
    elif expect == "eq":
        actual = r1 == r2
        cmp = "=="
    elif expect == "gt":
        actual = r1 > r2
        cmp = ">"

    asserts.equals(
        env,
        True,
        actual,
        "parse(\"{}\") {} parse(\"{}\")".format(v1, cmp, v2),
    )

def _parse_basics_test_impl(ctx):
    env = unittest.begin(ctx)

    _checked_parse(env, "git_revision:abcdef", expect = None)

    # No epoch
    _checked_parse(env, "version:1", expect = (0, (((0, 1, "1"),),), 1))
    _checked_parse(env, "version:1.0-pre", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),)), 0, ((1, 0, "pre"),)))
    _checked_parse(env, "version:1.0", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),)), 1))
    _checked_parse(env, "version:1.0.0-pre", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),), ((0, 0, "0"),)), 0, ((1, 0, "pre"),)))
    _checked_parse(env, "version:1.0.0", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),), ((0, 0, "0"),)), 1))
    _checked_parse(env, "version:1.0.1-pre", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),), ((0, 1, "1"),)), 0, ((1, 0, "pre"),)))

    return unittest.end(env)

parse_basics_test = unittest.make(_parse_basics_test_impl)

def _parse_epochs_test_impl(ctx):
    env = unittest.begin(ctx)

    # Epoch
    _checked_parse(env, "version:1@2", expect = (1, (((0, 2, "2"),),), 1))
    _checked_parse(env, "version:1@2.3", expect = (1, (((0, 2, "2"),), ((0, 3, "3"),)), 1))
    _checked_parse(env, "version:1@2.3.4", expect = (1, (((0, 2, "2"),), ((0, 3, "3"),), ((0, 4, "4"),)), 1))
    _checked_parse(env, "version:1@2.3.4.5", expect = (1, (((0, 2, "2"),), ((0, 3, "3"),), ((0, 4, "4"),), ((0, 5, "5"),)), 1))

    _checked_parse(env, "version:1@1.0.0-alpha", expect = (1, (((0, 1, "1"),), ((0, 0, "0"),), ((0, 0, "0"),)), 0, ((1, 0, "alpha"),)))
    _checked_parse(env, "version:1@1.0.0-rc1", expect = (1, (((0, 1, "1"),), ((0, 0, "0"),), ((0, 0, "0"),)), 0, ((1, 0, "rc1"),)))
    _checked_parse(env, "version:1@1.0.0-rc10", expect = (1, (((0, 1, "1"),), ((0, 0, "0"),), ((0, 0, "0"),)), 0, ((1, 0, "rc10"),)))
    _checked_parse(env, "version:1@1.0.0-alpha.1", expect = (1, (((0, 1, "1"),), ((0, 0, "0"),), ((0, 0, "0"),)), 0, ((1, 0, "alpha"), (0, 1, "1"))))

    _checked_parse(env, "version:1@everything.42-dent", expect = (1, (((1, 0, "everything"),), ((0, 42, "42"),)), 0, ((1, 0, "dent"),)))

    return unittest.end(env)

parse_epochs_test = unittest.make(_parse_epochs_test_impl)

def _parse_relaxed_edge_cases_test_impl(ctx):
    env = unittest.begin(ctx)

    # Additional edge cases for relaxed parsing
    _checked_parse(env, "version:1.0a1", expect = (0, (((0, 1, "1"),), ((0, 0, "0"), (1, 0, "a"), (0, 1, "1"))), 1))
    _checked_parse(env, "version:1.0.1a", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),), ((0, 1, "1"), (1, 0, "a"))), 1))
    _checked_parse(env, "version:1.0.0-rc.0", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),), ((0, 0, "0"),)), 0, ((1, 0, "rc"), (0, 0, "0"))))
    _checked_parse(env, "version:1.0.0-0", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),), ((0, 0, "0"),)), 0, ((0, 0, "0"),)))
    _checked_parse(env, "version:1.0.0-01", expect = None)  # Leading zeros in prerelease numeric segments disallowed
    _checked_parse(env, "version:1.0a.b3", expect = (0, (((0, 1, "1"),), ((0, 0, "0"), (1, 0, "a")), ((1, 0, "b"), (0, 3, "3"))), 1))

    return unittest.end(env)

parse_relaxed_edge_cases_test = unittest.make(_parse_relaxed_edge_cases_test_impl)

def _bazel_test_normalized_impl(ctx):
    env = unittest.begin(ctx)

    # inclusive-language: disable
    # Ported from Bazel's VersionTest.java:
    # https://github.com/bazelbuild/bazel/blob/master/src/test/java/com/google/devtools/build/lib/bazel/bzlmod/VersionTest.java
    # inclusive-language: enable

    # VersionTest#testNormalized
    _checked_parse(env, "version:1.0", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),)), 1))
    _checked_parse(env, "version:1.0+build", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),)), 1))
    _checked_parse(env, "version:1.0-pre", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),)), 0, ((1, 0, "pre"),)))
    _checked_parse(env, "version:1.0-pre+build-kek.lol", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),)), 0, ((1, 0, "pre"),)))
    _checked_parse(env, "version:1.0+build-notpre", expect = (0, (((0, 1, "1"),), ((0, 0, "0"),)), 1))

    return unittest.end(env)

bazel_normalized_test = unittest.make(_bazel_test_normalized_impl)

def _bazel_test_release_version_impl(ctx):
    env = unittest.begin(ctx)

    # inclusive-language: disable
    # Ported from Bazel's VersionTest.java:
    # https://github.com/bazelbuild/bazel/blob/master/src/test/java/com/google/devtools/build/lib/bazel/bzlmod/VersionTest.java
    # inclusive-language:enable

    # VersionTest#testReleaseVersion
    _check_cmp(env, "version:2.0", "version:1.0", expect = "gt")
    _check_cmp(env, "version:2.0", "version:1.9", expect = "gt")
    _check_cmp(env, "version:11.0", "version:3.0", expect = "gt")
    _check_cmp(env, "version:1.0.1", "version:1.0", expect = "gt")
    _check_cmp(env, "version:1.0.0", "version:1.0", expect = "gt")
    _check_cmp(env, "version:1.0+build2", "version:1.0+build3", expect = "eq")
    _check_cmp(env, "version:1.0", "version:1.0-pre", expect = "gt")
    _check_cmp(env, "version:1.0", "version:1.0+build-notpre", expect = "eq")

    _check_cmp(env, "version:1.0.patch.3", "version:1.0", expect = "gt")
    _check_cmp(env, "version:1.0.patch.3", "version:1.0.patch.2", expect = "gt")
    _check_cmp(env, "version:1.0.patch.3", "version:1.0.patch.10", expect = "lt")
    _check_cmp(env, "version:1.0.patch3", "version:1.0.patch10", expect = "lt")
    _check_cmp(env, "version:4", "version:a", expect = "lt")
    _check_cmp(env, "version:abc", "version:abd", expect = "lt")

    return unittest.end(env)

bazel_release_version_test = unittest.make(_bazel_test_release_version_impl)

def _bazel_test_prerelease_version_impl(ctx):
    env = unittest.begin(ctx)

    # inclusive-language: disable
    # Ported from Bazel's VersionTest.java:
    # https://github.com/bazelbuild/bazel/blob/master/src/test/java/com/google/devtools/build/lib/bazel/bzlmod/VersionTest.java
    # inclusive-language:enable

    # VersionTest#testPrereleaseVersion
    _check_cmp(env, "version:1.0-pre", "version:1.0-are", expect = "gt")
    _check_cmp(env, "version:1.0-3", "version:1.0-2", expect = "gt")
    _check_cmp(env, "version:1.0-pre", "version:1.0-pre.foo", expect = "lt")
    _check_cmp(env, "version:1.0-pre.3", "version:1.0-pre.2", expect = "gt")
    _check_cmp(env, "version:1.0-pre.10", "version:1.0-pre.2", expect = "gt")
    _check_cmp(env, "version:1.0-pre.10a", "version:1.0-pre.2a", expect = "lt")
    _check_cmp(env, "version:1.0-pre.99", "version:1.0-pre.2a", expect = "lt")
    _check_cmp(env, "version:1.0-pre.patch.3", "version:1.0-pre.patch.4", expect = "lt")
    _check_cmp(env, "version:1.0--", "version:1.0----", expect = "lt")
    _check_cmp(env, "version:2.1.1-develop.bcr.20250113215904", "version:2.1.1-develop.bcr.20250113215903", expect = "gt")

    return unittest.end(env)

bazel_prerelease_version_test = unittest.make(_bazel_test_prerelease_version_impl)

def _bazel_test_parse_exception_impl(ctx):
    env = unittest.begin(ctx)

    # Ported from Bazel's VersionTest.java:
    # https://github.com/bazelbuild/bazel/blob/master/src/test/java/com/google/devtools/build/lib/bazel/bzlmod/VersionTest.java
    # VersionTest#testParseException
    _checked_parse(env, "version:-abc", expect = None)
    _checked_parse(env, "version:1_2", expect = None)
    _checked_parse(env, "version:ßážëł", expect = None)
    _checked_parse(env, "version:1.0-pre?", expect = None)

    _checked_parse(env, "version:1.0-pre///", expect = None)
    _checked_parse(env, "version:1..0", expect = None)
    _checked_parse(env, "version:1.0-pre..erp", expect = None)

    return unittest.end(env)

bazel_parse_exception_test = unittest.make(_bazel_test_parse_exception_impl)

def test_suite(name):
    pw_unittest_suite(
        name,
        parse_basics_test,
        parse_epochs_test,
        parse_relaxed_edge_cases_test,
        bazel_normalized_test,
        bazel_release_version_test,
        bazel_prerelease_version_test,
        bazel_parse_exception_test,
    )
