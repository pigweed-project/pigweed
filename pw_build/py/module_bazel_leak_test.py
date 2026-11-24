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
"""Tests that Pigweed's MODULE.bazel does not leak dev-only declarations.

Root MODULE.bazel declarations must either be explicitly allowlisted as consumer
dependencies or be ignored by Bazel when @pigweed is consumed as a non-root
module. Files pulled in via include() have an empty non-dev allowlist, so every
declaration in them must be root-only (an override or `dev_dependency = True`).
"""

import ast
from collections.abc import Mapping
import dataclasses
import os
from pathlib import Path
import unittest


# Bzlmod dependencies in MODULE.bazel that are intentionally visible to
# downstream modules (not marked `dev_dependency = True`).
_ALLOWED_NON_DEV_BAZEL_DEPS = frozenset(
    {
        'aspect_bazel_lib',
        'aspect_rules_esbuild',
        'aspect_rules_js',
        'bazel_skylib',
        'boringssl',
        'emboss',
        'flatbuffers',
        'freertos',
        'fuzztest',
        'gazelle',
        'googletest',
        'grpc-java',
        'mbedtls',
        'nanopb',
        'pico-sdk',
        'picotool',
        'platforms',
        'protobuf',
        're2',
        'rules_android',
        'rules_cc',
        'rules_foreign_cc',
        'rules_fuzzing',
        'rules_go',
        'rules_java',
        'rules_jni',
        'rules_jvm_external',
        'rules_libusb',
        'rules_license',
        'rules_nodejs',
        'rules_platform',
        'rules_probe_rs',
        'rules_python',
        'rules_rust',
        'rules_rust_bindgen',
        'rules_rust_prost',
        'rules_shell',
        'sphinxdocs',
    }
)

# Repository rule instances in MODULE.bazel that are intentionally visible to
# downstream modules (not marked `dev_dependency = True`).
_ALLOWED_NON_DEV_REPO_RULE_INSTANCES = frozenset(
    {
        'bazel_supports',
        'macos_sysroot',
    }
)

# CIPD repositories on the non-dev `cipd_ext` handle that are intentionally
# visible to downstream modules.
_ALLOWED_NON_DEV_CIPD_REPOS = frozenset(
    {
        'pigweed.bloaty',
        'pigweed.buildifier',
        'pigweed.gn',
        'pigweed.go',
        'pigweed.linux_sysroot',
        'pigweed.qemu',
    }
)

# Module extension handles in MODULE.bazel that are intentionally evaluated for
# downstream modules (not marked `dev_dependency = True`).
_ALLOWED_NON_DEV_EXTENSION_HANDLES = frozenset(
    {
        'cc_compatibility_proxy',
        'cipd_ext',
        'crate',
        'go_deps',
        'go_sdk',
        'libusb',
        'maven',
        'non_module_dependencies',
        'pip',
        'probe_rs',
        'pw_cxx_toolchain',
        'pw_rust',
        'pw_rust_crates',
        'pw_zephyr_config',
        'python',
        'rust',
    }
)

# Python toolchains that are intentionally exported to downstream modules
# (see b/354241571: downstream wildcard MCU builds rely on
# runtime_env_toolchains as an unconstrained fallback for py_binary/py_test
# targets, and @pythons_hub//:all must precede it so hermetic Python takes
# priority on host platforms).
_ALLOWED_NON_DEV_TOOLCHAINS = frozenset(
    {
        '@pythons_hub//:all',
        '@rules_python//python/runtime_env_toolchains:all',
    }
)

# Root-only module segments included from MODULE.bazel.
_ALLOWED_INCLUDES = frozenset(
    {
        '//bazel:dev.MODULE.bazel',
        '//bazel:resolve.MODULE.bazel',
    }
)

# Directives that Bazel ignores when the module is not the root module.
_ROOT_ONLY_DIRECTIVES = frozenset(
    {
        'archive_override',
        'git_override',
        'inject_repo',
        'local_path_override',
        'multiple_version_override',
        'override_repo',
        'single_version_override',
    }
)


@dataclasses.dataclass(frozen=True)
class _Allowlist:
    allow_module: bool = False
    bazel_deps: frozenset[str] = frozenset()
    repo_rule_instances: frozenset[str] = frozenset()
    cipd_repos: frozenset[str] = frozenset()
    extension_handles: frozenset[str] = frozenset()
    toolchains: frozenset[str] = frozenset()
    includes: frozenset[str] = frozenset()


