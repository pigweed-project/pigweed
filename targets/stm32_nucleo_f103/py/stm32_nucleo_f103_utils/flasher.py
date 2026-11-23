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
"""Flashes binaries to an attached STM32 Nucleo-F103RB board via probe-rs."""

from __future__ import annotations

import argparse
import logging
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pw_cli.log

try:
    from stm32_nucleo_f103_utils import device_detector
    from stm32_nucleo_f103_utils.device_detector import BoardInfo
except ImportError:
    import device_detector  # type: ignore[import-not-found,no-redef]
    from device_detector import (  # type: ignore[import-not-found,no-redef]
        BoardInfo,
    )

try:
    from python.runfiles import runfiles  # type: ignore[import-not-found]
except ImportError:
    runfiles = None  # type: ignore[assignment]

_LOG = logging.getLogger(__name__)
DEFAULT_CHIP = 'STM32F103RB'
DEFAULT_PROBE_TIMEOUT_S = 30.0


def _resolve_probe_rs() -> str | None:
    if runfiles is not None:
        rf = runfiles.Create()
        if rf is not None:
            for candidate in (
                'probe_rs/probe-rs',
                'probe_rs/probe-rs.exe',
                'rules_probe_rs++probe_rs+probe_rs/probe-rs',
                'rules_probe_rs++probe_rs+probe_rs/probe-rs.exe',
            ):
                resolved = rf.Rlocation(candidate)
                if resolved and Path(resolved).is_file():
                    return resolved
    cipd_dir = os.environ.get('PW_PROBE_RS_CIPD_INSTALL_DIR')
    if cipd_dir:
        cipd_bin = shutil.which('probe-rs', path=str(Path(cipd_dir) / 'bin'))
        if cipd_bin is not None:
            return cipd_bin
    return shutil.which('probe-rs')


def _validate_serial_number(serial_number: str | None) -> str | None:
    if serial_number is None:
        return None
    if not device_detector.VALID_SERIAL_RE.fullmatch(serial_number):
        raise ValueError(
            f'Invalid ST-LINK serial number {serial_number!r}; '
            'expected only ASCII alphanumeric, underscore, or hyphen characters'
        )
    return serial_number


def resolve_binary_path(binary: Path) -> Path:
    """Resolves relative binary paths against BUILD_WORKING_DIRECTORY."""
    if not binary.is_absolute():
        working_dir = os.environ.get('BUILD_WORKING_DIRECTORY')
        if working_dir:
            candidate = Path(working_dir) / binary
            if candidate.exists():
                return candidate
    return binary


def _parse_probe_selector(
    probe: str,
    stlink_serial: str | None = None,
    port: str | None = None,
) -> BoardInfo:
    parts = probe.split(':', 2)
    if len(parts) < 2 or not parts[0] or not parts[1]:
        raise ValueError(
            f'Invalid probe selector {probe!r}; expected <vid>:<pid>[:<serial>]'
        )
    try:
        vid = int(parts[0], 16)
        pid = int(parts[1], 16)
    except ValueError as err:
        raise ValueError(
            f'Invalid hex VID/PID in probe selector {probe!r}; '
            'expected <vid>:<pid>[:<serial>]'
        ) from err
    if not (0 <= vid <= 0xFFFF and 0 <= pid <= 0xFFFF):
        raise ValueError(
            f'Out-of-range VID/PID in probe selector {probe!r}; '
            'expected 16-bit hex values'
        )
    if len(parts) == 3:
        if not parts[2]:
            raise ValueError(
                f'Empty serial number in probe selector {probe!r}; '
                'expected <vid>:<pid>[:<serial>]'
            )
        serial_num: str | None = _validate_serial_number(parts[2])
    else:
        serial_num = _validate_serial_number(stlink_serial)
    return BoardInfo(
        dev_name=port or '',
        serial_number=serial_num,
        vendor_id=vid,
        product_id=pid,
    )


def select_board(
    stlink_serial: str | None = None,
    port: str | None = None,
    probe: str | None = None,
) -> BoardInfo:
    """Selects a connected STM32 Nucleo board matching optional filters."""
    validated_serial = _validate_serial_number(stlink_serial)
    if probe is not None:
        return _parse_probe_selector(
            probe=probe,
            stlink_serial=validated_serial,
            port=port,
        )

    boards = device_detector.detect_boards()
    for board in boards:
        if validated_serial and board.serial_number != validated_serial:
            continue
        if port and board.dev_name != port:
            continue
        return board

    if validated_serial:
        return BoardInfo(dev_name=port or '', serial_number=validated_serial)
    if port:
        raise RuntimeError(
            f'No attached STM32 Nucleo-F103RB board detected on port {port}'
        )

    raise RuntimeError('No attached STM32 Nucleo-F103RB board detected')


