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
"""Extension for declaring Pigweed Rust toolchains."""

load("//pw_env_setup/bazel/cipd_setup:cipd_rules.bzl", "cipd_repository")
load(":templates.bzl", "rust_analyzer_toolchain_template", "rust_toolchain_no_prebuilt_template", "rust_toolchain_template", "rustfmt_toolchain_template", "toolchain_template")
load(":toolchains.bzl", "CHANNELS", "EXTRA_TARGETS", "HOSTS")

def _module_cipd_tag(module):
    """\
    Returns the `cipd_tag` tag for the given module.

    Latter delcations will take precedence of ealier ones.
    """
    cipd_tag = None
    for toolchain in module.tags.toolchain:
        cipd_tag = toolchain.cipd_tag

    return cipd_tag

def _find_cipd_tag(ctx):
    """\
    Returns the CIPD tag specified in either the root or pigweed modules.

    The tag from the root module will take priority over the tag from the
    pigweed module.
    """

    pigweed_module = None
    root_tag = None

    for module in ctx.modules:
        if module.is_root:
            root_tag = _module_cipd_tag(module)
        if module.name == "pigweed":
            pigweed_module = module

    if pigweed_module == None:
        fail("Unable to find pigweed module")

    return root_tag or _module_cipd_tag(pigweed_module)

def _normalize_os_to_cipd(os):
    """\
    Translate a bazel OS name to one used by CIPD.
    """
    if os == "macos":
        return "mac"

    return os

def _tags_to_hosts(tags):
    return [
        {
            "cipd_arch": host.cipd_arch,
            "cpu": host.cpu,
            "dylib_ext": host.dylib_ext,
            "os": host.os,
            "triple": host.triple,
        }
        for host in tags
    ]

def _tags_to_extra_targets(tags):
    return [
        {
            "build_std": target.build_std,
            "constraints": target.constraints,
            "cpu": target.cpu,
            "triple": target.triple,
        }
        for target in tags
    ]

def _tags_to_channels(tags):
    return [
        {
            "default_edition": channel.default_edition,
            "extra_rustc_flags": channel.extra_rustc_flags,
            "name": channel.name,
            "target_settings": channel.target_settings,
        }
        for channel in tags
    ]

def _pw_rust_impl(ctx):
    cipd_tag = _find_cipd_tag(ctx)

    hosts = None
    extra_targets = None
    channels = None

    pigweed_module = None
    root_module = None

    for module in ctx.modules:
        if module.name == "pigweed":
            pigweed_module = module

        if module.is_root:
            root_module = module

    # Extensions are non-hermetic and can be configured by the root module.
    # We allow the root module to override the default configuration for
    # hosts, targets, and channels. If the root module does not specify a
    # configuration, we check if the pigweed module has one. If not, we fall
    # back to the defaults.
    if root_module:
        if root_module.tags.host:
            hosts = _tags_to_hosts(root_module.tags.host)

        if root_module.tags.target:
            extra_targets = _tags_to_extra_targets(root_module.tags.target)

        if root_module.tags.channel:
            channels = _tags_to_channels(root_module.tags.channel)

    if pigweed_module:
        if hosts == None and pigweed_module.tags.host:
            hosts = _tags_to_hosts(pigweed_module.tags.host)

        if extra_targets == None and pigweed_module.tags.target:
            extra_targets = _tags_to_extra_targets(pigweed_module.tags.target)

        if channels == None and pigweed_module.tags.channel:
            channels = _tags_to_channels(pigweed_module.tags.channel)

    if hosts == None:
        hosts = HOSTS
    if extra_targets == None:
        extra_targets = EXTRA_TARGETS
    if channels == None:
        channels = CHANNELS

    # Register CIPD repositories for toolchain binaries
    for host in hosts:
        cipd_os = _normalize_os_to_cipd(host["os"])

        cipd_repository(
            name = "rust_toolchain_host_{}_{}".format(host["os"], host["cpu"]),
            build_file = "//pw_toolchain/rust:rust_toolchain.BUILD",
            path = "fuchsia/third_party/rust/host/{}-{}".format(cipd_os, host["cipd_arch"]),
            tag = cipd_tag,
        )

        cipd_repository(
            name = "rust_toolchain_target_{}_{}".format(host["triple"], host["cpu"]),
            build_file = "//pw_toolchain/rust:rust_stdlib.BUILD",
            path = "fuchsia/third_party/rust/target/{}".format(host["triple"]),
            tag = cipd_tag,
        )

    for target in extra_targets:
        build_std = target.get("build_std", False)
        if not build_std:
            cipd_repository(
                name = "rust_toolchain_target_{}_{}".format(target["triple"], target["cpu"]),
                build_file = "//pw_toolchain/rust:rust_stdlib.BUILD",
                path = "fuchsia/third_party/rust/target/{}".format(target["triple"]),
                tag = cipd_tag,
            )

    _toolchain_repository_hub(
        name = "pw_rust_toolchains",
        hosts = json.encode(hosts),
        extra_targets = json.encode(extra_targets),
        channels = json.encode(channels),
    )

