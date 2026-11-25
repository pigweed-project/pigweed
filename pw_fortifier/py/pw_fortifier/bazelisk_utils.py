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
"""Base package scanner for Bazel-based dependencies."""

import ast
import asyncio
from collections.abc import AsyncIterator, Iterable, Iterator, Sequence
import fnmatch
import json
import logging
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
from typing import NamedTuple

from pw_fortifier.code_snippet import CodeSnippet, find_location

_LOG = logging.getLogger(__name__)


class _UnusableModuleFile(Exception):
    """Raised when a module file cannot be read or parsed."""


def _parse_module_file(path: Path) -> ast.Module:
    """Parses a module file with Python's parser.

    Module files are Starlark, but the top-level calls searched for here are
    also valid Python.

    Raises:
        _UnusableModuleFile: The file cannot be read or parsed.
    """
    try:
        source = path.read_text(encoding='utf-8')
    except (OSError, UnicodeDecodeError) as err:
        raise _UnusableModuleFile(f'Cannot read {path} ({err})') from err
    try:
        return ast.parse(source)
    except SyntaxError as err:
        raise _UnusableModuleFile(
            f'Cannot parse {path} (line {err.lineno}: {err.msg})'
        ) from err
    # Depending on the Python version, null bytes raise ValueError, and deeply
    # nested expressions raise RecursionError or MemoryError ("too complex to
    # parse"), instead of SyntaxError.
    except (ValueError, RecursionError, MemoryError) as err:
        raise _UnusableModuleFile(f'Cannot parse {path} ({err})') from err


def _is_file(path: Path) -> bool:
    """Returns whether path is a file; False if that cannot be determined."""
    try:
        return path.is_file()
    # Before Python 3.14, is_file() raises errors such as PermissionError.
    except OSError:
        return False


def _top_level_calls(tree: ast.Module) -> Iterator[tuple[str, ast.Call]]:
    """Yields (function name, call) for each top-level ``name(...)`` call."""
    for node in tree.body:
        # Only bare call statements matter; skip assignments and docstrings.
        if not isinstance(node, ast.Expr):
            continue
        call = node.value
        if not isinstance(call, ast.Call):
            continue
        # Skip calls on objects, such as extension tags: ``ext.tag(...)``.
        if not isinstance(call.func, ast.Name):
            continue
        yield call.func.id, call


