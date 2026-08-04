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

"""Bazel transitions for the rp2xxx series."""

load("@rules_platform//platform_data:defs.bzl", "platform_data")
load("//third_party/freertos:flags.bzl", "FREERTOS_FLAGS")

# Typical RP2040 pw_system backends and other platform configuration flags.
RP2_SYSTEM_FLAGS = FREERTOS_FLAGS | {
    "@bazel_tools//tools/cpp:link_extra_libs": str(Label("@pigweed//targets/rp2040:extra_platform_libs")),
    "@freertos//:freertos_config": str(Label("//targets/rp2040:freertos_config")),
    "@pico-sdk//bazel/config:PICO_CLIB": "llvm_libc",
    "@pico-sdk//bazel/config:PICO_STDIO_UART": True,
    "@pico-sdk//bazel/config:PICO_STDIO_USB": True,
    "@pico-sdk//bazel/config:PICO_TOOLCHAIN": "clang",
    "@pigweed//pw_assert:assert_backend": str(Label("//pw_assert_trap:assert_backend")),
    "@pigweed//pw_assert:assert_backend_impl": str(Label("//pw_assert_trap:impl")),
    "@pigweed//pw_assert:check_backend": str(Label("//pw_assert_trap:check_backend")),
    "@pigweed//pw_assert:check_backend_impl": str(Label("//pw_assert_trap:impl")),
    "@pigweed//pw_bloat:pw_size_report_bloaty_config": str(Label("//targets/rp2040:bloaty_config")),
    "@pigweed//pw_build:default_module_config": str(Label("//targets/rp2040:pigweed_module_config")),
    "@pigweed//pw_cpu_exception:entry_backend": str(Label("//pw_cpu_exception_cortex_m:cpu_exception")),
    "@pigweed//pw_cpu_exception:entry_backend_impl": str(Label("//pw_cpu_exception_cortex_m:cpu_exception_impl")),
    "@pigweed//pw_cpu_exception:handler_backend": str(Label("//pw_cpu_exception:basic_handler")),
    "@pigweed//pw_cpu_exception:support_backend": str(Label("//pw_cpu_exception_cortex_m:support")),
    "@pigweed//pw_crypto:ecdsa_backend": str(Label("//pw_crypto:ecdsa_mbedtls")),
    "@pigweed//pw_crypto:sha256_backend": str(Label("//pw_crypto:sha256_mbedtls")),
    "@pigweed//pw_interrupt:backend": str(Label("//pw_interrupt_cortex_m:impl")),
    "@pigweed//pw_log:backend": str(Label("//pw_log_tokenized")),
    "@pigweed//pw_log:backend_impl": str(Label("//pw_log_tokenized:impl")),
    "@pigweed//pw_log_tokenized:handler_backend": str(Label("//pw_system:log_backend")),
    "@pigweed//pw_perf_test:test_timer_backend": str(Label("//pw_perf_test:arm_cortex_timer")),
    "@pigweed//pw_sys_io:backend": str(Label("//pw_sys_io_rp2040")),
    "@pigweed//pw_system:device_handler_backend": str(Label("//targets/rp2040:device_handler")),
    "@pigweed//pw_system:extra_platform_libs": str(Label("//targets/rp2040:extra_platform_libs")),
    "@pigweed//pw_toolchain:cortex-m_toolchain_kind": "clang",
    "@pigweed//pw_trace:backend": str(Label("//pw_trace_tokenized:pw_trace_tokenized")),
    "@pigweed//pw_unit_test:backend": str(Label("//pw_unit_test:light")),
    "@pigweed//pw_unit_test:main": str(Label("//targets/rp2040:unit_test_app")),
}

def rp2040_binary(name = "", binary = "", testonly = False):
    return platform_data(name = name, target = binary, testonly = testonly, platform = "//targets/rp2040:rp2040")

def rp2350_binary(name = "", binary = "", testonly = False):
    return platform_data(name = name, target = binary, testonly = testonly, platform = "//targets/rp2040:rp2350")
