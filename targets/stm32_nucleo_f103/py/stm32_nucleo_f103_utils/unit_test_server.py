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
"""Launches a pw_target_runner server for STM32 Nucleo-F103RB testing."""

from __future__ import annotations

import argparse
import logging
from pathlib import Path
import shutil
import sys
import tempfile

import pw_cli.log
import pw_cli.process

try:
    from stm32_nucleo_f103_utils import device_detector
except ImportError:
    import device_detector  # type: ignore[import-not-found,no-redef]

_LOG = logging.getLogger('stm32_nucleo_f103_unit_test_server')

DEFAULT_PORT = 34174

try:
    from python.runfiles import runfiles  # type: ignore[import-not-found]
except ImportError:
    runfiles = None  # type: ignore[assignment]


def _resolve_runfile(rlocation_path: str, fallback: str) -> str:
    if runfiles is not None:
        rf = runfiles.Create()
        if rf is not None:
            resolved = rf.Rlocation(rlocation_path)
            if resolved and Path(resolved).is_file():
                return resolved
    return fallback


def _escape_textproto(value: str) -> str:
    return (
        value.replace('\\', '\\\\')
        .replace('"', '\\"')
        .replace('\n', '\\n')
        .replace('\r', '\\r')
    )


def parse_args() -> argparse.Namespace:
    """Parses command-line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--server-port',
        type=int,
        default=DEFAULT_PORT,
        help='Port to launch the pw_target_runner server on',
    )
    parser.add_argument(
        '--server-config',
        type=Path,
        default=None,
        help='Optional explicit server config file',
    )
    parser.add_argument(
        '--verbose',
        '-v',
        action='store_true',
        help='Enable debug logging',
    )
    return parser.parse_args()


def generate_runner(command: str, arguments: list[str]) -> str:
    """Generates a text-proto runner block for pw_target_runner_server."""
    escaped_cmd = _escape_textproto(command)
    lines = ['runner {', f'  command:"{escaped_cmd}"']
    for arg in arguments:
        lines.append(f'  args: "{_escape_textproto(arg)}"')
    lines.append('}\n')
    return '\n'.join(lines)


def write_server_config(config_path: Path) -> None:
    """Writes a server config file for all detected Nucleo boards."""
    runner_cmd = _resolve_runfile(
        'pigweed/targets/stm32_nucleo_f103/py/unit_test_runner',
        'stm32_nucleo_f103_unit_test_runner',
    )
    extra_prefix_args: list[str] = []
    if (
        runner_cmd == 'stm32_nucleo_f103_unit_test_runner'
        and shutil.which(runner_cmd) is None
    ):
        runner_cmd = sys.executable
        extra_prefix_args = [
            str(Path(__file__).resolve().with_name('unit_test_runner.py'))
        ]

    boards = device_detector.detect_boards()
    if not boards:
        _LOG.critical('No attached STM32 Nucleo boards detected')
        sys.exit(1)

    blocks: list[str] = []
    for board in boards:
        runner_args = [*extra_prefix_args, '--port', board.dev_name]
        if board.serial_number:
            runner_args.extend(['--stlink-serial', board.serial_number])
        blocks.append(generate_runner(runner_cmd, runner_args))
    config_path.write_text(''.join(blocks), encoding='utf-8')


def launch_server(
    server_config: Path | None,
    server_port: int | None,
) -> int:
    """Launches pw_target_runner_server."""
    server_cmd = _resolve_runfile(
        'pigweed/pw_target_runner/go/cmd/server_/server',
        'pw_target_runner_server',
    )

    if server_config is not None:
        cmd = [server_cmd, '-config', str(server_config)]
        if server_port is not None:
            cmd.extend(['-port', str(server_port)])
        return pw_cli.process.run(*cmd, log_output=True).returncode

    with tempfile.NamedTemporaryFile(mode='wb', delete=False) as temp_file:
        temp_path = Path(temp_file.name)

    try:
        write_server_config(temp_path)
        cmd = [server_cmd, '-config', str(temp_path)]
        if server_port is not None:
            cmd.extend(['-port', str(server_port)])
        return pw_cli.process.run(*cmd, log_output=True).returncode
    finally:
        temp_path.unlink(missing_ok=True)


def main() -> int:
    """CLI entry point."""
    args = parse_args()
    pw_cli.log.install(level=logging.DEBUG if args.verbose else logging.INFO)
    try:
        return launch_server(args.server_config, args.server_port)
    except KeyboardInterrupt:
        _LOG.info('Stopping unit test server...')
        return 0


if __name__ == '__main__':
    sys.exit(main())
