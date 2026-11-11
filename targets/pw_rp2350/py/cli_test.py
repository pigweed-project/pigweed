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

"""Tests for pw_rp2350_debug_probe.cli."""

from __future__ import annotations

import io
import json
import logging
import unittest
from array import array
from typing import Any
from unittest.mock import patch

import usb.core  # type: ignore[import-untyped]
import usb.util  # type: ignore[import-untyped]
from pw_rp2350_debug_probe import cli
from pw_rp2350_debug_probe.debug_probe import SwdTarget
from rp2040_utils.device_detector import PicoBoardInfo, PicoDebugProbeBoardInfo

_HELLO_COMMAND = 0
_GET_TARGET_INFO_COMMAND = 2


def _make_probe_board(
    *,
    bus: int = 1,
    port: str = '1:4',
    serial_number: str = 'E6614103E7123456',
    manufacturer: str = 'Raspberry Pi',
    product: str = 'Debug Probe (CMSIS-DAP)',
) -> PicoDebugProbeBoardInfo:
    """Creates a PicoDebugProbeBoardInfo instance for CLI tests."""
    return PicoDebugProbeBoardInfo(
        bus=bus,
        port=port,
        serial_port='/dev/ttyACM0',
        manufacturer=manufacturer,
        product=product,
        serial_number=serial_number,
    )


class FakeUsbDevice:
    """Lightweight fake USB device recording vendor control transfers."""

    def __init__(
        self,
        *,
        greeting: bytes = b'pw_rp2350 probe v1',
        target_info: dict[str, Any] | None = None,
    ) -> None:
        """Initializes the fake USB device with canned IN responses."""
        self.calls: list[dict[str, Any]] = []
        self._greeting = greeting
        self._target_info = (
            target_info
            if target_info is not None
            else {
                'name': 'Pico 2 HIL',
                'mcu': 'RP2350',
                'capabilities': ['uart', 'spi', 'i2c'],
            }
        )

    # pylint: disable=invalid-name
    def ctrl_transfer(  # noqa: PLR0913, PLR0917
        self,
        bmRequestType: int,  # noqa: N803
        bRequest: int,  # noqa: N803
        wValue: int = 0,  # noqa: N803
        wIndex: int = 0,  # noqa: N803
        data_or_wLength: bytes | int | None = None,  # noqa: N803
        timeout: int | None = None,
    ) -> array[int] | int:
        """Records the transfer and returns canned HELLO or GET_TARGET_INFO."""
        self.calls.append(
            {
                'bmRequestType': bmRequestType,
                'bRequest': bRequest,
                'wValue': wValue,
                'wIndex': wIndex,
                'data_or_wLength': data_or_wLength,
                'timeout': timeout,
            }
        )
        if bmRequestType & 0x80:
            if wIndex == _HELLO_COMMAND:
                return array('B', self._greeting)
            if wIndex == _GET_TARGET_INFO_COMMAND:
                payload = json.dumps(self._target_info).encode('utf-8')
                return array('B', payload)
            return array('B')
        if isinstance(data_or_wLength, bytes):
            return len(data_or_wLength)
        return 0


