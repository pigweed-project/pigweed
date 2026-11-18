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
"""Tests for the command line interface"""

import os
import signal
import subprocess
import sys
import time
import unittest

from pathlib import Path

from mock_emu_frontend import _mock_emu
from config_helper import ConfigHelper


# TODO: b/301382004 - The Python Pigweed package install (into python-venv)
# races with running this test and there is no way to add that package as a test
# depedency without creating circular depedencies. This means we can't rely on
# using Pigweed tools like pw cli or the arm-none-eabi-gdb wrapper.
#
# Run the CLI directly instead of going through pw cli.
_cli_path = Path(
    os.path.join(os.environ['PW_ROOT'], 'pw_emu', 'py', 'pw_emu', '__main__.py')
).resolve()


class TestCli(ConfigHelper):
    """Test non-interactive commands"""

    _config = {
        'emulators': {
            'mock-emu': {
                'launcher': 'mock_emu_frontend.MockEmuLauncher',
                'connector': 'mock_emu_frontend.MockEmuConnector',
            }
        },
        'mock-emu': {
            'tcp_channel': True,
            'gdb_channel': True,
        },
        'gdb': _mock_emu + ['--exit', '--'],
        'targets': {'test-target': {'mock-emu': {}}},
    }

    def _build_cmd(self, args: list[str]) -> list[str]:
        cmd = [
            'python',
            str(_cli_path),
            '--working-dir',
            self._wdir.name,
            '--config',
            self._config_file,
        ] + args
        return cmd

    def _run(self, args: list[str], **kwargs) -> subprocess.CompletedProcess:
        """Run the CLI and wait for completion"""
        return subprocess.run(self._build_cmd(args), **kwargs)

    def _popen(self, args: list[str], **kwargs) -> subprocess.Popen:
        """Run the CLI in the background"""
        return subprocess.Popen(self._build_cmd(args), **kwargs)


class TestNonInteractive(TestCli):
    """Test non interactive commands."""

    def setUp(self) -> None:
        super().setUp()
        self.assertEqual(self._run(['start', 'test-target']).returncode, 0)

    def tearDown(self) -> None:
        self.assertEqual(self._run(['stop']).returncode, 0)
        super().tearDown()

    def test_already_running(self) -> None:
        self.assertNotEqual(self._run(['start', 'test-target']).returncode, 0)

    def test_gdb_cmds(self) -> None:
        status = self._run(
            ['gdb-cmds', 'show version'],
        )
        self.assertEqual(status.returncode, 0)

    def test_prop_ls(self) -> None:
        status = self._run(['prop-ls', 'path1'], stdout=subprocess.PIPE)
        self.assertEqual(status.returncode, 0)
        self.assertTrue('prop1' in status.stdout.decode('ascii'))
        status = self._run(['prop-ls', 'invalid path'], stdout=subprocess.PIPE)
        self.assertNotEqual(status.returncode, 0)

    def test_prop_get(self) -> None:
        status = self._run(
            ['prop-get', 'invalid path', 'prop1'],
            stdout=subprocess.PIPE,
        )
        self.assertNotEqual(status.returncode, 0)
        status = self._run(
            ['prop-get', 'path1', 'invalid prop'],
            stdout=subprocess.PIPE,
        )
        self.assertNotEqual(status.returncode, 0)
        status = self._run(
            ['prop-get', 'path1', 'prop1'],
            stdout=subprocess.PIPE,
        )
        self.assertEqual(status.returncode, 0)
        self.assertTrue('val1' in status.stdout.decode('ascii'))

    def test_prop_set(self) -> None:
        status = self._run(
            ['prop-set', 'invalid path', 'prop1', 'v'],
            stdout=subprocess.PIPE,
        )
        self.assertNotEqual(status.returncode, 0)
        status = self._run(
            ['prop-set', 'path1', 'invalid prop', 'v'],
            stdout=subprocess.PIPE,
        )
        self.assertNotEqual(status.returncode, 0)
        status = self._run(
            ['prop-set', 'path1', 'prop1', 'value'],
            stdout=subprocess.PIPE,
        )
        self.assertEqual(status.returncode, 0)
        status = self._run(
            ['prop-get', 'path1', 'prop1'],
            stdout=subprocess.PIPE,
        )
        self.assertEqual(status.returncode, 0)
        self.assertTrue('value' in status.stdout.decode('ascii'), status.stdout)

    def test_reset(self) -> None:
        self.assertEqual(self._run(['reset']).returncode, 0)
        self.assertTrue(os.path.exists(os.path.join(self._wdir.name, 'reset')))

    def test_load(self) -> None:
        self.assertEqual(self._run(['load', 'executable']).returncode, 0)

    def test_resume(self) -> None:
        self.assertEqual(self._run(['resume']).returncode, 0)