_ROOT_ALLOWLIST = _Allowlist(
    allow_module=True,
    bazel_deps=_ALLOWED_NON_DEV_BAZEL_DEPS,
    repo_rule_instances=_ALLOWED_NON_DEV_REPO_RULE_INSTANCES,
    cipd_repos=_ALLOWED_NON_DEV_CIPD_REPOS,
    extension_handles=_ALLOWED_NON_DEV_EXTENSION_HANDLES,
    toolchains=_ALLOWED_NON_DEV_TOOLCHAINS,
    includes=_ALLOWED_INCLUDES,
)

_INCLUDED_ALLOWLIST = _Allowlist()


@dataclasses.dataclass
class _SegmentState:  # pylint: disable=too-many-instance-attributes
    # Every name assigned from use_extension(...) or use_repo_rule(...).
    handle_vars: set[str] = dataclasses.field(default_factory=set)
    repo_rule_vars: set[str] = dataclasses.field(default_factory=set)
    dev_extension_handles: set[str] = dataclasses.field(default_factory=set)
    non_dev_cipd_handles: set[str] = dataclasses.field(default_factory=set)
    seen_bazel_deps: set[str] = dataclasses.field(default_factory=set)
    seen_repo_rule_instances: set[str] = dataclasses.field(default_factory=set)
    seen_cipd_repos: set[str] = dataclasses.field(default_factory=set)
    seen_non_dev_extension_handles: set[str] = dataclasses.field(
        default_factory=set
    )
    seen_non_dev_toolchains: set[str] = dataclasses.field(default_factory=set)
    seen_includes: set[str] = dataclasses.field(default_factory=set)
    # id()s of `handle = use_extension(...)` / `use_repo_rule(...)` calls.
    assigned_calls: set[int] = dataclasses.field(default_factory=set)
    # id()s of calls that are top-level statements and were checked as such.
    statement_calls: set[int] = dataclasses.field(default_factory=set)


def _get_kwarg_bool(call: ast.Call, arg_name: str) -> bool:
    for kw in call.keywords:
        if kw.arg == arg_name and isinstance(kw.value, ast.Constant):
            return kw.value.value is True
    return False


def _get_kwarg_str(call: ast.Call, arg_name: str) -> str | None:
    for kw in call.keywords:
        if kw.arg == arg_name and isinstance(kw.value, ast.Constant):
            if isinstance(kw.value.value, str):
                return kw.value.value
    return None


def _extension_identity(call: ast.Call) -> tuple[str, str] | None:
    """Returns (bzl_file, extension_name) of a use_extension() call.

    Returns None unless both are positional string literals. Bazel only accepts
    them positionally; any other form cannot be identified statically.
    """
    if len(call.args) < 2:
        return None
    bzl_file, ext_name = call.args[0], call.args[1]
    if (
        isinstance(bzl_file, ast.Constant)
        and isinstance(bzl_file.value, str)
        and isinstance(ext_name, ast.Constant)
        and isinstance(ext_name.value, str)
    ):
        return bzl_file.value, ext_name.value
    return None


def _is_cipd_extension(identity: tuple[str, str]) -> bool:
    bzl_file, ext_name = identity
    return (
        bzl_file.endswith('pw_env_setup/bazel/cipd:defs.bzl')
        and ext_name == 'cipd'
    )


def _find_stale_allowlist_entries(state: _SegmentState) -> list[str]:
    errors: list[str] = []
    for stale, table_name in (
        (_ALLOWED_INCLUDES - state.seen_includes, '_ALLOWED_INCLUDES'),
        (
            _ALLOWED_NON_DEV_BAZEL_DEPS - state.seen_bazel_deps,
            '_ALLOWED_NON_DEV_BAZEL_DEPS',
        ),
        (
            _ALLOWED_NON_DEV_REPO_RULE_INSTANCES
            - state.seen_repo_rule_instances,
            '_ALLOWED_NON_DEV_REPO_RULE_INSTANCES',
        ),
        (
            _ALLOWED_NON_DEV_CIPD_REPOS - state.seen_cipd_repos,
            '_ALLOWED_NON_DEV_CIPD_REPOS',
        ),
        (
            _ALLOWED_NON_DEV_EXTENSION_HANDLES
            - state.seen_non_dev_extension_handles,
            '_ALLOWED_NON_DEV_EXTENSION_HANDLES',
        ),
        (
            _ALLOWED_NON_DEV_TOOLCHAINS - state.seen_non_dev_toolchains,
            '_ALLOWED_NON_DEV_TOOLCHAINS',
        ),
    ):
        for item in sorted(stale):
            errors.append(
                f'Stale entry "{item}" in {table_name}: no longer present '
                'in MODULE.bazel.'
            )
    return errors


