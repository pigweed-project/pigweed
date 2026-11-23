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
"""Bazel platform flags and transition helpers for STM32 Nucleo-F103."""

load("@rules_platform//platform_data:defs.bzl", "platform_data")
load("//third_party/freertos:flags.bzl", "FREERTOS_FLAGS")

STM32_NUCLEO_F103_FLAGS = FREERTOS_FLAGS | {
    str(Label("//pw_assert:assert_backend")): str(Label("//pw_assert:assert_compatibility_backend")),
    str(Label("//pw_assert:assert_backend_impl")): str(Label("@rules_cc//:empty_lib")),
    str(Label("//pw_assert:check_backend")): str(Label("//pw_assert_basic")),
    str(Label("//pw_assert:check_backend_impl")): str(Label("//pw_assert_basic:impl")),
    str(Label("//pw_boot:backend")): str(Label("//pw_boot_cortex_m")),
    str(Label("//pw_build:default_module_config")): str(Label("//targets/stm32_nucleo_f103:pigweed_module_config")),
    str(Label("//pw_interrupt:backend")): str(Label("//pw_interrupt_cortex_m:context")),
    str(Label("//pw_log:backend")): str(Label("//pw_log_basic")),
    str(Label("//pw_log:backend_impl")): str(Label("//pw_log_basic:impl")),
    str(Label("//pw_malloc:backend")): str(Label("//pw_malloc:best_fit")),
    str(Label("//pw_perf_test:test_timer_backend")): str(Label("//pw_perf_test:arm_cortex_timer")),
    str(Label("//pw_sys_io:backend")): str(Label("//targets/stm32_nucleo_f103:sys_io")),
    str(Label("//pw_system:extra_platform_libs")): str(Label("//targets/stm32_nucleo_f103:extra_platform_libs")),
    str(Label("//pw_toolchain:cortex-m_toolchain_kind")): "gcc",
    str(Label("//pw_toolchain/arm_gcc:freestanding")): "False",
    str(Label("//pw_unit_test:backend")): str(Label("//pw_unit_test:light")),
    str(Label("//pw_unit_test:main")): str(Label("//targets/stm32_nucleo_f103:unit_test_main")),
    "@bazel_tools//tools/cpp:link_extra_libs": str(Label("//targets/stm32_nucleo_f103:extra_platform_libs")),
    "@freertos//:freertos_config": str(Label("//targets/stm32_nucleo_f103:freertos_config")),
}

def stm32_nucleo_f103_binary(*, name, binary, **kwargs):
    platform_data(
        name = name,
        target = binary,
        platform = Label("//targets/stm32_nucleo_f103:stm32_nucleo_f103"),
        **kwargs
    )
