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
"""Unit tests for platforms/platforms.bzl."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("//pw_build:pw_starlark_unittest.bzl", "pw_unittest_suite")
load("//pw_env_setup/bazel/cipd/private/platforms:platforms.bzl", "get_cipd_arch", "get_cipd_os_name", "get_cipd_platform")

def _os(name, arch):
    return struct(name = name, arch = arch)

# Supported platforms
_LINUX_AARCH64 = _os("linux", "aarch64")
_LINUX_AMD64 = _os("linux", "amd64")
_MAC_OS_X_AARCH64 = _os("mac os x", "aarch64")
_MAC_OS_X_X86_64 = _os("mac os x", "x86_64")
_WINDOWS_11_X86_64 = _os("windows 11", "x86_64")

# (Synthetic) unsupported platforms
_FREEBSD_I386 = _os("freebsd", "i386")

def _get_cipd_os_name_test_impl(ctx):
    env = unittest.begin(ctx)

    asserts.equals(env, "linux", get_cipd_os_name(_LINUX_AARCH64))
    asserts.equals(env, "linux", get_cipd_os_name(_LINUX_AMD64))
    asserts.equals(env, "mac", get_cipd_os_name(_MAC_OS_X_AARCH64))
    asserts.equals(env, "mac", get_cipd_os_name(_MAC_OS_X_X86_64))
    asserts.equals(env, "windows", get_cipd_os_name(_WINDOWS_11_X86_64))

    asserts.equals(env, None, get_cipd_os_name(_FREEBSD_I386))

    return unittest.end(env)

get_cipd_os_name_test = unittest.make(_get_cipd_os_name_test_impl)

def _get_cipd_arch_test_impl(ctx):
    env = unittest.begin(ctx)

    asserts.equals(env, "arm64", get_cipd_arch(_LINUX_AARCH64))
    asserts.equals(env, "amd64", get_cipd_arch(_LINUX_AMD64))
    asserts.equals(env, "arm64", get_cipd_arch(_MAC_OS_X_AARCH64))
    asserts.equals(env, "amd64", get_cipd_arch(_MAC_OS_X_X86_64))
    asserts.equals(env, "amd64", get_cipd_arch(_WINDOWS_11_X86_64))

    asserts.equals(env, None, get_cipd_arch(_FREEBSD_I386))

    return unittest.end(env)

get_cipd_arch_test = unittest.make(_get_cipd_arch_test_impl)

def _get_cipd_platform_test_impl(ctx):
    env = unittest.begin(ctx)

    asserts.equals(env, "linux-arm64", get_cipd_platform(_LINUX_AARCH64))
    asserts.equals(env, "linux-amd64", get_cipd_platform(_LINUX_AMD64))
    asserts.equals(env, "mac-arm64", get_cipd_platform(_MAC_OS_X_AARCH64))
    asserts.equals(env, "mac-amd64", get_cipd_platform(_MAC_OS_X_X86_64))
    asserts.equals(env, "windows-amd64", get_cipd_platform(_WINDOWS_11_X86_64))

    asserts.equals(env, None, get_cipd_platform(_FREEBSD_I386))

    return unittest.end(env)

get_cipd_platform_test = unittest.make(_get_cipd_platform_test_impl)

def test_suite(name):
    pw_unittest_suite(
        name,
        get_cipd_os_name_test,
        get_cipd_arch_test,
        get_cipd_platform_test,
    )
