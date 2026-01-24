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
from typing import NamedTuple

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


def _collect_fragments_via_glob(bazel_output_path: Path) -> Iterator[Path]:
    """Collects fragments by globbing the output directory."""
    _LOG.info("Searching for existing compile command fragments...")
    yield from bazel_output_path.rglob(f"*{_FRAGMENT_SUFFIX}")


def _collect_fragments(
    bazel_output_path: Path,
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
    else:
        yield from _collect_fragments_via_glob(bazel_output_path)


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
) -> list[dict] | None:
    """Load compile commands fragments for a single platform.

    Ingests each compile commands fragment JSON file, and merges the compile
    commands into a single database.

    Args:
        fragments: A list of paths to fragment files.
        platform: The name of the platform being processed, for logging.

    Returns:
        A list of compile commands objects as produced by the compile commands
        aspect. Returns None if conflicting entries are identified.
    """
    commands_by_file: dict[tuple[str, str], dict] = {}
    for fragment_path in fragments:
        with open(fragment_path, "r") as f:
            try:
                fragment_commands = json.load(f)
            except json.JSONDecodeError:
                _LOG.error("Could not parse %s, skipping.", fragment_path)
                continue

            for command_dict in fragment_commands:
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

    return list(commands_by_file.values())


def main() -> int:
    """Script entry point."""
    args = _parse_args()

    if args.verbose:
        log_level = logging.DEBUG
    else:
        log_level = logging.WARNING

    _setup_logging(log_level)

    workspace_root = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not workspace_root:
        _LOG.error("This script must be run with 'bazel run'.")
        return 1

    bazel_output_path = _get_bazel_path_info('output_path', workspace_root)
    if bazel_output_path is None:
        return 1

    # This is ABOVE output_path.
    output_base_path = _get_bazel_path_info('output_base', workspace_root)
    if output_base_path is None:
        return 1

    execution_root_path = _get_bazel_path_info('execution_root', workspace_root)
    if execution_root_path is None:
        return 1

    if not bazel_output_path.exists():
        _LOG.fatal("Bazel output directory '%s' not found.", bazel_output_path)
        _LOG.fatal(
            "Did you run 'bazel build --config=ide //your/target' first?"
        )
        return 1

    # Search for fragments with our unique suffix.
    all_fragments = list(
        _collect_fragments(
            bazel_output_path,
            execution_root_path,
            args.bazel_args,
            args.verbose,
            args.compile_command_groups,
        )
    )

    if not all_fragments:
        _LOG.error("Could not find any generated fragments.")
        return 1

    # Group fragments by platform using a more robust parsing method.
    fragments_by_platform = collections.defaultdict(list)
    for fragment in all_fragments:
        base_name = fragment.name.removesuffix(_FRAGMENT_SUFFIX)
        platform = base_name.split(".")[-1]
        fragments_by_platform[platform].append(fragment)

    _LOG.debug(
        "Found fragments for %s.",
        plural.plural(fragments_by_platform, 'platform'),
    )

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

    for platform, fragments in fragments_by_platform.items():
        _LOG.debug("Processing platform: %s...", platform)
        all_commands = _load_commands_for_platform(fragments, platform)
        if all_commands is None:
            return 1

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
            resolved_cmd = resolve_bazel_out_paths(cmd, bazel_output_path)
            resolved_cmd = resolve_external_paths(
                resolved_cmd, output_base_path
            )
            resolved_cmd = filter_unsupported_march_args(resolved_cmd)
            processed_commands.append(resolved_cmd._asdict())

        with open(merged_json_path, "w") as f:
            json.dump(processed_commands, f, indent=2)

        with open(merged_json_path, "r") as f:
            content = f.read()
        content = content.replace("__WORKSPACE_ROOT__", workspace_root)
        with open(merged_json_path, "w") as f:
            f.write(content)

    _LOG.info(
        "✅ Successfully created compilation databases in: %s", output_dir
    )
    (output_dir / 'pw_lastGenerationTime.txt').write_text(str(int(time.time())))
    return 0


def resolve_bazel_out_paths(
    command: CompileCommand, bazel_output_path: Path
) -> CompileCommand:
    """Replaces bazel-out paths with their real paths."""
    marker = 'bazel-out/'
    new_args = []

    for arg in command.arguments:
        if marker in arg:
            parts = arg.split(marker, 1)
            prefix = parts[0]
            suffix = parts[1]
            new_path = bazel_output_path.joinpath(*suffix.split('/'))
            new_arg = prefix + str(new_path)
            new_args.append(new_arg)
        else:
            new_args.append(arg)

    new_file = command.file
    if command.file.startswith(marker):
        path_suffix = command.file[len(marker) :]
        new_file = str(bazel_output_path.joinpath(*path_suffix.split('/')))

    return command._replace(arguments=new_args, file=new_file)


def resolve_external_paths(
    command: CompileCommand, output_base: Path
) -> CompileCommand:
    """Replaces external/ paths with their real paths."""
    # For now, don't support --experimental_sibling_repository_layout.
    marker = 'external/'
    new_args = []

    # The `external/` suffix is way more risky to replace every instance of
    # since it breaks if you do `--foo=my_lib/bar_external/`, so only replace
    # if the argument starts with the string (and a well-known prefix).
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
        for prefix in allowed_prefixes:
            if not arg.startswith(prefix + marker) and not arg.startswith(
                prefix + '=' + marker
            ):
                continue
            new_arg = arg.replace(
                marker,
                str(output_base / 'external') + '/',
                1,
            )
        new_args.append(new_arg)

    new_file = command.file
    if command.file.startswith(marker):
        path_suffix = command.file[len(marker) :]
        new_file = str(
            output_base.joinpath('external', *path_suffix.split('/'))
        )
        # Verify if it is a symlink and resolve it.
        # This fixes issues where local repositories are symlinked in the
        # bazel cache, causing editors to open the cached version instead
        # of the source version.
        new_file = str(Path(new_file).resolve())

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


if __name__ == "__main__":
    sys.exit(main())
