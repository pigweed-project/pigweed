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
"""Tests that Pigweed's MODULE.bazel does not leak dev-only declarations."""

import ast
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


def _is_cipd_use_extension(call: ast.Call) -> bool:
    if len(call.args) < 2:
        return False
    bzl_file, ext_name = call.args[0], call.args[1]
    return (
        isinstance(bzl_file, ast.Constant)
        and isinstance(bzl_file.value, str)
        and bzl_file.value.endswith('pw_env_setup/bazel/cipd:defs.bzl')
        and isinstance(ext_name, ast.Constant)
        and ext_name.value == 'cipd'
    )


def _find_stale_allowlist_entries(
    seen_bazel_deps: set[str],
    seen_repo_rule_instances: set[str],
    seen_cipd_repos: set[str],
    seen_non_dev_extension_handles: set[str],
    seen_non_dev_toolchains: set[str],
) -> list[str]:
    errors: list[str] = []
    for stale, table_name in (
        (
            _ALLOWED_NON_DEV_BAZEL_DEPS - seen_bazel_deps,
            '_ALLOWED_NON_DEV_BAZEL_DEPS',
        ),
        (
            _ALLOWED_NON_DEV_REPO_RULE_INSTANCES - seen_repo_rule_instances,
            '_ALLOWED_NON_DEV_REPO_RULE_INSTANCES',
        ),
        (
            _ALLOWED_NON_DEV_CIPD_REPOS - seen_cipd_repos,
            '_ALLOWED_NON_DEV_CIPD_REPOS',
        ),
        (
            _ALLOWED_NON_DEV_EXTENSION_HANDLES - seen_non_dev_extension_handles,
            '_ALLOWED_NON_DEV_EXTENSION_HANDLES',
        ),
        (
            _ALLOWED_NON_DEV_TOOLCHAINS - seen_non_dev_toolchains,
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
    errors: list[str],
) -> None:
    allowed_repos = _ALLOWED_NON_DEV_CIPD_REPOS | {'cipd_client'}
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


def _check_expr_call(  # pylint: disable=too-many-arguments,too-many-branches
    call: ast.Call,
    lineno: int,
    *,
    repo_rule_vars: set[str],
    non_dev_cipd_handles: set[str],
    seen_bazel_deps: set[str],
    seen_repo_rule_instances: set[str],
    seen_cipd_repos: set[str],
    seen_non_dev_toolchains: set[str],
    errors: list[str],
) -> None:
    if isinstance(call.func, ast.Name):
        func_name = call.func.id
        if func_name == 'include':
            errors.append(
                f'Line {lineno}: `include(...)` is not supported by '
                'module_bazel_leak_test without checking the included segment.'
            )
        elif func_name == 'bazel_dep':
            dep_name = _get_kwarg_str(call, 'name') or '<unknown>'
            if not _get_kwarg_bool(call, 'dev_dependency'):
                if dep_name in _ALLOWED_NON_DEV_BAZEL_DEPS:
                    seen_bazel_deps.add(dep_name)
                else:
                    errors.append(
                        f'Line {lineno}: `bazel_dep(name = "{dep_name}", ...)` '
                        'must set `dev_dependency = True` (or be explicitly '
                        'added to _ALLOWED_NON_DEV_BAZEL_DEPS).'
                    )
        elif func_name == 'register_toolchains':
            pos_args = [
                arg.value
                for arg in call.args
                if isinstance(arg, ast.Constant) and isinstance(arg.value, str)
            ]
            if not _get_kwarg_bool(call, 'dev_dependency'):
                if (
                    len(pos_args) == len(call.args)
                    and pos_args
                    and all(
                        arg in _ALLOWED_NON_DEV_TOOLCHAINS for arg in pos_args
                    )
                ):
                    seen_non_dev_toolchains.update(pos_args)
                else:
                    errors.append(
                        f'Line {lineno}: `{func_name}(...)` must set '
                        '`dev_dependency = True` so Pigweed does not '
                        'preempt downstream toolchain resolution.'
                    )
        elif func_name == 'register_execution_platforms':
            if not _get_kwarg_bool(call, 'dev_dependency'):
                errors.append(
                    f'Line {lineno}: `{func_name}(...)` must set '
                    '`dev_dependency = True` so Pigweed does not preempt '
                    'downstream platform resolution.'
                )
        elif (
            func_name == 'use_repo'
            and call.args
            and isinstance(call.args[0], ast.Name)
            and call.args[0].id in non_dev_cipd_handles
        ):
            _check_cipd_use_repo(call, lineno, call.args[0].id, errors)
        elif func_name in repo_rule_vars:
            repo_name = _get_kwarg_str(call, 'name') or '<unknown>'
            if not _get_kwarg_bool(call, 'dev_dependency'):
                if repo_name in _ALLOWED_NON_DEV_REPO_RULE_INSTANCES:
                    seen_repo_rule_instances.add(repo_name)
                else:
                    errors.append(
                        f'Line {lineno}: `{func_name}(name = '
                        f'"{repo_name}", ...)` must set '
                        '`dev_dependency = True` (or be explicitly added '
                        'to _ALLOWED_NON_DEV_REPO_RULE_INSTANCES).'
                    )
    elif (
        isinstance(call.func, ast.Attribute)
        and isinstance(call.func.value, ast.Name)
        and call.func.value.id in non_dev_cipd_handles
    ):
        handle_name = call.func.value.id
        if call.func.attr != 'require':
            errors.append(
                f'Line {lineno}: `{handle_name}.{call.func.attr}(...)` is not '
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
        elif repo_name in _ALLOWED_NON_DEV_CIPD_REPOS:
            seen_cipd_repos.add(repo_name)
        else:
            errors.append(
                f'Line {lineno}: `{handle_name}.require(name = '
                f'"{repo_name}")` is not in _ALLOWED_NON_DEV_CIPD_REPOS; '
                'use `cipd_dev_ext.require(...)` if it is dev-only.'
            )


def find_module_bazel_leaks(
    source: str,
    *,
    check_stale: bool = False,
) -> list[str]:
    """Parses a MODULE.bazel source string and returns any leak violations."""
    tree = ast.parse(source, filename='MODULE.bazel')
    errors: list[str] = []

    repo_rule_vars: set[str] = set()
    non_dev_cipd_handles: set[str] = set()
    seen_bazel_deps: set[str] = set()
    seen_repo_rule_instances: set[str] = set()
    seen_cipd_repos: set[str] = set()
    seen_non_dev_extension_handles: set[str] = set()
    seen_non_dev_toolchains: set[str] = set()
    assigned_calls: set[int] = set()

    for node in tree.body:
        if (
            isinstance(node, ast.Assign)
            and len(node.targets) == 1
            and isinstance(node.targets[0], ast.Name)
            and isinstance(node.value, ast.Call)
            and isinstance(node.value.func, ast.Name)
        ):
            assigned_calls.add(id(node.value))
            var_name = node.targets[0].id
            func_name = node.value.func.id
            if func_name == 'use_repo_rule':
                repo_rule_vars.add(var_name)
            elif func_name == 'use_extension':
                is_dev = _get_kwarg_bool(node.value, 'dev_dependency')
                if not is_dev:
                    if var_name in seen_non_dev_extension_handles:
                        errors.append(
                            f'Line {node.lineno}: Duplicate non-dev '
                            f'`{var_name} = use_extension(...)` assignment.'
                        )
                    elif var_name in _ALLOWED_NON_DEV_EXTENSION_HANDLES:
                        seen_non_dev_extension_handles.add(var_name)
                    else:
                        errors.append(
                            f'Line {node.lineno}: `{var_name} = '
                            'use_extension(...)` must set '
                            '`dev_dependency = True` (or be explicitly added '
                            'to _ALLOWED_NON_DEV_EXTENSION_HANDLES).'
                        )
                    if _is_cipd_use_extension(node.value):
                        non_dev_cipd_handles.add(var_name)

        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
            _check_expr_call(
                node.value,
                node.lineno,
                repo_rule_vars=repo_rule_vars,
                non_dev_cipd_handles=non_dev_cipd_handles,
                seen_bazel_deps=seen_bazel_deps,
                seen_repo_rule_instances=seen_repo_rule_instances,
                seen_cipd_repos=seen_cipd_repos,
                seen_non_dev_toolchains=seen_non_dev_toolchains,
                errors=errors,
            )

    for subnode in ast.walk(tree):
        if (
            isinstance(subnode, ast.Call)
            and id(subnode) not in assigned_calls
            and isinstance(subnode.func, ast.Name)
            and subnode.func.id in ('use_extension', 'use_repo_rule')
        ):
            if subnode.func.id == 'use_extension' and _get_kwarg_bool(
                subnode, 'dev_dependency'
            ):
                continue
            errors.append(
                f'Line {subnode.lineno}: `{subnode.func.id}(...)` must be '
                'assigned to a top-level handle variable.'
            )

    if check_stale:
        errors.extend(
            _find_stale_allowlist_entries(
                seen_bazel_deps,
                seen_repo_rule_instances,
                seen_cipd_repos,
                seen_non_dev_extension_handles,
                seen_non_dev_toolchains,
            )
        )

    return errors


class ModuleBazelLeakTest(unittest.TestCase):
    """Verifies MODULE.bazel dev_dependency hygiene."""

    def test_pigweed_module_bazel_has_no_leaks(self) -> None:
        """Checks Pigweed's root MODULE.bazel for non-dev leaks."""
        # Use os.path.abspath rather than Path.resolve() so that under Bazel we
        # walk up the .runfiles tree (verifying data = ["//:MODULE.bazel"])
        # instead of dereferencing symlinks out of the sandbox.
        module_bazel_path = (
            Path(os.path.abspath(__file__)).parents[2] / 'MODULE.bazel'
        )
        self.assertTrue(module_bazel_path.is_file())
        source = module_bazel_path.read_text(encoding='utf-8')
        leaks = find_module_bazel_leaks(source, check_stale=True)
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

    def test_detects_stale_allowlist_entries(self) -> None:
        """Verifies check_stale=True flags absent allowlist entries."""
        leaks = find_module_bazel_leaks('', check_stale=True)
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
