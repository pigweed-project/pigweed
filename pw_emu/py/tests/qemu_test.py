#!/usr/bin/env python
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
"""QEMU emulator tests."""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest

from pathlib import Path
from typing import Any

from pw_emu.core import InvalidChannelName, InvalidChannelType
from config_helper import check_prog, ConfigHelperWithEmulator


# TODO: b/301382004 - The Python Pigweed package install (into python-venv)
# races with running this test and there is no way to add that package as a test
# depedency without creating circular depedencies. This means we can't rely on
# using Pigweed tools like pw cli or the arm-none-eabi-gdb wrapper.
#
# run the arm_gdb.py wrapper directly
_arm_none_eabi_gdb_path = Path(
    os.path.join(
        os.environ['PW_ROOT'],
        'pw_env_setup',
        'py',
        'pw_env_setup',
        'entry_points',
        'arm_gdb.py',
    )
).resolve()


class TestQemu(ConfigHelperWithEmulator):
    """Tests for a valid qemu configuration."""

    _config = {
        'gdb': ['python', str(_arm_none_eabi_gdb_path)],
        'qemu': {
            'executable': 'qemu-system-arm',
        },
        'targets': {
            'test-target': {
                'ignore1': None,
                'qemu': {
                    'machine': 'lm3s6965evb',
                    'channels': {
                        'chardevs': {
                            'test_uart': {
                                'id': 'serial0',
                            }
                        }
                    },
                },
                'ignore2': None,
            }
        },
    }

    def setUp(self) -> None:
        super().setUp()
        # No image so start paused to avoid crashing.
        self._emu.start(target='test-target', pause=True)

    def tearDown(self) -> None:
        self._emu.stop()
        super().tearDown()

    def test_running(self) -> None:
        self.assertTrue(self._emu.running())

    def test_list_properties(self) -> None:
        self.assertIsNotNone(self._emu.list_properties('/machine'))

    def test_get_property(self) -> None:
        self.assertEqual(
            self._emu.get_property('/machine', 'type'), 'lm3s6965evb-machine'
        )

    def test_set_property(self) -> None:
        self._emu.set_property('/machine', 'graphics', False)
        self.assertFalse(self._emu.get_property('/machine', 'graphics'))

    def test_bad_channel_name(self) -> None:
        with self.assertRaises(InvalidChannelName):
            self._emu.get_channel_addr('serial1')

    def get_reg(self, addr: int) -> bytes:
        temp = tempfile.NamedTemporaryFile(delete=False)
        temp.close()

        res = self._emu.run_gdb_cmds(
            [
                f'dump val {temp.name} *(char*){addr}',
                'disconnect',
            ]
        )
        self.assertEqual(res.returncode, 0, res.stderr.decode('ascii'))

        with open(temp.name, 'rb') as file:
            ret = file.read(1)

        self.assertNotEqual(ret, b'', res.stderr.decode('ascii'))

        os.unlink(temp.name)

        return ret

    def poll_data(self, timeout: int) -> bytes | None:
        uartris = 0x4000C03C
        uartrd = 0x4000C000

        deadline = time.monotonic() + timeout
        while self.get_reg(uartris) == b'\x00':
            time.sleep(0.1)
            if time.monotonic() > deadline:
                return None
        return self.get_reg(uartrd)

    def test_channel_stream(self) -> None:
        ok, msg = check_prog('arm-none-eabi-gdb')
        if not ok:
            self.skipTest(msg)

        stream = self._emu.get_channel_stream('test_uart')
        stream.write('test\n'.encode('ascii'))

        self.assertEqual(self.poll_data(5), b't')
        self.assertEqual(self.poll_data(5), b'e')
        self.assertEqual(self.poll_data(5), b's')
        self.assertEqual(self.poll_data(5), b't')

    def test_gdb(self) -> None:
        self._emu.run_gdb_cmds(['c'])
        deadline = time.monotonic() + 5
        while self._emu.running():
            if time.monotonic() > deadline:
                return
        self.assertFalse(self._emu.running())


