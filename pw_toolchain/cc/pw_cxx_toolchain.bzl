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
"""The Bzlmod extension that manages Pigweed's C/C++ toolchain distributions.

This is currently only configurable by Pigweed, but is exposed to give
downstream projects the ability to share the toolchain implementation or
forcibly override via override_repo().
"""

load("//pw_env_setup/bazel/cipd_setup:cipd_rules.bzl", "cipd_repository")

_CipdResourceInfo = provider(
    "An internal provider that collects information about a CIPD-hosted toolchain package",
    fields = {
        "build_file": "(label) Label pointing to the build file to inject into the downloaded CIPD repository",
        "cipd_path": "(string) Path to the CIPD resource",
        "cipd_tag_allowlist": "(list[string]) An optional allowlist of versions (by CIPD tag) of valid choices. If unset, any tag is permitted.",
        "repo_name": "(string) The repository name to expose the CIPD package as",
    },
)

_TOOLCHAIN_VARIANTS = {
    "arm_gcc": _CipdResourceInfo(
        repo_name = "gcc_arm_none_eabi_toolchain",
        cipd_path = "fuchsia/third_party/armgcc/${os}-${arch}",
        build_file = "//pw_toolchain/build_external:arm_none_eabi_gcc.BUILD",
    ),
    "llvm": _CipdResourceInfo(
        repo_name = "llvm_toolchain",
        cipd_path = "fuchsia/third_party/clang/${os}-${arch}",
        build_file = "//pw_toolchain/build_external:llvm_clang.BUILD",
    ),
    "zephyr": _CipdResourceInfo(
        repo_name = "zephyr_toolchain",
        cipd_path = "infra/3pp/tools/zephyr_sdk/${os}-${arch}",
        build_file = "//pw_toolchain/build_external:zephyr.BUILD",
    ),
}

def _pw_cxx_toolchain_impl(ctx):
    pigweed_module = None

    # If this is unlocked to allow downstream to configure, the first module in
    # the modules list is the current project a user is working from. Just check
    # that for tags before checking Pigweed.
    downstream_has_configured = False
    for module in ctx.modules:
        if module.name == "pigweed":
            pigweed_module = module
        else:
            for tag in dir(module.tags):
                tags = getattr(module.tags, tag)
                if tags:
                    downstream_has_configured = True

    if downstream_has_configured:
        fail(
            "Only Pigweed is allowed to configure the pw_cxx_toolchain",
            "extension at this time. Use override_repo() if you must override",
            "any of the toolchain packages.",
        )

    for toolchain_flavor, cipd_info in _TOOLCHAIN_VARIANTS.items():
        tag_metadata = getattr(pigweed_module.tags, toolchain_flavor, [])
        if not tag_metadata:
            fail("Pigweed didn't configure the {} toolchain of the pw_cxx_toolchain extension".format(toolchain_flavor))
        if len(tag_metadata) > 1:
            fail("Specifying more than one definition of the {} toolchain is undefined".format(toolchain_flavor))
        toolchain_metadata = tag_metadata[0]
        if not toolchain_metadata.cipd_tag:
            fail("`cipd_tag` must be set once for the {} toolchain in Pigweed".format(toolchain_flavor))

        cipd_tag_allowlist = getattr(cipd_info, "cipd_tag_allowlist", [])
        if cipd_tag_allowlist and cipd_info.cipd_tag not in cipd_tag_allowlist:
            fail("The {} toolchain must be one of the following CIPD tags: {}".format(toolchain_flavor, cipd_tag_allowlist))

        cipd_repository(
            name = cipd_info.repo_name,
            build_file = cipd_info.build_file,
            path = cipd_info.cipd_path,
            tag = toolchain_metadata.cipd_tag,
        )

_COMMON_TAG_CLASS_ATTRS = {
    "cipd_tag": attr.string(
        doc = "The CIPD tag to use when fetching this toolchain.",
    ),
}

pw_cxx_toolchain = module_extension(
    implementation = _pw_cxx_toolchain_impl,
    tag_classes = {
        key: tag_class(attrs = _COMMON_TAG_CLASS_ATTRS)
        for key in _TOOLCHAIN_VARIANTS.keys()
    },
    doc = """Generates repositories containing Pigweed C/C++ toolchain binary distributions.

    At this time, this is NOT configurable by downstream Pigweed users. However,
    the definitions of these repositories may be overridden with the
    override_repo() builtin in a MODULE.bazel file.
    """,
)
