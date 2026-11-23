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
"""Bazel macro for flashing an STM32 Nucleo-F103 binary."""

load("@bazel_skylib//rules:native_binary.bzl", "native_binary")

def flash_stm32_nucleo_f103(
        *,
        name,
        stm32_binary,
        **kwargs):
    """Flash a binary to an attached STM32 Nucleo-F103RB board.

    Args:
      name: Name of the runnable flash target.
      stm32_binary: Binary target built for //targets/stm32_nucleo_f103.
      **kwargs: Additional attributes forwarded to native_binary.
    """
    native_binary(
        name = name,
        src = Label("//targets/stm32_nucleo_f103/py:flash"),
        args = ["$(rootpath " + str(stm32_binary) + ")"],
        data = [stm32_binary],
        out = name + ".exe",
        **kwargs
    )
