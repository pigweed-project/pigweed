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

import subprocess
import unittest
import time
from python.runfiles import runfiles


class AdventureSimulatedTest(unittest.TestCase):
    def test_adventure_simulated(self):
        r = runfiles.Create()
        self.assertIsNotNone(r, "Failed to initialize Bazel runfiles")

        qemu_runner_path = r.Rlocation("pigweed/pw_kernel/tooling/qemu")
        image_path = r.Rlocation(
            "_main/targets/riscv_qemu/simulated/adventure.elf"
        )

        self.assertIsNotNone(
            qemu_runner_path, "Could not find qemu runner in runfiles"
        )
        self.assertIsNotNone(
            image_path, "Could not find adventure system image in runfiles"
        )

        qemu_cmd = [
            qemu_runner_path,
            "--cpu",
            "rv32",
            "--machine",
            "virt",
            "--semihosting",
            "--image",
            image_path,
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

        # Wait for QEMU to exit cleanly under semihosting shutdown
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            print(
                "WARNING: QEMU wait timed out, killing process...", flush=True
            )
            proc.kill()
            proc.wait()

        self.assertEqual(
            proc.returncode,
            0,
            f"QEMU exited with non-zero status: {proc.returncode}",
        )
        print("✅ Simulated Target Execution Test PASSED successfully!")


if __name__ == '__main__':
    unittest.main()
