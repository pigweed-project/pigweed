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

import socket
import subprocess
import sys
import time
import unittest
from python.runfiles import runfiles


class AdventureIntegrationTest(unittest.TestCase):
    def find_free_port(self) -> int:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
        s.close()
        return port

    def read_until(
        self, sock: socket.socket, expected: bytes, timeout_sec: float = 10.0
    ) -> bytes:
        sock.settimeout(0.1)
        buf = bytearray()
        start_time = time.time()
        while time.time() - start_time < timeout_sec:
            try:
                chunk = sock.recv(1024)
                if chunk:
                    print(f"SOCKET READ CHUNK: {chunk!r}", flush=True)
                    buf.extend(chunk)
                    if expected in buf:
                        return bytes(buf)
            except socket.timeout:
                continue
            except socket.error as e:
                self.fail(f"Socket error: {e}")

        self.fail(
            f"Timed out waiting for {expected.decode()!r}. Current buffer: {buf.decode(errors='replace')!r}"
        )

    def test_adventure_end_to_end(self):
        r = runfiles.Create()
        self.assertIsNotNone(r, "Failed to initialize Bazel runfiles")

        qemu_runner_path = r.Rlocation("pigweed/pw_kernel/tooling/qemu")
        image_path = r.Rlocation(
            "_main/targets/riscv_qemu/hardware/adventure.elf"
        )

        self.assertIsNotNone(
            qemu_runner_path, "Could not find qemu runner in runfiles"
        )
        self.assertIsNotNone(
            image_path, "Could not find adventure system image in runfiles"
        )

        port = self.find_free_port()
        print(f"Exposing QEMU serial console over port: {port}")

        # Launch QEMU in a subprocess
        qemu_cmd = [
            qemu_runner_path,
            "--cpu",
            "rv32",
            "--machine",
            "virt",
            "--semihosting",
            "--image",
            image_path,
            "--serial-socket",
            str(port),
        ]

        print(f"Invoking QEMU runner: {qemu_cmd}", flush=True)
        proc = subprocess.Popen(
            qemu_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        import threading

        def log_pipe(pipe, label):
            for line in iter(pipe.readline, b''):
                print(
                    f"[{label}] {line.decode(errors='replace').rstrip()}",
                    flush=True,
                )

        stdout_thread = threading.Thread(
            target=log_pipe, args=(proc.stdout, "QEMU STDOUT"), daemon=True
        )
        stderr_thread = threading.Thread(
            target=log_pipe, args=(proc.stderr, "QEMU STDERR"), daemon=True
        )
        stdout_thread.start()
        stderr_thread.start()

        # Connect to QEMU serial TCP port
        sock = None
        connected = False
        retries = 50
        while retries > 0:
            if proc.poll() is not None:
                self.fail(
                    f"QEMU process exited early with code {proc.returncode}."
                )

            print(
                f"Connection attempt {51 - retries}/50 to localhost:{port}...",
                flush=True,
            )
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                sock.connect(('127.0.0.1', port))
                connected = True
                break
            except ConnectionRefusedError:
                sock.close()
                time.sleep(0.1)
                retries -= 1

        if not connected:
            proc.kill()
            stdout, stderr = proc.communicate()
            print(f"QEMU stdout: {stdout.decode()}")
            print(f"QEMU stderr: {stderr.decode()}")
            self.fail("Failed to connect to QEMU serial socket")

        try:
            # 1. Wait for the boot welcome messages on the UART socket
            print("Waiting for Adventure Game welcome messages...", flush=True)
            self.read_until(sock, b"Welcome to Text Adventure!")
            self.read_until(sock, b"You are in a dark room.")

            # 2. Type "help"
            print("Sending 'help' command...", flush=True)
            sock.sendall(b"help\n")
            self.read_until(sock, b"Available commands: help, look, go north")

            # 3. Type "look"
            print("Sending 'look' command...", flush=True)
            sock.sendall(b"look\n")
            self.read_until(sock, b"You see dusty walls")

            # 4. Type "go north"
            print("Sending 'go north' command...", flush=True)
            sock.sendall(b"go north\n")
            self.read_until(sock, b"The door is locked.")

            # 5. Type "crash" to trigger exception and self-healing restart
            print(
                "Sending 'crash' command to trigger Load Access Fault...",
                flush=True,
            )
            sock.sendall(b"crash\n")

            # 6. Verify the supervisor catches the crash and restarts the game
            # Symmetrical Check: wait for the new boot welcome messages again!
            print(
                "Verifying Supervisor crash detection and auto-restart...",
                flush=True,
            )
            self.read_until(sock, b"Welcome to Text Adventure!")
            self.read_until(sock, b"You are in a dark room.")
            print(
                "✅ Supervisor successfully detected the crash and auto-restarted the game process!",
                flush=True,
            )

            # 7. Type "help" again to verify IPC handshaking continues cleanly after restart!
            print("Sending 'help' command post-restart...", flush=True)
            sock.sendall(b"help\n")
            self.read_until(sock, b"Available commands: help, look, go north")

            # 8. Type "exit" to initiate platform shutdown
            print(
                "Sending 'exit' command to shut down system...",
                flush=True,
            )
            sock.sendall(b"exit\n")
            self.read_until(sock, b"Goodbye!")

        finally:
            sock.close()
            # Wait for QEMU to exit cleanly under semihosting shutdown
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                print(
                    "WARNING: QEMU wait timed out, killing process...",
                    flush=True,
                )
                proc.kill()
                proc.wait()

        self.assertEqual(
            proc.returncode,
            0,
            f"QEMU exited with non-zero status: {proc.returncode}",
        )
        print("✅ Interactive End-to-End Integration Test PASSED successfully!")


if __name__ == '__main__':
    unittest.main()
