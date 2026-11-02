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
"""Install and check status of the Raspberry Pi Pico SDK."""

import contextlib
import logging
from pathlib import Path
from typing import Sequence
import subprocess

import pw_package.git_repo
import pw_package.package_manager

_LOG = logging.getLogger(__package__)


class PiPicoSdk(pw_package.package_manager.Package):
    """Install and check status of the Raspberry Pi Pico SDK."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, name='pico_sdk', **kwargs)
        self._pico_sdk = pw_package.git_repo.GitRepo(
            name='pico_sdk',
            url=(
                'https://pigweed.googlesource.com/'
                'third_party/github/raspberrypi/pico-sdk'
            ),
            commit='6a7db34ff63345a7badec79ebea3aaef1712f374',
        )

    def install(self, path: Path) -> None:
        self._pico_sdk.install(path)

        # Run submodule update --init to fetch tinyusb.
        with contextlib.chdir(path):
            command = ['git', 'submodule', 'update', '--init']
            _LOG.info('==> %s', ' '.join(command))
            subprocess.run(command)

    def info(self, path: Path) -> Sequence[str]:
        return (
            f'{self.name} installed in: {path}',
            "Enable by running 'gn args out' and adding this line:",
            f'  PICO_SRC_DIR = "{path}"',
        )


pw_package.package_manager.register(PiPicoSdk)
