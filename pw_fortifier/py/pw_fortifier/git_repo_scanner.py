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
"""Abstract package scanner for Git repository-based dependencies."""

from datetime import date, datetime
import os
import tempfile
from typing import Iterator

from pw_fortifier.find_core_owners import run_git
from pw_fortifier.package_scanner import PackageScanner, PackageVersion


class GitRepoScanner(PackageScanner):
    """Abstract base class for scanners that query Git repositories."""

    def __init__(
        self,
        root: str | os.PathLike[str] | None = None,
        tmp_dir: tempfile.TemporaryDirectory | None = None,
    ):
        super().__init__(root)
        self._tmp_dir = tmp_dir

    def __del__(self):
        if getattr(self, '_tmp_dir', None):
            self._tmp_dir.cleanup()

    @classmethod
    def clone(
        cls, url: str, timestamp: datetime | date | str | None = None
    ) -> 'GitRepoScanner':
        """Creates a clone of the repo and returns a scanner instance."""
        tmp_dir = tempfile.TemporaryDirectory()
        clone_args = [
            'clone',
            '--no-checkout',
            url,
            'src',
        ]
        if timestamp is not None:
            if isinstance(timestamp, (date, datetime)):
                timestamp_str = timestamp.isoformat()
            else:
                timestamp_str = str(timestamp)
            clone_args.insert(1, f'--shallow-since={timestamp_str}')

        run_git(clone_args, cwd=tmp_dir.name)

        src_dir = os.path.join(tmp_dir.name, 'src')
        if not os.path.isdir(src_dir):
            tmp_dir.cleanup()
            raise RuntimeError(f'Failed to clone {url}')

        return cls(root=src_dir, tmp_dir=tmp_dir)

    def get_versions(
        self,
        num: int | str | None = None,
        pattern: str | None = None,
        scope: str | os.PathLike[str] | None = None,
    ) -> Iterator[PackageVersion]:
        """Runs git log in self.root and yields PackageVersions."""
        log_args = ['log', '--format=%H,%cd', '--date=iso']
        if num is not None:
            log_args += ['-n', str(num)]
        if pattern is not None:
            log_args += [f'--grep={pattern}']
        if scope is not None:
            log_args += ['--', str(scope)]

        log_lines = run_git(log_args, cwd=self.root)
        for line in log_lines:
            line_parts = line.split(',', 1)
            if len(line_parts) == 2:
                v_hash, v_date_str = line_parts
                try:
                    v_date = date.fromisoformat(v_date_str[:10])
                    yield PackageVersion(v_hash, v_date)
                except ValueError:
                    pass
