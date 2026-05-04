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
"""Argument parsing and execution for presubmit v2."""

import argparse
import sys
import logging
import os
from pathlib import Path
from typing import Collection, Mapping, Iterable, Pattern

from pw_cli import collect_files
import pw_cli.log
from pw_cli.file_filter import FileFilter
from pw_presubmit import git_repo
from pw_presubmit.private import orchestrator
from pw_presubmit.private import events
from pw_presubmit.private.step import Step

_LOG = logging.getLogger("pw_presubmit")


def _add_loglevel_argument(parser: argparse.ArgumentParser) -> None:
    """Adds the --loglevel argument to a parser."""
    parser.add_argument(
        '--loglevel',
        default='INFO',
        choices=['DEBUG', 'INFO', 'WARNING', 'ERROR', 'CRITICAL'],
        help='Set the logging level (default: INFO)',
    )


def add_arguments(
    parser: argparse.ArgumentParser,
    programs: Mapping[str, Collection[Step]],
    default_program: str | None = None,
) -> None:
    """Adds arguments for presubmit v2."""
    group = parser.add_mutually_exclusive_group(required=not default_program)
    group.add_argument(
        '-p',
        '--program',
        choices=list(programs.keys()),
        help='Which presubmit program to run.',
    )

    all_steps = {}
    for prog_steps in programs.values():
        for s in prog_steps:
            all_steps[s.name] = s

    def resolve_step(name: str) -> Step:
        if name not in all_steps:
            raise argparse.ArgumentTypeError(
                f"Unknown step: {name}. Please check the presubmit program "
                f"definition for valid step names."
            )
        return all_steps[name]

    group.add_argument(
        '--step',
        dest='steps',
        action='append',
        type=resolve_step,
        help='Specific presubmit steps to run.',
    )
    parser.add_argument(
        '--output-dir',
        type=Path,
        help='Directory where output files will be written.',
    )
    parser.add_argument(
        '--mode',
        type=orchestrator.Mode,
        choices=list(orchestrator.Mode),
        default=orchestrator.Mode.STOP,
        help='How to run the presubmit',
    )
    parser.add_argument(
        '--ui',
        choices=['boxes', 'minimal'],
        default='boxes',
        help='Select the UI style (default: boxes)',
    )

    _add_loglevel_argument(parser)
    collect_files.add_git_file_arguments(parser)


def _main(
    program: str,
    steps: Collection[Step],
    mode: orchestrator.Mode,
    output_dir: Path | None,
    base: str,
    paths: list[str],
    file_filter: FileFilter,
    ui: str,
) -> int:
    """Runs a presubmit program with typed arguments."""
    if mode is orchestrator.Mode.AUTO:
        success = orchestrator.auto(
            program,
            steps,
            git_repo.find_git_repo(Path.cwd()).root(),
            events.PresubmitEvents.get(ui),
            base,
            pathspecs=paths,
            output_dir=output_dir,
            file_filter=file_filter,
        )
        return 0 if success else 1

    result = orchestrator.run(
        program,
        steps,
        git_repo.find_git_repo(Path.cwd()).root(),
        events.PresubmitEvents.get(ui),
        base,
        pathspecs=paths,
        output_dir=output_dir,
        file_filter=file_filter,
        mode=mode,
    )

    return 0 if result.success else 1


def _parse_args(
    programs: Mapping[str, Collection[Step]],
    default_program: str | None = None,
    argv: list[str] | None = None,
) -> argparse.Namespace:
    """Parses arguments for presubmit v2 and returns the processed namespace."""
    parser = argparse.ArgumentParser(description='Run presubmit checks.')
    add_arguments(parser, programs, default_program)
    args = parser.parse_args(argv)

    pw_cli.log.install(level=getattr(logging, args.loglevel.upper()))

    # Ensure program and steps are set as needed.
    if args.program:
        args.steps = programs[args.program]
    elif args.steps:
        args.program = 'custom'
    else:
        assert default_program is not None
        args.program = default_program
        args.steps = programs[default_program]

    del args.loglevel  # Clean up attributes not needed by _main
    return args


def _parse_resume_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Resume presubmit.')
    parser.add_argument('--resume', type=Path, required=True)
    _add_loglevel_argument(parser)
    return parser.parse_args(argv)


def _resume(
    resume_file: Path,
    programs: Mapping[str, Collection[Step]],
    exclude: Iterable[Pattern[str] | str],
) -> int:
    """Invokes orchestrator.resume with all steps."""
    all_steps = {}
    for prog_steps in programs.values():
        for s in prog_steps:
            all_steps[s.name] = s

    try:
        success = orchestrator.resume(
            all_steps=all_steps,
            resume_file=resume_file,
            file_filter=FileFilter(exclude=exclude),
        )
        return 0 if success else 1
    except orchestrator.AutoModeError as e:
        _LOG.error('%s', e)
        return 1


def main(
    programs: Mapping[str, Collection[Step]],
    default_program: str | None = None,
    *,
    exclude: Iterable[Pattern[str] | str] = (),
    argv: list[str] | None = None,
) -> int:
    """Main entry point for presubmit v2.

    This function handles the command-line interface for running presubmit
    checks using the v2 infrastructure. It parses arguments, configures
    logging, and invokes the `Orchestrator` to run the checks.

    If running under Bazel (detected by the 'BUILD_WORKING_DIRECTORY'
    environment variable), this function changes the working directory to the
    original directory where Bazel was invoked before running checks.

    Args:
        programs: A mapping from program names to the collection of `Step`
          objects that belong to that program.
        default_program: The name of the program to run if neither a specific
          program nor specific steps are requested by the user.
        exclude: Patterns (strings or compiled regexes) matching files that
          should be excluded from all presubmit checks.
        argv: Command-line arguments to parse. Defaults to `sys.argv[1:]`.

    Returns:
        0 if all checks passed or were successfully fixed (in auto mode),
        or 1 if any check failed or was skipped due to an earlier failure.
    """
    # Change to working directory if running from Bazel.
    if 'BUILD_WORKING_DIRECTORY' in os.environ:
        os.chdir(os.environ['BUILD_WORKING_DIRECTORY'])

    actual_argv = sys.argv[1:] if argv is None else argv
    try:
        if '--resume' in actual_argv:
            args = _parse_resume_args(actual_argv)
            pw_cli.log.install(level=getattr(logging, args.loglevel.upper()))
            return _resume(args.resume, programs, exclude)

        return _main(
            **vars(_parse_args(programs, default_program, argv)),
            file_filter=FileFilter(exclude=exclude),
        )
    except orchestrator.AutoModeError as e:
        _LOG.error('%s', e)
        return 1
