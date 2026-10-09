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
"""Runs pw_rpc2 example servers and clients against each other.

Runs every client against every server, and compares the output of both
programs to the expected output.
"""

import argparse
import difflib
import os
from pathlib import Path
import pty
import re
import subprocess
import sys
import threading
import time
from typing import Optional

_LISTENING = re.compile(r'Listening on port (\d+)')
_COLOR_CODE = re.compile(r'\x1b\[[0-9;]*m')

_SERVER_START_TIMEOUT_S = 5.0
_CLIENT_TIMEOUT_S = 10.0


class _Program:
    """Runs a program and collects its output.

    The program writes to a pseudo-terminal, so it flushes each line of output
    immediately instead of when it exits. Servers run until they are
    terminated, so they never exit on their own.
    """

    def __init__(self, *command: str) -> None:
        controller, terminal = pty.openpty()
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=terminal,
            stderr=terminal,
        )
        os.close(terminal)
        self._controller = controller
        self._output = bytearray()
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._read_output)
        self._reader.start()

    def _read_output(self) -> None:
        while True:
            try:
                data = os.read(self._controller, 4096)
            except OSError:  # Linux raises EIO once the program exits.
                break
            if not data:
                break
            with self._lock:
                self._output += data
        os.close(self._controller)

    def output(self) -> str:
        with self._lock:
            return self._output.decode(errors='replace').replace('\r\n', '\n')

    def wait(self, timeout: Optional[float] = None) -> int:
        """Waits for the program to exit and returns its exit code."""
        returncode = self.process.wait(timeout)
        self._reader.join()
        return returncode


def _wait_for_port(server: _Program) -> Optional[int]:
    deadline = time.monotonic() + _SERVER_START_TIMEOUT_S
    while time.monotonic() < deadline:
        match = _LISTENING.search(server.output())
        if match:
            return int(match.group(1))
        if server.process.poll() is not None:
            return None
        time.sleep(0.01)
    return None


def _run(server_path: str, client_path: str) -> tuple[bool, str, str]:
    """Runs a client against a server.

    Returns whether the client ran and succeeded, and the output of the client
    and the server.
    """
    server = _Program(server_path, '0')
    client: Optional[_Program] = None
    try:
        port = _wait_for_port(server)
        if port is None:
            print('Error: The server did not start listening.', file=sys.stderr)
        else:
            client = _Program(client_path, str(port))
            try:
                client.wait(_CLIENT_TIMEOUT_S)
            except subprocess.TimeoutExpired:
                print(
                    f'Error: The client did not finish within '
                    f'{_CLIENT_TIMEOUT_S} seconds.',
                    file=sys.stderr,
                )
                client.process.kill()
                client.wait()
    finally:
        server.process.terminate()
        server.wait()

    if client is None:
        return False, '', server.output()

    if client.process.returncode != 0:
        print(
            f'Error: The client exited with status '
            f'{client.process.returncode}.',
            file=sys.stderr,
        )
    return client.process.returncode == 0, client.output(), server.output()


def _matches(name: str, output: str, expected_file: Path) -> bool:
    """Compares output to an expected output file, ignoring color codes and
    the server's port."""
    actual = [
        _LISTENING.sub('Listening on port <port>', _COLOR_CODE.sub('', line))
        for line in output.splitlines()
    ]
    expected = expected_file.read_text().splitlines()
    if actual == expected:
        return True

    print(
        f'Error: The {name} output does not match {expected_file}:',
        file=sys.stderr,
    )
    for line in difflib.unified_diff(
        expected, actual, 'expected', 'actual', lineterm=''
    ):
        print(line, file=sys.stderr)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--servers', nargs='+', required=True)
    parser.add_argument('--clients', nargs='+', required=True)
    parser.add_argument('--expected-server-output', type=Path, required=True)
    parser.add_argument('--expected-client-output', type=Path, required=True)
    args = parser.parse_args()

    passed = True
    for server in args.servers:
        for client in args.clients:
            print(f'=== {Path(client).name} -> {Path(server).name} ===')
            succeeded, client_output, server_output = _run(server, client)
            print(f'--- Client output ---\n{client_output}')
            print(f'--- Server output ---\n{server_output}')

            passed &= succeeded
            passed &= _matches(
                'client', client_output, args.expected_client_output
            )
            passed &= _matches(
                'server', server_output, args.expected_server_output
            )

    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