class CliTest(unittest.TestCase):
    """Tests for the debug_probe_cli command-line interface."""

    def setUp(self) -> None:
        """Patches global logging setup and libusb resource disposal."""
        super().setUp()
        self.mock_log_install = self.enterContext(patch('pw_cli.log.install'))
        self.mock_dispose_resources = self.enterContext(
            patch.object(usb.util, 'dispose_resources')
        )

    def test_help_exits_zero_and_describes_pw_rp2350(self) -> None:
        """Verifies `--help` exits cleanly and describes pw_rp2350."""
        with (
            patch('sys.stdout', new_callable=io.StringIO) as mock_stdout,
            self.assertRaises(SystemExit) as ctx,  # noqa: PT027
        ):
            cli.main(['--help'])

        self.assertEqual(ctx.exception.code, 0)
        help_text = mock_stdout.getvalue()
        self.assertIn('debug_probe_cli', help_text)
        self.assertIn(
            'CLI for interacting with the pw_rp2350 debug probe.',
            help_text,
        )

    def test_missing_subcommand_exits_with_error(self) -> None:
        """Verifies invoking the CLI with no subcommand exits with status 2."""
        with (
            patch('sys.stderr', new_callable=io.StringIO),
            self.assertRaises(SystemExit) as ctx,  # noqa: PT027
        ):
            cli.main([])

        self.assertEqual(ctx.exception.code, 2)

    def test_removed_list_only_flag_is_rejected(self) -> None:
        """Verifies the legacy `--list-only` flag is rejected."""
        with (
            patch('sys.stderr', new_callable=io.StringIO),
            self.assertRaises(SystemExit) as ctx,  # noqa: PT027
        ):
            cli.main(['--list-only'])

        self.assertEqual(ctx.exception.code, 2)

    def test_log_install_omits_use_color(self) -> None:
        """Verifies `pw_cli.log.install` omits `use_color=True`."""
        with patch(
            'rp2040_utils.device_detector.detect_boards', return_value=[]
        ):
            rc = cli.main(['list'])

        self.assertEqual(rc, 0)
        self.mock_log_install.assert_called_once_with(
            level=logging.WARNING,
            hide_timestamp=False,
        )
        self.assertNotIn('use_color', self.mock_log_install.call_args.kwargs)

    def test_list_subcommand_formats_detected_boards(self) -> None:
        """Verifies `list` prints formatted probe entries to stdout."""
        boards = [
            _make_probe_board(
                bus=1,
                port='1:4',
                serial_number='E6614103E7111111',
            ),
            _make_probe_board(
                bus=2,
                port='2:1:3',
                serial_number='E6614103E7222222',
                manufacturer='Pigweed',
                product='RP2350 HIL Probe',
            ),
        ]
        with (
            patch(
                'rp2040_utils.device_detector.detect_boards',
                return_value=boards,
            ) as mock_detect,
            patch('sys.stdout', new_callable=io.StringIO) as mock_stdout,
        ):
            rc = cli.main(['list'])

        self.assertEqual(rc, 0)
        mock_detect.assert_called_once_with(
            include_picos=False, include_debug_probes=True
        )
        self.assertEqual(
            mock_stdout.getvalue().splitlines(),
            [
                (
                    'bus 1, port 1:4 (Raspberry Pi - Debug Probe (CMSIS-DAP)'
                    ' - E6614103E7111111)'
                ),
                (
                    'bus 2, port 2:1:3 (Pigweed - RP2350 HIL Probe'
                    ' - E6614103E7222222)'
                ),
            ],
        )

    def test_target_subcommand_routes_dut_sim_and_buddy(self) -> None:
        """Verifies `target` maps `dut`, `sim`, and `buddy` to SwdTarget."""
        cases = [
            ('dut', SwdTarget.DUT),
            ('sim', SwdTarget.SIM),
            ('buddy', SwdTarget.SIM),
        ]
        for choice, expected_target in cases:
            with self.subTest(choice=choice):
                board = _make_probe_board()
                fake_dev = FakeUsbDevice()
                with (
                    patch(
                        'rp2040_utils.device_detector.detect_boards',
                        return_value=[board],
                    ),
                    patch.object(
                        PicoDebugProbeBoardInfo,
                        'usb_device',
                        return_value=fake_dev,
                    ),
                ):
                    rc = cli.main(['target', choice])

                self.assertEqual(rc, 0)
                self.assertEqual(len(fake_dev.calls), 1)
                self.assertEqual(fake_dev.calls[0]['wIndex'], 1)
                self.assertEqual(
                    fake_dev.calls[0]['wValue'], int(expected_target)
                )

    def test_info_subcommand_prints_greeting_and_target_info(self) -> None:
        """Verifies `info` prints probe greeting and target metadata."""
        board = _make_probe_board()
        fake_dev = FakeUsbDevice(
            greeting=b'pw_rp2350 debug probe v2.0',
            target_info={
                'name': 'Pico 2 Test Target',
                'mcu': 'RP2350B',
                'capabilities': ['uart', 'spi', 'digital_io'],
            },
        )
        with (
            patch(
                'rp2040_utils.device_detector.detect_boards',
                return_value=[board],
            ),
            patch.object(
                PicoDebugProbeBoardInfo, 'usb_device', return_value=fake_dev
            ),
            patch('sys.stdout', new_callable=io.StringIO) as mock_stdout,
        ):
            rc = cli.main(['info'])

        self.assertEqual(rc, 0)
        lines = mock_stdout.getvalue().splitlines()
        self.assertEqual(
            lines,
            [
                'Probe: pw_rp2350 debug probe v2.0',
                'Target: Pico 2 Test Target',
                'MCU: RP2350B',
                'Capabilities: UART, SPI, DIGITAL_IO',
            ],
        )

    def test_info_subcommand_prints_none_when_capabilities_empty(self) -> None:
        """Verifies `info` prints `none` when target capabilities are empty."""
        board = _make_probe_board()
        fake_dev = FakeUsbDevice(
            greeting=b'hello',
            target_info={
                'name': 'Minimal Board',
                'mcu': 'RP2350',
                'capabilities': [],
            },
        )
        with (
            patch(
                'rp2040_utils.device_detector.detect_boards',
                return_value=[board],
            ),
            patch.object(
                PicoDebugProbeBoardInfo, 'usb_device', return_value=fake_dev
            ),
            patch('sys.stdout', new_callable=io.StringIO) as mock_stdout,
        ):
            rc = cli.main(['info'])

        self.assertEqual(rc, 0)
        self.assertIn('Capabilities: none', mock_stdout.getvalue().splitlines())

    def test_usb_bus_and_port_flags_and_aliases_before_and_after_subcommand(
        self,
    ) -> None:
        """Verifies `--usb-bus`/`--usb-port` and aliases before and after."""
        invocations = [
            ['--usb-bus', '2', '--usb-port', '2:4', 'target', 'dut'],
            ['target', 'dut', '--usb-bus', '2', '--usb-port', '2:4'],
            ['--bus', '2', '--port', '2:4', 'target', 'dut'],
            ['target', 'dut', '--bus', '2', '--port', '2:4'],
        ]
        for argv in invocations:
            with self.subTest(argv=argv):
                board = _make_probe_board(bus=2, port='2:4')
                fake_dev = FakeUsbDevice()
                with (
                    patch(
                        'rp2040_utils.device_detector.board_from_usb_port',
                        return_value=board,
                    ) as mock_from_port,
                    patch.object(
                        PicoDebugProbeBoardInfo,
                        'usb_device',
                        return_value=fake_dev,
                    ),
                ):
                    rc = cli.main(argv)

                self.assertEqual(rc, 0)
                mock_from_port.assert_called_once_with(
                    2,
                    '2:4',
                    include_picos=True,
                    include_debug_probes=True,
                )
                self.assertEqual(len(fake_dev.calls), 1)
                self.assertEqual(
                    fake_dev.calls[0]['wValue'], int(SwdTarget.DUT)
                )

    def test_no_interactive_before_and_after_subcommand(self) -> None:
        """Verifies `--no-interactive` works before and after the subcommand."""
        board_first = _make_probe_board(bus=1, port='1:1', serial_number='S1')
        board_second = _make_probe_board(bus=1, port='1:2', serial_number='S2')
        for argv in (
            ['--no-interactive', 'target', 'sim'],
            ['target', 'sim', '--no-interactive'],
        ):
            with self.subTest(argv=argv):
                with (
                    patch(
                        'rp2040_utils.device_detector.detect_boards',
                        return_value=[board_first, board_second],
                    ),
                    patch('sys.stdin') as mock_stdin,
                    patch.object(
                        PicoDebugProbeBoardInfo,
                        'usb_device',
                    ) as mock_usb_device,
                    patch.object(
                        cli, 'interactive_index_select'
                    ) as mock_prompt,
                    self.assertLogs('debug_probe_cli', level='ERROR') as logs,
                ):
                    mock_stdin.isatty.return_value = True
                    rc = cli.main(argv)

                self.assertEqual(rc, 1)
                mock_prompt.assert_not_called()
                mock_usb_device.assert_not_called()
                self.assertTrue(
                    any(
                        'Multiple debug probes detected in non-interactive mode'
                        in entry
                        for entry in logs.output
                    )
                )

    def test_verbose_flag_before_after_and_combined_across_subcommand(
        self,
    ) -> None:
        """Verifies `-v` accumulates before and after any subcommand."""
        cases = [
            (['list'], logging.WARNING),
            (['-v', 'list'], logging.INFO),
            (['list', '-v'], logging.INFO),
            (['-vv', 'list'], logging.DEBUG),
            (['list', '-vv'], logging.DEBUG),
            (['-v', 'list', '-v'], logging.DEBUG),
        ]
        for argv, expected_level in cases:
            with self.subTest(argv=argv):
                self.mock_log_install.reset_mock()
                with patch(
                    'rp2040_utils.device_detector.detect_boards',
                    return_value=[],
                ):
                    rc = cli.main(argv)

                self.assertEqual(rc, 0)
                self.mock_log_install.assert_called_once_with(
                    level=expected_level,
                    hide_timestamp=False,
                )

    def test_led_rgb_accepts_decimal_and_hex_channels(self) -> None:
        """Verifies `--rgb` accepts both decimal and `0x` hex values."""
        cases = [
            (['led', '--rgb', '0', '128', '255'], bytes((0, 128, 255))),
            (['led', '--rgb', '0x00', '0x80', '0xff'], bytes((0, 128, 255))),
            (['led', '--rgb', '0X1A', '43', '0x3C'], bytes((0x1A, 43, 0x3C))),
        ]
        for argv, expected_payload in cases:
            with self.subTest(argv=argv):
                board = _make_probe_board()
                fake_dev = FakeUsbDevice()
                with (
                    patch(
                        'rp2040_utils.device_detector.detect_boards',
                        return_value=[board],
                    ),
                    patch.object(
                        PicoDebugProbeBoardInfo,
                        'usb_device',
                        return_value=fake_dev,
                    ),
                ):
                    rc = cli.main(argv)

                self.assertEqual(rc, 0)
                self.assertEqual(len(fake_dev.calls), 1)
                self.assertEqual(fake_dev.calls[0]['wIndex'], 3)
                self.assertEqual(
                    fake_dev.calls[0]['data_or_wLength'],
                    expected_payload,
                )

    def test_led_rgb_rejects_invalid_and_out_of_range_channels(self) -> None:
        """Verifies `--rgb` rejects negative, >255, and non-integer channels."""
        invalid_invocations = [
            ['led', '--rgb', '-1', '0', '0'],
            ['led', '--rgb', '0', '-5', '0'],
            ['led', '--rgb', '256', '0', '0'],
            ['led', '--rgb', '0', '0', '0x100'],
            ['led', '--rgb', 'not_an_int', '0', '0'],
            ['led', '--rgb', 'FF', '0', '0'],
        ]
        for argv in invalid_invocations:
            with self.subTest(argv=argv):
                with (
                    patch('sys.stderr', new_callable=io.StringIO),
                    self.assertRaises(SystemExit) as ctx,  # noqa: PT027
                ):
                    cli.main(argv)
                self.assertEqual(ctx.exception.code, 2)


