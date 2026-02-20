# Copyright 2022 The Pigweed Authors
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
"""Pigweed Project Builder Common argparse."""

import argparse
from pathlib import Path


def add_project_builder_arguments(
    parser: argparse.ArgumentParser,
) -> argparse.ArgumentParser:
    """Add ProjectBuilder.main specific arguments."""
    build_dir_group = parser.add_argument_group(
        title='Build Directory and Command Options'
    )

    build_dir_group.add_argument(
        '-C',
        '--build-directory',
        dest='build_directories',
        nargs='+',
        action='append',
        default=[],
        metavar=('directory', 'target'),
        help=(
            "Specify a build directory and optionally targets to "
            "build. `pw watch -C out target1 target2` is equivalent to 'ninja "
            "-C out taret1 target2'. The 'out' directory will be used if no "
            "others are provided."
        ),
    )

    build_dir_group.add_argument(
        'default_build_targets',
        nargs='*',
        metavar='target',
        default=[],
        help=(
            "Default build targets. For example if the build directory is "
            "'out' then, 'ninja -C out taret1 target2' will be run. To "
            "specify one or more directories, use the "
            "``-C / --build-directory`` option."
        ),
    )

    build_dir_group.add_argument(
        '--build-system-command',
        nargs=2,
        action='append',
        default=[],
        dest='build_system_commands',
        metavar=('directory', 'command'),
        help='Build system command for . Default: ninja',
    )

    build_dir_group.add_argument(
        '--run-command',
        action='append',
        default=[],
        help=(
            'Additional commands to run. These are run before any -C '
            'arguments and may be repeated. For example: '
            "--run-command 'bazel build //pw_cli/...' "
            "--run-command 'bazel test //pw_cli/...' "
            "-C out python.lint python.test"
        ),
    )
    build_dir_group.add_argument(
        '-S',
        '--source-path',
        type=Path,
        help=(
            'Path to the root of the source files. Defaults to the '
            'current working directory. If running under bazel this '
            'will be set to the $BUILD_WORKSPACE_DIRECTORY '
            'environment variable.'
        ),
    )
    build_dir_group.add_argument(
        '--default-build-system',
        choices=['ninja', 'bazel'],
        help=(
            'Build system to use when no build directories or build targets '
            'are specified on the command line.'
        ),
    )

    return add_project_builder_option_arguments(parser)


def add_project_builder_option_arguments(
    parser: argparse.ArgumentParser,
) -> argparse.ArgumentParser:
    """Add ProjectBuilder optional arguments."""
    build_options_group = parser.add_argument_group(
        title='Build Execution Options'
    )
    build_options_group.add_argument(
        '-j',
        '--jobs',
        type=int,
        help=(
            'Specify the number of cores to use for each build system. '
            'This is passed to ninja, bazel and make as "-j"'
        ),
    )
    build_options_group.add_argument(
        '-k',
        '--keep-going',
        action='store_true',
        help=(
            'Keep building past the first failure. This is equivalent to '
            'running "ninja -k 0" or "bazel build -k".'
        ),
    )
    build_options_group.add_argument(
        '--dry-run',
        action='store_true',
        help='Print build commands without running them.',
    )
    build_options_group.add_argument(
        '--parallel',
        action='store_true',
        help='Run all builds in parallel.',
    )
    build_options_group.add_argument(
        '--parallel-workers',
        default=0,
        type=int,
        help=(
            'How many builds may run at the same time when --parallel is '
            'enabled. Default: 0 meaning run all in parallel.'
        ),
    )

    logfile_group = parser.add_argument_group(title='Log File Options')
    logfile_group.add_argument(
        '--logfile',
        type=Path,
        help='Global build output log file.',
    )

    logfile_group.add_argument(
        '--separate-logfiles',
        action='store_true',
        help='Create separate log files per build directory.',
    )

    logfile_group.add_argument(
        '--debug-logging',
        action='store_true',
        help='Enable Python build execution tool debug logging.',
    )

    output_group = parser.add_argument_group(title='Display Output Options')

    output_group.add_argument(
        '--banners',
        action=argparse.BooleanOptionalAction,
        default=True,
        help='Show pass/fail banners.',
    )

    output_group.add_argument(
        '--colors',
        action=argparse.BooleanOptionalAction,
        default=True,
        help='Force color output from ninja.',
    )

    return parser
