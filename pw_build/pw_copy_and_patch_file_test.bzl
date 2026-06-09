# Copyright 2024 The Pigweed Authors
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
"""Test helpers for pw_copy_and_patch_file"""

load("@bazel_skylib//rules:diff_test.bzl", "diff_test")
load("//pw_build:compatibility.bzl", "incompatible_with_mcu")
load("//pw_build:pw_copy_and_patch_file.bzl", "pw_copy_and_patch_file")

def pw_copy_and_patch_file_test(*, name, src, patch_file, expected, out, **kwargs):
    """Validates that a patched files has the expected result.

    Args:
      name: The name of the target.
      src: The source file to be patched.
      out: The output file containing the patched contents.
      patch_file: The patch file.
      expected: The expected result of appling the patch.
      **kwargs: Passed to pw_copy_and_patch_file.
    """
    pw_copy_and_patch_file(
        name = name + ".test",
        src = src,
        out = out,
        patch_file = patch_file,
        target_compatible_with = incompatible_with_mcu(),
        testonly = True,
        **kwargs
    )
    diff_test(
        name = name,
        file1 = expected,
        file2 = ":" + name + ".test",
        target_compatible_with = incompatible_with_mcu(),
        **kwargs
    )
