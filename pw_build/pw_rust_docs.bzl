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
Rule to generates code documentation for multiple crates.
The rule depends on rules_rust being patched with
//pw_rust/bazel_patches:0002-PROTOTYPE-Add-ability-to-document-multiple-crates-at.patch"

This rule is implemented here, rather than in than in the patch, to remove the need
for downstream rust projects to also patch rules_rust.
"""

# buildifier: disable=bzl-visibility
load("@rules_rust//rust/private:common.bzl", "rust_common")

# buildifier: disable=bzl-visibility
load("@rules_rust//rust/private:rustdoc.bzl", "rustdoc_compile_action")

# buildifier: disable=bzl-visibility
load("@rules_rust//rust/private:utils.bzl", "dedent", "find_toolchain")

def _pw_rust_docs_impl(ctx):
    """The implementation of the `rust_doc` rule

    Args:
        ctx (ctx): The rule's context object
    """

    output_dir = ctx.actions.declare_directory("{}.rustdoc".format(ctx.label.name))

    rustdoc_scripts = []
    rustdoc_inputs = []

    for crate in ctx.attr.crates:
        crate_info = crate[rust_common.crate_info]

        # Add the current crate as an extern for the compile action
        rustdoc_flags = [
            "--extern",
            "{}={}".format(crate_info.name, crate_info.output.path),
        ]
        rustdoc_flags.extend(ctx.attr.rustdoc_flags)

        action = rustdoc_compile_action(
            ctx = ctx,
            toolchain = find_toolchain(ctx),
            crate_info = crate_info,
            output = output_dir,
            rustdoc_flags = rustdoc_flags,
        )

        arg_file = ctx.actions.declare_file("gendocs-{}.sh".format(crate.label.name))

        dump_args = ctx.actions.args()
        dump_args.add(action.executable)

        ctx.actions.run(
            mnemonic = "Args",
            progress_message = "Dumping args for {}".format(crate.label),
            outputs = [arg_file],
            executable = ctx.executable._capture_args,
            inputs = [],
            env = {"OUTPUT_FILE": arg_file.path},
            arguments = [dump_args] + action.arguments,
        )

        rustdoc_scripts.append(arg_file)
        rustdoc_inputs += [action.inputs, depset([action.executable])]

    # Generate a root index.html that redirects to the 'pigweed' crate's documentation.
    # By default, rustdoc doesn't generate a styled landing page for the workspace
    # (and we disabled --enable-index-page because it only generates a minimal flat list).
    # This redirect ensures users land on the proper front page.
    redirect_script = ctx.actions.declare_file("generate_redirect.sh")
    ctx.actions.write(
        output = redirect_script,
        content = "bash\n-c\necho '<meta http-equiv=\"refresh\" content=\"0; url=pigweed/index.html\" />' > {}/index.html\n".format(output_dir.path),
    )
    rustdoc_scripts.append(redirect_script)

    args = ctx.actions.args()
    for script in rustdoc_scripts:
        args.add(script)

    ctx.actions.run(
        mnemonic = "Rustdoc",
        progress_message = "Generating Rustdocs",
        outputs = [output_dir],
        executable = ctx.executable._run_scripts,
        inputs = depset(rustdoc_scripts, transitive = rustdoc_inputs),
        #env = action.env,
        arguments = [args],
        #tools = action.tools,
    )

    # This rule does nothing without a single-file output, though the directory should've suffixed.
    _zip(ctx, output_dir, ctx.outputs.rust_doc_zip)

    return [
        DefaultInfo(
            files = depset([ctx.outputs.rust_doc_zip]),
        ),
        OutputGroupInfo(
            rustdoc_dir = depset([output_dir]),
            rustdoc_zip = depset([ctx.outputs.rust_doc_zip]),
        ),
    ]

pw_rust_docs = rule(
    doc = dedent("""\
    Generates code documentation for multiple crates.

    Example:
    Suppose you have the following directory structure for two Rust library crates:

    ```
    [workspace]/
        WORKSPACE
        BUILD
        hello_lib/
            BUILD
            src/
                lib.rs
        world_lib/
            BUILD
            src/
                lib.rs
    ```

    To build [`rustdoc`][rustdoc] documentation for the `hello_lib` and `world_lib` \
    crates, define a `pw_rust_docs` rule that depends on the `hello_lib` and \
    `world_lib` `rust_library` targets:

    [rustdoc]: https://doc.rust-lang.org/book/documentation.html

    ```python
    package(default_visibility = ["//visibility:public"])

    load("@pigweed//pw_build:pw_rust_docs.bzl", "pw_rust_docs")

    pw_rust_docs(
        name = "workspace_docs",
        crates = ["//hello_lib", "//world_lib"],
    )
    ```

    Running `bazel build //:workspaces` will build a zip file containing \
    the documentation for the `hello_lib` and `world_lib` library crates \
    generated by `rustdoc`.
    """),
    implementation = _pw_rust_docs_impl,
    attrs = {
        "crates": attr.label_list(
            doc = (
                "A list of lablesThe label of the target to generate code documentation for.\n" +
                "\n" +
                "`rust_doc` can generate HTML code documentation for the source files of " +
                "`rust_library` or `rust_binary` targets."
            ),
            providers = [rust_common.crate_info],
            mandatory = True,
        ),
        "html_after_content": attr.label(
            doc = "File to add in `<body>`, after content.",
            allow_single_file = [".html", ".md"],
        ),
        "html_before_content": attr.label(
            doc = "File to add in `<body>`, before content.",
            allow_single_file = [".html", ".md"],
        ),
        "html_in_header": attr.label(
            doc = "File to add to `<head>`.",
            allow_single_file = [".html", ".md"],
        ),
        "markdown_css": attr.label_list(
            doc = "CSS files to include via `<link>` in a rendered Markdown file.",
            allow_files = [".css"],
        ),
        "rustdoc_flags": attr.string_list(
            doc = dedent("""\
                List of flags passed to `rustdoc`.

                These strings are subject to Make variable expansion for predefined
                source/output path variables like `$location`, `$execpath`, and
                `$rootpath`. This expansion is useful if you wish to pass a generated
                file of arguments to rustc: `@$(location //package:target)`.
            """),
        ),
        "_capture_args": attr.label(
            doc = "A tool for dumping arguments to a file",
            default = Label("//pw_build/pw_rust_docs/util/capture_args"),
            cfg = "exec",
            executable = True,
        ),
        "_cc_toolchain": attr.label(
            doc = "In order to use find_cpp_toolchain, you must define the '_cc_toolchain' attribute on your rule or aspect.",
            default = Label("@bazel_tools//tools/cpp:current_cc_toolchain"),
        ),
        "_dir_zipper": attr.label(
            doc = "A tool that orchestrates the creation of zip archives for rustdoc outputs.",
            default = Label("@rules_rust//rust/private/rustdoc/dir_zipper"),
            cfg = "exec",
            executable = True,
        ),
        "_process_wrapper": attr.label(
            doc = "A process wrapper for running rustdoc on all platforms",
            default = Label("@rules_rust//util/process_wrapper"),
            executable = True,
            allow_single_file = True,
            cfg = "exec",
        ),
        "_run_scripts": attr.label(
            doc = "A tool for running multiple scripts in the same action",
            default = Label("//pw_build/pw_rust_docs/util/run_scripts"),
            cfg = "exec",
            executable = True,
        ),
        "_zipper": attr.label(
            doc = "A Bazel provided tool for creating archives",
            default = Label("@bazel_tools//tools/zip:zipper"),
            cfg = "exec",
            executable = True,
        ),
    },
    fragments = ["cpp"],
    host_fragments = ["cpp"],
    outputs = {
        "rust_doc_zip": "%{name}.zip",
    },
    toolchains = [
        str(Label("@rules_rust//rust:toolchain_type")),
        "@bazel_tools//tools/cpp:toolchain_type",
    ],
)

def _zip(ctx, input_dir, output_zip):
    args = ctx.actions.args()
    args.add_all([
        ctx.executable._zipper,
        output_zip,
        ctx.bin_dir.path,
    ])
    args.add_all([input_dir], expand_directories = True)

    ctx.actions.run(
        executable = ctx.executable._dir_zipper,
        inputs = [input_dir],
        outputs = [output_zip],
        arguments = [args],
        mnemonic = "ZipDocs",
        tools = [ctx.executable._zipper],
    )