def _check_cipd_use_repo(
    call: ast.Call,
    lineno: int,
    handle_name: str,
    allowlist: _Allowlist,
    errors: list[str],
) -> None:
    allowed_repos = allowlist.cipd_repos | {'cipd_client'}
    for arg in call.args[1:]:
        if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
            if arg.value not in allowed_repos:
                errors.append(
                    f'Line {lineno}: `use_repo({handle_name}, '
                    f'"{arg.value}")` exports a repo not in '
                    '_ALLOWED_NON_DEV_CIPD_REPOS; use '
                    '`use_repo(cipd_dev_ext, ...)` if it is dev-only.'
                )
        else:
            errors.append(
                f'Line {lineno}: `use_repo({handle_name}, ...)` must only '
                'reference string literal repo names in '
                '_ALLOWED_NON_DEV_CIPD_REPOS.'
            )
    for kw in call.keywords:
        if isinstance(kw.value, ast.Constant) and isinstance(
            kw.value.value, str
        ):
            if kw.value.value not in allowed_repos:
                errors.append(
                    f'Line {lineno}: `use_repo({handle_name}, '
                    f'{kw.arg} = "{kw.value.value}")` exports a repo not in '
                    '_ALLOWED_NON_DEV_CIPD_REPOS; use '
                    '`use_repo(cipd_dev_ext, ...)` if it is dev-only.'
                )
        else:
            errors.append(
                f'Line {lineno}: `use_repo({handle_name}, ...)` must only '
                'reference string literal repo names in '
                '_ALLOWED_NON_DEV_CIPD_REPOS.'
            )


def _check_assign(
    node: ast.Assign,
    allowlist: _Allowlist,
    state: _SegmentState,
    errors: list[str],
) -> None:
    if not (
        len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
        and isinstance(node.value, ast.Call)
        and isinstance(node.value.func, ast.Name)
    ):
        return
    var_name = node.targets[0].id
    func_name = node.value.func.id
    if func_name not in ('use_extension', 'use_repo_rule'):
        return
    state.assigned_calls.add(id(node.value))

    # A handle is tracked as dev or non-dev by name, so a name must only ever
    # refer to one handle. Otherwise rebinding it could move later tags and
    # use_repo() calls onto the wrong side of the allowlist checks.
    if var_name in state.handle_vars:
        errors.append(
            f'Line {node.lineno}: `{var_name}` is assigned more than once; '
            'extension and repository rule handles must be assigned exactly '
            'once.'
        )
        return
    state.handle_vars.add(var_name)

    if func_name == 'use_repo_rule':
        state.repo_rule_vars.add(var_name)
        return
    if _get_kwarg_bool(node.value, 'dev_dependency'):
        state.dev_extension_handles.add(var_name)
        return
    if var_name in allowlist.extension_handles:
        state.seen_non_dev_extension_handles.add(var_name)
    else:
        errors.append(
            f'Line {node.lineno}: `{var_name} = use_extension(...)` must '
            'set `dev_dependency = True`.'
        )
    identity = _extension_identity(node.value)
    if identity is None:
        # Without this, a CIPD extension written in another form would not be
        # recognized and its repos would skip _ALLOWED_NON_DEV_CIPD_REPOS.
        errors.append(
            f'Line {node.lineno}: `{var_name} = use_extension(...)` must pass '
            'the .bzl file and extension name as positional string literals '
            'so that the extension can be identified.'
        )
    elif _is_cipd_extension(identity):
        state.non_dev_cipd_handles.add(var_name)


