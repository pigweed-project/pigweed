# Copyright 2023 The Pigweed Authors
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
"""Generates BUILD.gn files from rules in Bazel workspace."""

import argparse
import ast
import json
import os
import re

from collections import defaultdict, deque
from pathlib import Path, PurePath, PurePosixPath
from typing import (
    Deque,
    IO,
    Iterable,
    Iterator,
    Set,
)

from pw_build.bazel_query import (
    ParseError,
    BazelLabel,
    BazelRule,
    BazelRepo,
)
from pw_build.gn_target import GnTarget
from pw_build.gn_writer import GnFile

ROOT_DIR_VARNAMES = ['PW_ROOT', 'BUILD_WORKSPACE_DIRECTORY']


def _parse_module_file(path: Path) -> ast.Module | None:
    """Parses a module file with Python's parser.

    Module files are Starlark, but the top-level calls searched for here are
    also valid Python. Prints a warning and returns None if parsing fails.
    """
    source = path.read_text(encoding='utf-8')
    try:
        return ast.parse(source)
    except SyntaxError as err:
        reason = f'line {err.lineno}: {err.msg}'
    # Depending on the Python version, null bytes raise ValueError, and deeply
    # nested expressions raise RecursionError or MemoryError ("too complex to
    # parse"), instead of SyntaxError.
    except (ValueError, RecursionError, MemoryError) as err:
        reason = str(err)
    print(f'W: Cannot parse {path} ({reason}); ignoring its include() calls')
    return None


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
        label = call.args[0]
        if not isinstance(label, ast.Constant):
            continue
        if not isinstance(label.value, str):
            continue
        pkg_and_target = _split_include_label(label.value)
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
    tree = _parse_module_file(path)
    if tree is None:
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
        seen.add(real_included)
        yield included
        yield from _walk_includes(included, root_dir, real_root, seen)


def _module_bazel_files(
    module_bazel_path: str | os.PathLike[str],
) -> list[Path]:
    """Returns MODULE.bazel followed by every module file it include()s.

    Args:
        module_bazel_path: Path to the root MODULE.bazel file.

    Returns:
        The root file followed by every *.MODULE.bazel file it includes
        (directly or indirectly), depth first, which is the order in which
        Bazel evaluates them. Includes that resolve outside the root file's
        directory are ignored.

    Raises:
        OSError: A module file is missing or cannot be read.
        UnicodeDecodeError: A module file is not valid UTF-8.
    """
    root_module = Path(module_bazel_path)
    root_dir = root_module.parent
    files = [root_module]
    files.extend(
        _walk_includes(
            root_module, root_dir, root_dir.resolve(), {root_module.resolve()}
        )
    )
    return files