_RUST_TOOLCHAIN_TAG = tag_class(
    attrs = dict(
        cipd_tag = attr.string(
            doc = "The CIPD tag to use when fetching the Rust toolchain.",
        ),
    ),
)

_HOST_TAG = tag_class(
    attrs = {
        "cipd_arch": attr.string(mandatory = True),
        "cpu": attr.string(mandatory = True),
        "dylib_ext": attr.string(mandatory = True),
        "os": attr.string(mandatory = True),
        "triple": attr.string(mandatory = True),
    },
)

_TARGET_TAG = tag_class(
    attrs = {
        "build_std": attr.bool(default = False),
        "constraints": attr.string_list(),
        "cpu": attr.string(mandatory = True),
        "triple": attr.string(mandatory = True),
    },
)

_CHANNEL_TAG = tag_class(
    attrs = {
        "default_edition": attr.string(),
        "extra_rustc_flags": attr.string_list(),
        "name": attr.string(mandatory = True),
        "target_settings": attr.string_list(),
    },
)

pw_rust = module_extension(
    implementation = _pw_rust_impl,
    tag_classes = {
        "channel": _CHANNEL_TAG,
        "host": _HOST_TAG,
        "target": _TARGET_TAG,
        "toolchain": _RUST_TOOLCHAIN_TAG,
    },
    doc = """Generate a repository for all Pigweed Rust toolchains.

        Declares a suite of Rust toolchains that may be registered in a
        MODULE.bazel file. If you would like to use the Toolchains provided
        by Pigweed, add these lines to your MOUDLE.bazel:
        ```
        pw_rust = use_extension("@pigweed//pw_toolchain/rust:extensions.bzl", "pw_rust")
        use_repo(pw_rust, "pw_rust_toolchains")
        register_toolchains(
            "@pw_rust_toolchains//:all",
            dev_dependency = True,
        )
        ```

        If you would like to override the rust compiler version, you can specify a
        CIPD version for an alternative toolchain to use in your project. Note that
        only the root module's specification of this tag is applied, and that if no
        version tag is specified Pigweed's value will be used as a fallback.
        ```
        pw_rust = use_extension("@pigweed//pw_toolchain/rust:extensions.bzl", "pw_rust")
        pw_rust.toolchain(cipd_tag = "rust_revision:bf9c7a64ad222b85397573668b39e6d1ab9f4a72")
        use_repo(pw_rust, "pw_rust_toolchains")
        register_toolchains(
            "@pw_rust_toolchains//:all",
            dev_dependency = True,
        )
        ```

        To configure the toolchains, you can use the `host`, `target`, and `channel` tags.
        If any of these tags are specified, the defaults are ignored and only the
        configuration provided in the tags are used.

        ```
        pw_rust = use_extension("@pigweed//pw_toolchain/rust:extensions.bzl", "pw_rust")

        # Configure multiple host platforms (e.g., macOS ARM64 and x86_64)
        pw_rust.host(
            cipd_arch = "arm64",
            cpu = "aarch64",
            dylib_ext = ".dylib",
            os = "macos",
            triple = "aarch64-apple-darwin",
        )

        pw_rust.host(
            cipd_arch = "amd64",
            cpu = "x86_64",
            dylib_ext = ".so",
            os = "linux",
            triple = "x86_64-unknown-linux-gnu",
        )

        # Configure an embedded target (e.g., Cortex-M33)
        pw_rust.target(
            build_std = True,
            cpu = "armv8-m",
            triple = "thumbv8m.main-none-eabihf",
        )

        # Configure the stable channel
        pw_rust.channel(
            name = "stable",
            extra_rustc_flags = ["-Dwarnings", "-Zemit-stack-sizes", "-Zmacro-backtrace"],
            target_settings = ["@rules_rust//rust/toolchain/channel:stable"],
        )

        use_repo(pw_rust, "pw_rust_toolchains")
        register_toolchains(
            "@pw_rust_toolchains//:all",
        )
        ```
    """,
)