def _check_tag_call(
    call: ast.Call,
    func: ast.Attribute,
    lineno: int,
    allowlist: _Allowlist,
    state: _SegmentState,
    errors: list[str],
) -> None:
    if not isinstance(func.value, ast.Name):
        errors.append(
            f'Line {lineno}: `{ast.unparse(func)}(...)` must be called on a '
            'top-level extension handle.'
        )
        return
    handle_name = func.value.id
    if handle_name in state.dev_extension_handles:
        return
    if handle_name in state.non_dev_cipd_handles:
        if func.attr != 'require':
            errors.append(
                f'Line {lineno}: `{handle_name}.{func.attr}(...)` is not '
                f'allowed on the non-dev `{handle_name}` handle.'
            )
            return
        repo_name = _get_kwarg_str(call, 'name') or '<unknown>'
        if _get_kwarg_bool(call, 'dev_dependency'):
            errors.append(
                f'Line {lineno}: `{handle_name}.require(name = '
                f'"{repo_name}", dev_dependency = True)` on the non-dev '
                f'`{handle_name}` handle still leaks a stub repo via '
                f'`use_repo({handle_name}, ...)`; use '
                '`cipd_dev_ext.require(...)` instead.'
            )
        elif repo_name in allowlist.cipd_repos:
            state.seen_cipd_repos.add(repo_name)
        else:
            errors.append(
                f'Line {lineno}: `{handle_name}.require(name = '
                f'"{repo_name}")` is not in _ALLOWED_NON_DEV_CIPD_REPOS; '
                'use `cipd_dev_ext.require(...)` if it is dev-only.'
            )
        return
    if handle_name in state.seen_non_dev_extension_handles:
        return
    errors.append(
        f'Line {lineno}: `{ast.unparse(func)}(...)` must be called on an '
        'extension handle with `dev_dependency = True` (or an allowlisted '
        'non-dev handle).'
    )


def _check_include(
    call: ast.Call,
    lineno: int,
    allowlist: _Allowlist,
    includes: Mapping[str, str] | None,
    state: _SegmentState,
    errors: list[str],
) -> None:
    if not allowlist.includes:
        errors.append(
            f'Line {lineno}: `include(...)` is not allowed in an included '
            'module file.'
        )
        return
    if (
        len(call.args) != 1
        or call.keywords
        or not isinstance(call.args[0], ast.Constant)
        or not isinstance(call.args[0].value, str)
    ):
        errors.append(
            f'Line {lineno}: `{ast.unparse(call)}` must pass a single string '
            'literal label in _ALLOWED_INCLUDES.'
        )
        return
    label = call.args[0].value
    if label not in allowlist.includes:
        errors.append(
            f'Line {lineno}: `include("{label}")` is not in '
            '_ALLOWED_INCLUDES, so its contents are not checked.'
        )
        return
    state.seen_includes.add(label)
    if includes is None or label not in includes:
        errors.append(
            f'Line {lineno}: `include("{label}")` cannot be checked because '
            'its contents were not provided.'
        )
        return
    sub_errors, _ = _check_segment(
        includes[label],
        filename=label,
        allowlist=_INCLUDED_ALLOWLIST,
        includes=None,
    )
    errors.extend(f'{label}: {err}' for err in sub_errors)


# pylint: disable-next=too-many-branches,too-many-return-statements
def _check_name_call(
    call: ast.Call,
    func_name: str,
    lineno: int,
    allowlist: _Allowlist,
    includes: Mapping[str, str] | None,
    state: _SegmentState,
    errors: list[str],
) -> None:
    if func_name == 'module' and allowlist.allow_module:
        return
    if func_name in _ROOT_ONLY_DIRECTIVES or func_name in (
        'use_extension',
        'use_repo_rule',
    ):
        return
    if func_name == 'include':
        _check_include(call, lineno, allowlist, includes, state, errors)
        return
    if func_name == 'use_repo':
        if not call.args or not isinstance(call.args[0], ast.Name):
            errors.append(
                f'Line {lineno}: `use_repo(...)` must pass a top-level '
                'extension handle as its first argument.'
            )
            return
        handle_name = call.args[0].id
        if handle_name in state.dev_extension_handles:
            return
        if handle_name in state.non_dev_cipd_handles:
            _check_cipd_use_repo(call, lineno, handle_name, allowlist, errors)
            return
        if handle_name in state.seen_non_dev_extension_handles:
            return
        errors.append(
            f'Line {lineno}: `use_repo({handle_name}, ...)` references an '
            'unallowed or non-dev extension handle.'
        )
        return
    if func_name == 'bazel_dep':
        dep_name = _get_kwarg_str(call, 'name') or '<unknown>'
        if not _get_kwarg_bool(call, 'dev_dependency'):
            if dep_name in allowlist.bazel_deps:
                state.seen_bazel_deps.add(dep_name)
            else:
                errors.append(
                    f'Line {lineno}: `bazel_dep(name = "{dep_name}", ...)` '
                    'must set `dev_dependency = True`.'
                )
        return
    if func_name == 'register_toolchains':
        if _get_kwarg_bool(call, 'dev_dependency'):
            return
        pos_args = [
            arg.value
            for arg in call.args
            if isinstance(arg, ast.Constant) and isinstance(arg.value, str)
        ]
        if (
            pos_args
            and len(pos_args) == len(call.args)
            and all(arg in allowlist.toolchains for arg in pos_args)
        ):
            state.seen_non_dev_toolchains.update(pos_args)
        else:
            errors.append(
                f'Line {lineno}: `register_toolchains(...)` must set '
                '`dev_dependency = True` so Pigweed does not preempt '
                'downstream toolchain resolution.'
            )
        return
    if func_name == 'register_execution_platforms':
        if not _get_kwarg_bool(call, 'dev_dependency'):
            errors.append(
                f'Line {lineno}: `register_execution_platforms(...)` must set '
                '`dev_dependency = True` so Pigweed does not preempt '
                'downstream platform resolution.'
            )
        return
    if func_name in state.repo_rule_vars:
        repo_name = _get_kwarg_str(call, 'name') or '<unknown>'
        if not _get_kwarg_bool(call, 'dev_dependency'):
            if repo_name in allowlist.repo_rule_instances:
                state.seen_repo_rule_instances.add(repo_name)
            else:
                errors.append(
                    f'Line {lineno}: `{func_name}(name = "{repo_name}", ...)` '
                    'must set `dev_dependency = True`.'
                )
        return
    errors.append(
        f'Line {lineno}: `{func_name}(...)` is not allowed in '
        f'{_allowlist_scope(allowlist)}.'
    )


