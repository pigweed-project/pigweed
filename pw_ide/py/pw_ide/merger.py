#!/usr/bin/env python3
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
Finds all existing compile command fragments and merges them into
platform-specific compilation databases.
"""

import argparse
import collections
from collections.abc import Iterator
import json
import logging
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

import time
from typing import Any, NamedTuple

from pw_cli import color, plural

_LOG = logging.getLogger(__name__)

# A unique suffix to identify fragments created by our aspect.
_FRAGMENT_SUFFIX = '.pw_aspect.compile_commands.json'

_COMPILE_COMMANDS_OUTPUT_GROUP = 'pw_cc_compile_commands_fragments'

# pylint: disable=line-too-long
_COMPILE_COMMANDS_ASPECT = '@pigweed//pw_ide/bazel/compile_commands:pw_cc_compile_commands_aspect.bzl%pw_cc_compile_commands_aspect'
# pylint: enable=line-too-long

# Supported architectures for clangd, based on the provided list.
# TODO(b/442862617): A better way than this than hardcoded list.
SUPPORTED_MARCH_ARCHITECTURES = {
    "nocona",
    "core2",
    "penryn",
    "bonnell",
    "atom",
    "silvermont",
    "slm",
    "goldmont",
    "goldmont-plus",
    "tremont",
    "nehalem",
    "corei7",
    "westmere",
    "sandybridge",
    "corei7-avx",
    "ivybridge",
    "core-avx-i",
    "haswell",
    "core-avx2",
    "broadwell",
    "skylake",
    "skylake-avx512",
    "skx",
    "cascadelake",
    "cooperlake",
    "cannonlake",
    "icelake-client",
    "rocketlake",
    "icelake-server",
    "tigerlake",
    "sapphirerapids",
    "alderlake",
    "raptorlake",
    "meteorlake",
    "arrowlake",
    "arrowlake-s",
    "lunarlake",
    "gracemont",
    "pantherlake",
    "sierraforest",
    "grandridge",
    "graniterapids",
    "graniterapids-d",
    "emeraldrapids",
    "clearwaterforest",
    "diamondrapids",
    "knl",
    "knm",
    "k8",
    "athlon64",
    "athlon-fx",
    "opteron",
    "k8-sse3",
    "athlon64-sse3",
    "opteron-sse3",
    "amdfam10",
    "barcelona",
    "btver1",
    "btver2",
    "bdver1",
    "bdver2",
    "bdver3",
    "bdver4",
    "znver1",
    "znver2",
    "znver3",
    "znver4",
    "znver5",
    "x86-64",
    "x86-64-v2",
    "x86-64-v3",
    "x86-64-v4",
}


class CompileCommand(NamedTuple):
    file: str
    directory: str
    arguments: list[str]


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--out-dir',
        '-o',
        type=Path,
        help=(
            'Where to write merged compile commands. By default, outputs are '
            'written to $BUILD_WORKSPACE_DIRECTORY/.compile_commands'
        ),
    )
    parser.add_argument(
        '--compile-command-groups',
        type=Path,
        help='Path to a JSON file with compile command patterns.',
    )
    parser.add_argument(
        '--verbose',
        '-v',
        action='store_true',
        help='Enable verbose output.',
    )
    parser.add_argument(
        '--overwrite-threshold',
        type=int,
        help=(
            'Skips regeneration if any existing compile commands databases are '
            'newer than the specified unix timestamp. This is primarily '
            'intended for internal use to prevent manually generated compile '
            'commands from being clobbered by automatic generation.'
        ),
    )
    parser.add_argument(
        'bazel_args',
        nargs=argparse.REMAINDER,
        help=(
            'Arguments after "--" are used to guide compile command generation.'
        ),
    )
    return parser.parse_args()


def _relativize_path(path: str | Path, start: Path | str | None) -> str:
    """Relativizes a path against a start directory.

    If the path is already relative, returns it as-is.
    If the path cannot be relativized (e.g. different drives on Windows),
    returns the absolute path.
    """
    if start is None:
        return str(path)

    path_obj = Path(path)
    if not path_obj.is_absolute():
        return str(path)

    try:
        return os.path.relpath(path_obj, start)
    except ValueError:
        return str(path_obj.resolve())


_INCLUDE_PREFIXES = ("-I", "-isystem", "-iquote", "-isysroot")

_SUPPORTED_SUBCOMMANDS = set(
    (
        'build',
        'test',
        'run',
    )
)


def _run_bazel(
    args: list[str], cwd: str, capture_output: bool = True
) -> subprocess.CompletedProcess[str]:
    """Runs bazel with the given arguments."""
    cmd = (
        os.environ.get('BAZEL_REAL', 'bazelisk'),
        *args,
    )
    _LOG.debug('Executing Bazel command: %s', shlex.join(cmd))
    return subprocess.run(
        cmd,
        capture_output=capture_output,
        text=True,
        check=True,
        cwd=cwd,
    )


def _run_bazel_build_for_fragments(
    build_args: list[str],
    verbose: bool,
    execution_root: Path,
) -> set[Path]:
    """Runs a bazel build with aspects and returns fragment paths."""
    # We use a temporary directory as the symlink prefix so we can parse the
    # output and find resulting files.
    with tempfile.TemporaryDirectory() as tmp_dir:
        bep_path = Path(tmp_dir) / 'compile_command_bep.json'
        command = list(build_args)
        command.extend(
            (
                '--show_result=0',
                f'--aspects={_COMPILE_COMMANDS_ASPECT}',
                f'--output_groups={_COMPILE_COMMANDS_OUTPUT_GROUP}',
                # This also makes all paths resolve as absolute.
                '--experimental_convenience_symlinks=ignore',
                f'--build_event_json_file={bep_path}',
            )
        )

        try:
            _run_bazel(
                command,
                cwd=os.environ['BUILD_WORKING_DIRECTORY'],
                capture_output=not verbose,
            )
        except subprocess.CalledProcessError as e:
            _LOG.fatal('Failed to generate compile commands fragments: %s', e)
            return set()

        fragments = set()
        for line in bep_path.read_text().splitlines():
            event = json.loads(line)
            for file in event.get('namedSetOfFiles', {}).get('files', []):
                file_path = file.get('name', '')
                file_path_prefix = file.get('pathPrefix', [])

                if not file_path.endswith(_FRAGMENT_SUFFIX):
                    continue

                if not file_path or not file_path_prefix:
                    # This should never happen.
                    _LOG.warning(
                        'Malformed file entry missing `name` and/or '
                        '`pathPrefix`: %s',
                        file,
                    )
                    continue

                artifact_path = execution_root.joinpath(
                    *file_path_prefix
                ).joinpath(file_path)
                fragments.add(artifact_path.resolve())
        return fragments


def _build_and_collect_fragments(
    forwarded_args: list[str],
    verbose: bool,
    execution_root: Path,
) -> Iterator[Path]:
    """Collects fragments using `bazel cquery`."""
    if forwarded_args and forwarded_args[0] == '--':
        # Remove initial double-dash.
        forwarded_args.pop(0)

    # `bazel run` commands might bundle a `--`. These application-specific
    # arguments will cause problems when calling `bazel build`, so remove them.
    try:
        forwarded_args = forwarded_args[: forwarded_args.index('--')]
    except ValueError:
        pass

    subcommand_index = None
    for i, arg in enumerate(forwarded_args):
        if arg in _SUPPORTED_SUBCOMMANDS:
            subcommand_index = i
            break

    # This is an unsupported subcommand, so don't regenerate.
    if subcommand_index is None:
        return

    _LOG.info('⏳ Generating compile commands...')
    build_args = list(forwarded_args)
    build_args[subcommand_index] = 'build'
    yield from _run_bazel_build_for_fragments(
        build_args, verbose, execution_root
    )


def _build_and_collect_fragments_from_groups(
    groups_file: Path,
    verbose: bool,
    execution_root: Path,
) -> Iterator[Path]:
    """Collects fragments using patterns from a JSON file."""
    # When running interactively, passing a local file will cause this to fail
    # since the path isn't relative to the sandbox. This is intentional, as
    # the JSON format isn't intended to be user-facing.
    with open(groups_file, 'r') as f:
        try:
            patterns = json.load(f)
        except json.JSONDecodeError:
            _LOG.error("Could not parse %s, skipping.", groups_file)
            return

    known_fragments: dict[Path, int] = {}
    ignored_fragments: set[Path] = set()
    has_errors = False
    compile_commands_patterns = patterns.get('compile_commands_patterns', [])
    total_groups = len(compile_commands_patterns)

    for i, group in enumerate(compile_commands_patterns):
        platform = group.get('platform')
        targets = group.get('target_patterns')

        if not platform or not targets:
            _LOG.warning("Skipping invalid compile command group: %s", group)
            continue

        _LOG.info(
            '⏳ Generating compile commands for group %d/%d (%s)...',
            i + 1,
            total_groups,
            platform,
        )
        build_args = ['build', '--platforms', platform, *targets]
        group_fragments = _run_bazel_build_for_fragments(
            build_args, verbose, execution_root
        )

        # Multiple builds may generate the same fragments. It's possible that
        # you can end up with multiple conflicting definitions of the same
        # compile commands fragment due to how transitions work. This catches
        # cases where the content diverges, and raises errors to signal what
        # went wrong.
        for fragment in group_fragments:
            if fragment in ignored_fragments:
                continue

            content_hash = hash(fragment.read_text())
            if fragment in known_fragments:
                if known_fragments[fragment] != content_hash:
                    _LOG.warning(
                        'Fragment file %s was generated by multiple groups '
                        'with different content (this can happen if different '
                        'bazel flags produce different compile commands for '
                        'the same file). Skipping this fragment.',
                        fragment,
                    )
                    ignored_fragments.add(fragment)
                    del known_fragments[fragment]
            else:
                known_fragments[fragment] = content_hash

    if has_errors:
        return

    yield from known_fragments.keys()


def _collect_fragments(
    execution_root: Path,
    forwarded_args: list[str],
    verbose: bool,
    compile_command_groups: Path | None = None,
) -> Iterator[Path]:
    """Dispatches to the correct fragment collection method."""
    if compile_command_groups:
        yield from _build_and_collect_fragments_from_groups(
            compile_command_groups,
            verbose,
            execution_root,
        )
    elif forwarded_args:
        yield from _build_and_collect_fragments(
            forwarded_args,
            verbose,
            execution_root,
        )


class PrettyFormatter(logging.Formatter):
    """A logging formatter that tunes logging for this script."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._colors = color.colors()
        self._color_enabled = color.is_enabled()

    def _gray(self, msg: str) -> str:
        if self._color_enabled:
            return self._colors.gray(msg)
        return msg

    def format(self, record: logging.LogRecord) -> str:
        """Formats the log record."""
        message = record.getMessage()
        if record.levelno >= logging.ERROR:
            level_prefix = '❌ '
            message = self._colors.red(message)
        elif record.levelno >= logging.WARNING:
            level_prefix = '⚠️ '
            message = self._colors.yellow(message)
        elif record.levelno == logging.DEBUG:
            level_prefix = ''
            message = self._gray(message)
        else:
            level_prefix = ''
        return f'{level_prefix}{message}'


