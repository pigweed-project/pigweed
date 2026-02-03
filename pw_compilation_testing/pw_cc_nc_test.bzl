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
"""Bazel rule for running negative compilation tests."""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

# This need to be disabled until the `module_name` variable is properly
# set by create_compile_variables().
_UNSUPPORTED_FEATURES = ["module_maps", "use_header_modules"]

def _pw_cc_nc_test_impl(ctx):
    # Collect information about the C++ compiler.
    comp_context = ctx.attr.base[CcInfo].compilation_context

    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features + _UNSUPPORTED_FEATURES,
    )

    cc_compiler_path = cc_common.get_tool_for_action(
        feature_configuration = feature_configuration,
        action_name = ACTION_NAMES.cpp_compile,
    )

    # Declare a script that generate_and_run.py creates to display the results.
    result_script = ctx.actions.declare_file(ctx.label.name)

    generate_and_run_args = [
        "--name",
        str(ctx.label).replace("@@//", "//"),  # buildifier: disable=canonical-repository
        "--results",
        result_script.path,
    ]

    # Write each source's command line invocation to a file.
    invocations = []
    for i, src in enumerate(ctx.files.srcs):
        if src.short_path.endswith(".h") or src.short_path.endswith(".hh"):
            continue  # Header files are not compiled

        # Construct the command line from the compiler info.
        cc_compile_variables = cc_common.create_compile_variables(
            feature_configuration = feature_configuration,
            cc_toolchain = cc_toolchain,
            source_file = src.path,
            user_compile_flags = ctx.fragments.cpp.copts + ctx.fragments.cpp.cxxopts,
            include_directories = comp_context.includes,
            quote_include_directories = comp_context.quote_includes,
            system_include_directories = comp_context.system_includes,
            framework_include_directories = comp_context.framework_includes,
            preprocessor_defines = depset(transitive = [
                comp_context.defines,
                comp_context.local_defines,
            ]),
        )

        cc_compile_flags = cc_common.get_memory_inefficient_command_line(
            feature_configuration = feature_configuration,
            action_name = ACTION_NAMES.cpp_compile,
            variables = cc_compile_variables,
        )

        # Store the command line invocation in a file on disk.
        invocation_file = ctx.actions.declare_file(
            "{}_{}_invocation.txt".format(ctx.label.name, i),
        )
        ctx.actions.write(
            output = invocation_file,
            content = " ".join([cc_compiler_path] + cc_compile_flags),
        )
        invocations.append(invocation_file)

        generate_and_run_args.append(src.path)
        generate_and_run_args.append(invocation_file.path)

    inputs = invocations + ctx.files.srcs + cc_toolchain.all_files.to_list()

    # Run generate_and_run.py, which finds and executes all PW_NC_TESTs.
    ctx.actions.run(
        inputs = depset(inputs, transitive = [
            comp_context.headers,
            # Add the base test executable as a dependency to ensure it builds first.
            ctx.attr.base[DefaultInfo].files,
        ]),
        toolchain = "@bazel_tools//tools/cpp:toolchain_type",
        mnemonic = "CppNegativeCompilationTest",
        progress_message = "Running negative compilation test " + ctx.label.name,
        executable = ctx.executable._generate_and_run_py,
        arguments = generate_and_run_args,
        outputs = [result_script],
    )

    # Run the script created by generate_and_run.py that reports test results.
    return [DefaultInfo(executable = result_script)]

# Internal rule that declares a negative compilation test.
pw_cc_nc_test = rule(
    implementation = _pw_cc_nc_test_impl,
    test = True,
    attrs = {
        "base": attr.label(
            mandatory = True,
            providers = [CcInfo],
        ),
        "srcs": attr.label_list(
            allow_files = True,
            mandatory = True,
        ),
        "_cc_toolchain": attr.label(
            default = Label("@bazel_tools//tools/cpp:current_cc_toolchain"),
        ),
        "_generate_and_run_py": attr.label(
            executable = True,
            cfg = "exec",
            default = Label("//pw_compilation_testing/py:generate_and_run"),
        ),
    },
    toolchains = [
        "@bazel_tools//tools/cpp:toolchain_type",
    ],
    fragments = ["cpp"],
)

_NC_TEST_DEP = Label("//pw_compilation_testing:_internal_do_not_use_nc_test_header")

def check_and_update_nc_test_deps(has_nc_test, deps):
    """Ensures the user didn't manually add the NC test dep, then adds it if needed.

    Args:
        has_nc_test: whether NC tests are enabled for this test target.
        deps: Dependency list to check and possibly add to.
    """

    # Check if the NC test headers dep was manually added.
    for dep in deps:
        if Label(dep) == _NC_TEST_DEP:
            fail(
                'Do not depend directly on "{}"!'.format(_NC_TEST_DEP) +
                "Instead, declare negative compilation tests in a pw_cc_test() and pass `" +
                "`has_nc_test = True`",
            )

    if has_nc_test:
        deps.append(str(_NC_TEST_DEP))
