#!/usr/bin/env python3
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
"""Launches a pw_target_runner client for STM32 Nucleo-F103RB tests."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

try:
    from stm32_nucleo_f103_utils import unit_test_server
except ImportError:
    import unit_test_server  # type: ignore[import-not-found,no-redef]

SERVER_RUNNER_CMD = (
    'bazelisk run @pigweed//targets/stm32_nucleo_f103/py:unit_test_server'
)

try:
    from python.runfiles import runfiles  # type: ignore[import-not-found]
except ImportError:
    runfiles = None  # type: ignore[assignment]


def _resolve_target_client_command() -> str:
    if runfiles is not None:
        rf = runfiles.Create()
        if rf is not None:
            resolved = rf.Rlocation(
                'pigweed/pw_target_runner/go/cmd/client_/client'
            )
            if resolved and Path(resolved).is_file():
                return resolved
    return 'pw_target_runner_client'


def parse_args() -> argparse.Namespace:
    """Parses command-line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', help='The target test binary to run')
    parser.add_argument(
        '--server-port',
        type=int,
        default=unit_test_server.DEFAULT_PORT,
        help='Port the test server is listening on',
    )
    return parser.parse_args()


def main() -> int:
    """Sends a test execution request to the unit_test_server."""
    args = parse_args()
    client_cmd = _resolve_target_client_command()
    cmd = [
        client_cmd,
        '-binary',
        args.binary,
        '-port',
        str(args.server_port),
    ]
    # The prebuilt CIPD pw_target_runner_client binary predates the
    # -server_suggestion flag, and SERVER_RUNNER_CMD is Bazel-specific.
    if client_cmd != 'pw_target_runner_client':
        cmd.extend(['-server_suggestion', SERVER_RUNNER_CMD])
    return subprocess.call(cmd)


if __name__ == '__main__':
    sys.exit(main())
