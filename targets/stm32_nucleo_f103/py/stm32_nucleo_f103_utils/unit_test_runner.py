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
"""Flashes and runs pw_unit_test binaries on STM32 Nucleo-F103RB boards."""

from __future__ import annotations

import argparse
import logging
from pathlib import Path
import sys
import time

import pw_cli.log
import serial  # type: ignore[import-untyped]

try:
    from stm32_nucleo_f103_utils.flasher import (
        DEFAULT_CHIP,
        download_binary,
        reset_board,
        select_board,
    )
except ImportError:
    from flasher import (  # type: ignore[import-not-found,no-redef]
        DEFAULT_CHIP,
        download_binary,
        reset_board,
        select_board,
    )

_LOG = logging.getLogger(__name__)

_TESTS_STARTING_STRING = b'[==========] Running all tests.'
_TESTS_DONE_STRING = b'[==========] Done running all tests.'
_TEST_FAILURE_STRING = b'[  FAILED  ]'


def parse_args() -> argparse.Namespace:
    """Parses CLI arguments for the unit test runner."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path, help='Target test ELF binary')
    parser.add_argument(
        '--chip',
        default=DEFAULT_CHIP,
        help='Target STM32 chip name for probe-rs',
    )
    parser.add_argument(
        '--stlink-serial',
        default=None,
        help='ST-LINK USB serial number',
    )
    parser.add_argument(
        '--port',
        default=None,
        help='Serial port path for ST-LINK Virtual COM Port',
    )
    parser.add_argument(
        '--baud',
        type=int,
        default=115200,
        help='Serial baud rate for debug console',
    )
    parser.add_argument(
        '--test-timeout',
        type=float,
        default=15.0,
        help='Maximum seconds to wait for test output',
    )
    parser.add_argument(
        '--verbose',
        '-v',
        action='store_true',
        help='Enable debug logging',
    )
    return parser.parse_args()


def run_device_test(
    binary: Path,
    baud: int = 115200,
    test_timeout: float = 15.0,
    chip: str = DEFAULT_CHIP,
    stlink_serial: str | None = None,
    port: str | None = None,
) -> bool:
    """Flashes `binary`, captures serial output, and verifies test results."""
    board = select_board(stlink_serial=stlink_serial, port=port)
    if not board.dev_name:
        _LOG.error('No serial port found for board %s', board.probe_selector())
        return False

    _LOG.info('Downloading %s to %s', binary, board.probe_selector())
    if not download_binary(binary, board, chip=chip):
        return False

    output = bytearray()
    with serial.Serial(port=board.dev_name, baudrate=baud, timeout=0.25) as ser:
        ser.reset_input_buffer()
        if not reset_board(board, chip=chip):
            return False

        deadline = time.monotonic() + test_timeout
        done_seen_at: float | None = None
        while time.monotonic() < deadline:
            line = ser.readline()
            if line:
                output.extend(line)
                normalized = output.replace(b'\r', b'')
                last_start = normalized.rfind(_TESTS_STARTING_STRING)
                last_done = normalized.rfind(_TESTS_DONE_STRING)
                if last_start != -1 and last_done > last_start:
                    if done_seen_at is None:
                        done_seen_at = time.monotonic()
                else:
                    done_seen_at = None
            elif done_seen_at is not None and (
                time.monotonic() - done_seen_at >= 0.05
            ):
                break

    cleaned = output.replace(b'\r', b'')
    start_idx = cleaned.rfind(_TESTS_STARTING_STRING)
    if start_idx != -1:
        cleaned = cleaned[start_idx:]

    decoded = cleaned.decode('utf-8', errors='replace')
    if decoded:
        print(decoded, end='' if decoded.endswith('\n') else '\n', flush=True)

    if cleaned.find(_TESTS_STARTING_STRING) == -1:
        _LOG.error('Did not observe test start banner from target')
        return False
    if cleaned.rfind(_TESTS_DONE_STRING) == -1:
        _LOG.error('Tests timed out before completion banner')
        return False
    if cleaned.rfind(_TEST_FAILURE_STRING) != -1:
        _LOG.error('One or more unit tests failed on device')
        return False

    _LOG.info('All on-device unit tests passed!')
    return True


def main() -> int:
    """CLI entry point."""
    args = parse_args()
    pw_cli.log.install(level=logging.DEBUG if args.verbose else logging.INFO)
    try:
        passed = run_device_test(
            binary=args.binary,
            baud=args.baud,
            test_timeout=args.test_timeout,
            chip=args.chip,
            stlink_serial=args.stlink_serial,
            port=args.port,
        )
    except Exception as exc:  # pylint: disable=broad-exception-caught
        _LOG.error('Unit test run failed: %s', exc)
        return 1
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