def _parse_args() -> argparse.Namespace:
    """Parse arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '-r',
        '--root_dir',
        type=PurePath,
        help=('Root source directory; defaults to $PW_ROOT'),
    )
    parser.add_argument(
        'names',
        type=str,
        nargs='+',
        help=(
            'Third-party dependencies to generate GN for. '
            'Must match a subdirectory of ${{root_dir}}/third_party'
        ),
    )
    args = parser.parse_args()

    for varname in ROOT_DIR_VARNAMES:
        if args.root_dir:
            break
        pw_root = os.getenv(varname)
        if pw_root:
            args.root_dir = PurePath(pw_root)

    if not args.root_dir:
        raise RuntimeError('Unable to determine project root.')

    return args


def _normalize_repo_name(repo_name: str):
    """Normalizes an external repository name to the variable-safe name.

    E.g. abseil-cpp -> abseil_cpp
    """
    return repo_name.replace('-', '_')


class BazelToGnConverter:
    """Manages the conversion of Bazel rules into GN targets."""

    def __init__(self) -> None:
        """Instantiates a Bazel workspace.

        Args:
            pw_root: Path to Pigweed directory, e.g. "$PW_ROOT".
        """
        self._repo_names_by_alias: dict[str, str] = {}
        self._repo_names_build_arg: dict[str, str] = {}
        self._pending: Deque[BazelLabel] = deque()
        self._loaded: Set[str] = set()
        self._repos: dict[str, BazelRepo] = {'pigweed': BazelRepo('pigweed')}

    def load_modules(self, module_bazel: IO) -> None:
        """Scans a MODULE.bazel file-like object for bazel_deps."""
        for line in module_bazel:
            if not line.startswith('bazel_dep'):
                continue
            m = re.search(r'name = "([^"]+)', line)
            if m:
                repo_name = m.group(1)
            m = re.search(r'repo_name = "([^"]+)', line)
            if m:
                alias = m.group(1)
                self._repo_names_by_alias[alias] = repo_name
            self._repo_names_build_arg[_build_arg(repo_name)] = repo_name
            self._repos[repo_name] = BazelRepo(repo_name)

    def repo_names(self, only_generated=False) -> Iterable[str]:
        """Returns the names of repositories known from MODULE.bazel.

        Args:
            only_generated: If set, only includes repos that have a
                            //third_party/<repo_name>/bazel_to_gn.json file that
                            specifies `"generate": True`.
        """
        for repo_name, repo in self._repos.items():
            if not only_generated or repo.generate:
                yield repo_name

    def parse_bazel_to_gn(self, repo_name: str, bazel_to_gn: IO) -> None:
        """Parses a bazel_to_gn.json file and loads the workspace it describes.

        Recognized fields include:
            repo:     The Bazel name of the repository.
            generate: Disables generating GN if present and set to `False`.
            targets:  A list of Bazel labels to generate GN for.
            options:  A dictionary of mapping Bazel flags to build settings.

        Args:
            repo_name: Name of a third party module.
            bazel_to_gn: A file-like object describing the Bazel workspace.
        """
        repo = self._repos[repo_name]
        json_data = json.load(bazel_to_gn)
        repo.generate = json_data.get('generate', False)
        repo.targets = json_data.get('targets', [])
        repo.options = json_data.get('options', {})

    def get_initial_targets(self, repo_name: str) -> list[BazelLabel]:
        """Adds labels from a third party module to the converter queue.

        Returns the number of labels added.

        Args:
            repo_name: Name of a previously loaded repo.
        """
        repo = self._repos[repo_name]
        self._loaded = set(repo.targets)
        return [BazelLabel.from_string(label_str) for label_str in self._loaded]

    def pending(self) -> Iterable[BazelLabel]:
        """Returns the label for the next rule that needs to be loaed."""
        while self._pending:
            label = self._pending.popleft()
            if str(label) in self._loaded:
                continue
            self._loaded.add(str(label))
            yield label

    def load_rules(self, labels: list[BazelLabel]) -> Iterable[BazelRule]:
        """Queries a Bazel workspace to instantiate a rule.

        Return `None` if the GN files for the workspace are manually
        generated, otherwise returns the rule. Adds the rules deps to the queue
        of pending labels to be loaded.

        Args:
            label:  The Bazel label indicating the workspace and target.
        """
        by_repo_name: dict[str, list[BazelLabel]] = defaultdict(list)
        deps: Set[str] = set()
        for label in labels:
            repo_name = label.repo_name()
            if repo_name not in self._repos:
                repo_name = self._repo_names_by_alias[repo_name]
            by_repo_name[repo_name].append(label)
        for repo_name, repo_labels in by_repo_name.items():
            repo = self._repos[repo_name]
            for rule in repo.get_rules(repo_labels):
                for dep in rule.get_list('deps'):
                    deps.add(str(dep))
                for dep in rule.get_list('implementation_deps'):
                    deps.add(str(dep))
                yield rule
        self._pending.extend([BazelLabel.from_string(dep) for dep in deps])

    def convert_rule(self, rule: BazelRule) -> GnTarget:
        """Creates a GN target from a Bazel rule.

        Args:
            rule: The rule to convert into a GnTarget.
        """
        label = rule.label()
        repo_name = label.repo_name()
        if rule.kind() == 'cc_library':
            normalized_repo_name = _normalize_repo_name(repo_name)
            if rule.get_bool('linkstatic'):
                target_type = f'{normalized_repo_name}_static_library'
            else:
                target_type = f'{normalized_repo_name}_source_set'
        else:
            raise ParseError(f'unsupported Bazel kind: {rule.kind()}')
        gn_target = GnTarget(target_type, label.name())
        gn_target.origin = str(label)
        gn_target.attrs = {
            'public': list(_source_relative(rule, 'hdrs')),
            'sources': list(_source_relative(rule, 'srcs')),
            'inputs': list(_source_relative(rule, 'additional_linker_inputs')),
            'include_dirs': list(_source_relative(rule, 'includes')),
            'cflags': rule.get_list('copts'),
            'public_defines': rule.get_list('defines'),
            'ldflags': rule.get_list('linkopts'),
            'defines': rule.get_list('local_defines'),
            'public_deps': list(self._build_relative(rule, 'deps')),
            'deps': list(self._build_relative(rule, 'implementation_deps')),
        }

        return gn_target

    def num_loaded(self) -> int:
        """Returns the number of rules loaded thus far."""
        return len(self._loaded)

    def update_pw_package(
        self, repo_name: str, lines: Iterator[str]
    ) -> Iterable[str]:
        """Updates the third party package revision in the pw_package module.

        Args:
            lines: Contents of the existing pw_package package file.

        TODO: https://pwbug.dev/398015969 - Use HTTP archives directly.
        """
        revision = self._repos[repo_name].revision()
        for line in lines:
            line = line.rstrip()
            m = re.match(r'(.*commit=[\'"])([a-z0-9]*)([\'"],.*)', line)
            if m:
                yield f'{m.group(1)}{revision}{m.group(3)}'
            else:
                yield line
        yield ''

    def get_imports(self, gn_target: GnTarget) -> Iterable[str]:
        """Returns the GNI files needed by the given target."""
        for build_arg in gn_target.build_args():
            repo_name = self._repo_names_build_arg[build_arg]
            normalized_repo_name = _normalize_repo_name(repo_name)
            yield f'$pw_external_{normalized_repo_name}/{repo_name}.gni'

    def _build_relative(self, rule: BazelRule, attr_name: str) -> Iterable[str]:
        """Provides GN labels relative to the directory under //third_party."""
        label1 = rule.label()
        repo_name1 = label1.repo_name()
        package1 = label1.package()
        for item in rule.get_list(attr_name):
            label2 = BazelLabel.from_string(
                item, repo_name=repo_name1, package=package1
            )
            repo_name2 = label2.repo_name()
            repo_name2 = self._repo_names_by_alias.get(repo_name2, repo_name2)
            package2 = label2.package()
            partial_path1 = f'{repo_name1}/{package1}'
            partial_path2 = f'{repo_name2}/{package2}'

            # Abbreviate the label only if it is part of the same repo.
            if repo_name1 != repo_name2:
                path = PurePosixPath(
                    f'$pw_external_{_normalize_repo_name(partial_path2)}'
                )
            elif partial_path1 == partial_path2:
                path = None
            else:
                path1 = PurePosixPath(partial_path1)
                path2 = PurePosixPath(partial_path2)
                common = PurePosixPath(
                    *os.path.commonprefix([path1.parts, path2.parts])
                )
                walk_up = PurePosixPath(
                    *(['..'] * (len(path1.parts) - len(common.parts)))
                )
                walk_down = path2.relative_to(common)
                path = PurePosixPath(walk_up, walk_down)

            if not path:
                yield f':{label2.name()}'
            elif path.name == label2.name():
                yield f'{path}'
            else:
                yield f'{path}:{label2.name()}'


