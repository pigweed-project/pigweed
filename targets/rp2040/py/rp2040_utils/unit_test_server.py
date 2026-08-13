#!/usr/bin/env python3
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
"""Launch a pw_target_runner server to use for multi-device testing."""

import argparse
import logging
import sys
import tempfile
from typing import IO

import pw_cli.process
import pw_cli.log

try:
    from rp2040_utils import device_detector
except ImportError:
    # Load from this directory if rp2040_utils is not available.
    import device_detector  # type: ignore

_LOG = logging.getLogger('unit_test_server')

DEFAULT_PORT = 34172

# If the script is being run through Bazel, our runner and server are provided
# at well known locations in the runfiles.
try:
    from python.runfiles import runfiles  # type: ignore

    r = runfiles.Create()
    assert r is not None
    resolved_runner = r.Rlocation(
        'pigweed/targets/rp2040/py/rpc_unit_test_runner'
    )
    assert resolved_runner is not None
    _TEST_RUNNER_COMMAND = resolved_runner
    resolved_server = r.Rlocation(
        'pigweed/pw_target_runner/go/cmd/server_/server'
    )
    assert resolved_server is not None
    _TEST_SERVER_COMMAND = resolved_server
except ImportError:
    _TEST_RUNNER_COMMAND = 'rp2040_unit_test_runner'
    _TEST_SERVER_COMMAND = 'pw_target_runner_server'


def parse_args():
    """Parses command-line arguments."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--server-port',
        type=int,
        default=DEFAULT_PORT,
        help='Port to launch the pw_target_runner_server on',
    )
    parser.add_argument(
        '--server-config',
        type=argparse.FileType('r'),
        help='Path to server config file',
    )
    parser.add_argument(
        '--debug-probe-only',
        action='store_true',
        help='Only run tests on detected Pi Pico debug probes',
    )
    parser.add_argument(
        '--pico-only',
        action='store_true',
        help='Only run tests on detected Pi Pico boards (NOT debug probes)',
    )
    parser.add_argument(
        '--verbose',
        '-v',
        dest='verbose',
        action='store_true',
        help='Output additional logs as the script runs',
    )
    parser.add_argument(
        '--chip',
        dest='chip',
        type=str,
        required=True,
        choices=[
            'RP2040',
            'RP2350',
        ],
        help='RP2 chip connected to a debug probe (RP2040 or RP2350)',
    )

    parser.add_argument(
        '--no-rpc-logging',
        action='store_true',
        help='Disable RPC logging in runners',
    )

    return parser.parse_args()


def generate_runner(command: str, arguments: list[str]) -> str:
    """Generates a text-proto style pw_target_runner_server configuration."""
    # TODO(amontanez): Use a real proto library to generate this when we have
    # one set up.
    for i, arg in enumerate(arguments):
        arguments[i] = f'  args: "{arg}"'
    runner = ['runner {', f'  command:"{command}"']
    runner.extend(arguments)
    runner.append('}\n')
    return '\n'.join(runner)


def generate_server_config(
    chip: str,
    include_picos: bool = True,
    include_debug_probes: bool = True,
    no_rpc_logging: bool = False,
) -> IO[bytes]:
    """Returns a temporary generated file for use as the server config."""
    boards = device_detector.detect_boards(
        include_picos=include_picos,
        include_debug_probes=include_debug_probes,
    )

    if not boards:
        _LOG.critical('No attached boards detected')
        sys.exit(1)

    if (
        len({'b' if b.is_debug_probe() else 'p': True for b in boards}.keys())
        > 1
    ):
        _LOG.critical(
            'Debug probes and picos both detected. Mixed device configurations '
            'are not supported. Please only connect Picos directly, or only '
            'connect debug probes! You can also use --pico-only or '
            '--debug-probe-only to filter attached devices.'
        )
        sys.exit(1)

    config_file = tempfile.NamedTemporaryFile()
    _LOG.debug('Generating test server config at %s', config_file.name)
    _LOG.debug('Found %d attached devices', len(boards))

    picotool_boards = [board for board in boards if not board.is_debug_probe()]
    if len(picotool_boards) > 1:
        # TODO: https://pwbug.dev/290245354 - Multi-device flashing doesn't work
        # due to limitations of picotool. Limit to one device even if multiple
        # are connected.
        _LOG.warning(
            'TODO: https://pwbug.dev/290245354 - Multiple non-debugprobe '
            ' boards attached. '
            'Disabling parallel testing.'
        )
        boards = boards[:1]

    for board in boards:
        test_runner_args = [
            '--usb-bus',
            str(board.bus),
            '--usb-port',
            str(board.port),
            '--chip',
            chip,
        ]
        if no_rpc_logging:
            test_runner_args.append('--no-rpc-logging')
        config_file.write(
            generate_runner(_TEST_RUNNER_COMMAND, test_runner_args).encode(
                'utf-8'
            )
        )
    config_file.flush()
    return config_file


def launch_server(
    server_config: IO[bytes] | None,
    server_port: int | None,
    chip: str,
    include_picos: bool,
    include_debug_probes: bool,
    no_rpc_logging: bool = False,
) -> int:
    """Launch a device test server with the provided arguments."""
    if server_config is None:
        # Auto-detect attached boards if no config is provided.
        server_config = generate_server_config(
            chip, include_picos, include_debug_probes, no_rpc_logging
        )

    cmd = [_TEST_SERVER_COMMAND, '-config', server_config.name]

    if server_port is not None:
        cmd.extend(['-port', str(server_port)])

    return pw_cli.process.run(*cmd, log_output=True).returncode


def main():
    """Launch a device test server with the provided arguments."""
    args = parse_args()

    log_level = logging.DEBUG if args.verbose else logging.INFO
    pw_cli.log.install(level=log_level)

    if args.pico_only and args.debug_probe_only:
        _LOG.critical('Cannot specify both --pico-only and --debug-probe-only')
        sys.exit(1)

    exit_code = launch_server(
        args.server_config,
        args.server_port,
        args.chip,
        not args.debug_probe_only,
        not args.pico_only,
        no_rpc_logging=args.no_rpc_logging,
    )
    sys.exit(exit_code)


if __name__ == '__main__':
    main()
