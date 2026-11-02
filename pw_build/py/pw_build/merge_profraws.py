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
"""Merge multiple profraws together using llvm-profdata."""

import argparse
import enum
import json
import logging
import subprocess
import sys
from pathlib import Path
from typing import Any

_LOG = logging.getLogger(__name__)


class FailureMode(enum.StrEnum):
    ANY = 'any'
    ALL = 'all'


def _parse_args() -> dict[str, Any]:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--llvm-profdata-path',
        type=Path,
        required=True,
        help='Path to the llvm-profdata binary to use for merging.',
    )
    parser.add_argument(
        '--test-metadata-path',
        type=Path,
        required=True,
        help='Path to the *.test_metadata.json file that describes all of the '
        'tests being used to generate a coverage report.',
    )
    parser.add_argument(
        '--profdata-path',
        type=Path,
        required=True,
        help='Path for the output merged profdata file to use with generating a'
        ' coverage report for the tests described in --test-metadata.',
    )
    parser.add_argument(
        '--depfile-path',
        type=Path,
        required=True,
        help='Path for the output depfile to convey the extra input '
        'requirements from parsing --test-metadata.',
    )
    parser.add_argument(
        '--failure-mode',
        type=FailureMode,
        choices=list(FailureMode),
        required=False,
        default=FailureMode.ANY,
        help='Sets the llvm-profdata --failure-mode option.',
    )
    return vars(parser.parse_args())


def merge_profraws(
    llvm_profdata_path: Path,
    test_metadata_path: Path,
    profdata_path: Path,
    depfile_path: Path,
    failure_mode: FailureMode,
) -> int:
    """Merge multiple profraws together using llvm-profdata."""

    # Open the test_metadata_path, parse it to JSON, and extract out the
    # profraws.
    test_metadata = json.loads(test_metadata_path.read_text())
    profraw_paths = [
        str(obj['path'])
        for obj in test_metadata
        if 'type' in obj and obj['type'] == 'profraw'
    ]

    # Generate merged profdata.
    command = [
        str(llvm_profdata_path),
        'merge',
        '--sparse',
        '--failure-mode',
        str(failure_mode),
        '-o',
        str(profdata_path),
    ] + profraw_paths

    _LOG.info('')
    _LOG.info(' '.join(command))
    _LOG.info('')

    output = subprocess.run(command)
    if output.returncode != 0:
        return output.returncode

    # Generate the depfile that describes the dependency on the profraws used to
    # create profdata_path.
    depfile_path.write_text(
        ''.join(
            [
                str(profdata_path),
                ': \\\n',
                *[str(path) + ' \\\n' for path in profraw_paths],
            ]
        )
    )

    return 0


def main() -> int:
    return merge_profraws(**_parse_args())


if __name__ == "__main__":
    sys.exit(main())
