#!/usr/bin/env python3
# Copyright 2019 The Pigweed Authors
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
"""Launch a pw_target_runner client that sends a test request."""

import argparse
import subprocess
import sys

SERVER_RUNNER_CMD = (
    'bazelisk run @pigweed//targets/stm32f429i_disc1/py:unit_test_server'
)

try:
    from stm32f429i_disc1_utils import unit_test_server
except ImportError:
    import unit_test_server  # type: ignore

# If the script is being run through Bazel, our client is provided at a well
# known location in its runfiles.
try:
    from python.runfiles import runfiles  # type: ignore

    r = runfiles.Create()
    _TARGET_CLIENT_COMMAND = r.Rlocation(
        'pigweed/pw_target_runner/go/cmd/client_/client'
    )
except ImportError:
    _TARGET_CLIENT_COMMAND = 'pw_target_runner_client'


def parse_args() -> argparse.Namespace:
    """Parses command-line arguments."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', help='The target test binary to run')
    parser.add_argument(
        '--server-port',
        type=int,
        default=unit_test_server.DEFAULT_PORT,
        help='Port the test server is located on',
    )
    parser.add_argument(
        '--use-rpc',
        action=argparse.BooleanOptionalAction,
        default=True,
        help='Use the RPC interface to trigger tests on-device or gather'
        'results. If disabled, results are parsed directly from UART.',
    )

    return parser.parse_args()


def launch_client(binary: str, server_port: int, use_rpc: bool) -> int:
    """Sends a test request to the specified server port."""
    cmd = [
        _TARGET_CLIENT_COMMAND,
        '-binary',
        binary,
        '-port',
        str(server_port),
        '-server_suggestion',
        SERVER_RUNNER_CMD,
    ]
    if not use_rpc:
        cmd.extend(['-runner_arg', '--no-use-rpc'])

    return subprocess.call(cmd)


def main() -> int:
    """Launch a test by sending a request to a pw_target_runner_server."""
    args = parse_args()
    return launch_client(args.binary, args.server_port, args.use_rpc)


if __name__ == '__main__':
    sys.exit(main())
