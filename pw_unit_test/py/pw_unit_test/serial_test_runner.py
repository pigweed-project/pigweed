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
"""Facilitates automating unit tests on devices with serial ports.

This library assumes that the on-device test runner emits the test results
as plain-text over a serial port, and that tests are triggered by a pre-defined
input (``DEFAULT_TEST_START_CHARACTER``) over the same serial port that results
are emitted from.
"""

import abc
import logging
from pathlib import Path

import serial  # type: ignore


_LOG = logging.getLogger("serial_test_runner")

# Verification of test pass/failure depends on these strings. If the formatting
# or output of the simple_printing_event_handler changes, this may need to be
# updated.
_TESTS_STARTING_STRING = b'[==========] Running all tests.'
_TESTS_DONE_STRING = b'[==========] Done running all tests.'
_TEST_FAILURE_STRING = b'[  FAILED  ]'

# Character used to trigger test start.
DEFAULT_TEST_START_CHARACTER = ' '.encode('utf-8')


class FlashingFailure(Exception):
    """A simple exception to be raised when flashing fails."""


class TestingFailure(Exception):
    """A simple exception to be raised when a testing step fails."""


class DeviceNotFound(Exception):
    """A simple exception to be raised when unable to connect to a device."""


class SerialTestingDevice(abc.ABC):
    """A device that supports automated testing via parsing serial output."""

    @abc.abstractmethod
    def load_binary(self, binary: Path) -> bool:
        """Flashes the specified binary to this device.

        Raises:
          DeviceNotFound: This device is no longer available.
          FlashingFailure: The binary could not be flashed.

        Returns:
          True if the binary was loaded successfully.
        """

    @abc.abstractmethod
    def serial_port(self) -> str:
        """Returns the name of the com port this device is enumerated on.

        Raises:
          DeviceNotFound: This device is no longer available.
        """

    @abc.abstractmethod
    def baud_rate(self) -> int:
        """Returns the baud rate to use when connecting to this device.

        Raises:
          DeviceNotFound: This device is no longer available.
        """


def _log_subprocess_output(level, output: bytes, logger: logging.Logger):
    """Logs subprocess output line-by-line."""

    lines = output.decode('utf-8', errors='replace').splitlines()
    for line in lines:
        logger.log(level, line)


def trigger_test_run(
    port: str,
    baud_rate: int,
    test_timeout: float,
    trigger_data: bytes = DEFAULT_TEST_START_CHARACTER,
) -> bytes:
    """Triggers a test run, and returns captured test results."""

    serial_data = bytearray()
    device = serial.Serial(baudrate=baud_rate, port=port, timeout=test_timeout)
    if not device.is_open:
        raise TestingFailure('Failed to open device')

    # Flush input buffer and trigger the test start.
    device.reset_input_buffer()
    device.write(trigger_data)

    # Block and wait for the first byte.
    serial_data += device.read()
    if not serial_data:
        raise TestingFailure('Device not producing output')

    # Read with a reasonable timeout until we stop getting characters.
    while True:
        bytes_read = device.readline()
        if not bytes_read:
            break
        serial_data += bytes_read
        if serial_data.rfind(_TESTS_DONE_STRING) != -1:
            # Set to much more aggressive timeout since the last one or two
            # lines should print out immediately. (one line if all fails or all
            # passes, two lines if mixed.)
            device.timeout = 0.01

    # Remove carriage returns.
    serial_data = serial_data.replace(b"\r", b"")

    # Try to trim captured results to only contain most recent test run.
    test_start_index = serial_data.rfind(_TESTS_STARTING_STRING)
    return bytes(
        serial_data
        if test_start_index == -1
        else serial_data[test_start_index:]
    )


def handle_test_results(
    test_output: bytes, logger: logging.Logger = _LOG
) -> None:
    """Parses test output to determine whether tests passed or failed.

    Raises:
      TestingFailure if any tests fail or if test results are incomplete.
    """

    if test_output.find(_TESTS_STARTING_STRING) == -1:
        raise TestingFailure('Failed to find test start')

    if test_output.rfind(_TESTS_DONE_STRING) == -1:
        _log_subprocess_output(logging.INFO, test_output, logger)
        raise TestingFailure('Tests did not complete')

    if test_output.rfind(_TEST_FAILURE_STRING) != -1:
        _log_subprocess_output(logging.INFO, test_output, logger)
        raise TestingFailure('Test suite had one or more failures')

    _log_subprocess_output(logging.DEBUG, test_output, logger)

    logger.info('Test passed!')


def run_device_test(
    device: SerialTestingDevice,
    binary: Path,
    test_timeout: float,
    logger: logging.Logger = _LOG,
) -> bool:
    """Runs tests on a device.

    When a unit test run fails, results are logged as an error.

    Args:
      device: The device to run tests on.
      binary: The binary containing tests to flash on the device.
      test_timeout: If the device stops producing output longer than this
        timeout, the test is considered stuck and is aborted.

    Returns:
      True if all tests passed.
    """

    logger.info('Flashing binary to device')
    device.load_binary(binary)
    try:
        logger.info('Running test')
        test_output = trigger_test_run(
            device.serial_port(), device.baud_rate(), test_timeout
        )
        if test_output:
            handle_test_results(test_output, logger)
    except TestingFailure as err:
        logger.error(err)
        return False

    return True
