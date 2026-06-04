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
"""Atomic lock file management helper."""

import logging
import os
from pathlib import Path
import time

_LOG = logging.getLogger(__name__)


class LockAlreadyHeldError(Exception):
    """Raised when the lock file is already held by another live process."""

    def __init__(self, pid: int):
        super().__init__(f"Lock file is already held by PID: {pid}")
        self.pid = pid


class LockFile:
    """A context manager for atomic lock file management."""

    def __init__(self, lockfile_path: Path):
        self.lockfile_path = lockfile_path
        self.current_pid = os.getpid()

    @staticmethod
    def _is_pid_alive(pid: int) -> bool:
        if pid <= 0:
            return False
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False

    def acquire(self) -> None:
        """Acquires the lock atomically, checking for stale locks."""
        while True:
            try:
                with open(self.lockfile_path, "x") as f:
                    f.write(str(self.current_pid))
                break
            except FileExistsError:
                try:
                    lock_content = self.lockfile_path.read_text().strip()
                    if lock_content:
                        existing_pid = int(lock_content)
                        if self._is_pid_alive(existing_pid):
                            raise LockAlreadyHeldError(existing_pid)
                        _LOG.warning(
                            "Lockfile found with dead PID %d. "
                            "Deleting and retrying.",
                            existing_pid,
                        )
                        self.lockfile_path.unlink(missing_ok=True)
                    else:
                        _LOG.warning(
                            "Lockfile exists but is empty. "
                            "Deleting and retrying."
                        )
                        self.lockfile_path.unlink(missing_ok=True)
                except (ValueError, OSError) as e:
                    _LOG.warning(
                        "Lockfile found but could not be read or deleted: %s. "
                        "Retrying.",
                        e,
                    )
                    self.lockfile_path.unlink(missing_ok=True)
            time.sleep(0.01)

    def release(self) -> None:
        """Releases the lock file if it belongs to the current PID."""
        try:
            if self.lockfile_path.exists():
                file_content = self.lockfile_path.read_text().strip()
                if file_content == str(self.current_pid):
                    self.lockfile_path.unlink()
                else:
                    _LOG.warning(
                        "Lockfile content '%s' does not match PID '%s'. "
                        "Not deleting.",
                        file_content,
                        self.current_pid,
                    )
        except OSError as e:
            _LOG.warning("Failed to clean up lockfile: %s", e)

    def __enter__(self):
        self.acquire()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.release()
