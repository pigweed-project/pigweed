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
"""Default pw build script for upstream Pigweed.

Usage:

   pw build --list
   pw build --recipe default_gn
   pw build --recipe default_* --watch
   pw build --step gn_combined_build_check --step gn_python_*
   pw build -C out/gn --watch
"""

import argparse
import logging
from pathlib import Path
import shutil
import sys

import pw_cli.env
import pw_presubmit.pigweed_presubmit
from pw_presubmit.build import gn_args

from pw_build.build_recipe import (
    BuildCommand,
    BuildRecipe,
    should_gn_gen,
    should_regenerate_cmake,
)
from pw_build.project_builder_presubmit_runner import (
    get_parser,
    main,
)


_LOG = logging.getLogger('pw_build')


def gn_recipe() -> BuildRecipe:
    """Return the default_gn recipe."""
    default_gn_gen_command = [
        'gn',
        'gen',
        # NOTE: Not an f-string. BuildRecipe will replace with the out dir.
        '{build_dir}',
        '--export-compile-commands',
    ]

    if shutil.which('ccache'):
        default_gn_gen_command.append(gn_args(pw_command_launcher='ccache'))

    return BuildRecipe(
        build_dir=Path('out/gn'),
        title='default_gn',
        steps=[
            BuildCommand(
                run_if=should_gn_gen,
                command=default_gn_gen_command,
            ),
            BuildCommand(
                build_system_command='ninja',
                targets=['default'],
            ),
        ],
    )


def bazel_recipe() -> BuildRecipe:
    """Return the default_bazel recipe."""
    default_bazel_targets = ['//...']

    return BuildRecipe(
        build_dir=Path('out/bazel'),
        title='default_bazel',
        steps=[
            BuildCommand(
                build_system_command='bazel',
                build_system_extra_args=[
                    'build',
                    '--verbose_failures',
                    '--worker_verbose',
                ],
                targets=default_bazel_targets,
            ),
            BuildCommand(
                build_system_command='bazel',
                build_system_extra_args=[
                    'test',
                    '--test_output=errors',
                ],
                targets=default_bazel_targets,
            ),
        ],
    )


def cmake_recipe(repo_root: Path, package_root: Path) -> BuildRecipe:
    """Construct the default_cmake recipe."""
    toolchain_path = (
        repo_root / 'pw_toolchain' / 'host_clang' / 'toolchain.cmake'
    )

    cmake_generate_command = [
        'cmake',
        '--fresh',
        '--debug-output',
        '-DCMAKE_MESSAGE_LOG_LEVEL=WARNING',
        '-S',
        str(repo_root),
        '-B',
        # NOTE: Not an f-string. BuildRecipe will replace with the out dir.
        '{build_dir}',
        '-G',
        'Ninja',
        f'-DCMAKE_TOOLCHAIN_FILE={toolchain_path}',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=1',
        f'-Ddir_pw_third_party_nanopb={package_root / "nanopb"}',
        '-Dpw_third_party_nanopb_ADD_SUBDIRECTORY=ON',
        f'-Ddir_pw_third_party_emboss={package_root / "emboss"}',
    ]

    if shutil.which('ccache'):
        cmake_generate_command.append("-DCMAKE_C_COMPILER_LAUNCHER=ccache")
        cmake_generate_command.append("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")

    pw_package_install_steps = [
        BuildCommand(
            command=['pw', '--no-banner', 'package', 'install', package],
        )
        for package in ['emboss', 'nanopb']
    ]

    return BuildRecipe(
        build_dir=Path('out/cmake'),
        title='default_cmake',
        steps=pw_package_install_steps
        + [
            BuildCommand(
                run_if=should_regenerate_cmake(cmake_generate_command),
                command=cmake_generate_command,
            ),
            BuildCommand(
                build_system_command='ninja',
                targets=pw_presubmit.pigweed_presubmit.CMAKE_TARGETS,
            ),
        ],
    )


def _get_repo_and_package_root() -> tuple[Path, Path]:
    pw_env = pw_cli.env.pigweed_environment()
    repo_root = pw_cli.env.project_root()
    package_root = pw_env.PW_PACKAGE_ROOT
    if not package_root:
        package_root = repo_root / 'environment/packages'
    return (repo_root, package_root)


def pigweed_upstream_main() -> int:
    """Entry point for Pigweed upstream ``pw build`` command.

    Defines one or more BuildRecipes and passes that along with all of Pigweed
    upstream presubmit programs to the project_builder_presubmit_runner.main to
    start a pw build invocation.

    Returns:
      An int representing the success or failure status of the build; 0 if
      successful, 1 if failed.

    Command line usage examples:

    .. code-block:: bash

       pw build --list
       pw build --recipe default_gn
       pw build --recipe default_* --watch
       pw build --step gn_combined_build_check --step gn_python_*
       pw build -C out/gn --watch
    """
    repo_root, package_root = _get_repo_and_package_root()

    return main(
        presubmit_programs=pw_presubmit.pigweed_presubmit.PROGRAMS,
        build_recipes=[
            gn_recipe(),
            bazel_recipe(),
            cmake_recipe(repo_root, package_root),
        ],
        default_root_logfile=Path('out/build.txt'),
    )


def _build_argument_parser() -> argparse.ArgumentParser:
    return get_parser(
        pw_presubmit.pigweed_presubmit.PROGRAMS,
        [
            gn_recipe(),
            bazel_recipe(),
            # Empty cmake recipe for argparse validation by display_name.
            # This is needed because project_root can only be determined with
            # bazel run and the sphinx docs build runs this function for
            # argparse documentation.
            BuildRecipe(
                build_dir=Path('out/cmake'),
                title='default_cmake',
                steps=[],
            ),
        ],
    )


if __name__ == '__main__':
    sys.exit(pigweed_upstream_main())