def _setup_logging(log_level: int):
    handler = logging.StreamHandler()
    handler.setFormatter(PrettyFormatter())
    root_logger = logging.getLogger()
    root_logger.addHandler(handler)
    root_logger.setLevel(log_level)


def _get_bazel_path_info(key: str, cwd: str) -> Path | None:
    """Gets a value from `bazel info {key}`."""
    try:
        output_str = _run_bazel(
            ['info', key],
            cwd=cwd,
        ).stdout.strip()
    except subprocess.CalledProcessError as e:
        _LOG.fatal("Error getting bazel %s: %s", key, e)
        return None
    return Path(output_str)


def _load_commands_for_platform(
    fragments: list[Path], platform: str
) -> tuple[list[dict], list[dict], dict[str, str]] | None:
    """Load compile commands fragments for a single platform.

    Ingests each compile commands fragment JSON file, and merges the compile
    commands into a single database.

    Args:
        fragments: A list of paths to fragment files.
        platform: The name of the platform being processed, for logging.

    Returns:
        A tuple of (compile commands, target infos, virtual mappings). Returns
        None if conflicting entries are identified.
    """
    commands_by_file: dict[tuple[str, str], dict] = {}
    all_infos: list[dict] = []
    all_vrmaps: dict[str, str] = {}
    for fragment_path in fragments:
        with open(fragment_path, "r") as f:
            try:
                fragment_commands = json.load(f)
            except json.JSONDecodeError:
                _LOG.error("Could not parse %s, skipping.", fragment_path)
                continue

            if isinstance(fragment_commands, list):
                # Legacy fragment format (top level is a list of commands)
                commands_list = fragment_commands
            else:
                # New fragment format
                virtual_mappings = fragment_commands.get("virtual_mappings", [])
                for mapping in virtual_mappings:
                    v_path = mapping.get("virtual_path")
                    r_path = mapping.get("real_path")
                    if v_path and r_path:
                        all_vrmaps[v_path] = r_path

                target_info_field = fragment_commands.get("target_info")
                if isinstance(target_info_field, dict):
                    all_infos.append(target_info_field)
                elif isinstance(target_info_field, list):
                    for info_dict in target_info_field:
                        if isinstance(info_dict, dict):
                            all_infos.append(info_dict)
                        else:
                            _LOG.warning(
                                "Skipping malformed target_info entry "
                                "(expected dict, got %s): %s",
                                type(info_dict),
                                info_dict,
                            )
                elif target_info_field is not None:
                    _LOG.warning(
                        "Skipping malformed target_info field "
                        "(expected dict or list, got %s): %s",
                        type(target_info_field),
                        target_info_field,
                    )
                commands_list = fragment_commands.get("commands", [])

            for command_dict in commands_list:
                # TODO: https://pwbug.dev/446688765 - Support headers, either by
                # extracting them from the fragments before they hit the compile
                # commands database, or by enumerating them in separate files.
                if command_dict['file'].endswith('.h'):
                    continue

                # Different compiled files may occur multiple times. This
                # can be because of PIC/non-PIC variants of a library, or
                # because a source file is used in multiple build targets
                # (e.g. two cc_library targets with different flags/names).
                # To differentiate, we key based on the outputs, which
                # reliably will tell us whether or not definitions truly
                # collide.
                command_tuple = (
                    command_dict['file'],
                    str(command_dict.get('outputs', [])),
                )
                if command_tuple in commands_by_file:
                    existing_cmd = commands_by_file[command_tuple]
                    if tuple(existing_cmd['arguments']) != tuple(
                        command_dict['arguments']
                    ):
                        _LOG.error(
                            'Conflict for file %s in platform %s',
                            command_dict['file'],
                            platform,
                        )
                        _LOG.error(
                            'Existing command: %s',
                            shlex.join(existing_cmd['arguments']),
                        )
                        _LOG.error(
                            'New command: %s',
                            shlex.join(command_dict['arguments']),
                        )
                        return None
                else:
                    commands_by_file[command_tuple] = command_dict

    return list(commands_by_file.values()), all_infos, all_vrmaps


