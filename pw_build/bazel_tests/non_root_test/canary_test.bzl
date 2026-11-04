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
"""Analysis tests verifying that @pigweed does not leak non-dev toolchains."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

_CanaryToolchainInfo = provider(
    "Captures whether a C++ toolchain resolved for the canary platform.",
    fields = ["has_cc_toolchain"],
)

def _cortex_m0_transition_impl(_settings, _attr):
    return {
        "//command_line_option:platforms": str(
            Label("//:cortex_m0_canary_platform"),
        ),
    }

_cortex_m0_transition = transition(
    implementation = _cortex_m0_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
)

def _probe_mcu_cc_toolchain_impl(ctx):
    cc_toolchain = ctx.toolchains["@bazel_tools//tools/cpp:toolchain_type"]
    return [
        _CanaryToolchainInfo(
            has_cc_toolchain = (cc_toolchain != None),
        ),
    ]

probe_mcu_cc_toolchain = rule(
    implementation = _probe_mcu_cc_toolchain_impl,
    cfg = _cortex_m0_transition,
    toolchains = [
        config_common.toolchain_type(
            "@bazel_tools//tools/cpp:toolchain_type",
            mandatory = False,
        ),
    ],
)

def _no_leaked_mcu_toolchain_test_impl(ctx):
    env = analysistest.begin(ctx)
    target_under_test = analysistest.target_under_test(env)
    asserts.false(
        env,
        target_under_test[_CanaryToolchainInfo].has_cc_toolchain,
        "Expected no C++ toolchain to resolve for cortex_m0_canary_platform " +
        "when the root module only registers host toolchains, but one " +
        "resolved (indicating a non-dev register_toolchains leak in @pigweed).",
    )
    return analysistest.end(env)

no_leaked_mcu_toolchain_test = analysistest.make(
    _no_leaked_mcu_toolchain_test_impl,
)
