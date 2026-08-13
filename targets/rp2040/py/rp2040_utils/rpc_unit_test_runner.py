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
"""This script flashes and runs RPC unit tests on Raspberry Pi Pico boards."""

import argparse
import logging
import sys
from pathlib import Path

import serial  # type: ignore

import pw_cli.log
from pw_hdlc import rpc
from pw_log.proto import log_pb2
from pw_rpc.callback_client.errors import RpcTimeout
from pw_system import device
from pw_tokenizer import detokenize
from pw_unit_test_proto import unit_test_pb2

from rp2040_utils.device_detector import PicoBoardInfo
from rp2040_utils.flasher import (
    create_flash_parser,
    device_from_args,
    flash,
    find_elf,
)

_LOG = logging.getLogger()


def create_test_runner_parser() -> argparse.ArgumentParser:
    """Parses command-line arguments."""
    parser = create_flash_parser()
    parser.description = __doc__
    parser.add_argument(
        '--test-timeout',
        type=float,
        default=5.0,
        help='Maximum communication delay in seconds before a '
        'test is considered unresponsive and aborted',
    )
    parser.add_argument(
        '--no-rpc-logging',
        action='store_true',
        help='Disable RPC logging and use raw HDLC logging instead',
    )
    return parser


def run_test_on_board(
    board: PicoBoardInfo,
    baud_rate: int,
    chip: str,
    binary: Path,
    test_timeout_seconds: float,
    use_rpc_logging: bool = True,
) -> bool:
    """Run an RPC unit test on this device.

    Returns whether it succeeded.
    """
    if not flash(board, chip, binary):
        return False
    serial_device = serial.Serial(board.serial_port, baud_rate, timeout=0.1)
    reader = rpc.SerialReader(serial_device, 8192)
    elf_path = find_elf(binary)
    if not elf_path:
        return False
    with device.Device(
        channel_id=rpc.DEFAULT_CHANNEL_ID,
        reader=reader,
        write=serial_device.write,
        proto_library=[log_pb2, unit_test_pb2],
        detokenizer=detokenize.Detokenizer(elf_path),
        use_rpc_logging=use_rpc_logging,
    ) as dev:
        try:
            test_results = dev.run_tests(test_timeout_seconds)
            _LOG.info('Test run complete')
        except RpcTimeout:
            _LOG.error('Test timed out after %s seconds.', test_timeout_seconds)
            return False
        if not test_results.all_tests_passed():
            return False
    return True


def main():
    """Set up runner and then flash/run device test."""
    args = create_test_runner_parser().parse_args()
    # Log to stdout, which will be captured by the unit test server.
    pw_cli.log.install(
        level=logging.DEBUG if args.verbose else logging.INFO,
    )
    board = device_from_args(args, interactive=False)
    test_passed = run_test_on_board(
        board,
        args.baud,
        args.chip,
        args.binary,
        args.test_timeout,
        use_rpc_logging=not args.no_rpc_logging,
    )
    sys.exit(0 if test_passed else 1)


if __name__ == '__main__':
    main()
