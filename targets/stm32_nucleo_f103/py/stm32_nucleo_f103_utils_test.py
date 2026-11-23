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
"""Unit tests for stm32_nucleo_f103_utils."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from stm32_nucleo_f103_utils import (
    device_detector,
    flasher,
    unit_test_client,
    unit_test_runner,
    unit_test_server,
)
from stm32_nucleo_f103_utils.device_detector import BoardInfo


# pylint: disable=protected-access
class Stm32NucleoF103UtilsTest(unittest.TestCase):
    """Tests host utility helpers for STM32 Nucleo-F103RB."""

    def test_probe_selector_with_and_without_serial(self) -> None:
        with_serial = BoardInfo(
            dev_name='/dev/ttyACM0',
            serial_number='066DFF55',
            vendor_id=0x0483,
            product_id=0x374B,
        )
        self.assertEqual(with_serial.probe_selector(), '0483:374b:066DFF55')

        without_serial = BoardInfo(
            dev_name='/dev/ttyACM0',
            serial_number=None,
            vendor_id=0x0483,
            product_id=0x374E,
        )
        self.assertEqual(without_serial.probe_selector(), '0483:374e')

    def test_select_board_explicit_probe_selector(self) -> None:
        board = flasher.select_board(
            probe='0483:374b:066DFF55', port='/dev/ttyACM1'
        )
        self.assertEqual(board.vendor_id, 0x0483)
        self.assertEqual(board.product_id, 0x374B)
        self.assertEqual(board.serial_number, '066DFF55')
        self.assertEqual(board.dev_name, '/dev/ttyACM1')

        board_two_part = flasher.select_board(
            probe='0483:3752', stlink_serial='ABC123'
        )
        self.assertEqual(board_two_part.vendor_id, 0x0483)
        self.assertEqual(board_two_part.product_id, 0x3752)
        self.assertEqual(board_two_part.serial_number, 'ABC123')

    def test_select_board_rejects_malformed_probe_selector(self) -> None:
        for invalid in (
            '',
            '0483',
            ':374b',
            '0483:',
            '0483:374b:',
            'zzzz:374b',
            '10000:374b',
        ):
            with self.subTest(probe=invalid):
                with self.assertRaises(ValueError):
                    flasher.select_board(probe=invalid)

    def test_select_board_filters_detected_boards(self) -> None:
        fake_boards = [
            BoardInfo(dev_name='/dev/ttyACM0', serial_number='SER_A'),
            BoardInfo(dev_name='/dev/ttyACM1', serial_number='SER_B'),
        ]
        with mock.patch.object(
            device_detector, 'detect_boards', return_value=fake_boards
        ):
            selected = flasher.select_board(stlink_serial='SER_B')
            self.assertEqual(selected.dev_name, '/dev/ttyACM1')

            selected_by_port = flasher.select_board(port='/dev/ttyACM0')
            self.assertEqual(selected_by_port.serial_number, 'SER_A')

        with mock.patch.object(
            device_detector, 'detect_boards', return_value=[]
        ):
            with self.assertRaises(RuntimeError):
                flasher.select_board()

    def test_resolve_binary_path_uses_build_working_directory(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = Path(tmp)
            elf = tmp_dir / 'sub' / 'firmware.elf'
            elf.parent.mkdir(parents=True)
            elf.write_bytes(b'\x7fELF')

            with mock.patch.dict(
                os.environ, {'BUILD_WORKING_DIRECTORY': str(tmp_dir)}
            ):
                resolved = flasher.resolve_binary_path(
                    Path('sub') / 'firmware.elf'
                )
                self.assertEqual(resolved, elf)

                missing = Path('sub') / 'nonexistent.elf'
                self.assertEqual(flasher.resolve_binary_path(missing), missing)

    def test_generate_runner_escapes_backslashes_and_quotes(self) -> None:
        block = unit_test_server.generate_runner(
            r'C:\tools\runner.exe',
            ['--port', 'COM3', '--flag="quoted"'],
        )
        self.assertIn('command:"C:\\\\tools\\\\runner.exe"', block)
        self.assertIn('args: "--port"', block)
        self.assertIn('args: "COM3"', block)
        self.assertIn('args: "--flag=\\"quoted\\""', block)

    def test_write_server_config_writes_valid_blocks(self) -> None:
        fake_boards = [
            BoardInfo(dev_name='/dev/ttyACM0', serial_number='SN001'),
            BoardInfo(dev_name='/dev/ttyACM1', serial_number=None),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            cfg_path = Path(tmp) / 'server.cfg'
            with mock.patch.object(
                device_detector, 'detect_boards', return_value=fake_boards
            ):
                unit_test_server.write_server_config(cfg_path)
            contents = cfg_path.read_text(encoding='utf-8')
            self.assertEqual(contents.count('runner {'), 2)
            self.assertIn('args: "/dev/ttyACM0"', contents)
            self.assertIn('args: "SN001"', contents)
            self.assertIn('args: "/dev/ttyACM1"', contents)

    def test_client_and_server_handle_missing_runfiles_gracefully(self) -> None:
        with mock.patch.object(unit_test_client, 'runfiles', None):
            self.assertEqual(
                unit_test_client._resolve_target_client_command(),
                'pw_target_runner_client',
            )
        with mock.patch.object(unit_test_server, 'runfiles', None):
            self.assertEqual(
                unit_test_server._resolve_runfile('some/path', 'fallback_bin'),
                'fallback_bin',
            )

        fake_rf = mock.Mock()
        fake_rf.Rlocation.return_value = '/nonexistent/runfiles/path/bin'
        fake_runfiles = mock.Mock()
        fake_runfiles.Create.return_value = fake_rf
        with mock.patch.object(unit_test_client, 'runfiles', fake_runfiles):
            self.assertEqual(
                unit_test_client._resolve_target_client_command(),
                'pw_target_runner_client',
            )
        with mock.patch.object(unit_test_server, 'runfiles', fake_runfiles):
            self.assertEqual(
                unit_test_server._resolve_runfile('some/path', 'fallback_bin'),
                'fallback_bin',
            )

    def test_resolve_probe_rs_prefers_cipd_over_path(self) -> None:
        """Verifies _resolve_probe_rs prefers CIPD install dir over PATH."""
        with tempfile.TemporaryDirectory() as tmp:
            cipd_bin_dir = Path(tmp) / 'bin'
            cipd_bin_dir.mkdir()
            expected_path = str(cipd_bin_dir / 'probe-rs')

            def fake_which(cmd: str, path: str | None = None) -> str | None:
                self.assertEqual(cmd, 'probe-rs')
                if path == str(cipd_bin_dir):
                    return expected_path
                return '/home/user/.cargo/bin/probe-rs'

            with (
                mock.patch.object(flasher, 'runfiles', None),
                mock.patch.object(
                    flasher.shutil, 'which', side_effect=fake_which
                ),
                mock.patch.dict(
                    os.environ, {'PW_PROBE_RS_CIPD_INSTALL_DIR': tmp}
                ),
            ):
                self.assertEqual(flasher._resolve_probe_rs(), expected_path)

    def test_resolve_probe_rs_falls_back_to_path_when_cipd_missing(
        self,
    ) -> None:
        """Verifies _resolve_probe_rs falls back to PATH if CIPD dir empty."""
        with tempfile.TemporaryDirectory() as tmp:

            def fake_which(cmd: str, path: str | None = None) -> str | None:
                self.assertEqual(cmd, 'probe-rs')
                if path is not None:
                    return None
                return '/usr/bin/probe-rs'

            with (
                mock.patch.object(flasher, 'runfiles', None),
                mock.patch.object(
                    flasher.shutil, 'which', side_effect=fake_which
                ),
                mock.patch.dict(
                    os.environ, {'PW_PROBE_RS_CIPD_INSTALL_DIR': tmp}
                ),
            ):
                self.assertEqual(
                    flasher._resolve_probe_rs(), '/usr/bin/probe-rs'
                )

    def test_download_and_reset_uses_probe_rs(self) -> None:
        """Verifies flash() invokes probe-rs download and reset."""
        board = BoardInfo(
            dev_name='/dev/ttyACM0',
            serial_number='066BFF36',
            vendor_id=0x0483,
            product_id=0x374B,
        )
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / 'firmware.elf'
            elf.write_bytes(b'\x7fELF')
            ok_proc = mock.Mock(returncode=0, stdout=b'')
            with (
                mock.patch.object(
                    flasher, '_resolve_probe_rs', return_value='/cipd/probe-rs'
                ),
                mock.patch.object(
                    flasher.subprocess, 'run', return_value=ok_proc
                ) as run_mock,
            ):
                self.assertTrue(flasher.flash(elf, board))
                self.assertEqual(run_mock.call_count, 2)
                self.assertEqual(
                    run_mock.call_args_list[0].args[0],
                    [
                        '/cipd/probe-rs',
                        'download',
                        '--probe',
                        '0483:374b:066BFF36',
                        '--chip',
                        'STM32F103RB',
                        '--protocol',
                        'swd',
                        '--connect-under-reset',
                        str(elf),
                    ],
                )
                self.assertEqual(
                    run_mock.call_args_list[0].kwargs['timeout'],
                    flasher.DEFAULT_PROBE_TIMEOUT_S,
                )
                self.assertEqual(
                    run_mock.call_args_list[1].args[0],
                    [
                        '/cipd/probe-rs',
                        'reset',
                        '--probe',
                        '0483:374b:066BFF36',
                        '--chip',
                        'STM32F103RB',
                        '--protocol',
                        'swd',
                        '--connect-under-reset',
                    ],
                )
                self.assertEqual(
                    run_mock.call_args_list[1].kwargs['timeout'],
                    flasher.DEFAULT_PROBE_TIMEOUT_S,
                )

    def test_flash_short_circuits_when_download_fails(self) -> None:
        """Verifies flash() does not reset the board if download fails."""
        board = BoardInfo(dev_name='/dev/ttyACM0', serial_number='066BFF36')
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / 'firmware.elf'
            elf.write_bytes(b'\x7fELF')
            fail_proc = mock.Mock(returncode=1, stdout=b'SWD error')
            with (
                mock.patch.object(
                    flasher, '_resolve_probe_rs', return_value='/cipd/probe-rs'
                ),
                mock.patch.object(
                    flasher.subprocess, 'run', return_value=fail_proc
                ) as run_mock,
            ):
                self.assertFalse(flasher.flash(elf, board))
                self.assertEqual(run_mock.call_count, 1)

    def test_run_probe_rs_handles_timeout_and_oserror(self) -> None:
        """Verifies _run_probe_rs catches TimeoutExpired and OSError."""
        board = BoardInfo(dev_name='/dev/ttyACM0', serial_number='066BFF36')
        with (
            mock.patch.object(
                flasher, '_resolve_probe_rs', return_value='/cipd/probe-rs'
            ),
            mock.patch.object(
                flasher.subprocess,
                'run',
                side_effect=subprocess.TimeoutExpired(
                    cmd=['/cipd/probe-rs'], timeout=1.0
                ),
            ),
        ):
            self.assertFalse(flasher.reset_board(board))

        with (
            mock.patch.object(
                flasher, '_resolve_probe_rs', return_value='/cipd/probe-rs'
            ),
            mock.patch.object(
                flasher.subprocess,
                'run',
                side_effect=OSError('permission denied'),
            ),
        ):
            self.assertFalse(flasher.reset_board(board))

    def test_detect_boards_deduplicates_dual_vcp_by_location(self) -> None:
        """Verifies composite Dual-VCP ST-LINKs select primary USB interface."""
        secondary_vcp = mock.Mock(
            device='COM10',
            location='1-2:1.4',
            serial_number='00290035',
            vid=0x0483,
            pid=0x374F,
        )
        primary_vcp = mock.Mock(
            device='COM2',
            location='1-2:1.2',
            serial_number='00290035',
            vid=0x0483,
            pid=0x374F,
        )
        second_board = mock.Mock(
            device='COM5',
            location='1-3:1.2',
            serial_number='066DFF55',
            vid=0x0483,
            pid=0x374B,
        )
        with mock.patch(
            'serial.tools.list_ports.comports',
            return_value=[secondary_vcp, second_board, primary_vcp],
        ):
            boards = device_detector.detect_boards()
        self.assertEqual(
            boards,
            [
                BoardInfo(
                    dev_name='COM2',
                    serial_number='00290035',
                    vendor_id=0x0483,
                    product_id=0x374F,
                ),
                BoardInfo(
                    dev_name='COM5',
                    serial_number='066DFF55',
                    vendor_id=0x0483,
                    product_id=0x374B,
                ),
            ],
        )

    def test_select_board_raises_on_unmatched_port_without_serial(self) -> None:
        """Verifies --port alone raises if no detected ST-LINK VCP matches."""
        fake_boards = [
            BoardInfo(dev_name='/dev/ttyACM0', serial_number='SER_A')
        ]
        with mock.patch.object(
            device_detector, 'detect_boards', return_value=fake_boards
        ):
            with self.assertRaises(RuntimeError):
                flasher.select_board(port='/dev/ttyACM99')

            external_uart = flasher.select_board(
                port='/dev/ttyUSB0', stlink_serial='SER_EXT'
            )
            self.assertEqual(external_uart.dev_name, '/dev/ttyUSB0')
            self.assertEqual(external_uart.serial_number, 'SER_EXT')

    def test_run_device_test_passes_on_clean_output(self) -> None:
        """Verifies run_device_test returns True when all tests pass."""
        board = BoardInfo(dev_name='/dev/ttyACM0', serial_number='SER_A')
        lines = [
            b'[==========] Running all tests.\r\n',
            b'[       OK ] Suite.Case\r\n',
            b'[==========] Done running all tests.\r\n',
            b'',
        ]
        fake_ser = mock.MagicMock()
        fake_ser.__enter__.return_value = fake_ser
        fake_ser.readline.side_effect = lines
        clock = iter([0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6])
        with (
            mock.patch.object(
                unit_test_runner, 'select_board', return_value=board
            ),
            mock.patch.object(
                unit_test_runner, 'download_binary', return_value=True
            ),
            mock.patch.object(
                unit_test_runner, 'reset_board', return_value=True
            ),
            mock.patch.object(
                unit_test_runner.serial, 'Serial', return_value=fake_ser
            ),
            mock.patch.object(
                unit_test_runner.time, 'monotonic', side_effect=clock
            ),
        ):
            self.assertTrue(
                unit_test_runner.run_device_test(Path('status_test.elf'))
            )

    def test_run_device_test_fails_when_failure_banner_present(self) -> None:
        """Verifies run_device_test returns False when [  FAILED  ] appears."""
        board = BoardInfo(dev_name='/dev/ttyACM0', serial_number='SER_A')
        lines = [
            b'[==========] Running all tests.\r\n',
            b'[  FAILED  ] Suite.Broken\r\n',
            b'[==========] Done running all tests.\r\n',
            b'',
        ]
        fake_ser = mock.MagicMock()
        fake_ser.__enter__.return_value = fake_ser
        fake_ser.readline.side_effect = lines
        clock = iter([0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6])
        with (
            mock.patch.object(
                unit_test_runner, 'select_board', return_value=board
            ),
            mock.patch.object(
                unit_test_runner, 'download_binary', return_value=True
            ),
            mock.patch.object(
                unit_test_runner, 'reset_board', return_value=True
            ),
            mock.patch.object(
                unit_test_runner.serial, 'Serial', return_value=fake_ser
            ),
            mock.patch.object(
                unit_test_runner.time, 'monotonic', side_effect=clock
            ),
        ):
            self.assertFalse(
                unit_test_runner.run_device_test(Path('status_test.elf'))
            )

    def test_run_device_test_fails_on_timeout_before_done_banner(self) -> None:
        """Verifies run_device_test returns False if done banner never comes."""
        board = BoardInfo(dev_name='/dev/ttyACM0', serial_number='SER_A')
        fake_ser = mock.MagicMock()
        fake_ser.__enter__.return_value = fake_ser
        fake_ser.readline.side_effect = [
            b'[==========] Running all tests.\r\n',
            b'',
        ]
        clock = iter([0.0, 0.1, 0.5, 2.0])
        with (
            mock.patch.object(
                unit_test_runner, 'select_board', return_value=board
            ),
            mock.patch.object(
                unit_test_runner, 'download_binary', return_value=True
            ),
            mock.patch.object(
                unit_test_runner, 'reset_board', return_value=True
            ),
            mock.patch.object(
                unit_test_runner.serial, 'Serial', return_value=fake_ser
            ),
            mock.patch.object(
                unit_test_runner.time, 'monotonic', side_effect=clock
            ),
        ):
            self.assertFalse(
                unit_test_runner.run_device_test(
                    Path('status_test.elf'), test_timeout=1.0
                )
            )

    def test_run_device_test_fails_when_start_banner_missing(self) -> None:
        """Verifies stale done banner without start banner returns False."""
        board = BoardInfo(dev_name='/dev/ttyACM0', serial_number='SER_A')
        fake_ser = mock.MagicMock()
        fake_ser.__enter__.return_value = fake_ser
        fake_ser.readline.side_effect = [
            b'[==========] Done running all tests.\r\n',
            b'',
        ]
        clock = iter([0.0, 0.1, 0.5, 2.0])
        with (
            mock.patch.object(
                unit_test_runner, 'select_board', return_value=board
            ),
            mock.patch.object(
                unit_test_runner, 'download_binary', return_value=True
            ),
            mock.patch.object(
                unit_test_runner, 'reset_board', return_value=True
            ),
            mock.patch.object(
                unit_test_runner.serial, 'Serial', return_value=fake_ser
            ),
            mock.patch.object(
                unit_test_runner.time, 'monotonic', side_effect=clock
            ),
        ):
            self.assertFalse(
                unit_test_runner.run_device_test(
                    Path('status_test.elf'), test_timeout=1.0
                )
            )

    def test_run_device_test_ignores_pre_reset_done_banner_across_sleep(
        self,
    ) -> None:
        """Verifies pre-reset done banner does not cut off post-reset sleep."""
        board = BoardInfo(dev_name='/dev/ttyACM0', serial_number='SER_A')
        fake_ser = mock.MagicMock()
        fake_ser.__enter__.return_value = fake_ser
        fake_ser.readline.side_effect = [
            b'[==========] Running all tests.\r\n',
            b'[  FAILED  ] StalePreReset.Failure\r\n',
            b'[==========] Done running all tests.\r\n',
            b'[==========] Running all tests.\r\n',
            b'',  # 250 ms serial timeout during on-device test sleep
            b'[       OK ] PostReset.SleepTest\r\n',
            b'[==========] Done running all tests.\r\n',
            b'',
        ]
        now = 0.0

        def fake_monotonic() -> float:
            nonlocal now
            now += 0.1
            return now

        with (
            mock.patch.object(
                unit_test_runner, 'select_board', return_value=board
            ),
            mock.patch.object(
                unit_test_runner, 'download_binary', return_value=True
            ),
            mock.patch.object(
                unit_test_runner, 'reset_board', return_value=True
            ),
            mock.patch.object(
                unit_test_runner.serial, 'Serial', return_value=fake_ser
            ),
            mock.patch.object(
                unit_test_runner.time, 'monotonic', side_effect=fake_monotonic
            ),
        ):
            self.assertTrue(
                unit_test_runner.run_device_test(
                    Path('sleep_test.elf'), test_timeout=5.0
                )
            )
            self.assertEqual(fake_ser.readline.call_count, 8)

    def test_rejects_invalid_serial_numbers(self) -> None:
        """Verifies serials with invalid characters are rejected."""
        with self.assertRaises(ValueError):
            flasher.select_board(stlink_serial='066B:extra')
        with self.assertRaises(ValueError):
            flasher.select_board(probe='0483:374b:066B;bad')
        bad_board = BoardInfo(
            dev_name='/dev/ttyACM0',
            serial_number='066B:bad',
        )
        self.assertFalse(flasher.reset_board(bad_board))

        bad_usb = mock.Mock(
            device='/dev/ttyACM0',
            serial_number='066B bad serial',
            vid=0x0483,
            pid=0x374B,
        )
        with mock.patch(
            'serial.tools.list_ports.comports', return_value=[bad_usb]
        ):
            boards = device_detector.detect_boards()
            self.assertEqual(len(boards), 1)
            self.assertIsNone(boards[0].serial_number)


if __name__ == '__main__':
    unittest.main()
