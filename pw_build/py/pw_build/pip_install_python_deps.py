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
"""Pip install Pigweed Python packages."""

import argparse
import logging
from pathlib import Path
import subprocess
import shlex
import sys

try:
    from pw_build.python_package import load_packages
except (ImportError, OSError):
    # Load from python_package from this directory if pw_build is not available.
    from python_package import load_packages  # type: ignore

_LOG = logging.getLogger('pw_build.pip_install_python_deps')


def _parse_args() -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--python-dep-list-files',
        type=Path,
        required=True,
        help=(
            'Path to a text file containing the list of Python package '
            'metadata json files.'
        ),
    )
    parser.add_argument(
        '--gn-packages',
        required=True,
        help=(
            'Comma separated list of GN python package ' 'targets to install.'
        ),
    )
    parser.add_argument(
        '--editable-pip-install',
        action='store_true',
        help=(
            'If true run the pip install command with the '
            '\'--editable\' option.'
        ),
    )
    return parser.parse_known_args()


class NoMatchingGnPythonDependency(Exception):
    """An error occurred while processing a Python dependency."""


def main(
    python_dep_list_files: Path,
    editable_pip_install: bool,
    gn_targets: list[str],
    pip_args: list[str],
) -> int:
    """Find matching python packages to pip install.

    Raises:
      NoMatchingGnPythonDependency: if a given gn_target is missing.
      FileNotFoundError: if a Python wheel was not found when using pip install
          with --require-hashes.
    """
    pip_target_dirs: list[Path] = []

    py_packages = load_packages([python_dep_list_files], ignore_missing=True)
    for pkg in py_packages:
        valid_target = [target in pkg.gn_target_name for target in gn_targets]
        if not any(valid_target):
            continue
        top_level_source_dir = pkg.package_dir
        pip_target_dirs.append(top_level_source_dir.parent)

    if not pip_target_dirs:
        raise NoMatchingGnPythonDependency(
            'No matching GN Python dependency found to install.\n'
            'GN Targets to pip install:\n' + '\n'.join(gn_targets) + '\n\n'
            'Declared Python Dependencies:\n'
            + '\n'.join(pkg.gn_target_name for pkg in py_packages)
            + '\n\n'
        )

    for target in pip_target_dirs:
        command_args = [sys.executable, "-m", "pip"]
        command_args += pip_args
        if editable_pip_install:
            command_args.append('--editable')

        if '--require-hashes' in pip_args:
            build_wheel_path = target.with_suffix('._build_wheel')
            req_file = build_wheel_path / 'requirements.txt'
            req_file_str = str(req_file.resolve())
            if not req_file.exists():
                raise FileNotFoundError(
                    'Missing Python wheel requirement file: ' + req_file_str
                )
            # Install the wheel requirement file
            command_args.extend(['--requirement', req_file_str])
            # Add the wheel dir to --find-links
            command_args.extend(
                ['--find-links', str(build_wheel_path.resolve())]
            )
            # Switch any constraint files to requirements. Hashes seem to be
            # ignored in constraint files.
            command_args = list(
                '--requirement' if arg == '--constraint' else arg
                for arg in command_args
            )

        else:
            # Pass the target along to pip with no modifications.
            command_args.append(str(target.resolve()))

        quoted_command_args = ' '.join(shlex.quote(arg) for arg in command_args)
        _LOG.info('Run ==> %s', quoted_command_args)

        process = subprocess.run(
            command_args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
        pip_output = process.stdout.decode()
        if process.returncode != 0:
            print(pip_output)
            return process.returncode
    return 0


if __name__ == '__main__':
    logging.basicConfig(format='%(message)s', level=logging.DEBUG)

    # Parse this script's args and pass any remaining args to pip.
    argparse_args, remaining_args_for_pip = _parse_args()

    # Split the comma separated string and remove leading slashes.
    gn_target_names = [
        target.lstrip('/')
        for target in argparse_args.gn_packages.split(',')
        if target  # The last target may be an empty string.
    ]

    result = main(
        python_dep_list_files=argparse_args.python_dep_list_files,
        editable_pip_install=argparse_args.editable_pip_install,
        gn_targets=gn_target_names,
        pip_args=remaining_args_for_pip,
    )
    sys.exit(result)