def _run_probe_rs(
    *,
    action: str,
    board: BoardInfo,
    args: list[str],
    timeout: float = DEFAULT_PROBE_TIMEOUT_S,
) -> bool:
    try:
        _validate_serial_number(board.serial_number)
    except ValueError as err:
        _LOG.error('%s', err)
        return False

    probe_rs_bin = _resolve_probe_rs()
    if probe_rs_bin is None:
        _LOG.error(
            'probe-rs was not found; ensure the Pigweed environment is '
            'activated'
        )
        return False

    cmd = [probe_rs_bin, *args]
    _LOG.debug('Running: %s', ' '.join(cmd))
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        _LOG.error('probe-rs %s timed out after %.1fs', action, timeout)
        return False
    except OSError as err:
        _LOG.error('Failed to execute probe-rs %s: %s', action, err)
        return False
    if proc.returncode != 0:
        _LOG.error(
            'probe-rs %s failed:\n%s',
            action,
            proc.stdout.decode('utf-8', errors='replace'),
        )
        return False
    return True


def download_binary(
    binary: Path,
    board: BoardInfo,
    chip: str = DEFAULT_CHIP,
) -> bool:
    """Downloads `binary` onto `board` and leaves the target halted."""
    resolved = resolve_binary_path(binary)
    if not resolved.is_file():
        _LOG.error('Binary file does not exist: %s', resolved)
        return False

    return _run_probe_rs(
        action='download',
        board=board,
        args=[
            'download',
            '--probe',
            board.probe_selector(),
            '--chip',
            chip,
            '--protocol',
            'swd',
            '--connect-under-reset',
            str(resolved),
        ],
    )


def reset_board(
    board: BoardInfo,
    chip: str = DEFAULT_CHIP,
) -> bool:
    """Resets `board` so flashed firmware starts running."""
    return _run_probe_rs(
        action='reset',
        board=board,
        args=[
            'reset',
            '--probe',
            board.probe_selector(),
            '--chip',
            chip,
            '--protocol',
            'swd',
            '--connect-under-reset',
        ],
    )


def flash(
    binary: Path,
    board: BoardInfo,
    chip: str = DEFAULT_CHIP,
) -> bool:
    """Downloads `binary` to `board` and resets the MCU to start execution."""
    if not download_binary(binary, board, chip=chip):
        return False
    if not reset_board(board, chip=chip):
        return False
    _LOG.info(
        'Successfully flashed %s to %s (%s)',
        binary.name,
        board.probe_selector(),
        board.dev_name or 'no VCP',
    )
    return True


def create_flash_parser() -> argparse.ArgumentParser:
    """Creates argument parser for the STM32 Nucleo-F103 flasher."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path, help='Target ELF binary to flash')
    parser.add_argument(
        '--chip',
        default=DEFAULT_CHIP,
        help='Target STM32 chip name for probe-rs',
    )
    parser.add_argument(
        '--probe',
        default=None,
        help='Explicit probe-rs selector (<vid>:<pid>[:<serial>])',
    )
    parser.add_argument(
        '--stlink-serial',
        default=None,
        help='ST-LINK USB serial number',
    )
    parser.add_argument(
        '--port',
        default=None,
        help='ST-LINK Virtual COM Port path',
    )
    parser.add_argument(
        '--verbose',
        '-v',
        action='store_true',
        help='Enable debug logging',
    )
    return parser


def main() -> int:
    """CLI entry point for flashing an STM32 Nucleo-F103RB binary."""
    args = create_flash_parser().parse_args()
    pw_cli.log.install(level=logging.DEBUG if args.verbose else logging.INFO)
    try:
        board = select_board(
            stlink_serial=args.stlink_serial,
            port=args.port,
            probe=args.probe,
        )
    except (RuntimeError, ValueError) as err:
        _LOG.error('%s', err)
        return 1
    return 0 if flash(args.binary, board, chip=args.chip) else 1


if __name__ == '__main__':
    sys.exit(main())