class TestForeground(TestCli):
    """Test starting in foreground"""

    def _test_common(self, cmd) -> None:
        # Run the CLI process in a new session so that we can terminate both the
        # CLI and the mock emulator it spawns in the foreground.
        args = {}
        if sys.platform == 'win32':
            args['creationflags'] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            args['start_new_session'] = True
        proc = self._popen(cmd, stdout=subprocess.PIPE, **args)
        assert proc.stdout
        output = proc.stdout.readline()
        self.assertTrue(
            'starting mock emulator' in output.decode('utf-8'),
            output.decode('utf-8'),
        )
        if sys.platform == 'win32':
            # See https://bugs.python.org/issue26350
            os.kill(proc.pid, signal.CTRL_BREAK_EVENT)
        else:
            os.kill(-proc.pid, signal.SIGTERM)
        proc.wait()
        proc.stdout.close()

    def test_foreground(self) -> None:
        self._test_common(['start', '--foreground', 'test-target'])

    def test_debug(self) -> None:
        self._test_common(['start', '--debug', 'test-target'])


class TestInteractive(TestCli):
    """Test interactive commands"""

    def setUp(self) -> None:
        super().setUp()
        self.assertEqual(self._run(['start', 'test-target']).returncode, 0)

    def tearDown(self) -> None:
        self.assertEqual(self._run(['stop']).returncode, 0)
        super().tearDown()

    @staticmethod
    def _read_nonblocking(fd: int, size: int) -> bytes:
        try:
            return os.read(fd, size)
        except BlockingIOError:
            return b''

    def test_term(self) -> None:
        """Test the pw emu term command"""

        if sys.platform == 'win32':
            self.skipTest('pty not supported on win32')

        # pylint: disable=import-outside-toplevel
        # Can't import pty on win32.
        import pty

        # pylint: disable=no-member
        # Avoid pylint false positive on win32.
        pid, fd = pty.fork()  # pyrefly: ignore[missing-attribute]
        if pid == 0:
            status = self._run(['term', 'tcp'])
            # pylint: disable=protected-access
            # Use os._exit instead of os.exit after fork.
            os._exit(status.returncode)
        else:
            expected = '--- Miniterm on tcp ---'

            # Read the expected string with a timeout.
            os.set_blocking(fd, False)  # pyrefly: ignore[missing-attribute]
            deadline = time.monotonic() + 5
            data = self._read_nonblocking(fd, len(expected))
            while len(data) < len(expected):
                time.sleep(0.1)
                data += self._read_nonblocking(fd, len(expected) - len(data))
                if time.monotonic() > deadline:
                    break
            self.assertTrue(
                expected in data.decode('ascii'),
                data + self._read_nonblocking(fd, 100),
            )

            # send CTRL + ']' to terminate miniterm
            os.write(fd, b'\x1d')

            # wait for the process to exit, with a timeout
            deadline = time.monotonic() + 5
            wait_pid, ret = os.waitpid(
                pid,
                os.WNOHANG,  # pyrefly: ignore[missing-attribute]
            )
            while wait_pid == 0:
                time.sleep(0.1)
                # Discard input to avoid writer hang on MacOS,
                # see https://github.com/python/cpython/issues/97001.
                try:
                    self._read_nonblocking(fd, 100)
                except OSError:
                    # Avoid read errors when the child pair of the pty
                    # closes when the child terminates.
                    pass
                wait_pid, ret = os.waitpid(
                    pid,
                    os.WNOHANG,  # pyrefly: ignore[missing-attribute]
                )
                if time.monotonic() > deadline:
                    break
            self.assertEqual(wait_pid, pid)
            self.assertEqual(ret, 0)

    def test_gdb(self) -> None:
        res = self._run(['gdb', '-e', 'executable'], stdout=subprocess.PIPE)
        self.assertEqual(res.returncode, 0)
        output = res.stdout.decode('ascii')
        self.assertTrue('target remote' in output, output)
        self.assertTrue('executable' in output, output)


if __name__ == '__main__':
    unittest.main()