def _pw_rust_toolchain(
        name,
        exec_triple,
        target_triple,
        toolchain_repo,
        target_repo,
        dylib_ext,
        exec_compatible_with,
        target_compatible_with,
        target_settings,
        extra_rustc_flags,
        analyzer_toolchain_name = None,
        rustfmt_toolchain_name = None,
        build_std = False,
        default_edition = None):
    if build_std:
        build_file = rust_toolchain_no_prebuilt_template(
            name = name,
            exec_compatible_with = exec_compatible_with,
            target_compatible_with = target_compatible_with,
            dylib_ext = dylib_ext,
            toolchain_repo = toolchain_repo,
            exec_triple = exec_triple,
            target_triple = target_triple,
            extra_rustc_flags = extra_rustc_flags,
            default_edition = default_edition,
        )
    else:
        build_file = rust_toolchain_template(
            name = name,
            exec_compatible_with = exec_compatible_with,
            target_compatible_with = target_compatible_with,
            dylib_ext = dylib_ext,
            target_repo = target_repo,
            toolchain_repo = toolchain_repo,
            exec_triple = exec_triple,
            target_triple = target_triple,
            extra_rustc_flags = extra_rustc_flags,
            default_edition = default_edition,
        )

    build_file += toolchain_template(
        name = name,
        exec_compatible_with = exec_compatible_with,
        target_compatible_with = target_compatible_with,
        target_settings = target_settings,
    )

    if analyzer_toolchain_name:
        build_file += rust_analyzer_toolchain_template(
            name = analyzer_toolchain_name,
            toolchain_repo = toolchain_repo,
            exec_compatible_with = exec_compatible_with,
            target_compatible_with = target_compatible_with,
            target_settings = target_settings,
        )

    if rustfmt_toolchain_name:
        build_file += rustfmt_toolchain_template(
            name = rustfmt_toolchain_name,
            toolchain_repo = toolchain_repo,
            exec_compatible_with = exec_compatible_with,
            target_compatible_with = target_compatible_with,
            target_settings = target_settings,
        )

    return build_file

