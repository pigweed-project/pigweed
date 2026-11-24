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
"""Starts a fan-in fan-out aggregator server and 3 downstream temperature nodes.

cluster_coro.py and cluster_polling.py call main() with the server to run.
"""

import os
from pathlib import Path
import pty
import re
import signal
import subprocess
import sys
import threading
import time
from typing import Optional, Union

_LISTENING = re.compile(r'Listening on port (\d+)')
_START_TIMEOUT_S = 5.0
_TEMPERATURES = ('15', '20', '25')


class _Process:
    """Runs a subprocess attached to a pseudo-terminal."""

    def __init__(
        self, *command: Union[str, Path], stream_to_stdout: bool = False
    ) -> None:
        controller, terminal = pty.openpty()
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=terminal,
            stderr=terminal,
        )
        os.close(terminal)
        self._controller = controller
        self._stream_to_stdout = stream_to_stdout
        self._output = bytearray()
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._read_output)
        self._reader.start()

    def _read_output(self) -> None:
        while True:
            try:
                data = os.read(self._controller, 4096)
            except OSError:
                break
            if not data:
                break
            with self._lock:
                self._output += data
            if self._stream_to_stdout:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
        os.close(self._controller)

    def output(self) -> str:
        with self._lock:
            return self._output.decode(errors='replace').replace('\r\n', '\n')

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self._reader.join()


def _wait_for_port(proc: _Process) -> Optional[int]:
    deadline = time.monotonic() + _START_TIMEOUT_S
    while time.monotonic() < deadline:
        match = _LISTENING.search(proc.output())
        if match:
            return int(match.group(1))
        if proc.process.poll() is not None:
            return None
        time.sleep(0.01)
    return None


def _find_binary(name: str) -> Path:
    for base in (
        Path(sys.argv[0]).parent,
        Path(__file__).parent,
        Path(sys.argv[0]).resolve().parent,
    ):
        candidate = base / name
        if candidate.exists():
            return candidate
    raise FileNotFoundError(f'Could not find binary: {name}')


def main(server_name: str) -> int:
    """Runs the aggregator server named server_name and 3 downstream nodes.

    The server listens on the port given as the first command-line argument, or
    8000 by default. Runs until SIGTERM or SIGINT, or until the server exits.
    """
    port = sys.argv[1] if len(sys.argv) > 1 else '8000'

    node_bin = _find_binary('node')
    server_bin = _find_binary(server_name)

    stop_event = threading.Event()

    def handle_signal(signum: int, _frame: object) -> None:
        del signum
        stop_event.set()

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    nodes: list[_Process] = []
    server: Optional[_Process] = None
    try:
        node_ports: list[str] = []
        for temp in _TEMPERATURES:
            node = _Process(node_bin, '0', temp)
            nodes.append(node)
            node_port = _wait_for_port(node)
            if node_port is None:
                print(
                    'Error: Downstream node did not start listening.',
                    file=sys.stderr,
                )
                return 1
            node_ports.append(str(node_port))

        server = _Process(server_bin, port, *node_ports, stream_to_stdout=True)
        while not stop_event.is_set() and server.process.poll() is None:
            stop_event.wait(timeout=0.05)

        if not stop_event.is_set():
            returncode = server.process.poll()
            return returncode if returncode else 1

        return 0
    finally:
        if server is not None:
            server.stop()
        for node in nodes:
            node.stop()
            sys.stdout.write(node.output())
        sys.stdout.flush()