def _validate_environment() -> tuple[Path, Path, Path, Path]:
    """Validates the environment and returns Bazel paths.

    Returns:
        A tuple of (workspace_root, bazel_output_path, output_base_path,
        execution_root_path).

    Raises:
        SystemExit: If the environment is invalid or paths cannot be found.
    """
    workspace_root_str = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not workspace_root_str:
        _LOG.error("This script must be run with 'bazel run'.")
        sys.exit(1)

    workspace_root = Path(workspace_root_str).resolve()

    bazel_output_path = _get_bazel_path_info('output_path', workspace_root_str)
    if bazel_output_path is None:
        sys.exit(1)
    bazel_output_path = bazel_output_path.resolve()

    output_base_path = _get_bazel_path_info('output_base', workspace_root_str)
    if output_base_path is None:
        sys.exit(1)
    output_base_path = output_base_path.resolve()

    execution_root_path = _get_bazel_path_info(
        'execution_root', workspace_root_str
    )
    if execution_root_path is None:
        sys.exit(1)
    execution_root_path = execution_root_path.resolve()

    if not bazel_output_path.exists():
        _LOG.fatal("Bazel output directory '%s' not found.", bazel_output_path)
        _LOG.fatal(
            "Did you run 'bazel build --config=ide //your/target' first?"
        )
        sys.exit(1)

    return (
        workspace_root,
        bazel_output_path,
        output_base_path,
        execution_root_path,
    )


