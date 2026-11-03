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
"""Bazel rules for generating Python enums from pw_enum targets."""

load("@rules_python//python:py_info.bzl", "PyInfo")
load("//pw_enum:pw_enum.bzl", "PwEnumInfo", "join_paths", "pw_enum_codegen_args")

_PwPyEnumInfo = provider(
    doc = "Python code generated from a pw_enum target.",
    fields = {
        "imports": "depset[str]: Import paths to add to the Python path.",
        "srcs": "depset[File]: The generated Python modules.",
    },
)

def _pw_py_enum_aspect_impl(target, ctx):
    info = target[PwEnumInfo]

    srcs = [ctx.actions.declare_file(stem + ".py") for stem in info.output_stems]

    args, inputs = pw_enum_codegen_args(ctx.actions, "py", info)
    args.add("--outputs")
    args.add_all(srcs)

    # The code generator checks that these are valid Python module names.
    args.add("--import-paths")
    args.add_all(info.import_paths)

    ctx.actions.run(
        inputs = inputs,
        outputs = srcs,
        executable = ctx.executable._codegen,
        arguments = [args],
        mnemonic = "PwEnumPyGen",
        progress_message = "Generating pw_enum Python modules for %{label}",
    )

    imports = []
    if info.include_root:
        imports.append(join_paths(
            ctx.label.workspace_name or ctx.workspace_name,
            ctx.label.package,
            info.include_root,
        ))
    elif ctx.label.workspace_name:
        imports.append(ctx.label.workspace_name)

    return [_PwPyEnumInfo(imports = depset(imports), srcs = depset(srcs))]

# Generates Python modules once per pw_enum target, no matter how many
# pw_py_enum_library targets reference it.
_pw_py_enum_aspect = aspect(
    implementation = _pw_py_enum_aspect_impl,
    attrs = {
        "_codegen": attr.label(
            default = Label("//pw_enum/py:codegen"),
            executable = True,
            cfg = "exec",
        ),
    },
    required_providers = [PwEnumInfo],
    provides = [_PwPyEnumInfo],
)

def _pw_py_enum_library_impl(ctx):
    infos = [e[_PwPyEnumInfo] for e in ctx.attr.enums]
    srcs = depset(transitive = [i.srcs for i in infos])
    return [
        DefaultInfo(
            files = srcs,
            default_runfiles = ctx.runfiles(transitive_files = srcs),
        ),
        PyInfo(
            transitive_sources = srcs,
            imports = depset(transitive = [i.imports for i in infos]),
        ),
    ]

pw_py_enum_library = rule(
    implementation = _pw_py_enum_library_impl,
    doc = """Generates Python enums from pw_enum targets.

A `pw_enum` source at `bar/baz/foo.toml` generates the module `bar.baz.foo`.
Each enum is an `enum.IntEnum`. Generated modules only depend on the Python
standard library.
""",
    attrs = {
        "enums": attr.label_list(
            doc = "pw_enum targets to generate Python modules for.",
            providers = [PwEnumInfo],
            aspects = [_pw_py_enum_aspect],
            allow_empty = False,
            mandatory = True,
        ),
    },
    provides = [PyInfo],
)