def _string_literal(node: ast.expr) -> str | None:
    """Returns the value of a string literal, or None for anything else."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    return None


def _split_include_label(label: str) -> tuple[str, str] | None:
    """Splits an include() label into its package and target.

    Returns None unless the label names a ``*.MODULE.bazel`` file in the root
    repository using only path segments that Bazel accepts.
    """
    # include() only accepts files from the root module's own repository.
    if not label.startswith('//'):
        return None
    pkg, colon, target = label.removeprefix('//').partition(':')
    # Bazel requires the "//package:target" form and this file name suffix.
    if not colon or not target.endswith('.MODULE.bazel'):
        return None
    # Labels separate segments with "/" only. A backslash or a second ":"
    # could otherwise name a Windows drive or UNC path.
    if '\\' in label or ':' in target:
        return None
    # Reject absolute paths (empty segments) and "." or ".." segments.
    segments = target.split('/') + (pkg.split('/') if pkg else [])
    if any(segment in ('', '.', '..') for segment in segments):
        return None
    return pkg, target


def _included_labels(tree: ast.Module) -> Iterator[tuple[str, str]]:
    """Yields the (package, target) of each well-formed include() call."""
    for name, call in _top_level_calls(tree):
        if name != 'include':
            continue
        # include() takes exactly one argument: a string literal label.
        if len(call.args) != 1 or call.keywords:
            continue
        label = _string_literal(call.args[0])
        if label is None:
            continue
        pkg_and_target = _split_include_label(label)
        if pkg_and_target is not None:
            yield pkg_and_target


def _walk_includes(
    path: Path, root_dir: Path, real_root: Path, seen: set[Path]
) -> Iterator[Path]:
    """Yields every file that path includes, depth first.

    Bazel evaluates an included file in place of its include() call, so this
    is the order in which Bazel sees the declarations.

    Args:
        path: Module file to read.
        root_dir: Directory holding the root MODULE.bazel file.
        real_root: root_dir with symlinks resolved.
        seen: Resolved paths of the files visited so far; updated in place.
    """
    try:
        tree = _parse_module_file(path)
    except _UnusableModuleFile as err:
        _LOG.warning('%s; ignoring its include() calls', err)
        return
    for pkg, target in _included_labels(tree):
        included = root_dir / pkg / target
        # Compare resolved paths so that a file reached through a symlink is
        # visited only once and cannot lead outside the repository.
        real_included = included.resolve()
        if real_included in seen:
            continue
        if not real_included.is_relative_to(real_root):
            continue
        if not _is_file(real_included):
            continue
        seen.add(real_included)
        yield included
        yield from _walk_includes(included, root_dir, real_root, seen)


def module_bazel_files(
    module_bazel_path: str | os.PathLike[str],
) -> list[Path]:
    """Returns MODULE.bazel followed by every module file it include()s.

    Args:
        module_bazel_path: Path to the root MODULE.bazel file.

    Returns:
        The root file followed by every ``*.MODULE.bazel`` file it includes
        (directly or indirectly), depth first, which is the order in which
        Bazel evaluates them. Includes that resolve outside the root file's
        directory or do not exist are ignored. A file that cannot be read or
        parsed is logged as a warning and its include() calls are ignored.
    """
    root_module = Path(module_bazel_path)
    files = [root_module]
    # A missing root file is still returned so that callers can name it.
    if _is_file(root_module):
        root_dir = root_module.parent
        files.extend(
            _walk_includes(
                root_module,
                root_dir,
                root_dir.resolve(),
                {root_module.resolve()},
            )
        )
    return files


def _relative_to_root(
    root_path: str | os.PathLike[str],
    file_path: str | os.PathLike[str],
) -> str | os.PathLike[str]:
    """Strips a relative root_path from a relative file_path below it.

    find_location() treats relative file paths as relative to root_path, but
    module_bazel_files() returns paths that start with the root directory.
    """
    if not os.path.isabs(file_path) and not os.path.isabs(root_path):
        path = Path(file_path)
        if path.is_relative_to(root_path):
            return path.relative_to(root_path)
    return file_path


def find_module_location(
    root_path: str | os.PathLike[str],
    module_files: Sequence[str | os.PathLike[str]],
    *args: str | re.Pattern,
) -> CodeSnippet:
    """Finds the first line matching args in any of the given module files.

    Args:
        root_path: Path to the repository root.
        module_files: Files to search in order; see module_bazel_files().
        *args: Substrings or Patterns that must all appear on the line.

    Returns:
        A CodeSnippet for the first match, or a CodeSnippet for the first file
        with lines=None if nothing matches.
    """
    if not module_files:
        raise ValueError('module_files must not be empty')
    paths = [_relative_to_root(root_path, path) for path in module_files]
    for path in paths:
        location = find_location(root_path, path, *args)
        if location.lines:
            return location
    return find_location(root_path, paths[0])


def _bazel_dep_name(call: ast.Call) -> str | None:
    """Returns the module name that a bazel_dep() call declares."""
    for keyword in call.keywords:
        if keyword.arg == 'name':
            return _string_literal(keyword.value)
    # name is the first parameter of bazel_dep(), so it may be positional.
    return _string_literal(call.args[0]) if call.args else None


def find_bazel_dep_location(
    root_path: str | os.PathLike[str],
    module_files: Sequence[str | os.PathLike[str]],
    name: str,
) -> CodeSnippet:
    """Finds the bazel_dep() declaration of a module in the given files.

    Unlike find_module_location(), this finds declarations that span several
    lines, and it ignores comments and modules whose names only contain name.

    Args:
        root_path: Path to the repository root.
        module_files: Files to search in order; see module_bazel_files().
        name: Name of the module, as passed to bazel_dep().

    Returns:
        A CodeSnippet spanning the declaration. If no file that can be parsed
        declares the module, the result of searching the text of the files
        with find_module_location().
    """
    for path in module_files:
        try:
            tree = _parse_module_file(Path(path))
        except _UnusableModuleFile:
            # module_bazel_files() already reported this file.
            continue
        for func, call in _top_level_calls(tree):
            if func == 'bazel_dep' and _bazel_dep_name(call) == name:
                rel_path = _relative_to_root(root_path, path)
                return CodeSnippet(
                    find_location(root_path, rel_path).file,
                    (call.lineno, call.end_lineno or call.lineno),
                )
    return find_module_location(root_path, module_files, 'bazel_dep', name)


def _matches_pattern(rel_posix: str, pattern: str) -> bool:
    """Returns whether a root-relative path matches a --files pattern.

    As in Path.glob(), ``*`` stays within one path segment and ``**`` matches
    any number of segments, including none.
    """
    path_parts = rel_posix.split('/')
    # Positions in path_parts that the pattern segments so far can end at.
    positions = {0}
    for part in pattern.split('/'):
        if part == '**':
            positions = {
                end
                for start in positions
                for end in range(start, len(path_parts) + 1)
            }
        else:
            positions = {
                start + 1
                for start in positions
                if start < len(path_parts)
                and fnmatch.fnmatchcase(path_parts[start], part)
            }
        if not positions:
            return False
    return len(path_parts) in positions


def _normalize_pattern(entry: str, root: Path) -> str | None:
    p = Path(entry)
    if p.is_absolute():
        try:
            return p.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            return None
    return PurePosixPath(entry.replace('\\', '/')).as_posix()


def is_module_file_selected(
    root_path: str | os.PathLike[str], files: Iterable[str]
) -> bool:
    """Returns whether files matches MODULE.bazel or a file it include()s.

    Args:
        root_path: Path to the repository root.
        files: Root-relative paths, absolute paths inside root_path, or
            wildcard patterns (as accepted by --files).
    """
    root = Path(root_path)
    patterns = [
        norm
        for entry in files
        if (norm := _normalize_pattern(entry, root)) is not None
    ]
    if not patterns:
        return False
    real_root = root.resolve()
    for mod_path in module_bazel_files(root / 'MODULE.bazel'):
        # A symlinked module file is selected by its own path or its target's.
        candidates = {mod_path.relative_to(root).as_posix()}
        real_path = mod_path.resolve()
        if real_path.is_relative_to(real_root):
            candidates.add(real_path.relative_to(real_root).as_posix())
        if any(
            _matches_pattern(candidate, pattern)
            for candidate in candidates
            for pattern in patterns
        ):
            return True
    return False


async def run_bazelisk(
    args: list[str], cwd: str | os.PathLike[str]
) -> subprocess.CompletedProcess:
    """Runs a bazelisk command asynchronously in an executor.

    Args:
        args: List of command-line arguments for the bazelisk command.
        cwd: Working directory where the command is executed.

    Returns:
        A subprocess.CompletedProcess instance containing execution results.
    """
    cmd = ['bazelisk'] + args
    loop = asyncio.get_running_loop()

    def _run():
        return subprocess.run(
            cmd,
            cwd=cwd,
            capture_output=True,
            text=True,
        )

    try:
        result = await loop.run_in_executor(None, _run)
        if result.returncode != 0:
            raise subprocess.CalledProcessError(
                result.returncode, cmd, result.stdout, result.stderr
            )
        return result
    except FileNotFoundError as e:
        raise FileNotFoundError(
            'bazelisk command not found. '
            'Please ensure bazelisk is in your PATH.'
        ) from e


class BazelRepo(NamedTuple):
    """Represents a Bazel repository and its metadata from MODULE.bazel."""

    canonical_name: str
    """Canonical repository name string."""

    rule_name: str | None
    """Optional repository rule name string."""

    location: CodeSnippet | None
    """Optional location code snippet."""

    attributes: list[dict]
    """List of attribute dictionaries parsed from show_repo."""

    assignee: str | None = None
    """Optional legacy assignee field."""

    def get_attr_str(self, name: str) -> str:
        """Finds an attribute and returns stringValue or empty string.

        Args:
            name: Name of the attribute to find.

        Returns:
            The stringValue if found, or empty string.
        """
        for attr in self.attributes:
            if attr.get('name') == name:
                return attr.get('stringValue', '')
        return ''

    def get_attr_str_list(self, name: str) -> list[str]:
        """Finds an attribute and returns stringListValue or empty list.

        Args:
            name: Name of the attribute to find.

        Returns:
            The stringListValue if found, or an empty list.
        """
        for attr in self.attributes:
            if attr.get('name') == name:
                return attr.get('stringListValue', [])
        return []

    def get_attr_str_dict(self, name: str) -> dict[str, str]:
        """Finds an attribute and returns stringDictValue as a dict.

        Args:
            name: Name of the attribute to find.

        Returns:
            The dictionary of attribute keys and values.
        """
        for attr in self.attributes:
            if attr.get('name') == name:
                raw = attr.get('stringDictValue')
                if not raw:
                    return {}
                if isinstance(raw, list):
                    return {
                        entry.get('key'): entry.get('value')
                        for entry in raw
                        if 'key' in entry and 'value' in entry
                    }
                if isinstance(raw, dict):
                    return raw
        return {}

    @staticmethod
    async def load(
        module_bazel_path: str | os.PathLike[str],
    ) -> AsyncIterator['BazelRepo']:
        """Loads repository mappings and attributes from MODULE.bazel.

        Args:
            module_bazel_path: Path to the MODULE.bazel file to parse. Files it
                include()s are searched as well.

        Yields:
            BazelRepo instances for each discovered external repository.
        """
        module_path = Path(module_bazel_path)
        cwd = module_path.parent
        module_files = module_bazel_files(module_path)

        # 1. Run dump_repo_mapping
        result = await run_bazelisk(
            ['mod', 'dump_repo_mapping', '--lockfile_mode=off', ''],
            cwd,
        )
        mapping = json.loads(result.stdout)

        root_canonical_name = mapping.get('')
        repo_names = []
        for k, v in mapping.items():
            if k == '' or v == root_canonical_name:
                continue
            repo_names.append(f'@{k}')

        if not repo_names:
            return

        # 2. Run show_repo in bulk
        result = await run_bazelisk(
            ['mod', 'show_repo', '--lockfile_mode=off']
            + repo_names
            + ['--output=streamed_jsonproto'],
            cwd,
        )
        show_repo_output = result.stdout

        # Parse streamed JSON
        for line in show_repo_output.splitlines():
            if not line.strip():
                continue
            obj = json.loads(line)

            # Canonical naames can be mapped to repo names, e.g.
            #   aspect_bazel_lib+                 => aspect_bazel_lib
            #   +_repo_rules2+bazel_clang_tidy    => bazel_clang_tidy
            #   rules_python++python+pythons_hub  => pythons_hub
            canonical_name = obj.get('canonicalName')
            assert canonical_name
            repo_name = canonical_name.rstrip('+').split('+')[-1]

            # Find where the version is set. Skip if not set by MODULE.bazel
            # or a file it includes.
            rule_name = obj.get('repoRuleName')
            location = find_module_location(cwd, module_files, repo_name)
            if not location.lines:
                continue
            attributes = obj.get('attribute', [])

            yield BazelRepo(canonical_name, rule_name, location, attributes)