def _build_arg(repo_name: str) -> str:
    """Returns the GN build argument for a third party module."""
    return f'$dir_pw_third_party_{_normalize_repo_name(repo_name)}'


def _source_relative(rule: BazelRule, attr_name: str) -> Iterable[str]:
    """Provides GN paths relative to the third party source directory."""
    if not rule.has_attr(attr_name):
        return
    attr_type = rule.attr_type(attr_name)
    label = rule.label()
    repo_name = label.repo_name()
    build_arg = _build_arg(repo_name)
    package = label.package()
    if attr_type == 'string_list':
        for item in rule.get_list(attr_name):
            yield f'{build_arg}/{item}'
    elif attr_type == 'label_list':
        for item in rule.get_list(attr_name):
            label = BazelLabel.from_string(
                item, repo_name=repo_name, package=package
            )
            yield f'{build_arg}/{label.package()}/{label.name()}'
    else:
        raise ParseError(f'unknown attribute type: {attr_type}')


def _overprint(msg: str) -> None:
    """Prints with a carriage return instead of a newline."""
    print(msg.ljust(80), end='\r', flush=True)


def _bazel_to_gn(args: argparse.Namespace) -> None:
    """Generates BUILD.gn files from rules in Bazel workspace.

    This script is intended to be as unit-testable as possible. As a result,
    most functionality has been pushed into testable methods of
    BazelToGnConverter.

    This method primarily consists of three things:
      1. Print statements to provide feedback to the user.
      2. File operations, to make subroutines more unit testable.
      3. Control flow and loops around the two previous categories.

    Args:
        args: Script arguments. See `_parse_args`.
    """
    b2g = BazelToGnConverter()
    root_dir = Path(args.root_dir)
    os.chdir(root_dir)

    print('[~] Converting Bazel rules and their dependencies to GN targets...')
    for module_file in _module_bazel_files(root_dir / 'MODULE.bazel'):
        with open(module_file, encoding='utf-8') as file:
            b2g.load_modules(file)

    third_party_path = Path(root_dir, 'third_party')
    for repo_name in b2g.repo_names():
        bazel_to_gn = Path(third_party_path, repo_name, 'bazel_to_gn.json')
        try:
            with open(bazel_to_gn) as file:
                b2g.parse_bazel_to_gn(repo_name, file)
            print(f'Using conversion options from //third_party/{bazel_to_gn}')
        except FileNotFoundError:
            pass

    print('Starting from:')
    for name in args.names:
        try:
            labels = b2g.get_initial_targets(name)
        except KeyError:
            print(f'E: Unable to get initial targets for "{name}".')
            print(f'E: Is "//third_party/{name}/bazel_to_gn.json" missing?')
            return
        print(f'  {len(labels)} initial rule(s) in {name}')

    by_partial_path: dict[str, list[GnTarget]] = defaultdict(list)
    while labels:
        for rule in b2g.load_rules(labels):
            label = rule.label()
            partial_path = f'{label.repo_name()}/{label.package()}'
            by_partial_path[partial_path].append(b2g.convert_rule(rule))
            _overprint(f'[{b2g.num_loaded()}] {rule.label()}')
        labels = list(b2g.pending())
    print(f'[{b2g.num_loaded()}] Bazel rule conversion complete!'.ljust(80))
    print()

    print('[~] Generating BUILD.gn files...')
    for partial_path, gn_targets in sorted(by_partial_path.items()):
        build_gn_path = third_party_path.joinpath(partial_path, 'BUILD.gn')
        imports = set().union(
            *[b2g.get_imports(gn_target) for gn_target in gn_targets]
        )
        _overprint(f'Writing {build_gn_path}...')
        with GnFile(build_gn_path) as build_gn:
            build_gn.write_file(imports, gn_targets)
    print('[+] BUILD.gn file generation complete!')
    print()

    print('[~] Updating presubmit packages...')
    for repo_name in b2g.repo_names(only_generated=True):
        update_path = root_dir.joinpath(
            'pw_package',
            'py',
            'pw_package',
            'packages',
            _normalize_repo_name(repo_name) + '.py',
        )
        _overprint(f'Updating {update_path}...')
        with open(update_path, 'r') as pkg_file:
            contents = '\n'.join(b2g.update_pw_package(repo_name, pkg_file))
        with open(update_path, 'w') as pkg_file:
            pkg_file.write(contents)
        print(f'Updating {update_path}  with current revision.')
    print('[+] Presubmit package update complete!')
    print()
    print('[+] Bazel to GN translation complete!')
    print()
    print('Next steps:')
    print('  1. Update Python packages using `ninja -C out python.install`.')
    print('  2. Build, e.g. using `pw build --step <some_gn_build>`.')
    print('  3. If applicable update a recurring task bug, e.g. b/315368642.')


if __name__ == '__main__':
    _bazel_to_gn(_parse_args())
