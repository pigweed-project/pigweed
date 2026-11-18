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

"""Aspect for running Pyrefly static type checking on Python targets."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("@rules_python//python:py_info.bzl", "PyInfo")
load(
    "//pw_build:pw_pylint.bzl",
    "is_rules_python_generated_source",
)

_PYTHON_TOOLCHAIN_TYPE = Label("@rules_python//python:toolchain_type")
_COLORS = ["auto", "always", "never"]
_MIN_SEVERITIES = ["ignore", "info", "warn", "error"]
_DEFAULT_SUPPRESSION_TAGS = ["no-pyrefly"]
_VIRTUAL_IMPORTS_SUFFIX = "_virtual_imports"
_PROTO_VIRTUAL_IMPORTS_SEGMENT = "/_virtual_imports/"
_STUBS_DIR_SEGMENT = "-stubs/"
_EXTERNAL_SHORT_PATH_PREFIX = "../"
_PW_PY_TEST_RUNNER_SUFFIX = "/pw_py_test_runner.py"
_INIT_PY_SUFFIX = "/__init__.py"
_INIT_PYI_SUFFIX = "/__init__.pyi"

def _dirname(path):
    """Returns the parent directory of a slash-delimited relative path."""
    index = path.rfind("/")
    if index < 0:
        return ""
    return path[:index]

def _has_any_tag(tags, rule_tags):
    """Returns True if any tag in `tags` is present in `rule_tags`."""
    for tag in tags:
        if tag in rule_tags:
            return True
    return False

def _is_checkable_source(src, extension, target, ctx):
    """Returns True if `src` is a first-party source file owned by `target`.

    Excludes external repository files, generated `_virtual_imports` proto
    files, `rules_python` entry-point wrappers, and the `pw_py_test_runner.py`
    wrapper injected into `py_test` targets.
    """
    if src.owner == None or src.owner.workspace_root != target.label.workspace_root:
        return False
    if (
        _PROTO_VIRTUAL_IMPORTS_SEGMENT in src.short_path or
        src.extension != extension
    ):
        return False
    if is_rules_python_generated_source(src.basename, ctx.label.name):
        return False
    if (
        ctx.rule.kind == "py_test" and
        src.short_path.endswith(_PW_PY_TEST_RUNNER_SUFFIX)
    ):
        return False
    return True

def _filter_by_extension(files, extension):
    """Returns all `File`s in `files` matching `extension`."""
    return [src for src in files if src.extension == extension]

def _filter_owned_files(files, extension, target, ctx):
    """Returns checkable `File`s in `files` matching `extension` and `target`."""
    return [
        src
        for src in files
        if _is_checkable_source(src, extension, target, ctx)
    ]

def _sorted_short_paths(files):
    """Returns the sorted `short_path` strings for `files`."""
    return sorted([src.short_path for src in files])

def _top_level_import_packages(target, workspace_name, transitive_files):
    """Returns top-level directories added directly to PYTHONPATH via imports.

    When a target declares `imports = ["."]`, its package directory (e.g.
    `pw_rpc2/py`) is added to the Python search path so modules underneath it
    (`pw_rpc2/py/pw_rpc2/...`) are imported as `pw_rpc2.*`, not `pw_rpc2.py.*`.
    Synthesizing `pw_rpc2/__init__.py` at the workspace root would shadow the
    real `pw_rpc2` package under `pw_rpc2/py/pw_rpc2`.
    """
    workspace_prefix = workspace_name + "/"
    import_roots = {}
    for import_dir in target[PyInfo].imports.to_list():
        if _VIRTUAL_IMPORTS_SUFFIX in import_dir:
            continue
        import_prefix = import_dir + "/"
        for src in transitive_files:
            if src.short_path.startswith(_EXTERNAL_SHORT_PATH_PREFIX):
                continue
            workspace_path = workspace_prefix + src.short_path
            if workspace_path.startswith(import_prefix):
                rel_path = workspace_path.removeprefix(import_prefix)
                if "/" in rel_path:
                    top_package = rel_path.split("/")[0]
                    import_roots[top_package] = True
    return import_roots

def _existing_init_paths(target, workspace_name, transitive_files):
    """Returns short paths and import-relative paths of existing `__init__` files."""
    workspace_prefix = workspace_name + "/"
    import_prefixes = [
        import_dir + "/"
        for import_dir in target[PyInfo].imports.to_list()
    ]
    seen = {}
    for src in transitive_files:
        if src.short_path.endswith(_INIT_PY_SUFFIX):
            init_path = src.short_path
        elif src.short_path.endswith(_INIT_PYI_SUFFIX):
            init_path = src.short_path.removesuffix("i")
        else:
            continue
        seen[init_path] = True
        workspace_path = workspace_prefix + init_path
        for prefix in import_prefixes:
            if workspace_path.startswith(prefix):
                seen[workspace_path.removeprefix(prefix)] = True
    return seen

def _virtual_import_package_root_index(path_parts):
    """Finds the first Python package segment inside a virtual import tree.

    `py_proto_library` (`_virtual_imports/<target_name>/<py_pkg>/...`) and
    `pw_py_enum_library` (`_pw_enum_virtual_imports/<target_name>/<py_pkg>/...`)
    place `<py_pkg>` at `idx + 2`, whereas `pw_py_importable_runfile`
    (`_<name>_virtual_imports/<py_pkg>/...`) places `<py_pkg>` at `idx + 1`.
    """
    for idx, part in enumerate(path_parts):
        if (
            part in ("_virtual_imports", "_pw_enum_virtual_imports") and
            idx + 1 < len(path_parts)
        ):
            return idx + 2, True
        if part.endswith(_VIRTUAL_IMPORTS_SUFFIX):
            return idx + 1, True
    return 0, False

def _synthesize_virtual_import_inits(ctx, target, transitive_files):
    """Synthesizes empty `__init__.py` overlays for generated package dirs.

    Code generators such as `py_proto_library` and `pw_py_enum_library` emit
    `.py` and `.pyi` files into `bazel-bin` without intermediate `__init__.py`
    files, which Pyrefly requires when resolving package imports across
    overlay directories.
    """
    import_roots = _top_level_import_packages(
        target,
        ctx.workspace_name,
        transitive_files,
    )
    seen_inits = _existing_init_paths(
        target,
        ctx.workspace_name,
        transitive_files,
    )
    empty_init_file = None
    synth_inits = []

    for src in transitive_files:
        if (
            src.path == src.short_path or
            src.short_path.startswith(_EXTERNAL_SHORT_PATH_PREFIX)
        ):
            continue
        parts = src.short_path.split("/")
        root_idx, _ = _virtual_import_package_root_index(parts)
        for dir_idx in range(root_idx, len(parts) - 1):
            if dir_idx == root_idx and parts[root_idx] in import_roots:
                continue
            short_dir = "/".join(parts[:dir_idx + 1])
            init_short_path = short_dir + _INIT_PY_SUFFIX
            rel_init_path = "/".join(parts[root_idx:dir_idx + 1]) + _INIT_PY_SUFFIX
            if init_short_path in seen_inits or rel_init_path in seen_inits:
                continue
            seen_inits[init_short_path] = True
            if empty_init_file == None:
                empty_init_file = ctx.actions.declare_file(
                    ctx.rule.attr.name + ".pyrefly-empty-init.py",
                )
                ctx.actions.write(output = empty_init_file, content = "")
            synth_inits.append((init_short_path, empty_init_file))

    synth_files = [empty_init_file] if empty_init_file != None else []
    return synth_inits, synth_files

def _record_overlay_entry(entries, short_path, actual_path):
    """Records a mapping from logical `short_path` to physical `actual_path`."""
    key = short_path + "\0" + actual_path
    entries[key] = {
        "path": actual_path,
        "short_path": short_path,
    }

def _path_overlays(files, synth_inits = []):
    """Builds the sorted list of path overlay mappings for `pyrefly bazel-check`."""
    entries = {}
    for src in files:
        short_path = src.short_path
        if _STUBS_DIR_SEGMENT in short_path:
            # Map PEP 561 `<pkg>-stubs/` directories onto `<pkg>/` so Pyrefly
            # resolves third-party stub packages (e.g. `google-stubs/protobuf`)
            # alongside runtime namespace packages.
            short_path = short_path.replace(_STUBS_DIR_SEGMENT, "/")
        if src.path != short_path:
            _record_overlay_entry(entries, short_path, src.path)

    for init_short_path, empty_file in synth_inits:
        _record_overlay_entry(entries, init_short_path, empty_file.path)

    return [entries[key] for key in sorted(entries.keys())]

def _repository_roots(files, workspace_name, import_all_repositories):
    """Returns the sorted list of repository root names for Python imports."""
    if not import_all_repositories:
        return [workspace_name]

    roots = {workspace_name: True}
    for src in files:
        if src.short_path.startswith(_EXTERNAL_SHORT_PATH_PREFIX):
            repo_path = src.short_path.removeprefix(_EXTERNAL_SHORT_PATH_PREFIX)
            roots[repo_path.split("/")[0]] = True
    return sorted(roots.keys())

def _direct_sources(target, ctx):
    """Returns checkable `.py` source files directly owned by `target`."""
    return _filter_owned_files(
        target[PyInfo].direct_original_sources.to_list(),
        "py",
        target,
        ctx,
    )

def _direct_stubs(target, ctx):
    """Returns checkable `.pyi` stub files directly owned by `target`."""
    py_info = target[PyInfo]
    if not hasattr(py_info, "direct_pyi_files"):
        return []
    return _filter_owned_files(
        py_info.direct_pyi_files.to_list(),
        "pyi",
        target,
        ctx,
    )

def _collect_py_info_files(py_info, include_pyi = False):
    """Collects all transitive source (and optionally `.pyi`) files from `PyInfo`."""
    files = (
        py_info.transitive_original_sources.to_list() +
        py_info.transitive_sources.to_list()
    )
    if include_pyi and hasattr(py_info, "transitive_pyi_files"):
        files.extend(py_info.transitive_pyi_files.to_list())
    return files

def _type_stub_targets(ctx):
    """Returns configured `_type_stubs` targets that provide `PyInfo`."""
    return [
        stub_target
        for stub_target in getattr(ctx.attr, "_type_stubs", [])
        if PyInfo in stub_target
    ]

def _transitive_check_files(target, ctx):
    """Returns all transitive `.py` and `.pyi` files needed to check `target`."""
    py_info = target[PyInfo]
    py_files = _filter_by_extension(_collect_py_info_files(py_info), "py")

    stub_files = []
    for stub_target in _type_stub_targets(ctx):
        stub_files.extend(
            _collect_py_info_files(stub_target[PyInfo], include_pyi = True),
        )
    if hasattr(py_info, "transitive_pyi_files"):
        stub_files.extend(py_info.transitive_pyi_files.to_list())

    return py_files + _filter_by_extension(stub_files, "pyi")

def _find_main_file(ctx, direct_sources):
    """Finds the `short_path` of a `py_binary` or `py_test` main source file."""
    if not hasattr(ctx.rule.attr, "main"):
        return None

    main = ctx.rule.attr.main
    if not main:
        candidates = [ctx.rule.attr.name + ".py"]
    elif type(main) == "string":
        candidates = [main]
    elif type(main) == "Target" and DefaultInfo in main:
        candidates = [src.short_path for src in main[DefaultInfo].files.to_list()]
    else:
        candidates = []

    for candidate in candidates:
        for src in direct_sources:
            if (
                src.short_path == candidate or
                src.short_path.endswith("/" + candidate)
            ):
                return src.short_path

    return None

def _target_info(target, ctx):
    """Builds the `target` metadata dictionary for `pyrefly bazel-check`."""
    return {
        "label": str(target.label),
        "name": target.label.name,
        "package": target.label.package,
        "rule_kind": getattr(ctx.rule, "kind", ""),
        "workspace_name": ctx.workspace_name,
    }

def _python_import_all_repositories(ctx):
    """Returns whether `python_import_all_repositories` is enabled."""
    if hasattr(ctx.fragments, "bazel_py"):
        return ctx.fragments.bazel_py.python_import_all_repositories
    return ctx.attr._python_import_all_repositories_flag[BuildSettingInfo].value

def _search_path_info(target, ctx, direct_sources, transitive_files):
    """Builds the `search_path` configuration dictionary for `pyrefly bazel-check`."""
    main_file = _find_main_file(ctx, direct_sources)
    import_all_repositories = _python_import_all_repositories(ctx)
    raw_imports = list(target[PyInfo].imports.to_list())
    for stub_target in _type_stub_targets(ctx):
        for import_dir in stub_target[PyInfo].imports.to_list():
            if import_dir not in raw_imports:
                raw_imports.append(import_dir)
    explicit_imports = [
        d
        for d in raw_imports
        if _VIRTUAL_IMPORTS_SUFFIX in d
    ] + [
        d
        for d in raw_imports
        if _VIRTUAL_IMPORTS_SUFFIX not in d
    ]
    return {
        "explicit_imports": explicit_imports,
        "main_file": main_file,
        "main_file_directory": _dirname(main_file) if main_file else None,
        "python_import_all_repositories": import_all_repositories,
        "repository_roots": _repository_roots(
            transitive_files,
            ctx.workspace_name,
            import_all_repositories,
        ),
        "workspace_name": ctx.workspace_name,
    }

def _infer_python_version(ctx):
    """Infers the `major.minor` Python version from aspect attrs or toolchain."""
    if ctx.attr._python_version:
        return ctx.attr._python_version

    toolchain = ctx.toolchains[_PYTHON_TOOLCHAIN_TYPE]
    if toolchain and hasattr(toolchain, "py3_runtime"):
        version_info = toolchain.py3_runtime.interpreter_version_info
        if (
            version_info and
            version_info.major != None and
            version_info.minor != None
        ):
            return "{}.{}".format(version_info.major, version_info.minor)

    fail(
        "Could not infer python_version from the rules_python toolchain. " +
        "Pass pyrefly(python_version = ...).",
    )

def _infer_system_platform(ctx):
    """Infers the target `sys.platform` string (`linux`, `darwin`, `win32`)."""
    if ctx.attr._system_platform:
        return ctx.attr._system_platform

    constraints = [
        (ctx.attr._linux_constraint[platform_common.ConstraintValueInfo], "linux"),
        (ctx.attr._macos_constraint[platform_common.ConstraintValueInfo], "darwin"),
        (ctx.attr._windows_constraint[platform_common.ConstraintValueInfo], "win32"),
    ]
    for constraint, system_platform in constraints:
        if ctx.target_platform_has_constraint(constraint):
            return system_platform

    fail(
        "Could not infer system_platform from the target platform. " +
        "Pass pyrefly(system_platform = ...).",
    )

def _config_info(ctx):
    """Builds the `config` dictionary for `pyrefly bazel-check`."""
    error_severities = ctx.attr._error_severities
    preset = ctx.attr._preset
    return {
        "error_severities": error_severities if error_severities else None,
        "preset": preset if preset else None,
        "python_version": _infer_python_version(ctx),
        "system_platform": _infer_system_platform(ctx),
    }

def _bazel_check_input(
        target,
        ctx,
        direct_sources,
        direct_stubs,
        transitive_files,
        synth_inits):
    """Constructs the top-level JSON payload for `pyrefly bazel-check`."""
    return {
        "check_roots": {
            "sources": _sorted_short_paths(direct_sources),
            "stubs": _sorted_short_paths(direct_stubs),
        },
        "config": _config_info(ctx),
        "path_overlays": _path_overlays(transitive_files, synth_inits),
        "search_path": _search_path_info(
            target,
            ctx,
            direct_sources,
            transitive_files,
        ),
        "target": _target_info(target, ctx),
    }

def _pyrefly_impl(target, ctx):
    """Aspect implementation that runs `pyrefly bazel-check` on a Python target."""
    if target.label.workspace_root != "" or ctx.rule.kind == "py_proto_library":
        return []

    rule_tags = getattr(ctx.rule.attr, "tags", [])
    opt_in_tags = ctx.attr._opt_in_tags
    if _has_any_tag(ctx.attr._suppression_tags, rule_tags):
        return []
    if opt_in_tags and not _has_any_tag(opt_in_tags, rule_tags):
        return []

    direct_sources = _direct_sources(target, ctx)
    direct_stubs = _direct_stubs(target, ctx)
    if not direct_sources and not direct_stubs:
        return []

    pyrefly_files = ctx.attr._pyrefly_cli[DefaultInfo].files.to_list()
    if not pyrefly_files:
        return []
    pyrefly_executable = pyrefly_files[0]

    transitive_files = _transitive_check_files(target, ctx)
    synth_inits, synth_files = _synthesize_virtual_import_inits(
        ctx,
        target,
        transitive_files,
    )
    check_input = _bazel_check_input(
        target,
        ctx,
        direct_sources,
        direct_stubs,
        transitive_files,
        synth_inits,
    )
    input_json = ctx.actions.declare_file(
        ctx.rule.attr.name + ".pyrefly-input.json",
    )
    ctx.actions.write(
        output = input_json,
        content = json.encode_indent(check_input) + "\n",
    )

    diagnostics_json = ctx.actions.declare_file(
        ctx.rule.attr.name + ".pyrefly-diagnostics.json",
    )
    args = ctx.actions.args()
    args.add("--color", ctx.attr._color)
    args.add("bazel-check")
    args.add("--output", diagnostics_json)
    args.add("--min-severity", ctx.attr._min_severity)
    args.add(input_json)

    ctx.actions.run(
        executable = pyrefly_executable,
        arguments = [args],
        inputs = depset([input_json] + synth_files + transitive_files),
        outputs = [diagnostics_json],
        mnemonic = "PyreflyBazelCheck",
        progress_message = "Pyrefly checking %{label}",
    )

    return [OutputGroupInfo(
        pyrefly = depset([diagnostics_json]),
        pyrefly_input = depset([input_json]),
    )]

def pyrefly(
        pyrefly_cli,
        python_version = None,
        system_platform = None,
        preset = None,
        min_severity = "error",
        error_severities = None,
        suppression_tags = None,
        opt_in_tags = None,
        type_stubs = None,
        color = "always"):
    """Create a Pyrefly aspect.

    Args:
        pyrefly_cli: Label pointing to the pyrefly executable.
        python_version: Optional modeled Python version.
        system_platform: Optional modeled sys.platform value.
        preset: Optional Pyrefly preset config.
        min_severity: Minimum diagnostic severity.
        error_severities: Optional per-error-kind severity overrides.
        suppression_tags: Optional tags that suppress checking.
        opt_in_tags: Optional tags that enable opt-in checking.
        type_stubs: Optional list of stub package Labels (e.g. types-protobuf).
        color: When to use colored Pyrefly output: "auto", "always", or "never".

    Returns:
        An aspect that exposes diagnostics through the pyrefly output group.
    """
    if suppression_tags == None:
        suppression_tags = _DEFAULT_SUPPRESSION_TAGS
    if opt_in_tags == None:
        opt_in_tags = []
    if color not in _COLORS:
        fail("color must be one of {}".format(_COLORS))
    if min_severity not in _MIN_SEVERITIES:
        fail("min_severity must be one of {}".format(_MIN_SEVERITIES))

    attrs = {
        "_color": attr.string(default = color),
        "_error_severities": attr.string_dict(default = error_severities or {}),
        "_linux_constraint": attr.label(default = Label("@platforms//os:linux")),
        "_macos_constraint": attr.label(default = Label("@platforms//os:macos")),
        "_min_severity": attr.string(default = min_severity),
        "_opt_in_tags": attr.string_list(default = opt_in_tags),
        "_preset": attr.string(default = preset or ""),
        "_pyrefly_cli": attr.label(
            allow_files = True,
            cfg = "exec",
            default = pyrefly_cli,
        ),
        "_python_import_all_repositories_flag": attr.label(
            default = Label("@rules_python//python/config_settings:experimental_python_import_all_repositories"),
        ),
        "_python_version": attr.string(default = python_version or ""),
        "_suppression_tags": attr.string_list(default = suppression_tags),
        "_system_platform": attr.string(default = system_platform or ""),
        "_type_stubs": attr.label_list(default = type_stubs or []),
        "_windows_constraint": attr.label(default = Label("@platforms//os:windows")),
    }

    return aspect(
        implementation = _pyrefly_impl,
        attr_aspects = ["deps"],
        attrs = attrs,
        doc = "Run Pyrefly over Python targets.",
        fragments = ["bazel_py"],
        required_providers = [PyInfo],
        toolchains = [
            config_common.toolchain_type(
                _PYTHON_TOOLCHAIN_TYPE,
                mandatory = False,
            ),
        ],
    )

pyrefly_aspect = pyrefly(
    pyrefly_cli = Label("//pw_build/pyrefly:pyrefly"),
    # LINT.IfChange(pyrefly_config)
    preset = "legacy",
    suppression_tags = [
        "nopyrefly",
        "no-pyrefly",
    ],
    # LINT.ThenChange(//.pyrefly.toml, //pw_build/pyrefly/pyrefly.bzl)
    type_stubs = [
        Label("@pigweed_python_packages//types_protobuf:pkg"),
    ],
)
