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
"""Unit tests for resolver.bzl."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("//pw_build:pw_starlark_unittest.bzl", "pw_unittest_suite")
load("//pw_env_setup/bazel/cipd/private/resolver:resolver.bzl", "resolve_repo_configuration")

def _resolve_repo_configurations_test_impl(ctx):
    env = unittest.begin(ctx)

    fake_os = struct(name = "linux", arch = "amd64")

    def cfg(packages = None, patches = None, patch_args = None, build_file = None, is_dev = False, from_name = None, from_root = False, from_source = None, reason = None):
        return struct(
            packages = packages or {},
            patches = patches or [],
            patch_args = patch_args or [],
            build_file = build_file,
            is_dev = is_dev,
            from_name = from_name,
            from_root = from_root,
            from_source = from_source,
            reason = reason,
        )

    def check(id, doc = None, configs = None, expect = None, expect_err = None):
        actual, actual_err = resolve_repo_configuration(id, os = fake_os, configs = configs)

        if expect_err:
            if actual_err:
                all_found = True
                for sub in expect_err:
                    found = sub in actual_err.msg
                    for src in actual_err.srcs:
                        if sub in str(src):
                            found = True
                    if not found:
                        all_found = False
                        break
                if all_found:
                    expect_err = actual_err
                else:
                    expect_err = "[Contains: {}]".format(", ".join(expect_err))
            else:
                expect_err = "[Contains: {}]".format(", ".join(expect_err))

        asserts.equals(
            env,
            (expect, expect_err),
            (actual, actual_err),
            "{} - {}\n".format(id, doc),
        )

    check(
        "reserved-scope-ok",
        doc = "If a scoped name is used, it must match the requestors module name",
        configs = [
            cfg({"pkg": "version:1"}, from_name = "test", from_source = "src1"),
        ],
        expect = cfg({"pkg": "version:1"}, from_name = "test", from_source = "src1", reason = "unique"),
    )

    check(
        "other.scope-err",
        doc = "Requesting scopes for other modules is reserved for them.",
        configs = [
            cfg({"pkg": "version:1"}, from_name = "fail", from_source = "src1"),
        ],
        expect_err = ("reserved", "other", "fail", "src1"),
    )

    check(
        "one-request-per-module-err",
        doc = "Require only one such request from any requestor",
        configs = [
            cfg({"pkg": "version:1"}, from_name = "test", from_source = "src1"),
            cfg({"pkg": "version:1"}, from_name = "test", from_source = "src2"),
        ],
        expect_err = ("multiple locations", "src1", "src2"),
    )

    check(
        "test.patched-repos-must-be-namespaced-ok-1",
        doc = "Patched repos must be namespaced.",
        configs = [
            cfg({"a": "version:1", "b": "version:2"}, patches = ["p1"], from_name = "test"),
        ],
        expect = cfg({"a": "version:1", "b": "version:2"}, patches = ["p1"], from_name = "test", reason = "unique"),
    )

    check(
        "test.patched-repos-must-be-namespaced-ok-2",
        doc = "Patched repos must be namespaced.",
        configs = [
            cfg({"a": "version:1", "b": "version:2"}, patch_args = ["pa"], from_name = "test"),
        ],
        expect = cfg({"a": "version:1", "b": "version:2"}, patch_args = ["pa"], from_name = "test", reason = "unique"),
    )

    check(
        "patched-repos-must-be-namespaced-err-1",
        doc = "Patched repos must be namespaced.",
        configs = [
            cfg({"a": "version:1", "b": "version:2"}, patches = ["p1"], from_name = "test", from_source = "src1"),
        ],
        expect_err = ("namespace prefix", "test", "src1"),
    )

    check(
        "patched-repos-must-be-namespaced-err-2",
        doc = "Patched repos must be namespaced.",
        configs = [
            cfg({"a": "version:1", "b": "version:2"}, patch_args = ["pa"], from_name = "test", from_source = "src1"),
        ],
        expect_err = ("namespace prefix", "test", "src1"),
    )

    check(
        "dev-is-not-stub-if-root",
        doc = "A dev_dependency from the root repo is not stubbed",
        configs = [
            cfg({"pkg": "version:1"}, is_dev = True, from_root = True),
        ],
        expect = cfg({"pkg": "version:1"}, reason = "unique"),
    )

    check(
        "dev-is-stub-if-not-root",
        doc = "A dev_dependency from a non-root is stubbed",
        configs = [
            cfg({"pkg": "version:1"}, is_dev = True),
        ],
        expect = cfg(packages = None, reason = "dev-only"),
    )

    check(
        "dev-is-not-stub-if-shared",
        doc = "A dev_dependency is not stubbed if shared",
        configs = [
            cfg({"pkg": "version:1"}, is_dev = True),
            cfg({"pkg": "version:1"}),
        ],
        expect = cfg({"pkg": "version:1"}, reason = "unique"),
    )

    check(
        "packages-must-match-1",
        doc = "Multiple requests must specify the same packages",
        configs = [
            cfg({"a": "version:1"}),
            cfg({"a": "version:1"}),
        ],
        expect = cfg({"a": "version:1"}, reason = "unique"),
    )

    check(
        "packages-must-match-2",
        doc = "Multiple requests must specify the same packages",
        configs = [
            cfg({"a": "version:1", "b": "version:2"}),
            cfg({"a": "version:1", "b": "version:2"}),
        ],
        expect = cfg({"a": "version:1", "b": "version:2"}, reason = "unique"),
    )

    check(
        "packages-must-match-err-1",
        doc = "Multiple requests must specify the same packages",
        configs = [
            cfg({"a": "version:1"}, from_source = "src1"),
            cfg({"b": "version:1"}, from_source = "src2"),
        ],
        expect_err = ("must be the same", "src1", "src2"),
    )

    check(
        "packages-must-match-err-2",
        doc = "Multiple requests must specify the same packages",
        configs = [
            cfg({"a": "version:1", "b": "version:1"}, from_source = "src1"),
            cfg({"b": "version:1"}, from_source = "src2"),
        ],
        expect_err = ("must be the same", "src1", "src2"),
    )

    check(
        "semantic-versions-resolve-to-latest",
        doc = "Multiple requests must specify the same packages",
        configs = [
            cfg({"a": "version:1"}),
            cfg({"a": "version:2-rc1"}),
            cfg({"a": "version:2"}),
        ],
        expect = cfg({"a": "version:2"}, reason = "resolved"),
    )

    check(
        "nonsemantic-versions-cannot-be-compared-err",
        doc = "Multiple requests must specify the same packages",
        configs = [
            cfg({"a": "version:1"}, from_source = "src1"),
            cfg({"a": "latest"}, from_source = "src2"),
        ],
        expect_err = ("compare non-semantic versions", "src1", "src2"),
    )

    check(
        "multipackage-versions-must-compare-ok",
        doc = """If multiple versions a requested with multiple packages, there
                 must be an unambiguous "newest".""",
        configs = [
            cfg({"a": "version:1", "b": "version:1"}, from_name = "r1", from_source = "src1"),
            cfg({"a": "version:2", "b": "version:2"}, from_name = "r2", from_source = "src2"),
        ],
        expect = cfg({"a": "version:2", "b": "version:2"}, from_name = "r2", from_source = "src2", reason = "resolved"),
    )

    check(
        "multipackage-versions-must-compare-ok",
        doc = """If multiple versions a requested with multiple packages, there
                 must be an unambiguous "newest".""",
        configs = [
            cfg({"a": "tag1", "b": "tag2"}),
            cfg({"a": "tag1", "b": "tag2"}),
        ],
        expect = cfg({"a": "tag1", "b": "tag2"}, reason = "unique"),
    )

    check(
        "multipackage-versions-must-compare-error",
        doc = """If multiple versions a requested with multiple packages, there
                 must be an unambiguous "newest".""",
        configs = [
            cfg({"a": "tag1", "b": "tag2"}, from_source = "src1"),
            cfg({"a": "tag2", "b": "tag2"}, from_source = "src2"),
        ],
        expect_err = ("compare non-semantic versions", "src1", "src2"),
    )

    check(
        "multipackage-versions-disjoint-err",
        doc = """The "newest" must match one of the requested configs.""",
        configs = [
            cfg({"a": "version:1", "b": "version:2"}, from_source = "src1"),
            cfg({"a": "version:2", "b": "version:1"}, from_source = "src2"),
        ],
        expect_err = ("irreconcilable version", "src1", "src2"),
    )

    check(
        "different-versions-same-build_file-ok",
        doc = "Different versions can use the same injected build file",
        configs = [
            cfg({"a": "version:1"}, build_file = "b1"),
            cfg({"a": "version:2"}, build_file = "b1"),
        ],
        expect = cfg({"a": "version:2"}, build_file = "b1", reason = "resolved"),
    )

    check(
        "different-build_file-err",
        doc = "The build file must be the same between requests",
        configs = [
            cfg({"a": "version:1"}, build_file = "b1", from_source = "src1"),
            cfg({"a": "version:1"}, build_file = "b2", from_source = "src2"),
        ],
        expect_err = ("same value for \"build_file\"", "src1", "src2"),
    )

    check(
        "arch-expansion",
        doc = "{arch} is expanded in package paths.",
        configs = [
            cfg({"pkg-${arch}": "latest"}),
        ],
        expect = cfg({"pkg-amd64": "latest"}, reason = "unique"),
    )

    check(
        "os-expansion",
        doc = "{os} is expanded in package paths.",
        configs = [
            cfg({"pkg-${os}": "latest"}),
        ],
        expect = cfg({"pkg-linux": "latest"}, reason = "unique"),
    )

    check(
        "platform-expansion",
        doc = "{platform} is expanded in package paths.",
        configs = [
            cfg({"pkg-${platform}": "latest"}),
        ],
        expect = cfg({"pkg-linux-amd64": "latest"}, reason = "unique"),
    )

    check(
        "placeholder-restrictions-can-select-versions",
        doc = "Placeholder restrictions can be used to select versions.",
        configs = [
            cfg({
                "pkg-${os=linux}": "linux-ver",
                "pkg-${os=windows}": "windows-ver",
            }),
        ],
        expect = cfg({"pkg-linux": "linux-ver"}, reason = "unique"),
    )

    check(
        "placeholder-restrictions-can-select-stub",
        doc = "If the restrictions exclude all packages a stub is used.",
        configs = [
            cfg({"pkg-${os=prod_os}": "prod"}),
        ],
        expect = cfg(packages = None, reason = "empty-stub"),
    )

    check(
        "mixed-placeholder-use-is-err",
        doc = "All requests must use placeholders consistently.",
        configs = [
            cfg({"pkg-${os=linux}": "test"}, from_source = "src1"),
            cfg({"pkg-linux": "test"}, from_source = "src2"),
        ],
        expect_err = ("must be the same", "src1", "src2"),
    )

    check(
        "requested-packages-empty-err",
        doc = "Treat an empty packages dict as an error.",
        configs = [
            cfg({}, from_source = "src1"),
        ],
        expect_err = ("no packages", "src1"),
    )

    check(
        "requested-versions-empty-err",
        doc = "Treat an empty version as an error.",
        configs = [
            cfg({"pkg": ""}, from_source = "src1"),
        ],
        expect_err = ("invalid version", "src1"),
    )

    check(
        "requested-packages-not-blank-err",
        doc = "Package paths before placeholder expansion cannot be blank.",
        configs = [
            cfg({"": "version:1"}, from_source = "src1"),
        ],
        expect_err = ("invalid package", "src1"),
    )

    return unittest.end(env)

resolve_repo_configurations_test = unittest.make(_resolve_repo_configurations_test_impl)

def test_suite(name):
    pw_unittest_suite(
        name,
        resolve_repo_configurations_test,
    )