def _allowlist_scope(allowlist: _Allowlist) -> str:
    return (
        'MODULE.bazel' if allowlist.allow_module else 'an included module file'
    )


def _find_unchecked_calls(tree: ast.Module, state: _SegmentState) -> list[str]:
    """Flags calls that the per-statement checks never looked at.

    Bazel evaluates a declaration wherever it appears, for example in
    `_ = bazel_dep(...)` or `[register_toolchains(...) for _ in [1]]`. The
    checks above only inspect calls that are a statement on their own, so every
    other call is rejected here.
    """
    errors: list[str] = []
    for subnode in ast.walk(tree):
        if not isinstance(subnode, ast.Call):
            continue
        if id(subnode) in state.assigned_calls:
            continue
        if isinstance(subnode.func, ast.Name) and subnode.func.id in (
            'use_extension',
            'use_repo_rule',
        ):
            if subnode.func.id == 'use_extension' and _get_kwarg_bool(
                subnode, 'dev_dependency'
            ):
                continue
            errors.append(
                f'Line {subnode.lineno}: `{subnode.func.id}(...)` must be '
                'assigned to a top-level handle variable.'
            )
        elif id(subnode) not in state.statement_calls:
            errors.append(
                f'Line {subnode.lineno}: `{ast.unparse(subnode.func)}(...)` '
                'must be a top-level statement so that it can be checked.'
            )
    return errors


def _check_segment(
    source: str,
    *,
    filename: str,
    allowlist: _Allowlist,
    includes: Mapping[str, str] | None,
) -> tuple[list[str], _SegmentState]:
    tree = ast.parse(source, filename=filename)
    errors: list[str] = []
    state = _SegmentState()

    for node in tree.body:
        if isinstance(node, ast.Assign):
            _check_assign(node, allowlist, state, errors)
        elif isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
            call = node.value
            if isinstance(call.func, ast.Attribute):
                state.statement_calls.add(id(call))
                _check_tag_call(
                    call, call.func, node.lineno, allowlist, state, errors
                )
            elif isinstance(call.func, ast.Name):
                state.statement_calls.add(id(call))
                _check_name_call(
                    call,
                    call.func.id,
                    node.lineno,
                    allowlist,
                    includes,
                    state,
                    errors,
                )
            elif isinstance(call.func, ast.Call):
                # `f(...)(...)`: the inner call is reported by
                # _find_unchecked_calls, so do not report the outer one too.
                state.statement_calls.add(id(call))

    errors.extend(_find_unchecked_calls(tree, state))
    return errors, state


def find_module_bazel_leaks(
    source: str,
    *,
    check_stale: bool = False,
    includes: Mapping[str, str] | None = None,
) -> list[str]:
    """Returns leak errors for MODULE.bazel and its included segments."""
    errors, state = _check_segment(
        source,
        filename='MODULE.bazel',
        allowlist=_ROOT_ALLOWLIST,
        includes=includes,
    )
    if check_stale:
        errors.extend(_find_stale_allowlist_entries(state))
    return errors