def _find_fragments(
    execution_root_path: Path,
    args: argparse.Namespace,
) -> dict[str, list[Path]]:
    """Finds all compile command fragments and groups them by platform.

    Returns:
        A dictionary mapping platform names to lists of fragment paths.

    Raises:
        SystemExit: If no fragments are found.
    """
    # Search for fragments with our unique suffix.
    all_fragments = list(
        _collect_fragments(
            execution_root_path,
            args.bazel_args,
            args.verbose,
            args.compile_command_groups,
        )
    )

    if not all_fragments:
        _LOG.error("Could not find any generated fragments.")
        sys.exit(1)

    fragments_by_platform: dict[str, list[Path]] = collections.defaultdict(list)
    for fragment in all_fragments:
        base_name = fragment.name.removesuffix(_FRAGMENT_SUFFIX)
        platform = base_name.split(".")[-1]
        fragments_by_platform[platform].append(fragment)

    return fragments_by_platform


def _process_platform(
    platform: str,
    fragments: list[Path],
    workspace_root: Path,
    output_dir: Path,
    bazel_paths: tuple[Path, Path, Path],
) -> None:
    """Processes a single platform, merging fragments and writing output files.

    Args:
        platform: The platform name (e.g., 'macos').
        fragments: List of fragment paths for this platform.
        workspace_root: Path to the workspace root.
        output_dir: Path to the output directory.
        bazel_paths: Tuple of (bazel_output_path, output_base_path,
                     execution_root_path).

    Raises:
        SystemExit: If loading commands fails.
    """
    (
        bazel_output_path,
        output_base_path,
        execution_root_path,
    ) = bazel_paths

    _LOG.debug("Processing platform: %s...", platform)
    if fragments:
        result = _load_commands_for_platform(fragments, platform)
        if result is None:
            sys.exit(1)
        all_commands, target_infos, all_vrmaps = result
    else:
        all_commands = []
        target_infos = []
        all_vrmaps = {}

    workspace_root_path = workspace_root.resolve()
    relative_to = workspace_root_path

    platform_dir = output_dir / platform
    platform_dir.mkdir(exist_ok=True)
    merged_json_path = platform_dir / "compile_commands.json"

    processed_commands = []
    for command_dict in all_commands:
        cmd = CompileCommand(
            file=command_dict["file"],
            directory=command_dict["directory"],
            arguments=command_dict["arguments"],
        )
        cmd = remap_virtual_includes(
            cmd, all_vrmaps, bazel_output_path, execution_root_path
        )
        resolved_cmd = resolve_external_paths(
            cmd, output_base_path, relative_to=relative_to
        )
        resolved_cmd = resolve_bazel_out_paths(
            resolved_cmd, bazel_output_path, relative_to=relative_to
        )
        resolved_cmd = filter_unsupported_march_args(resolved_cmd)
        processed_commands.append(resolved_cmd._asdict())

    with open(merged_json_path, "w") as f:
        json.dump(processed_commands, f, indent=2)

    with open(merged_json_path, "r") as f:
        content = f.read()
    content = content.replace("__WORKSPACE_ROOT__", str(workspace_root))
    with open(merged_json_path, "w") as f:
        f.write(content)

    # Process target info (deps mapping)
    if target_infos:
        _process_target_infos(
            target_infos,
            bazel_output_path,
            output_base_path,
            relative_to,
            platform_dir,
        )

    (output_dir / 'pw_lastGenerationTime.txt').write_text(str(int(time.time())))


