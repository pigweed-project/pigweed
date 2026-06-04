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
"""Tests for pw_ide.lock"""

import os
import unittest
from unittest.mock import patch

from test_cases import TempDirTestCase
from pw_ide.lock import LockFile, LockAlreadyHeldError


class TestLockFile(TempDirTestCase):
    """Tests LockFile atomic locking."""

    def test_acquire_and_release(self):
        """Tests that a lock is successfully acquired and released."""
        lockfile_path = self.path_in_temp_dir("pw.lock")
        lock = LockFile(lockfile_path)

        self.assertFalse(lockfile_path.exists())

        with lock:
            self.assertTrue(lockfile_path.exists())
            self.assertEqual(
                lockfile_path.read_text().strip(), str(os.getpid())
            )

        self.assertFalse(lockfile_path.exists())

    def test_acquire_fails_when_pid_alive(self):
        """Tests that acquire exits if another instance is alive."""
        lockfile_path = self.path_in_temp_dir("pw.lock")
        lockfile_path.write_text("99999")  # Mock a running PID

        lock = LockFile(lockfile_path)

        with patch.object(lock, "_is_pid_alive", return_value=True):
            with self.assertRaises(LockAlreadyHeldError) as cm:
                lock.acquire()
            self.assertEqual(cm.exception.pid, 99999)

    def test_acquire_cleans_dead_pid(self):
        """Tests that acquire cleans up a dead PID and successfully retries."""
        lockfile_path = self.path_in_temp_dir("pw.lock")
        lockfile_path.write_text("99999")  # Mock a dead PID

        lock = LockFile(lockfile_path)

        with patch.object(lock, "_is_pid_alive", return_value=False):
            with lock:
                self.assertTrue(lockfile_path.exists())
                self.assertEqual(
                    lockfile_path.read_text().strip(), str(os.getpid())
                )

    def test_release_does_not_delete_unowned_lock(self):
        """Tests that release does not delete a lock owned by another PID."""
        lockfile_path = self.path_in_temp_dir("pw.lock")
        lockfile_path.write_text("99999")

        lock = LockFile(lockfile_path)
        lock.release()

        self.assertTrue(lockfile_path.exists())
        self.assertEqual(lockfile_path.read_text().strip(), "99999")


if __name__ == "__main__":
    unittest.main()
