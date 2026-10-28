# Copyright 2025 The Pigweed Authors
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

"""
A build file for qemu binaries.
"""

load("@bazel_skylib//rules:native_binary.bzl", "native_binary")
load("@pigweed//pw_build:pw_py_importable_runfile.bzl", "pw_py_importable_runfile")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

exports_files([
    "bin/qemu-system-arm",
    "bin/qemu-system-riscv32",
])

native_binary(
    name = "qemu-system-arm",
    src = "bin/qemu-system-arm",
)

native_binary(
    name = "qemu-system-riscv32",
    src = "bin/qemu-system-riscv32",
)

pw_py_importable_runfile(
    name = "qemu-system-arm-runfiles",
    src = ":qemu-system-arm",
    executable = True,
    import_location = "qemu.qemu_system_arm",
)

pw_py_importable_runfile(
    name = "qemu-system-riscv32-runfiles",
    src = ":qemu-system-riscv32",
    executable = True,
    import_location = "qemu.qemu_system_riscv32",
)