def _BUILD_for_toolchain_repo(hosts, extra_targets, channels):
    # Declare rust toolchains
    build_file = """load("@rules_rust//rust:toolchain.bzl", "rust_analyzer_toolchain", "rustfmt_toolchain", "rust_toolchain")\n"""
    build_file += """load("@rules_rust//rust:defs.bzl", "rust_library_group")\n"""
    build_file += """load("@rules_rust_prost//:defs.bzl", "rust_prost_toolchain")\n"""

    for channel in channels:
        for host in hosts:
            build_file += _pw_rust_toolchain(
                name = "host_rust_toolchain_{}_{}_{}".format(host["os"], host["cpu"], channel["name"]),
                analyzer_toolchain_name = "host_rust_analyzer_toolchain_{}_{}_{}".format(host["os"], host["cpu"], channel["name"]),
                rustfmt_toolchain_name = "host_rustfmt_toolchain_{}_{}_{}".format(host["os"], host["cpu"], channel["name"]),
                exec_compatible_with = [
                    "@platforms//cpu:{}".format(host["cpu"]),
                    "@platforms//os:{}".format(host["os"]),
                ],
                target_compatible_with = [
                    "@platforms//cpu:{}".format(host["cpu"]),
                    "@platforms//os:{}".format(host["os"]),
                ],
                target_settings = channel["target_settings"],
                dylib_ext = host["dylib_ext"],
                target_repo = "@rust_toolchain_target_{}_{}".format(host["triple"], host["cpu"]),
                toolchain_repo = "@rust_toolchain_host_{}_{}".format(host["os"], host["cpu"]),
                exec_triple = host["triple"],
                target_triple = host["triple"],
                extra_rustc_flags = channel["extra_rustc_flags"],
                default_edition = channel.get("default_edition"),
            )

            for target in extra_targets:
                build_file += _pw_rust_toolchain(
                    name = "{}_{}_rust_toolchain_{}_{}_{}".format(host["os"], host["cpu"], target["triple"], target["cpu"], channel["name"]),
                    exec_triple = host["triple"],
                    target_triple = target["triple"],
                    target_repo = "@rust_toolchain_target_{}_{}".format(target["triple"], target["cpu"]),
                    toolchain_repo = "@rust_toolchain_host_{}_{}".format(host["os"], host["cpu"]),
                    dylib_ext = "*.so",
                    exec_compatible_with = [
                        "@platforms//cpu:{}".format(host["cpu"]),
                        "@platforms//os:{}".format(host["os"]),
                    ],
                    target_compatible_with = [
                        "@platforms//cpu:{}".format(target["cpu"]),
                    ] + target.get("constraints", []),
                    target_settings = channel["target_settings"],
                    extra_rustc_flags = channel["extra_rustc_flags"],
                    build_std = target.get("build_std", False),
                    default_edition = channel.get("default_edition"),
                )

    build_file += """
rust_library_group(
    name = "prost_runtime",
    deps = [
        "@rust_crates//:prost",
    ],
)

rust_prost_toolchain(
    name = "prost_toolchain_impl",
    prost_plugin = "@rust_crates//:protoc-gen-prost__protoc-gen-prost",
    prost_runtime = ":prost_runtime",
    prost_types = "@rust_crates//:prost-types",
    tonic_plugin_flag = "",
    visibility = ["//visibility:public"],
)

toolchain(
    name = "prost_toolchain",
    toolchain = "prost_toolchain_impl",
    toolchain_type = "@rules_rust_prost//:toolchain_type",
    visibility = ["//visibility:public"],
)
"""
    return build_file

def _toolchain_repository_hub_impl(repository_ctx):
    repository_ctx.file("WORKSPACE.bazel", """workspace(name = "{}")""".format(
        repository_ctx.name,
    ))

    hosts = json.decode(repository_ctx.attr.hosts)
    extra_targets = json.decode(repository_ctx.attr.extra_targets)
    channels = json.decode(repository_ctx.attr.channels)

    repository_ctx.file("BUILD.bazel", _BUILD_for_toolchain_repo(hosts, extra_targets, channels))

_toolchain_repository_hub = repository_rule(
    doc = "A repository of Pigweed Rust toolchains",
    implementation = _toolchain_repository_hub_impl,
    attrs = {
        "channels": attr.string(mandatory = True),
        "extra_targets": attr.string(mandatory = True),
        "hosts": attr.string(mandatory = True),
    },
)
