#!/usr/bin/env python3
# Copyright 2024 The Pigweed Authors
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
"""Flashes binaries to attached Raspberry Pi Pico boards."""

import argparse
import logging
import os
from pathlib import Path
import subprocess
import sys
import time

import pw_cli.log
from pw_cli.interactive_prompts import interactive_index_select

from rp2040_utils import device_detector
from rp2040_utils.device_detector import PicoBoardInfo, PicoDebugProbeBoardInfo

_LOG = logging.getLogger()

# If the script is being run through Bazel, our support binaries are provided
# at well known locations in its runfiles.
try:
    from python.runfiles import runfiles  # type: ignore

    r = runfiles.Create()
    assert r is not None
    probe_rs = r.Rlocation('probe_rs/probe-rs')
    assert probe_rs is not None
    _PROBE_RS_COMMAND = probe_rs
    picotool = r.Rlocation('picotool/picotool')
    assert picotool is not None
    _PICOTOOL_COMMAND = picotool
except ImportError:
    _PROBE_RS_COMMAND = 'probe-rs'
    _PICOTOOL_COMMAND = 'picotool'


def flash(board_info: PicoBoardInfo, chip: str, binary: Path) -> bool:
    """Load `binary` onto `board_info` and wait for the device to become
    available.

    Returns whether or not flashing was successful."""
    if isinstance(board_info, PicoDebugProbeBoardInfo):
        return _load_debugprobe_binary(board_info, chip, binary)
    if not _load_picotool_binary(board_info, binary):
        return False
    if not _wait_for_serial_port(board_info):
        _LOG.error(
            'Binary flashed but unable to connect to the serial port: %s',
            board_info.serial_port,
        )
        return False
    _LOG.info(
        'Successfully flashed Pico on bus %s, port %s, serial port %s',
        board_info.bus,
        board_info.port,
        board_info.serial_port,
    )
    return True


def find_elf(binary: Path) -> Path | None:
    """Attempt to find and return the path to an ELF file for a binary.

    Args:
      binary: A relative path to a binary.

    Returns the path to the associated ELF file, or None if none was found.
    """
    if binary.suffix == '.elf' or not binary.suffix:
        return binary
    choices = (
        binary.parent / f'{binary.stem}.elf',
        binary.parent / 'bin' / f'{binary.stem}.elf',
        binary.parent / 'test' / f'{binary.stem}.elf',
    )
    for choice in choices:
        if choice.is_file():
            return choice

    _LOG.error(
        'Cannot find ELF file to use as a token database for binary: %s',
        binary,
    )
    return None


def _load_picotool_binary(board_info: PicoBoardInfo, binary: Path) -> bool:
    """Flash a binary to this device using picotool, returning success or
    failure."""
    cmd = [
        _PICOTOOL_COMMAND,
        'load',
        '-x',
        # We use the absolute path since `cwd` is changed below.
        str(binary.absolute()),
    ]

    # If the binary has not file extension, assume that it is ELF and
    # explicitly tell `picotool` that.
    if not binary.suffix:
        cmd += ['-t', 'elf']

    cmd += [
        '--bus',
        str(board_info.bus),
        '--address',
        str(board_info.current_address()),
        '-F',
    ]

    _LOG.debug('Flashing ==> %s', ' '.join(cmd))

    # If the script is running inside Bazel, `picotool` needs to run from
    # the project root so that it can find libusb.
    cwd = None
    if 'BUILD_WORKING_DIRECTORY' in os.environ:
        cwd = os.environ['BUILD_WORKING_DIRECTORY']

    process = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=cwd,
    )
    if process.returncode:
        err = (
            'Flashing command failed: ' + ' '.join(cmd),
            str(board_info),
            process.stdout.decode('utf-8', errors='replace'),
        )
        _LOG.error('\n\n'.join(err))
        return False
    return True


def _wait_for_serial_port(board_info: PicoBoardInfo) -> bool:
    """Waits for a serial port to enumerate."""
    start_time = time.monotonic()
    timeout_seconds = 10.0
    while time.monotonic() - start_time < timeout_seconds:
        # If the serial port path isn't known, watch for a newly
        # enumerated path.
        if board_info.serial_port is None:
            # Wait a bit before checking for a new port.
            time.sleep(0.3)
            # Check for updated serial port path.
            for device in device_detector.detect_boards():
                if (
                    device.bus == board_info.bus
                    and device.port == board_info.port
                ):
                    board_info.serial_port = device.serial_port
                    # Serial port found, break out of device for loop.
                    break

        # Serial port known try to connect to it.
        if board_info.serial_port is not None:
            # Connect to the serial port.
            try:
                with open(board_info.serial_port, 'r+b', buffering=0):
                    return True
            except (OSError, IOError):
                _LOG.debug(
                    'Unable to connect to %s, retrying', board_info.serial_port
                )
                time.sleep(0.1)
    _LOG.error(
        'Binary flashed but unable to connect to the serial port: %s',
        board_info.serial_port,
    )
    return False