def _update_files_map(
    files_map: dict[str, list[str]],
    label: str,
    files: list[str],
    relative_to: Path | None,
) -> None:
    """Updates the files_map with the given files and label."""
    for file_path in files:
        rel_path = (
            _relativize_path(file_path, relative_to)
            if relative_to
            else file_path
        )
        # We append the label because multiple targets might own the file
        if label not in files_map[rel_path]:
            files_map[rel_path].append(label)


def _process_target_infos(
    target_infos: list[dict],
    bazel_output_path: Path,
    output_base_path: Path,
    relative_to: Path | None,
    platform_dir: Path,
) -> None:
    """Processes target infos and writes deps_mapping.json."""
    final_target_infos: dict[str, Any] = {}
    files_map: dict[str, list[str]] = collections.defaultdict(list)

    for info in target_infos:
        label = info.get("label")
        if not label:
            continue
        # Resolve paths in info with relative_to support
        resolved_info = resolve_target_info_paths(
            info,
            bazel_output_path,
            output_base_path,
            relative_to=relative_to,
        )

        if label in final_target_infos:
            existing_info = final_target_infos[label]
            for key in ("srcs", "hdrs", "deps"):
                existing_info[key] = list(
                    set(existing_info.get(key, []) + resolved_info.get(key, []))
                )
        else:
            final_target_infos[label] = resolved_info

        # Retrieve potentially merged paths to update files_map
        current_srcs = final_target_infos[label].get("srcs", [])
        current_hdrs = final_target_infos[label].get("hdrs", [])

        _update_files_map(files_map, label, current_srcs, relative_to)
        _update_files_map(files_map, label, current_hdrs, relative_to)

    deps_mapping = {
        "files": files_map,
        "targets": final_target_infos,
    }

    deps_mapping_path = platform_dir / "deps_mapping.json"
    deps_mapping_path.write_text(json.dumps(deps_mapping, indent=2))


