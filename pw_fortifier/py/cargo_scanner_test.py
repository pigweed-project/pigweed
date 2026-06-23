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
"""Tests for cargo_scanner."""
# pylint: disable=protected-access

from datetime import date
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch, MagicMock

from pw_fortifier.cargo_scanner import CargoScanner
from pw_fortifier.package_scanner import PackageVersion


class TestCargoScanner(unittest.TestCase):
    """Tests for CargoScanner."""

    def setUp(self):
        """Set up test environment."""
        self.test_dir = tempfile.mkdtemp()

        # Create fake no_std Cargo.toml
        pw_foo_dir = os.path.join(self.test_dir, "pw_foo", "no_std")
        os.makedirs(pw_foo_dir)
        foo_cargo = os.path.join(pw_foo_dir, "Cargo.toml")
        # Line 4: foo-pkg
        # Line 6: dev-pkg
        foo_cargo_content = (
            "[package]\n"
            "name = \"pw_foo\"\n"
            "[dependencies]\n"
            "foo-pkg = \"1.0.0\"\n"
            "[dev-dependencies]\n"
            "dev-pkg = \"3.0.0\"\n"
        )
        with open(foo_cargo, "w") as f:
            f.write(foo_cargo_content)

        # Create fake standard Cargo.toml
        pw_bar_dir = os.path.join(self.test_dir, "pw_bar")
        os.makedirs(pw_bar_dir)
        bar_cargo = os.path.join(pw_bar_dir, "Cargo.toml")
        # Line 4: bar-pkg
        bar_cargo_content = (
            "[package]\n"
            "name = \"pw_bar\"\n"
            "[dependencies]\n"
            "bar-pkg = { version = \"2.0.0\" }\n"
        )
        with open(bar_cargo, "w") as f:
            f.write(bar_cargo_content)

        # Create fake Cargo.lock for pw_foo
        foo_lock = os.path.join(pw_foo_dir, "Cargo.lock")
        foo_lock_content = (
            "[[package]]\n"
            "name = \"foo-pkg\"\n"
            "version = \"1.0.0\"\n"
            "\n"
            "[[package]]\n"
            "name = \"dev-pkg\"\n"
            "version = \"3.0.0\"\n"
        )
        with open(foo_lock, "w") as f:
            f.write(foo_lock_content)

        # Create fake Cargo.lock for pw_bar
        bar_lock = os.path.join(pw_bar_dir, "Cargo.lock")
        bar_lock_content = (
            "[[package]]\n" "name = \"bar-pkg\"\n" "version = \"2.0.0\"\n"
        )
        with open(bar_lock, "w") as f:
            f.write(bar_lock_content)

    def tearDown(self):
        """Tear down test environment."""
        shutil.rmtree(self.test_dir)

    def _mock_subprocess_run(self, cmd, **_kwargs):
        """Mock git commands."""
        # Handle git commands for CoreOwnerFinder
        if cmd[0] == "git":
            if cmd[1] == "log":
                return subprocess.CompletedProcess(
                    args=cmd,
                    returncode=0,
                    stdout="hash1 Fake commit\n",
                    stderr="",
                )
            if cmd[1] == "show":
                return subprocess.CompletedProcess(
                    args=cmd,
                    returncode=0,
                    stdout="pw_foo/no_std/Cargo.toml\npw_bar/Cargo.toml\n",
                    stderr="",
                )
            if cmd[1] == "blame":
                has_l_arg = "-L" in cmd
                if has_l_arg:
                    l_val = cmd[cmd.index("-L") + 1]
                    filepath = cmd[-1]
                    if os.path.isabs(filepath):
                        rel_path = os.path.relpath(filepath, self.test_dir)
                    else:
                        rel_path = filepath
                    norm_rel_path = os.path.normpath(rel_path)

                    if norm_rel_path == os.path.normpath(
                        "pw_foo/no_std/Cargo.toml"
                    ):
                        if l_val == "4,4":
                            stdout = (
                                "hash1 (<owner-foo@google.com> "
                                "2026-06-05 10:00:00 +0000 4) "
                                "foo-pkg = \"1.0.0\"\n"
                            )
                        elif l_val == "6,6":
                            stdout = (
                                "hash1 (<owner-dev@google.com> "
                                "2026-06-05 10:00:00 +0000 6) "
                                "dev-pkg = \"3.0.0\"\n"
                            )
                        else:
                            stdout = ''
                    elif (
                        norm_rel_path == os.path.normpath("pw_bar/Cargo.toml")
                        and l_val == "4,4"
                    ):
                        stdout = (
                            "hash1 (<owner-bar@google.com> "
                            "2026-06-05 10:00:00 +0000 4) "
                            "bar-pkg = { version = \"2.0.0\" }\n"
                        )
                    else:
                        stdout = ""
                else:
                    stdout = ""
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=stdout, stderr=""
                )

        raise ValueError(f"Unexpected command: {cmd}")

    @patch("requests.get")
    @patch("subprocess.run")
    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    def test_scan_no_std(self, mock_cores, mock_run, mock_get):
        """Test scanning no_std cargo dependencies (tier 0)."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {
            "owner-foo@google.com",
            "owner-bar@google.com",
            "owner-dev@google.com",
        }

        # Mock requests.get for crates.io
        def mock_requests_get(url, **_kwargs):
            mock_resp = MagicMock()
            mock_resp.status_code = 200

            if "foo-pkg" in url:
                data = {
                    "versions": [
                        {
                            "num": "1.0.0",
                            "created_at": "2026-01-02T12:00:00.000000Z",
                        },
                        {
                            "num": "1.1.0",
                            "created_at": "2026-02-01T12:00:00.000000Z",
                        },
                        {
                            "num": "1.2.0-alpha.1",
                            "created_at": "2026-03-01T12:00:00.000000Z",
                        },
                        {
                            "num": "2.0.0",
                            "created_at": "2026-06-05T12:00:00.000000Z",
                        },
                        {
                            "num": "2.1.0-alpha.1",
                            "created_at": "2026-06-10T12:00:00.000000Z",
                        },
                    ]
                }
            elif "dev-pkg" in url:
                data = {
                    "versions": [
                        {
                            "num": "3.0.0",
                            "created_at": "2026-03-03T12:00:00.000000Z",
                        },
                        {
                            "num": "3.1.0",
                            "created_at": "2026-06-05T12:00:00.000000Z",
                        },
                    ]
                }
            else:
                data = {}

            mock_resp.json.return_value = data
            return mock_resp

        mock_get.side_effect = mock_requests_get

        scanner = CargoScanner(self.test_dir)
        scanner.date = date(2026, 6, 10)
        foo_cargo = os.path.join("pw_foo", "no_std", "Cargo.toml")
        results = list(scanner.scan(foo_cargo))

        self.assertEqual(len(results), 2)

        foo_res = next(r for r in results if r.package == "foo-pkg")
        self.assertEqual(
            foo_res.source,
            os.path.normpath("pw_foo/no_std/Cargo.toml"),
        )
        self.assertEqual(foo_res.pkg_type, "cargo")
        self.assertEqual(
            foo_res.current, PackageVersion("1.0.0", date(2026, 1, 2))
        )
        self.assertEqual(
            foo_res.earliest, PackageVersion("1.1.0", date(2026, 2, 1))
        )
        self.assertEqual(foo_res.tier, 0)  # no_std dependencies -> tier 0
        self.assertEqual(foo_res.owner, "owner-foo@google.com")

        dev_res = next(r for r in results if r.package == "dev-pkg")
        self.assertEqual(
            dev_res.source,
            os.path.normpath("pw_foo/no_std/Cargo.toml"),
        )
        self.assertEqual(dev_res.pkg_type, "cargo")
        self.assertEqual(
            dev_res.current, PackageVersion("3.0.0", date(2026, 3, 3))
        )
        self.assertEqual(
            dev_res.earliest, PackageVersion("3.0.0", date(2026, 3, 3))
        )
        self.assertEqual(dev_res.tier, 2)  # dev-dependencies -> tier 2
        self.assertEqual(dev_res.owner, "owner-dev@google.com")

    @patch("requests.get")
    @patch("subprocess.run")
    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    def test_scan_standard(self, mock_cores, mock_run, mock_get):
        """Test scanning standard cargo dependencies."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {"owner-bar@google.com"}

        def mock_requests_get(url, **_kwargs):
            mock_resp = MagicMock()
            mock_resp.status_code = 200

            if "bar-pkg" in url:
                data = {
                    "versions": [
                        {
                            "num": "2.0.0",
                            "created_at": "2026-02-02T12:00:00.000000Z",
                        },
                        {
                            "num": "3.0.0",
                            "created_at": "2026-06-05T12:00:00.000000Z",
                        },
                    ]
                }
            else:
                data = {}

            mock_resp.json.return_value = data
            return mock_resp

        mock_get.side_effect = mock_requests_get

        scanner = CargoScanner(self.test_dir)
        scanner.date = date(2026, 6, 10)
        bar_cargo = os.path.join("pw_bar", "Cargo.toml")
        results = list(scanner.scan(bar_cargo))

        self.assertEqual(len(results), 1)

        bar_res = results[0]
        self.assertEqual(bar_res.package, "bar-pkg")
        self.assertEqual(
            bar_res.source,
            os.path.normpath("pw_bar/Cargo.toml"),
        )
        self.assertEqual(bar_res.pkg_type, "cargo")
        self.assertEqual(
            bar_res.current, PackageVersion("2.0.0", date(2026, 2, 2))
        )
        self.assertEqual(
            bar_res.earliest, PackageVersion("2.0.0", date(2026, 2, 2))
        )
        self.assertEqual(bar_res.tier, 2)  # standard dependencies -> tier 2
        self.assertEqual(bar_res.owner, "owner-bar@google.com")

    def test_get_resolved_versions(self):
        """Test parsing Cargo.lock for resolved versions."""
        cargo_lock_path = os.path.join(self.test_dir, "Cargo.lock")
        cargo_lock_content = (
            '[[package]]\n'
            'name = "foo-pkg"\n'
            'version = "1.0.5"\n'
            '\n'
            '[[package]]\n'
            'name = "bar-pkg"\n'
            'version = "2.1.1"\n'
        )
        with open(cargo_lock_path, "w") as f:
            f.write(cargo_lock_content)

        scanner = CargoScanner(self.test_dir)
        scanner._get_resolved_versions(cargo_lock_path)

        self.assertEqual(
            scanner._versions, {"foo-pkg": "1.0.5", "bar-pkg": "2.1.1"}
        )

    @patch("requests.get")
    @patch("subprocess.run")
    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    def test_scan_missing_lockfile_entry_raises(
        self, mock_cores, mock_run, _mock_get
    ):
        """Test scan when package is missing from lockfile."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {"owner-bar@google.com"}

        # Overwrite lockfile to not contain bar-pkg
        bar_lock = os.path.join(self.test_dir, "pw_bar", "Cargo.lock")
        with open(bar_lock, "w") as f:
            f.write(
                "[[package]]\n" "name = \"other-pkg\"\n" "version = \"1.0.0\"\n"
            )

        scanner = CargoScanner(self.test_dir)
        bar_cargo = os.path.join("pw_bar", "Cargo.toml")
        with self.assertRaises(KeyError):
            list(scanner.scan(bar_cargo))


if __name__ == "__main__":
    unittest.main()
