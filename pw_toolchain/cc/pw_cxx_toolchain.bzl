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

Pigweed provides default CIPD tags for each toolchain variant, and the root
module may optionally override individual toolchain tags.
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
        cipd_path = "fuchsia/third_party/3pp/armgcc/${os}-${arch}",
        build_file = "//pw_toolchain/build_external:arm_none_eabi_gcc.BUILD",
    ),
    "llvm": _CipdResourceInfo(
        repo_name = "llvm_toolchain",
        cipd_path = "fuchsia/third_party/clang/${os}-${arch}",
        build_file = "//pw_toolchain/build_external:llvm_clang.BUILD",
    ),
    "zephyr_gcc": _CipdResourceInfo(
        repo_name = "zephyr_toolchain",
        cipd_path = "infra/3pp/tools/zephyr_sdk/${os}-${arch}",
        build_file = "//pw_toolchain/build_external:zephyr_gnu.BUILD",
    ),
    "zephyr_llvm": _CipdResourceInfo(
        repo_name = "zephyr_llvm_toolchain",
        cipd_path = "infra/3pp/tools/zephyr_sdk/${os}-${arch}",
        build_file = "//pw_toolchain/build_external:zephyr_llvm.BUILD",
    ),
}

def _get_toolchain_tag(module, toolchain_flavor, mandatory = False):
    if not module:
        if mandatory:
            return None, "Expected `pigweed` module to be present in module graph"
        return None, None
    tag_metadata = getattr(module.tags, toolchain_flavor, [])
    if not tag_metadata:
        if mandatory:
            return None, "Pigweed didn't configure the {} toolchain of the pw_cxx_toolchain extension".format(toolchain_flavor)
        return None, None
    if len(tag_metadata) > 1:
        return None, "Specifying more than one definition of the {} toolchain is undefined".format(toolchain_flavor)
    toolchain_metadata = tag_metadata[0]
    if not toolchain_metadata.cipd_tag:
        return None, "`cipd_tag` must be set once for the {} toolchain in {}".format(toolchain_flavor, module.name)
    return toolchain_metadata, None

def resolve_toolchain_tags(modules, variants = _TOOLCHAIN_VARIANTS):
    """Resolves CIPD tags for each C/C++ toolchain variant across Bzlmod modules.

    Args:
        modules: Sequence of module objects from `module_ctx.modules`.
        variants: Mapping of toolchain flavor name to `_CipdResourceInfo`.

    Returns:
        Tuple of `(resolved_tags, err)` where `resolved_tags` maps each
        `toolchain_flavor` to its selected tag metadata struct and `err` is an
        error string (or `None` on success).
    """
    root_module = None
    pigweed_module = None

    for module in modules:
        if module.is_root:
            root_module = module
        if module.name == "pigweed":
            pigweed_module = module

    resolved = {}
    for toolchain_flavor, cipd_info in variants.items():
        root_tag, err = _get_toolchain_tag(root_module, toolchain_flavor)
        if err:
            return None, err
        pigweed_tag, err = _get_toolchain_tag(
            pigweed_module,
            toolchain_flavor,
            mandatory = True,
        )
        if err:
            return None, err
        toolchain_metadata = root_tag or pigweed_tag

        cipd_tag_allowlist = getattr(cipd_info, "cipd_tag_allowlist", [])
        if cipd_tag_allowlist and toolchain_metadata.cipd_tag not in cipd_tag_allowlist:
            return None, "The {} toolchain must be one of the following CIPD tags: {}".format(toolchain_flavor, cipd_tag_allowlist)

        resolved[toolchain_flavor] = toolchain_metadata

    return resolved, None

def _pw_cxx_toolchain_impl(ctx):
    resolved, err = resolve_toolchain_tags(ctx.modules)
    if err:
        fail(err)

    for toolchain_flavor, cipd_info in _TOOLCHAIN_VARIANTS.items():
        toolchain_metadata = resolved[toolchain_flavor]
        cipd_repository(
            name = cipd_info.repo_name,
            build_file = cipd_info.build_file,
            path = cipd_info.cipd_path,
            tag = toolchain_metadata.cipd_tag,
        )

    return ctx.extension_metadata(reproducible = True)

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

    Pigweed configures default CIPD tags for each toolchain variant. The root
    module may optionally configure individual toolchain tags to override
    Pigweed's defaults, or override repository definitions with override_repo()
    in a MODULE.bazel file.
    """,
)