def main() -> int:
    """Script entry point."""
    args = _parse_args()

    if args.verbose:
        log_level = logging.DEBUG
    else:
        log_level = logging.WARNING

    _setup_logging(log_level)

    (
        workspace_root,
        bazel_output_path,
        output_base_path,
        execution_root_path,
    ) = _validate_environment()

    fragments_by_platform = _find_fragments(execution_root_path, args)

    _LOG.debug(
        "Found fragments for %s.",
        plural.plural(fragments_by_platform, 'platform'),
    )

    # Union of all platforms found
    all_platforms = set(fragments_by_platform.keys())

    output_dir = args.out_dir
    if not output_dir:
        output_dir = Path(workspace_root) / ".compile_commands"

    if output_dir.exists():
        # Make the generator a list so it can be reused.
        existing_databases = list(output_dir.rglob('*/compile_commands.json'))

        if args.overwrite_threshold and any(
            db.stat().st_mtime > args.overwrite_threshold
            for db in existing_databases
        ):
            _LOG.debug(
                'Skipping regeneration; fresh compile commands database '
                'already exists'
            )
            return 0

        for f in existing_databases:
            f.unlink()

    output_dir.mkdir(exist_ok=True)

    for platform in all_platforms:
        _process_platform(
            platform,
            fragments_by_platform.get(platform, []),
            workspace_root,
            output_dir,
            (bazel_output_path, output_base_path, execution_root_path),
        )

    (output_dir / 'pw_lastGenerationTime.txt').write_text(str(int(time.time())))
    _LOG.info(
        "✅ Successfully created compilation databases in: %s", output_dir
    )
    return 0


def _resolve_virtual_include_dynamic(
    abs_v_dir: Path, execution_root_path: Path, prefix: str
) -> str | None:
    """Helper to dynamically resolve a virtual include directory's real path."""
    try:
        for f in abs_v_dir.rglob('*'):
            if f.is_file() or f.is_symlink():
                real_f = f.resolve()
                rel_parts = f.relative_to(abs_v_dir).parts
                real_dir = real_f.parents[len(rel_parts) - 1]

                try:
                    rel_real_dir = real_dir.relative_to(execution_root_path)
                    return prefix + str(rel_real_dir)
                except ValueError:
                    # Not relative to execroot, maybe it's external?
                    return prefix + str(real_dir)
    except OSError:
        # rglob errors (e.g. permission denied) are possible.
        pass
    return None


def remap_virtual_includes(
    command: CompileCommand,
    virtual_mappings_map: dict[str, str],
    bazel_output_path: Path,
    execution_root_path: Path,
) -> CompileCommand:
    """Remaps virtual includes using a map or dynamic symlink resolution.

    While the Bazel aspect generates `virtual_mappings_map` for most explicit
    targets by traversing `attr_aspects = ["deps", "implementation_deps",
    "srcs", "target", "backend", "includes"]`, it misses virtual includes
    generated by implicit dependencies (like toolchain libraries, e.g.
    `pw_preprocessor`). We cannot use `["*"]` to traverse all attributes
    because it causes Bazel to crash on internal dependencies like
    `@bazel_tools//tools/def_parser:def_parser`.

    To compensate for the aspect missing these implicit generic dependencies,
    this function acts as a safety net. It first applies the explicit
    dictionary mappings. Then, if it encounters any remaining
    `_virtual_includes` paths, it resolves them dynamically by physically
    looking inside the generated `_virtual_includes` directory in `bazel-out`,
    following the symlinks inside it, and determining exactly what source
    directory they point to relative to the execution root.
    """
    new_args = []
    for arg in command.arguments:
        # Check against _INCLUDE_PREFIXES to handle both prefix+path and raw
        # paths.
        prefix = ""
        # The order matters here because -isystem is a prefix of -isystem-after
        # but the tuple order handles it if we just take the first match.
        # However, for simplicity let's stick to the known prefixes.
        for p in _INCLUDE_PREFIXES:
            if arg.startswith(p):
                prefix = p
                break

        # Check if we have a direct mapping for the raw path (without prefix)
        # We strip the prefix if found.
        path_part = arg[len(prefix) :]
        if path_part in virtual_mappings_map:
            new_args.append(prefix + virtual_mappings_map[path_part])
            continue

        if '_virtual_includes/' not in arg:
            new_args.append(arg)
            continue

        # Convert to Path for manipulation
        path_str = arg[len(prefix) :]
        path = Path(path_str)

        if path.is_relative_to('bazel-out'):
            # Handle bazel-out paths by anchoring them to the actual bazel
            # output path. path is something like
            # bazel-out/k8-fastbuild/bin/...
            # we want to strip bazel-out/ and join with bazel_output_path
            abs_v_dir = bazel_output_path / path.relative_to('bazel-out')
        else:
            abs_v_dir = path

        if abs_v_dir.exists() and abs_v_dir.is_dir():
            resolved_arg = _resolve_virtual_include_dynamic(
                abs_v_dir, execution_root_path, prefix
            )
            if resolved_arg:
                new_args.append(resolved_arg)
            else:
                new_args.append(arg)
        else:
            new_args.append(arg)

    return command._replace(arguments=new_args)