def _load_debugprobe_binary(
    board_info: PicoDebugProbeBoardInfo, chip: str, binary: Path
) -> bool:
    """Flash a binary to this device using a debug probe, returning success
    or failure."""
    elf_path = find_elf(binary)
    if not elf_path:
        return False

    # `probe-rs` takes a `--probe` argument of the form:
    #  <vendor_id>:<product_id>:<serial_number>
    probe = "{:04x}:{:04x}:{}".format(
        board_info.vendor_id(),
        board_info.device_id(),
        board_info.serial_number,
    )

    download_cmd = (
        _PROBE_RS_COMMAND,
        'download',
        '--probe',
        probe,
        '--chip',
        chip,
        '--speed',
        '10000',
        str(elf_path),
    )
    _LOG.debug('Flashing ==> %s', ' '.join(download_cmd))
    process = subprocess.run(
        download_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    if process.returncode:
        err = (
            'Flashing command failed: ' + ' '.join(download_cmd),
            str(board_info),
            process.stdout.decode('utf-8', errors='replace'),
        )
        _LOG.error('\n\n'.join(err))
        return False

    # `probe-rs download` leaves the device halted so it needs to be reset
    # to run.
    reset_cmd = (
        _PROBE_RS_COMMAND,
        'reset',
        '--probe',
        probe,
        '--chip',
        chip,
    )
    _LOG.debug('Resetting ==> %s', ' '.join(reset_cmd))
    process = subprocess.run(
        reset_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    if process.returncode:
        err = (
            'Resetting command failed: ' + ' '.join(reset_cmd),
            str(board_info),
            process.stdout.decode('utf-8', errors='replace'),
        )
        _LOG.error('\n\n'.join(err))
        return False

    # Give time for the device to reset. Ideally the common unit test
    # runner would wait for input but this is not the case.
    time.sleep(0.5)

    _LOG.info(
        'Successfully flashed Pico on bus %s, port %s, serial number %s',
        board_info.bus,
        board_info.port,
        board_info.serial_number,
    )
    return True


def create_flash_parser() -> argparse.ArgumentParser:
    """Returns a parser for flashing command-line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path, help='The target binary to flash')
    parser.add_argument(
        '--usb-bus',
        type=int,
        help='The bus this Pi Pico is on',
    )
    parser.add_argument(
        '--usb-port',
        type=str,
        help=(
            'The port chain as a colon-separated list of integers of this Pi '
            'Pico on the specified USB bus (e.g. 1:4:2:2)'
        ),
    )
    parser.add_argument(
        '-b',
        '--baud',
        type=int,
        default=115200,
        help='Baud rate to use for serial communication with target device',
    )
    parser.add_argument(
        '--debug-probe-only',
        action='store_true',
        help='Only flash on detected Pi Pico debug probes',
    )
    parser.add_argument(
        '--pico-only',
        action='store_true',
        help='Only flash on detected Pi Pico boards',
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

    return parser


def device_from_args(
    args: argparse.Namespace, interactive: bool
) -> PicoBoardInfo:
    """Select a PicoBoardInfo using the provided `args`.

    This function will exit if no compatible board is discovered.

    Args:
        args: The parsed args namespace. This must be a set of arguments parsed
            using `create_flash_parser`.
        interactive: If true, multiple detected boards will result in a user
            interaction to select which to use. If false, the first compatible
            board will be used.

    Returns:
        Selected PicoBoardInfo.
    """
    if args.pico_only and args.debug_probe_only:
        _LOG.critical('Cannot specify both --pico-only and --debug-probe-only')
        sys.exit(1)

    # For now, require manual configurations to be fully specified.
    if (args.usb_port is None) != (args.usb_bus is None):
        _LOG.critical(
            'Must specify BOTH --usb-bus and --usb-port when manually '
            'specifying a device'
        )
        sys.exit(1)

    if args.usb_bus and args.usb_port:
        return device_detector.board_from_usb_port(args.usb_bus, args.usb_port)

    _LOG.debug('Attempting to automatically detect dev board')
    boards = device_detector.detect_boards(
        include_picos=not args.debug_probe_only,
        include_debug_probes=not args.pico_only,
    )
    if not boards:
        _LOG.error('Could not find an attached device')
        sys.exit(1)
    if len(boards) == 1:
        _LOG.info('Only one device detected.')
        return boards[0]
    if not interactive:
        _LOG.info(
            'Interactive mode disabled. Defaulting to first discovered device.'
        )
        return boards[0]

    print('Multiple devices detected. Please select one:')
    board_lines = list(
        f'bus {board.bus}, port {board.port}'
        f' ({board.manufacturer} - {board.product} -'
        f' {board.current_serial_number()})'
        for board in boards
    )
    user_input_index, _user_input_text = interactive_index_select(board_lines)
    return boards[user_input_index]


def main():
    """Flash a binary."""
    args = create_flash_parser().parse_args()
    log_level = logging.DEBUG if args.verbose else logging.INFO
    pw_cli.log.install(level=log_level)
    board = device_from_args(args, interactive=True)
    _LOG.info('Flashing bus %s port %s', board.bus, board.port)
    flashed = flash(board, args.chip, args.binary)
    sys.exit(0 if flashed else 1)


if __name__ == '__main__':
    main()
