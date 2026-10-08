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
"""Runs the framed TCP example server and client against each other."""

import re
import subprocess
import sys
import threading
from typing import Optional

from python.runfiles import runfiles

# LINT.IfChange(listening_log)
_LISTENING = re.compile(r'Listening on port (\d+)')
# LINT.ThenChange(//pw_transport/cpp/examples/server.cc:listening_log)

_SERVER_START_TIMEOUT_S = 5.0
_CLIENT_TIMEOUT_S = 10.0


def run_harness() -> int:
    """Runs the server and client. Returns the exit status for the harness."""
    r = runfiles.Create()
    if r is None:
        print('Error: Could not find runfiles.', file=sys.stderr)
        return 1
    server_path = r.Rlocation('pigweed/pw_transport/cpp/examples/server')
    client_path = r.Rlocation('pigweed/pw_transport/cpp/examples/client')
    if not server_path or not client_path:
        print(
            'Error: Could not find the server or client binary in runfiles.',
            file=sys.stderr,
        )
        return 1

    # Port 0 makes the server pick an available port, which it logs.
    print(f'Starting server: {server_path}')
    server = subprocess.Popen(
        [server_path, '0'],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    server_lines: list[str] = []
    port: Optional[int] = None
    server_ready = threading.Event()

    def read_server_output() -> None:
        nonlocal port
        assert server.stdout is not None
        for line in server.stdout:
            server_lines.append(line)
            match = _LISTENING.search(line)
            if match and port is None:
                port = int(match.group(1))
                server_ready.set()
        server_ready.set()  # The server exited.

    reader = threading.Thread(target=read_server_output, daemon=True)
    reader.start()

    try:
        server_ready.wait(timeout=_SERVER_START_TIMEOUT_S)
        if port is None:
            print('Error: The server did not start listening.', file=sys.stderr)
            return 1

        print(f'Running client: {client_path} {port}')
        try:
            client = subprocess.run(
                [client_path, str(port)],
                capture_output=True,
                text=True,
                timeout=_CLIENT_TIMEOUT_S,
            )
        except subprocess.TimeoutExpired:
            print(
                f'Error: The client did not finish within '
                f'{_CLIENT_TIMEOUT_S} seconds.',
                file=sys.stderr,
            )
            return 1

        print('\n=== Client output ===')
        print(client.stdout, end='')
        print(client.stderr, end='', file=sys.stderr)
        if client.returncode != 0:
            print(
                f'Error: The client exited with status {client.returncode}.',
                file=sys.stderr,
            )
            return 1
        return 0

    finally:
        server.terminate()
        try:
            server.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        reader.join(timeout=1.0)
        print('\n=== Server output ===')
        print(''.join(server_lines), end='')


if __name__ == '__main__':
    sys.exit(run_harness())