def resolve_bazel_out_paths(
    command: CompileCommand,
    bazel_output_path: Path,
    relative_to: Path | None = None,
) -> CompileCommand:
    """Replaces bazel-out paths with their real paths."""
    marker = 'bazel-out/'
    new_args = []

    if relative_to:
        relative_to = Path(relative_to)

    for arg in command.arguments:
        if marker in arg:
            parts = arg.split(marker, 1)
            prefix = parts[0]
            suffix = parts[1]
            if relative_to:
                new_path_str = f"bazel-out/{suffix}"
            else:
                new_path_str = str(
                    bazel_output_path.joinpath(*suffix.split('/'))
                )
            new_arg = prefix + new_path_str
            new_args.append(new_arg)
        else:
            new_args.append(arg)

    new_file = command.file
    if command.file.startswith(marker):
        path_suffix = command.file[len(marker) :]
        if relative_to:
            new_file = f"bazel-out/{path_suffix}"
            # Need to normalize path (remove ../) but keep it relative
            # If it's just bazel-out/..., it's already refined.
        else:
            new_file = str(bazel_output_path.joinpath(*path_suffix.split('/')))
            # Treat bazel-out files as generated and resolve symlinks
            new_file = str(Path(new_file).resolve())
    elif relative_to and Path(command.file).is_absolute():
        new_file = _relativize_path(command.file, relative_to)

    return command._replace(arguments=new_args, file=new_file)


def _resolve_single_external_path(
    path: Path, output_base: Path, relative_to: Path | None = None
) -> Path | None:
    """Resolves a single external path against the output base."""
    external_root = output_base / 'external'

    # Check if inside external root
    if path.is_relative_to(external_root):
        rel_path = path.relative_to(external_root)
    else:
        # Try resolving the path (e.g. handle /var vs /private/var)
        path = path.resolve()
        if not path.is_relative_to(external_root):
            return None
        rel_path = path.relative_to(external_root)

    full_path = external_root / rel_path

    if relative_to:
        if full_path.is_relative_to(relative_to):
            return full_path.relative_to(relative_to)
        # If we can't be relative to the workspace root (e.g.
        # output_base is entirely outside), we still want "external/..."
        # because that's how Bazel (and our symlinks) structure the
        # world.
        return Path(f"external/{rel_path}")
    return full_path


