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
"""Install and check status of Zephyr."""
import importlib.resources
import json
import pathlib
import subprocess
import sys
import tempfile

from typing import Sequence

import pw_env_setup.virtualenv_setup

import pw_package.git_repo
import pw_package.package_manager

# BEGIN-ZEPHYR-ROLL-METADATA
# ROLL: Warning: this variable is automatically updated.
# ROLL: Last updated 2025-10-20.
# ROLL: By https://cr-buildbucket.appspot.com/build/8700500463417867425.
_ZEPHYR_COMMIT_SHA = '169fd6a4a98eafcee0212242b0ad5ee533bbb883'
# END-ZEPHYR-ROLL-METADATA


class Zephyr(pw_package.git_repo.GitRepo):
    """Install and check status of Zephyr."""

    def __init__(self, *args, **kwargs):
        super().__init__(
            *args,
            name='zephyr',
            url=(
                'https://pigweed.googlesource.com/third_party/'
                'github/zephyrproject-rtos/zephyr'
            ),
            commit=_ZEPHYR_COMMIT_SHA,
            **kwargs,
        )

    def info(self, path: pathlib.Path) -> Sequence[str]:
        return (
            f'{self.name} installed in: {path}',
            'Enable by running "gn args out" and adding this line:',
            f'  dir_pw_third_party_zephyr = "{path}"',
        )

    @staticmethod
    def __populate_download_cache_from_cipd(path: pathlib.Path) -> None:
        """Check for Zephyr SDK in cipd"""
        package_path = path.parent.resolve()
        core_cache_path = package_path / 'zephyr_sdk'
        core_cache_path.mkdir(parents=True, exist_ok=True)

        cipd_package_subpath = 'infra/3pp/tools/zephyr_sdk/${platform}'

        # Check if the zephyr_sdk cipd package is readable
        with tempfile.NamedTemporaryFile(
            prefix='cipd', delete=True
        ) as temp_json:
            temp_json_path = pathlib.Path(temp_json.name)
            cipd_acl_check_command = [
                'cipd',
                'acl-check',
                cipd_package_subpath,
                '-reader',
                '-json-output',
                str(temp_json_path),
            ]
            subprocess.run(cipd_acl_check_command, capture_output=True)

            # Return if no packages are readable.
            if not temp_json_path.is_file():
                raise RuntimeError(
                    'Failed to verify zephyr_sdk cipd package is readable.'
                )
            result_text = temp_json_path.read_text()
            result_dict = json.loads(result_text)
            if 'result' not in result_dict:
                raise RuntimeError(
                    'Failed to verify zephyr_sdk cipd package is readable.'
                )

        # Initialize cipd
        subprocess.check_call(
            [
                'cipd',
                'init',
                '-force',
                str(core_cache_path),
            ]
        )
        # Install the Zephyr SDK
        subprocess.check_call(
            [
                'cipd',
                'install',
                cipd_package_subpath,
                '-root',
                str(core_cache_path),
                '-force',
            ]
        )
        # Setup Zephyr SDK
        subprocess.check_call(
            [
                'cmake',
                '-P',
                str(core_cache_path / 'cmake' / 'zephyr_sdk_export.cmake'),
            ]
        )

    def install(self, path: pathlib.Path) -> None:
        super().install(path)

        self.__populate_download_cache_from_cipd(path)
        with importlib.resources.as_file(
            importlib.resources.files(pw_env_setup.virtualenv_setup).joinpath(
                'constraint.list'
            )
        ) as constraint:
            subprocess.check_call(
                [
                    sys.executable,
                    '-m',
                    'pip',
                    'install',
                    '-r',
                    f'{path}/scripts/requirements.txt',
                    '-c',
                    str(constraint),
                    # Pin zephyr numpy to a version with prebuilts available for
                    # Python: 3.11, 3.12, 3.13, 3.14
                    'numpy==2.3.5',
                ]
            )


pw_package.package_manager.register(Zephyr)