class ModuleBazelLeakTest(unittest.TestCase):
    """Verifies MODULE.bazel dev_dependency hygiene."""

    def test_pigweed_module_bazel_has_no_leaks(self) -> None:
        """Checks Pigweed's MODULE.bazel and its includes for non-dev leaks."""
        # Use os.path.abspath rather than Path.resolve() so that under Bazel we
        # walk up the .runfiles tree (verifying the `data` files are present)
        # instead of dereferencing symlinks out of the sandbox.
        root = Path(os.path.abspath(__file__)).parents[2]
        module_bazel_path = root / 'MODULE.bazel'
        self.assertTrue(module_bazel_path.is_file())
        source = module_bazel_path.read_text(encoding='utf-8')

        includes: dict[str, str] = {}
        for label in _ALLOWED_INCLUDES:
            pkg, target = label.removeprefix('//').split(':', 1)
            include_path = root / pkg / target
            self.assertTrue(include_path.is_file(), include_path)
            includes[label] = include_path.read_text(encoding='utf-8')

        leaks = find_module_bazel_leaks(
            source, check_stale=True, includes=includes
        )
        self.assertEqual(
            leaks,
            [],
            'Found non-dev dependency leaks in MODULE.bazel:\n'
            + '\n'.join(leaks),
        )

    def test_detects_leaked_toolchains_and_platforms(self) -> None:
        """Verifies the AST checker flags non-dev toolchains and platforms."""
        snippet = """
register_toolchains("//pw_toolchain:host", dev_dependency = True)
register_toolchains("@pythons_hub//:all")
register_toolchains("@pythons_hub//:all", EXTRA_TOOLCHAIN)
register_toolchains("//pw_toolchain/arm_gcc:arm_gcc_toolchain_cortex-m")
register_execution_platforms("//pw_grpc:test_platform")
"""
        leaks = find_module_bazel_leaks(snippet)
        self.assertEqual(len(leaks), 3)
        self.assertIn('Line 4: `register_toolchains', leaks[0])
        self.assertIn('Line 5: `register_toolchains', leaks[1])
        self.assertIn('register_execution_platforms', leaks[2])

    def test_detects_leaked_deps_repo_rules_and_extensions(self) -> None:
        """Verifies the AST checker flags non-dev deps, repo rules, and exts."""
        snippet = """
bazel_dep(name = "rules_cc", version = "0.2.22")
bazel_dep(name = "rules_doxygen", version = "2.4.2")
http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
http_archive(name = "bazel_supports")
http_archive(name = "leaked_sdk")
pnpm = use_extension("@aspect_rules_js//npm:extensions.bzl", "pnpm")
unlisted_ext = use_extension("//some:extensions.bzl", "unlisted_ext")
cipd_ext = use_extension("//pw_env_setup/bazel/cipd:defs.bzl", "cipd")
cipd_ext.require(name = "pigweed.pw_transfer_test_binaries", dev_dependency = True)
cipd_ext.require(name = "pigweed.doxygen_linux_amd64")
"""
        leaks = find_module_bazel_leaks(snippet)
        self.assertEqual(len(leaks), 6)
        self.assertIn('rules_doxygen', leaks[0])
        self.assertIn('leaked_sdk', leaks[1])
        self.assertIn('pnpm', leaks[2])
        self.assertIn('unlisted_ext', leaks[3])
        self.assertIn('pw_transfer_test_binaries', leaks[4])
        self.assertIn('doxygen_linux_amd64', leaks[5])

    def test_detects_cipd_use_repo_and_inline_bypasses(self) -> None:
        """Verifies the AST checker flags cipd use_repo and inline bypasses."""
        snippet = """
include("//some:segment.MODULE.bazel")
cipd_ext = use_extension("//pw_env_setup/bazel/cipd:defs.bzl", "cipd")
use_repo(cipd_ext, "cipd_client")
use_repo(cipd_ext, doxygen_linux_amd64 = "pigweed.doxygen_linux_amd64")
cipd_ext.client(cipd_bin = "@some_cipd//:cipd")
use_extension("//some:extensions.bzl", "unassigned_ext")
use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")(
    name = "inline_leaked",
)
"""
        leaks = find_module_bazel_leaks(snippet)
        self.assertEqual(len(leaks), 5)
        self.assertIn('include', leaks[0])
        self.assertIn('use_repo(cipd_ext', leaks[1])
        self.assertIn('cipd_ext.client', leaks[2])
        self.assertIn('use_extension', leaks[3])
        self.assertIn('use_repo_rule', leaks[4])

    def test_detects_wrapped_declarations(self) -> None:
        """Verifies declarations hidden inside other expressions are flagged.

        Bazel evaluates these declarations, but they are not statements of
        their own, so the per-statement checks never see them.
        """
        wrapped = (
            '_ = bazel_dep(name = "leaked_dep", version = "1.0")',
            '_ = register_toolchains("//pw_toolchain:host")',
            '[register_execution_platforms("//x:y") for _ in [1]]',
            '(bazel_dep(name = "tuple_dep", version = "1.0"),)',
            'register_toolchains(str("//x"), dev_dependency = True)',
            'fns = {"dep": bazel_dep}\nfns["dep"](name = "indexed_dep")',
        )
        for snippet in wrapped:
            with self.subTest(snippet=snippet):
                leaks = find_module_bazel_leaks(snippet)
                self.assertEqual(len(leaks), 1, leaks)
                self.assertIn('must be a top-level statement', leaks[0])

        # The same holds in an included file, even for a dev declaration.
        label = '//bazel:dev.MODULE.bazel'
        segment = (
            '_ = bazel_dep(name = "x", version = "1", dev_dependency = True)'
        )
        leaks = find_module_bazel_leaks(
            f'include("{label}")', includes={label: segment}
        )
        self.assertEqual(len(leaks), 1, leaks)
        self.assertTrue(leaks[0].startswith(label))
        self.assertIn('must be a top-level statement', leaks[0])

        # Plain constants are not calls, so they stay allowed.
        self.assertEqual(find_module_bazel_leaks('VERSION = "1.2.3"'), [])

    def test_detects_unidentifiable_non_dev_extensions(self) -> None:
        """Verifies non-dev extensions must be named by positional literals.

        Otherwise the CIPD extension would not be recognized and its repos
        would skip the _ALLOWED_NON_DEV_CIPD_REPOS checks.
        """
        cipd_bzl = '//pw_env_setup/bazel/cipd:defs.bzl'
        for args in (
            f'extension_bzl_file = "{cipd_bzl}", extension_name = "cipd"',
            f'"{cipd_bzl}", extension_name = "cipd"',
            'CIPD_BZL, "cipd"',
            f'"{cipd_bzl}", CIPD_NAME',
            '*CIPD_ARGS',
        ):
            with self.subTest(args=args):
                leaks = find_module_bazel_leaks(
                    f'cipd_ext = use_extension({args})\n'
                    'cipd_ext.require(name = "pigweed.doxygen_linux_amd64")\n'
                )
                self.assertEqual(len(leaks), 1, leaks)
                self.assertIn('Line 1: `cipd_ext = use_extension', leaks[0])
                self.assertIn('positional string literals', leaks[0])

        # Dev extensions are ignored downstream, so they need no identity.
        self.assertEqual(
            find_module_bazel_leaks(
                'ext = use_extension(SOME_BZL, NAME, dev_dependency = True)\n'
                'ext.tag(name = "anything")\n'
            ),
            [],
        )

    def test_detects_reassigned_handles(self) -> None:
        """Verifies a handle name cannot be rebound to another handle."""
        cipd = '"//pw_env_setup/bazel/cipd:defs.bzl", "cipd"'
        leaked = 'cipd_ext.require(name = "pigweed.doxygen_linux_amd64")\n'

        # A dev handle rebound to an allowlisted non-dev handle. Before this
        # was rejected, the later require() was treated as dev and skipped.
        leaks = find_module_bazel_leaks(
            f'cipd_ext = use_extension({cipd}, dev_dependency = True)\n'
            f'cipd_ext = use_extension({cipd})\n' + leaked
        )
        self.assertEqual(len(leaks), 1, leaks)
        self.assertIn('Line 2: `cipd_ext` is assigned more than once', leaks[0])

        # A non-dev handle rebound to a dev handle stays non-dev.
        leaks = find_module_bazel_leaks(
            f'cipd_ext = use_extension({cipd})\n'
            f'cipd_ext = use_extension({cipd}, dev_dependency = True)\n'
            + leaked
        )
        self.assertEqual(len(leaks), 2, leaks)
        self.assertIn('Line 2: `cipd_ext` is assigned more than once', leaks[0])
        self.assertIn('Line 3: `cipd_ext.require', leaks[1])

        # Repository rule handles, and rebinding across the two kinds.
        for second in (
            'rule = use_repo_rule("//x:defs.bzl", "other")',
            'rule = use_extension("//x:e.bzl", "e", dev_dependency = True)',
        ):
            with self.subTest(second=second):
                leaks = find_module_bazel_leaks(
                    f'rule = use_repo_rule("//x:defs.bzl", "rule")\n{second}\n'
                )
                self.assertEqual(len(leaks), 1, leaks)
                self.assertIn('assigned more than once', leaks[0])

    def test_accepts_root_only_included_segment(self) -> None:
        """Verifies overrides and dev declarations pass in an included file."""
        label = '//bazel:dev.MODULE.bazel'
        segment = """
VERSION = "1.2.3"
bazel_dep(name = "rules_doxygen", version = "2.4.2", dev_dependency = True)
single_version_override(module_name = "rules_rust", patches = [])
git_override(module_name = "fuchsia_infra", commit = "abc", remote = "x")
http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
http_archive(name = "dev_sdk", dev_dependency = True)
cipd_dev_ext = use_extension("//pw_env_setup/bazel/cipd:defs.bzl", "cipd", dev_dependency = True)
cipd_dev_ext.require(name = "pigweed.doxygen_linux_amd64", packages = {"a": VERSION})
use_repo(cipd_dev_ext, doxygen_linux_amd64 = "pigweed.doxygen_linux_amd64")
override_repo(cipd_dev_ext, "something")
register_toolchains("//pw_toolchain:host", dev_dependency = True)
register_execution_platforms("//pw_grpc:test_platform", dev_dependency = True)
"""
        leaks = find_module_bazel_leaks(
            f'include("{label}")', includes={label: segment}
        )
        self.assertEqual(leaks, [])

    def test_detects_leaks_in_included_segment(self) -> None:
        """Verifies non-root-only declarations fail in an included file."""
        label = '//bazel:resolve.MODULE.bazel'
        segment = """
module(name = "pigweed")
bazel_dep(name = "rules_cc", version = "0.2.22")
http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
http_archive(name = "leaked_sdk")
crate = use_extension("@rules_rust//crate_universe:extensions.bzl", "crate")
crate.from_cargo(name = "crates_std")
use_repo(crate, "crates_std")
register_toolchains("//pw_toolchain:host")
include("//bazel:dev.MODULE.bazel")
"""
        leaks = find_module_bazel_leaks(
            f'include("{label}")', includes={label: segment}
        )
        self.assertEqual(len(leaks), 8)
        self.assertTrue(all(leak.startswith(label) for leak in leaks))
        self.assertIn('Line 2: `module(...)` is not allowed', leaks[0])
        self.assertIn('rules_cc', leaks[1])
        self.assertIn('leaked_sdk', leaks[2])
        self.assertIn('crate = use_extension', leaks[3])
        self.assertIn('crate.from_cargo', leaks[4])
        self.assertIn('use_repo(crate', leaks[5])
        self.assertIn('register_toolchains', leaks[6])
        self.assertIn('Line 10: `include(...)` is not allowed', leaks[7])

    def test_detects_unchecked_includes(self) -> None:
        """Verifies include() fails unless the file is known and provided."""
        leaks = find_module_bazel_leaks('include("//other:x.MODULE.bazel")')
        self.assertEqual(len(leaks), 1)
        self.assertIn('_ALLOWED_INCLUDES', leaks[0])

        leaks = find_module_bazel_leaks('include("//bazel:dev.MODULE.bazel")')
        self.assertEqual(len(leaks), 1)
        self.assertIn('were not provided', leaks[0])

        leaks = find_module_bazel_leaks('include(DYNAMIC_LABEL)')
        self.assertEqual(len(leaks), 1)
        self.assertIn('include(DYNAMIC_LABEL)', leaks[0])

    def test_detects_stale_allowlist_entries(self) -> None:
        """Verifies check_stale=True flags absent allowlist entries."""
        leaks = find_module_bazel_leaks('', check_stale=True)
        self.assertTrue(any('_ALLOWED_INCLUDES' in err for err in leaks))
        self.assertTrue(
            any('_ALLOWED_NON_DEV_BAZEL_DEPS' in err for err in leaks)
        )
        self.assertTrue(
            any('_ALLOWED_NON_DEV_TOOLCHAINS' in err for err in leaks)
        )
        self.assertTrue(
            any('_ALLOWED_NON_DEV_CIPD_REPOS' in err for err in leaks)
        )
        self.assertTrue(
            any('_ALLOWED_NON_DEV_EXTENSION_HANDLES' in err for err in leaks)
        )


if __name__ == '__main__':
    unittest.main()