def resolve_external_paths(
    command: CompileCommand,
    output_base: Path,
    relative_to: Path | None = None,
) -> CompileCommand:
    """Replaces relative external/ paths with their real paths.

    If a path starts with `external/`, resolving it natively to the Bazel
    workspace will fail in newer versions of Bazel since the root `external/`
    symlink tree is no longer created by default.
    This replaces `external/...` with the absolute resolved path to that
    external dependency inside the `bazel_output_base`.

    If `relative_to` is provided, it will attempt to make that absolute path
    relative to `relative_to`. If it cannot be made relative (e.g. the external
    cache is in a completely different directory tree from the workspace), it
    safely falls back to returning the absolute path instead of a broken
    `external/` prefix.
    """
    # For now, don't support --experimental_sibling_repository_layout.
    marker = 'external/'
    new_args = []

    allowed_prefixes = [
        '--sysroot',
        '--warning-suppression-mappings',
        '-fsanitize-blacklist',  # inclusive-language: ignore
        '-fsanitize-ignorelist',
        '-I',
        '-imacros',
        '-include',
        '-iquote',
        '-isysroot',
        '-isystem',
        '-L',
        '-resource-dir',
        '',
    ]

    for arg in command.arguments:
        new_arg = arg

        # Check if arg contains an absolute path to external repo
        # Use simple string checks first for performance
        if Path(arg).is_absolute() or str(output_base) in arg:
            # Try to replace absolute path with relative external/ path
            # We iterate over allowed prefixes to find where the path starts
            for prefix in allowed_prefixes:
                # Find start of path
                if prefix and not arg.startswith(prefix):
                    continue

                # Assume arg structure is [prefix][path] or [prefix]=[path]
                is_eq = arg.startswith(prefix + '=') if prefix else False
                prefix_len = len(prefix) + (1 if is_eq else 0)

                relevant_part = Path(arg[prefix_len:])
                if not relevant_part.is_absolute():
                    continue

                resolved = _resolve_single_external_path(
                    relevant_part, output_base, relative_to
                )
                if resolved:
                    new_arg = arg[:prefix_len] + str(resolved)
                    break

        # Existing logic for handling relative external/ paths (if any remain)
        for prefix in allowed_prefixes:
            if not arg.startswith(prefix + marker) and not arg.startswith(
                prefix + '=' + marker
            ):
                continue

            # Extract the actual path part from the argument
            is_eq = arg.startswith(prefix + '=')
            prefix_len = len(prefix) + (1 if is_eq else 0)

            path_part_with_marker = arg[prefix_len:]
            path_suffix = path_part_with_marker[len(marker) :]

            abs_path = output_base.joinpath('external', *path_suffix.split('/'))
            resolved_path = abs_path.resolve()

            if relative_to:
                try:
                    new_path_str = str(resolved_path.relative_to(relative_to))
                except ValueError:
                    new_path_str = f"external/{path_suffix}"
            else:
                new_path_str = str(resolved_path)

            new_arg = arg[:prefix_len] + new_path_str
            break  # Found a matching prefix, no need to check others

        new_args.append(new_arg)

    new_file = command.file
    # Check if file is absolute path to external repo
    if Path(command.file).is_absolute():
        path_obj = Path(command.file)
        external_root = output_base / 'external'

        # If the file is in the external root, using 'external/...' is
        # preferred over a relative path.
        is_external = False
        try:
            if path_obj.is_relative_to(external_root):
                new_file = f'external/{path_obj.relative_to(external_root)}'
                is_external = True
        except ValueError:
            pass

        if not is_external and relative_to:
            new_file = _relativize_path(command.file, relative_to)

    elif command.file.startswith(marker):
        path_suffix = command.file[len(marker) :]
        abs_path = output_base.joinpath('external', *path_suffix.split('/'))
        resolved_path = abs_path.resolve()

        try:
            external_root = (output_base / 'external').resolve()
            if resolved_path.is_relative_to(external_root):
                new_file = (
                    f'external/{resolved_path.relative_to(external_root)}'
                )
            elif relative_to:
                try:
                    new_file = str(resolved_path.relative_to(relative_to))
                except ValueError:
                    # Keep it as external/... if we can't relativize
                    new_file = f"external/{path_suffix}"
            else:
                new_file = str(resolved_path)
        except ValueError:
            # If resolution fails weirdly, fallback
            new_file = str(resolved_path)
    elif relative_to and os.path.isabs(command.file):
        new_file = _relativize_path(command.file, relative_to)

    return command._replace(arguments=new_args, file=new_file)


def filter_unsupported_march_args(command: CompileCommand) -> CompileCommand:
    """Removes -march arguments if the arch is not supported by clangd."""
    new_args = []
    for arg in command.arguments:
        if arg.startswith("-march="):
            arch = arg.split("=", 1)[1]
            if arch in SUPPORTED_MARCH_ARCHITECTURES:
                new_args.append(arg)
        else:
            new_args.append(arg)
    return command._replace(arguments=new_args)


def resolve_target_info_paths(
    info: dict,
    bazel_output_path: Path,
    output_base_path: Path,
    relative_to: Path | None = None,
) -> dict:
    """Resolves paths in a target info dictionary."""
    resolved = info.copy()

    def resolve_list(paths_list):
        new_list = []
        for p in paths_list:
            # Re-use the existing logic by creating a placeholder CompileCommand
            # This is a bit hacky but avoids duplicating logic
            cmd = CompileCommand(file=p, directory="", arguments=[])
            cmd = resolve_external_paths(
                cmd, output_base_path, relative_to=relative_to
            )
            cmd = resolve_bazel_out_paths(
                cmd, bazel_output_path, relative_to=relative_to
            )
            new_list.append(cmd.file)
        return new_list

    resolved["srcs"] = resolve_list(info.get("srcs", []))
    resolved["hdrs"] = resolve_list(info.get("hdrs", []))
    return resolved


if __name__ == "__main__":
    sys.exit(main())
