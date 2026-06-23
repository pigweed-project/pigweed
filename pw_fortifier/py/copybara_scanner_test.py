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
"""Tests for copybara_scanner."""
# pylint: disable=protected-access

from datetime import date
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from pw_fortifier.copybara_scanner import CopybaraScanner
from pw_fortifier.package_scanner import PackageVersion


class TestCopybaraScanner(unittest.TestCase):
    """Tests for CopybaraScanner."""

    def setUp(self):
        """Set up test environment."""
        self.test_dir = tempfile.mkdtemp()

        # Create fake third_party/fuchsia/copy.bara.sky
        self.fuchsia_dir = os.path.join(self.test_dir, "third_party", "fuchsia")
        os.makedirs(self.fuchsia_dir)
        self.copybara_sky = os.path.join(self.fuchsia_dir, "copy.bara.sky")
        with open(self.copybara_sky, "w") as f:
            f.write(
                'core.workflow(\n'
                '    name = "default",\n'
                '    origin = git.origin(\n'
                '        url = "https://github.com/fuchsia/something.git",\n'
                '    ),\n'
                ')\n'
            )

        # Create fake repo dir so git log path exists (though mock
        # doesn't check disk, it's good practice).
        self.repo_dir = os.path.join(self.fuchsia_dir, "repo")
        os.makedirs(self.repo_dir)

    def tearDown(self):
        """Tear down test environment."""
        shutil.rmtree(self.test_dir)

    def _mock_subprocess_run(self, cmd, **_kwargs):
        """Mock git commands."""
        if cmd[0] == "git":
            if cmd[1] == "log":
                if "--grep=GitOrigin-RevId" in cmd and '--format=%H,%cd' in cmd:
                    return subprocess.CompletedProcess(
                        args=cmd,
                        returncode=0,
                        stdout="curr_rev_123,2026-06-01 12:00:00 -0700\n",
                        stderr="",
                    )
                if '--format=%H,%cd' in cmd:
                    stdout = (
                        "migrated_rev_456_long,2026-06-05 12:00:00 -0700\n"
                        "migrated_rev_123_mid,2026-06-03 12:00:00 -0700\n"
                        "curr_rev_123,2026-06-01 12:00:00 -0700\n"
                    )
                    return subprocess.CompletedProcess(
                        args=cmd, returncode=0, stdout=stdout, stderr=""
                    )
                return subprocess.CompletedProcess(
                    args=cmd,
                    returncode=0,
                    stdout="hash1 Fake commit\n",
                    stderr="",
                )
            if cmd[1] == "clone":
                cwd = _kwargs.get('cwd')
                if cwd:
                    os.makedirs(os.path.join(cwd, 'src'), exist_ok=True)
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout="", stderr=""
                )
            if cmd[1] == "show":
                arg = cmd[2]
                path = arg.split(':')[-1]
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=f"{path}\n", stderr=""
                )
            if cmd[1] == "blame":
                filepath = cmd[-1]
                if os.path.isabs(filepath):
                    rel_path = os.path.relpath(filepath, self.test_dir)
                else:
                    rel_path = filepath
                if "copy.bara.sky" in rel_path:
                    norm_rel_path = rel_path.replace(os.sep, '/')
                    parts = norm_rel_path.split('/')
                    project = parts[-2] if len(parts) > 1 else "default"
                    stdout = (
                        f"hash1 (<owner-{project}@google.com> "
                        "2026-06-01 10:00:00 +0000 1) // "
                        "fake copy.bara.sky\n"
                    )
                    return subprocess.CompletedProcess(
                        args=cmd, returncode=0, stdout=stdout, stderr=""
                    )
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout="", stderr=""
                )

        raise ValueError(f"Unexpected command: {cmd}")

    @patch("subprocess.run")
    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    def test_scan(self, mock_cores, mock_run):
        """Test scanning copybara packages."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {"owner-fuchsia@google.com"}

        scanner = CopybaraScanner(self.test_dir)
        scanner.date = date(2026, 6, 10)
        results = list(scanner.scan('third_party/fuchsia/copy.bara.sky'))

        self.assertEqual(len(results), 1)
        res = results[0]
        self.assertEqual(res.package, "fuchsia")
        self.assertEqual(res.source, "third_party/fuchsia/copy.bara.sky")
        self.assertEqual(res.pkg_type, "copybara")
        self.assertEqual(
            res.current, PackageVersion("curr_rev_123", date(2026, 6, 1))
        )
        self.assertEqual(
            res.earliest, PackageVersion("curr_rev_123", date(2026, 6, 1))
        )
        self.assertEqual(res.tier, 0)
        self.assertEqual(res.owner, "owner-fuchsia@google.com")

    @patch("subprocess.run")
    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    def test_scan_nested(self, mock_cores, mock_run):
        """Test scanning nested copybara packages."""
        # Create fake third_party/nested/foo/copy.bara.sky
        nested_dir = os.path.join(self.test_dir, "third_party", "nested", "foo")
        os.makedirs(nested_dir)
        copybara_sky = os.path.join(nested_dir, "copy.bara.sky")
        with open(copybara_sky, "w") as f:
            f.write(
                'core.workflow(\n'
                '    name = "default",\n'
                '    origin = git.origin(\n'
                '        url = "https://github.com/nested/foo.git",\n'
                '    ),\n'
                ')\n'
            )
        repo_dir = os.path.join(nested_dir, "repo")
        os.makedirs(repo_dir)

        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {"owner-foo@google.com"}

        scanner = CopybaraScanner(self.test_dir)
        scanner.date = date(2026, 6, 10)
        results = list(scanner.scan('third_party/nested/foo/copy.bara.sky'))

        self.assertEqual(len(results), 1)
        res = results[0]
        self.assertEqual(res.package, "foo")
        self.assertEqual(res.source, "third_party/nested/foo/copy.bara.sky")
        self.assertEqual(res.pkg_type, "copybara")
        self.assertEqual(res.owner, "owner-foo@google.com")


if __name__ == "__main__":
    unittest.main()