class TestQemuChannelsTcp(TestQemu):
    """Tests for configurations using TCP channels."""

    _config: dict[str, Any] = {}
    _config.update(json.loads(json.dumps(TestQemu._config)))
    _config['qemu']['channels'] = {'type': 'tcp'}

    def test_get_channel_addr(self) -> None:
        host, port = self._emu.get_channel_addr('test_uart')
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        sock.close()


class TestQemuChannelsPty(TestQemu):
    """Tests for configurations using PTY channels."""

    _config: dict[str, Any] = {}
    _config.update(json.loads(json.dumps(TestQemu._config)))
    _config['qemu']['channels'] = {'type': 'pty'}

    def _get_gdb_version(self) -> str:
        gdb_cmd = self._config.get('gdb', [])
        if not gdb_cmd:
            return ''
        gdb_cmd = gdb_cmd.copy()
        gdb_cmd.append('--version')
        try:
            proc = subprocess.run(gdb_cmd, capture_output=True, text=True)
            if proc.returncode == 0 and proc.stdout:
                first_line = proc.stdout.splitlines()[0]
                version_str = first_line.split()[-1]
                return version_str
        except (
            subprocess.SubprocessError,
            OSError,
            IndexError,
            ValueError,
        ):
            pass
        return ''

    def _apply_gdb_version_workaround(self) -> None:
        # Apply a workaround for GDB spinning at 100% CPU when making remote
        # connections over PTY. As this problem seems to be specific to the
        # version of GDB used (included with the ARM GCC 15 toolchain), we
        # apply the workaround for that version only.

        # Detect the problematic version
        version_str = self._get_gdb_version()
        if version_str == '16.3.90.20250906-git':
            # Use tcp when connecting with gdb as the workaround.
            self._config['qemu']['channels']['gdb'] = {
                'type': 'tcp',
            }

    def setUp(self):
        if sys.platform == 'win32':
            self.skipTest('pty not supported on win32')

        self._apply_gdb_version_workaround()
        super().setUp()

    def test_get_path(self) -> None:
        self.assertTrue(os.path.exists(self._emu.get_channel_path('test_uart')))


class TestQemuInvalidChannelType(ConfigHelperWithEmulator):
    """Test invalid channel type configuration."""

    _config = {
        'qemu': {
            'executable': 'qemu-system-arm',
            'channels': {'type': 'invalid'},
        },
        'targets': {
            'test-target': {
                'qemu': {
                    'machine': 'lm3s6965evb',
                }
            }
        },
    }

    def test_start(self) -> None:
        with self.assertRaises(InvalidChannelType):
            self._emu.start('test-target', pause=True)


class TestQemuTargetChannelsMixed(ConfigHelperWithEmulator):
    """Test configuration with mixed channels types."""

    _config = {
        'qemu': {
            'executable': 'qemu-system-arm',
        },
        'targets': {
            'test-target': {
                'qemu': {
                    'machine': 'lm3s6965evb',
                    'channels': {
                        'chardevs': {
                            'test_uart0': {
                                'id': 'serial0',
                            },
                            'test_uart1': {
                                'id': 'serial1',
                                'type': 'tcp',
                            },
                            'test_uart2': {
                                'id': 'serial2',
                                'type': 'pty',
                            },
                        }
                    },
                }
            }
        },
    }

    def setUp(self) -> None:
        if sys.platform == 'win32':
            self.skipTest('pty not supported on win32')
        super().setUp()
        # no image to run so start paused
        self._emu.start('test-target', pause=True)

    def tearDown(self) -> None:
        self._emu.stop()
        super().tearDown()

    def test_uart0_addr(self) -> None:
        host, port = self._emu.get_channel_addr('test_uart0')
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        sock.close()

    def test_uart1_addr(self) -> None:
        host, port = self._emu.get_channel_addr('test_uart1')
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        sock.close()

    def test_uart2_path(self) -> None:
        self.assertTrue(
            os.path.exists(self._emu.get_channel_path('test_uart2'))
        )


def main() -> None:
    ok, msg = check_prog('qemu-system-arm')
    if not ok:
        print(f'skipping tests: {msg}')
        sys.exit(0)

    unittest.main()


if __name__ == '__main__':
    main()