class CliSelectionAndErrorTest(unittest.TestCase):
    """Tests for device selection, flag validation, LED mixing, and errors."""

    def setUp(self) -> None:
        """Patches global logging setup and libusb resource disposal."""
        super().setUp()
        self.mock_log_install = self.enterContext(patch('pw_cli.log.install'))
        self.mock_dispose_resources = self.enterContext(
            patch.object(usb.util, 'dispose_resources')
        )

    def test_select_device_succeeds_when_usb_bus_is_zero(self) -> None:
        """Verifies `--usb-bus 0` uses port lookup rather than falling back."""
        board = _make_probe_board(bus=0, port='1:4')
        fake_dev = FakeUsbDevice()
        with (
            patch(
                'rp2040_utils.device_detector.board_from_usb_port',
                return_value=board,
            ) as mock_from_port,
            patch(
                'rp2040_utils.device_detector.detect_boards',
            ) as mock_detect,
            patch.object(
                PicoDebugProbeBoardInfo, 'usb_device', return_value=fake_dev
            ),
        ):
            rc = cli.main(
                ['--usb-bus', '0', '--usb-port', '1:4', 'target', 'dut']
            )

        self.assertEqual(rc, 0)
        mock_from_port.assert_called_once_with(
            0,
            '1:4',
            include_picos=True,
            include_debug_probes=True,
        )
        mock_detect.assert_not_called()
        self.assertEqual(len(fake_dev.calls), 1)
        self.mock_dispose_resources.assert_called_once_with(fake_dev)

    def test_select_device_returns_none_on_board_lookup_value_error(
        self,
    ) -> None:
        """Verifies `cli.main` logs and returns 1 on lookup ValueError."""
        with (
            patch(
                'rp2040_utils.device_detector.board_from_usb_port',
                side_effect=ValueError(
                    'No matching device found on bus 1 port 9:9'
                ),
            ),
            self.assertLogs('debug_probe_cli', level='ERROR') as logs,
        ):
            rc = cli.main(['--usb-bus', '1', '--usb-port', '9:9', 'info'])

        self.assertEqual(rc, 1)
        self.assertTrue(
            any(
                'No matching device found on bus 1 port 9:9' in entry
                for entry in logs.output
            )
        )

    def test_select_device_returns_none_for_plain_pico_board(self) -> None:
        """Verifies `cli.main` rejects a non-probe PicoBoardInfo."""
        pico_board = PicoBoardInfo(
            bus=1,
            port='1:4',
            serial_port='/dev/ttyACM0',
            manufacturer='Raspberry Pi',
            product='Pico',
        )
        with (
            patch(
                'rp2040_utils.device_detector.board_from_usb_port',
                return_value=pico_board,
            ) as mock_from_port,
            self.assertLogs('debug_probe_cli', level='ERROR') as logs,
        ):
            rc = cli.main(['--usb-bus', '1', '--usb-port', '1:4', 'info'])

        self.assertEqual(rc, 1)
        mock_from_port.assert_called_once_with(
            1,
            '1:4',
            include_picos=True,
            include_debug_probes=True,
        )
        self.assertTrue(
            any(
                'Device on bus 1 port 1:4 is not a debug probe.' in entry
                for entry in logs.output
            )
        )

    def test_multiple_probes_non_tty_returns_one_without_prompting(
        self,
    ) -> None:
        """Verifies multiple probes with non-TTY stdin exit 1 without prompt."""
        board_first = _make_probe_board(bus=1, port='1:1', serial_number='S1')
        board_second = _make_probe_board(bus=1, port='1:2', serial_number='S2')
        with (
            patch(
                'rp2040_utils.device_detector.detect_boards',
                return_value=[board_first, board_second],
            ),
            patch('sys.stdin') as mock_stdin,
            patch.object(
                PicoDebugProbeBoardInfo, 'usb_device'
            ) as mock_usb_device,
            patch.object(cli, 'interactive_index_select') as mock_prompt,
            self.assertLogs('debug_probe_cli', level='ERROR') as logs,
        ):
            mock_stdin.isatty.return_value = False
            rc = cli.main(['target', 'dut'])

        self.assertEqual(rc, 1)
        mock_prompt.assert_not_called()
        mock_usb_device.assert_not_called()
        self.assertTrue(
            any(
                'Multiple debug probes detected in non-interactive mode.'
                in entry
                for entry in logs.output
            )
        )

    def test_multiple_probes_interactive_tty_prompts_for_selection(
        self,
    ) -> None:
        """Verifies multiple probes on an interactive TTY prompt the user."""
        board_first = _make_probe_board(bus=1, port='1:1', serial_number='S1')
        board_second = _make_probe_board(bus=1, port='1:2', serial_number='S2')
        fake_dev = FakeUsbDevice()
        expected_lines = [
            'bus 1, port 1:1 (Raspberry Pi - Debug Probe (CMSIS-DAP) - S1)',
            'bus 1, port 1:2 (Raspberry Pi - Debug Probe (CMSIS-DAP) - S2)',
        ]
        with (
            patch(
                'rp2040_utils.device_detector.detect_boards',
                return_value=[board_first, board_second],
            ),
            patch('sys.stdin') as mock_stdin,
            patch('sys.stdout', new_callable=io.StringIO),
            patch.object(
                cli,
                'interactive_index_select',
                return_value=(1, expected_lines[1]),
            ) as mock_prompt,
            patch.object(
                PicoDebugProbeBoardInfo,
                'usb_device',
                autospec=True,
                return_value=fake_dev,
            ) as mock_usb_device,
        ):
            mock_stdin.isatty.return_value = True
            rc = cli.main(['target', 'sim'])

        self.assertEqual(rc, 0)
        mock_prompt.assert_called_once_with(expected_lines)
        mock_usb_device.assert_called_once_with(board_second)
        self.assertEqual(len(fake_dev.calls), 1)
        self.assertEqual(fake_dev.calls[0]['wValue'], int(SwdTarget.SIM))

    def test_unpaired_usb_bus_or_port_exits_without_selecting_device(
        self,
    ) -> None:
        """Verifies specifying only `--usb-bus` or `--usb-port` exits 2."""
        for argv in (
            ['--usb-bus', '0', 'info'],
            ['--usb-port', '1:4', 'info'],
        ):
            with self.subTest(argv=argv):
                with (
                    patch('sys.stderr', new_callable=io.StringIO),
                    patch.object(cli, '_select_device') as mock_select,
                    self.assertRaises(SystemExit) as ctx,  # noqa: PT027
                ):
                    cli.main(argv)

                self.assertEqual(ctx.exception.code, 2)
                mock_select.assert_not_called()

    def test_led_missing_or_conflicting_flags_exit_without_selecting_device(
        self,
    ) -> None:
        """Verifies invalid `led` flag combinations fail before USB lookup."""
        invalid_invocations = [
            ['led'],
            ['led', '--off', '-r'],
            ['led', '--rgb', '10', '20', '30', '-b'],
            ['led', '--off', '--rgb', '10', '20', '30'],
        ]
        for argv in invalid_invocations:
            with self.subTest(argv=argv):
                with (
                    patch('sys.stderr', new_callable=io.StringIO) as stderr,
                    patch.object(cli, '_select_device') as mock_select,
                    self.assertRaises(SystemExit) as ctx,  # noqa: PT027
                ):
                    cli.main(argv)

                self.assertEqual(ctx.exception.code, 2)
                mock_select.assert_not_called()
                self.assertIn(
                    'Specify exactly one of --off, --rgb, or -r/-g/-b.',
                    stderr.getvalue(),
                )

    def test_led_channel_mixing_off_and_zero_rgb_issue_single_transfer(
        self,
    ) -> None:
        """Verifies `led` sends a single RGB control transfer for all modes."""
        cases = [
            (['led', '-r'], bytes((255, 0, 0))),
            (['led', '-g'], bytes((0, 255, 0))),
            (['led', '-b'], bytes((0, 0, 255))),
            (['led', '-r', '-g'], bytes((255, 255, 0))),
            (['led', '-r', '-g', '-b'], bytes((255, 255, 255))),
            (['led', '--off'], bytes((0, 0, 0))),
            (['led', '--rgb', '0', '0', '0'], bytes((0, 0, 0))),
        ]
        for argv, expected_payload in cases:
            with self.subTest(argv=argv):
                board = _make_probe_board()
                fake_dev = FakeUsbDevice()
                with (
                    patch(
                        'rp2040_utils.device_detector.detect_boards',
                        return_value=[board],
                    ),
                    patch.object(
                        PicoDebugProbeBoardInfo,
                        'usb_device',
                        return_value=fake_dev,
                    ),
                ):
                    rc = cli.main(argv)

                self.assertEqual(rc, 0)
                self.assertEqual(len(fake_dev.calls), 1)
                self.assertEqual(fake_dev.calls[0]['wIndex'], 3)
                self.assertEqual(
                    fake_dev.calls[0]['data_or_wLength'],
                    expected_payload,
                )

    def test_usb_device_value_error_returns_one_cleanly(self) -> None:
        """Verifies `device.usb_device()` ValueError logs and returns 1."""
        board = _make_probe_board()
        with (
            patch(
                'rp2040_utils.device_detector.detect_boards',
                return_value=[board],
            ),
            patch.object(
                PicoDebugProbeBoardInfo,
                'usb_device',
                side_effect=ValueError('No Pico found'),
            ),
            self.assertLogs('debug_probe_cli', level='ERROR') as logs,
        ):
            rc = cli.main(['info'])

        self.assertEqual(rc, 1)
        self.assertTrue(
            any(
                'Debug probe communication error: No Pico found' in entry
                for entry in logs.output
            )
        )

    def test_ctrl_transfer_usb_error_returns_one_and_disposes_resources(
        self,
    ) -> None:
        """Verifies USBError returns 1 and still disposes libusb resources."""
        board = _make_probe_board()
        fake_dev = FakeUsbDevice()
        self.mock_dispose_resources.reset_mock()
        with (
            patch(
                'rp2040_utils.device_detector.detect_boards',
                return_value=[board],
            ),
            patch.object(
                PicoDebugProbeBoardInfo,
                'usb_device',
                return_value=fake_dev,
            ),
            patch.object(
                fake_dev,
                'ctrl_transfer',
                side_effect=usb.core.USBError('Pipe error'),
            ),
            self.assertLogs('debug_probe_cli', level='ERROR') as logs,
        ):
            rc = cli.main(['target', 'dut'])

        self.assertEqual(rc, 1)
        self.mock_dispose_resources.assert_called_once_with(fake_dev)
        self.assertTrue(
            any(
                'Debug probe communication error:' in entry
                and 'Pipe error' in entry
                for entry in logs.output
            )
        )

    def test_usb_enumeration_errors_return_one_cleanly(self) -> None:
        """Verifies USB/backend errors during enumeration log and return 1."""

        class _InaccessibleUsbDevice:
            """Simulates a USB device whose descriptors fail udev permission."""

            @property
            def serial_number(self) -> str:
                """Simulates pyusb USBError when reading serial_number."""
                err = usb.core.USBError(
                    'Access denied (insufficient permissions)'
                )
                raise err

        with (
            patch(
                'rp2040_utils.device_detector._detect_pico_serial_ports',
                return_value={},
            ),
            patch(
                'rp2040_utils.device_detector._libusb_raspberry_pi_devices',
                return_value=[_InaccessibleUsbDevice()],
            ),
            self.assertLogs('debug_probe_cli', level='ERROR') as logs,
        ):
            rc = cli.main(['list'])

        self.assertEqual(rc, 1)
        self.assertTrue(
            any(
                'Access denied (insufficient permissions)' in entry
                for entry in logs.output
            )
        )

        for argv in (['list'], ['info']):
            with (
                self.subTest(argv=argv),
                patch(
                    'rp2040_utils.device_detector.detect_boards',
                    side_effect=usb.core.USBError('No backend available'),
                ),
                self.assertLogs('debug_probe_cli', level='ERROR') as logs,
            ):
                self.assertEqual(cli.main(argv), 1)
                self.assertTrue(
                    any(
                        'No backend available' in entry for entry in logs.output
                    )
                )

        with (
            patch(
                'rp2040_utils.device_detector.board_from_usb_port',
                side_effect=usb.core.USBError('Access denied'),
            ),
            self.assertLogs('debug_probe_cli', level='ERROR') as logs,
        ):
            rc = cli.main(['--usb-bus', '1', '--usb-port', '1:4', 'info'])

        self.assertEqual(rc, 1)
        self.assertTrue(any('Access denied' in entry for entry in logs.output))

    def test_info_malformed_payload_returns_one_cleanly(self) -> None:
        """Verifies malformed target-info JSON logs an error and returns 1."""
        board = _make_probe_board()
        fake_dev = FakeUsbDevice(target_info={'name': 'Missing Keys'})
        with (
            patch(
                'rp2040_utils.device_detector.detect_boards',
                return_value=[board],
            ),
            patch.object(
                PicoDebugProbeBoardInfo, 'usb_device', return_value=fake_dev
            ),
            self.assertLogs('debug_probe_cli', level='ERROR') as logs,
        ):
            rc = cli.main(['info'])

        self.assertEqual(rc, 1)
        self.mock_dispose_resources.assert_called_once_with(fake_dev)
        self.assertTrue(
            any('Invalid target info payload' in entry for entry in logs.output)
        )


if __name__ == '__main__':
    unittest.main()
