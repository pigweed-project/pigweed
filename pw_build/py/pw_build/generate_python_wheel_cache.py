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
"""Download all Python packages required for a pw_python_venv."""

import argparse
import logging
from pathlib import Path
import shlex
import subprocess
import sys


_LOG = logging.getLogger('pw_build.generate_python_wheel_cache')


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--pip-download-log',
        type=Path,
        required=True,
        help='Path to the root gn build dir.',
    )
    parser.add_argument(
        '--wheel-dir',
        type=Path,
        required=True,
        help='Path to save wheel files.',
    )
    parser.add_argument(
        '--download-all-platforms',
        action='store_true',
        help='Download Python precompiled wheels for all platforms.',
    )
    parser.add_argument(
        '-r',
        '--requirement',
        type=Path,
        nargs='+',
        required=True,
        help='Requirements files',
    )
    return parser.parse_args()


def main(
    pip_download_log: Path,
    wheel_dir: Path,
    requirement: list[Path],
    download_all_platforms: bool = False,
) -> int:
    """Download all Python packages required for a pw_python_venv."""

    # Delete existing wheels from the out dir, there may be stale versions.
    # shutil.rmtree(wheel_dir, ignore_errors=True)
    wheel_dir.mkdir(parents=True, exist_ok=True)

    pip_download_args = [
        sys.executable,
        "-m",
        "pip",
        "--log",
        str(pip_download_log),
        "download",
        "--dest",
        str(wheel_dir),
    ]
    for req in requirement:
        pip_download_args.extend(["--requirement", str(req)])

    if not download_all_platforms:
        # Download for the current platform only.
        quoted_pip_download_args = ' '.join(
            shlex.quote(arg) for arg in pip_download_args
        )
        _LOG.info('Run ==> %s', quoted_pip_download_args)
        # Download packages
        subprocess.run(pip_download_args, check=True)
        return 0

    # DOCSTAG: [wheel-platform-args]
    # These platform args are derived from the cffi pypi package:
    #   https://pypi.org/project/cffi/#files
    # See also these pages on Python wheel filename format:
    #   https://peps.python.org/pep-0491/#file-name-convention
    # and Platform compatibility tags:
    #   https://packaging.python.org/en/latest/specifications/
    #      platform-compatibility-tags/
    platform_args = [
        '--platform=any',
        '--platform=macosx_10_9_universal2',
        '--platform=macosx_10_9_x86_64',
        '--platform=macosx_11_0_arm64',
        '--platform=manylinux2010_x86_64',
        '--platform=manylinux2014_aarch64',
        '--platform=manylinux2014_x86_64',
        '--platform=manylinux_2_17_aarch64',
        '--platform=manylinux_2_17_x86_64',
        '--platform=musllinux_1_1_x86_64',
        '--platform=win_amd64',
        # Note: These 32bit platforms are omitted
        # '--platform=manylinux2010_i686',
        # '--platform=manylinux2014_i686',
        # '--platform=manylinux_2_12_i686'
        # '--platform=musllinux_1_1_i686',
        # '--platform=win32',
    ]

    # Pigweed supports Python 3.11 and up.
    python_version_args = [
        [
            '--implementation=py3',
            '--abi=none',
        ],
        [
            '--implementation=cp',
            '--python-version=3.11',
            '--abi=cp311',
        ],
    ]
    # DOCSTAG: [wheel-platform-args]

    # --no-deps is required when downloading binary packages for different
    # platforms other than the current one. The requirements.txt files already
    # has the fully expanded deps list using pip-compile so this is not a
    # problem.
    pip_download_args.append('--no-deps')

    # Run pip download once for each Python version. Multiple platform args can
    # be added to the same command.
    for py_version_args in python_version_args:
        for platform_arg in platform_args:
            final_pip_download_args = list(pip_download_args)
            final_pip_download_args.append(platform_arg)
            final_pip_download_args.extend(py_version_args)
            quoted_pip_download_args = ' '.join(
                shlex.quote(arg) for arg in final_pip_download_args
            )
            _LOG.info('')
            _LOG.info('Fetching packages for:')
            _LOG.info(
                'Python %s and Platforms: %s', py_version_args, platform_args
            )
            _LOG.info('Run ==> %s', quoted_pip_download_args)

            # Download packages
            subprocess.run(final_pip_download_args, check=True)

    return 0


if __name__ == '__main__':
    logging.basicConfig(format='%(message)s', level=logging.DEBUG)
    sys.exit(main(**vars(_parse_args())))
