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
"""Bazel rules for language-neutral pw_enum definitions."""

PwEnumInfo = provider(
    doc = "Language-neutral enum definitions from a pw_enum target.",
    fields = {
        "dep_srcs": "list[File]: Enum sources of direct deps, which srcs " +
                    "may reference.",
        "import_paths": "list[str]: Import path for each source, without " +
                        "an extension (e.g. bar/baz/foo).",
        "include_root": "str | None: Package-relative directory that " +
                        "generated code must add to its include or import " +
                        "path, or None if no directory is needed.",
        "output_stems": "list[str]: Package-relative path for code " +
                        "generated from each source, without an extension.",
        "srcs": "list[File]: TOML or JSON enum sources.",
        "transitive_srcs": "depset[File]: Enum sources of this target and " +
                           "all of its transitive deps.",
    },
)

def pw_enum_codegen_args(actions, command, info):
    """Returns arguments and inputs for running the pw_enum code generator.

    Args:
      actions: ctx.actions.
      command: The codegen.py subcommand.
      info: The PwEnumInfo to generate code for.

    Returns:
      A tuple of (Args, depset of input files).
    """
    args = actions.args()
    args.add(command)
    args.add("--sources")
    args.add_all(info.srcs)
    if info.dep_srcs:
        args.add("--deps")
        args.add_all(info.dep_srcs)
    args.add("--transitive-deps")
    args.add_all(info.transitive_srcs)
    return args, info.transitive_srcs

_SOURCE_EXTENSIONS = [".toml", ".json"]

def _strip_source_extension(path):
    for ext in _SOURCE_EXTENSIONS:
        if path.endswith(ext):
            return path[:-len(ext)]
    fail("pw_enum sources must be .toml or .json files: " + path)

def _repo_relative_path(file):
    """Returns a file's path relative to the root of its repository."""
    path = file.short_path
    if path.startswith("../"):
        # External repository files have short paths like ../repo/path.
        path = path.split("/", 2)[2]
    return path

def join_paths(*parts):
    return "/".join([p for p in parts if p])

def _import_path(ctx, repo_relative_path):
    """Applies strip_import_prefix and import_prefix to a path."""
    path = repo_relative_path
    strip = ctx.attr.strip_import_prefix
    if strip:
        if strip.startswith("/"):
            root = strip[1:]
        elif strip == ".":
            root = ctx.label.package
        else:
            root = join_paths(ctx.label.package, strip)
        root = root.rstrip("/")

        if root:
            if not path.startswith(root + "/"):
                fail("{} is not under strip_import_prefix {}".format(
                    path,
                    strip,
                ))
            path = path[len(root) + 1:]

    return join_paths(ctx.attr.import_prefix.strip("/"), path)

def _pw_enum_impl(ctx):
    package = ctx.label.package
    uses_virtual_root = bool(
        ctx.attr.strip_import_prefix or ctx.attr.import_prefix,
    )

    # Like proto_library's _virtual_imports, Python tooling (e.g. the pylint
    # aspect) recognizes "_virtual_imports" as a directory of generated code.
    include_root = None
    if uses_virtual_root:
        include_root = "_pw_enum_virtual_imports/" + ctx.label.name

    import_paths = []
    output_stems = []

    for src in ctx.files.srcs:
        path = _repo_relative_path(src)
        if package and not path.startswith(package + "/"):
            fail("pw_enum sources must be in the target's package: " + path)
        stem = _strip_source_extension(path)

        import_path = _strip_source_extension(_import_path(ctx, path))
        if uses_virtual_root:
            output_stem = join_paths(include_root, import_path)
        else:
            output_stem = stem[len(package) + 1 if package else 0:]

        import_paths.append(import_path)
        output_stems.append(output_stem)

    deps = [dep[PwEnumInfo] for dep in ctx.attr.deps]
    info = PwEnumInfo(
        dep_srcs = [src for dep in deps for src in dep.srcs],
        import_paths = import_paths,
        include_root = include_root,
        output_stems = output_stems,
        srcs = ctx.files.srcs,
        transitive_srcs = depset(
            ctx.files.srcs,
            transitive = [dep.transitive_srcs for dep in deps],
        ),
    )

    # Validate the sources when this target is built, even if no code is
    # generated for it.
    stamp = ctx.actions.declare_file(ctx.label.name + ".pw_enum_check")
    args, inputs = pw_enum_codegen_args(ctx.actions, "check", info)
    args.add("--stamp", stamp)
    ctx.actions.run(
        inputs = inputs,
        outputs = [stamp],
        executable = ctx.executable._codegen,
        arguments = [args],
        mnemonic = "PwEnumCheck",
        progress_message = "Checking pw_enum definitions for %{label}",
    )

    return [
        info,
        DefaultInfo(files = depset(ctx.files.srcs)),
        OutputGroupInfo(_validation = depset([stamp])),
    ]

pw_enum = rule(
    implementation = _pw_enum_impl,
    doc = """Declares language-neutral enums defined in TOML or JSON files.

Language-specific rules, such as `pw_cc_enum_library` and
`pw_py_enum_library`, generate code from `pw_enum` targets. A source at
`bar/baz/foo.toml` generates `#include "bar/baz/foo.h"` in C++ and
`import bar.baz.foo` in Python.
""",
    attrs = {
        "deps": attr.label_list(
            doc = "Other pw_enum targets whose enumerators the sources " +
                  "reference.",
            providers = [PwEnumInfo],
        ),
        "import_prefix": attr.string(
            doc = "Prefix to add to the import paths of the sources.",
        ),
        "srcs": attr.label_list(
            doc = "TOML or JSON enum definitions.",
            allow_files = _SOURCE_EXTENSIONS,
            allow_empty = False,
            mandatory = True,
        ),
        "strip_import_prefix": attr.string(
            doc = "Prefix to remove from the import paths of the sources. " +
                  "Relative to the package, or to the repository root if " +
                  "it starts with /.",
        ),
        "_codegen": attr.label(
            default = Label("//pw_enum/py:codegen"),
            executable = True,
            cfg = "exec",
        ),
    },
    provides = [PwEnumInfo],
)
