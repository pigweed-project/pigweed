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
"""Tests for npm_scanner."""
# pylint: disable=protected-access

from datetime import date
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from pw_fortifier.npm_scanner import NpmScanner
from pw_fortifier.package_scanner import PackageVersion


class TestNpmScanner(unittest.TestCase):
    """Tests for NpmScanner."""

    def setUp(self):
        """Set up test environment."""
        self.test_dir = tempfile.mkdtemp()
        self.package_json_path = os.path.join(self.test_dir, "package.json")

        # Create a fake package.json
        # Line 1: {
        # Line 2:   "name": "test-project",
        # Line 3:   "dependencies": {
        # Line 4:     "foo-pkg": "^1.0.0"
        # Line 5:   },
        # Line 6:   "devDependencies": {
        # Line 7:     "bar-pkg": "^2.0.0"
        # Line 8:   }
        # Line 9: }
        self.package_json_content = (
            "{\n"
            "  \"name\": \"test-project\",\n"
            "  \"dependencies\": {\n"
            "    \"foo-pkg\": \"^1.0.0\"\n"
            "  },\n"
            "  \"devDependencies\": {\n"
            "    \"bar-pkg\": \"^2.0.0\"\n"
            "  }\n"
            "}\n"
        )
        with open(self.package_json_path, "w") as f:
            f.write(self.package_json_content)

        self.package_lock_json_path = os.path.join(
            self.test_dir, "package-lock.json"
        )
        self.package_lock_json_content = (
            "{\n"
            "  \"packages\": {\n"
            "    \"\": {\n"
            "      \"dependencies\": {\n"
            "        \"foo-pkg\": \"^1.0.0\"\n"
            "      },\n"
            "      \"devDependencies\": {\n"
            "        \"bar-pkg\": \"^2.0.0\"\n"
            "      }\n"
            "    }\n"
            "  }\n"
            "}\n"
        )
        with open(self.package_lock_json_path, "w") as f:
            f.write(self.package_lock_json_content)

    def tearDown(self):
        """Tear down test environment."""
        shutil.rmtree(self.test_dir)

    @staticmethod
    def _mock_subprocess_run(cmd, **_kwargs):
        """Mock git and npm commands."""
        # Handle npm commands
        if cmd[0] == "npm":
            if cmd[1] == "view":
                pkg = cmd[2]
                if cmd[3] == "time":
                    if pkg == "foo-pkg":
                        time_data = {
                            "modified": "2026-06-05T12:00:00.000Z",
                            "created": "2026-01-01T12:00:00.000Z",
                            "1.0.0": "2026-01-02T12:00:00.000Z",
                            "1.1.0": "2026-02-01T12:00:00.000Z",
                            "2.0.0": "2026-06-05T12:00:00.000Z",
                        }
                    elif pkg == "bar-pkg":
                        time_data = {
                            "modified": "2026-06-05T12:00:00.000Z",
                            "created": "2026-02-01T12:00:00.000Z",
                            "2.0.0": "2026-02-02T12:00:00.000Z",
                            "2.1.0": "2026-06-05T12:00:00.000Z",
                        }
                    else:
                        time_data = {}
                    return subprocess.CompletedProcess(
                        args=cmd,
                        returncode=0,
                        stdout=json.dumps(time_data),
                        stderr="",
                    )

        # Handle git commands
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
                    args=cmd, returncode=0, stdout="package.json\n", stderr=""
                )
            if cmd[1] == "blame":
                has_l_arg = "-L" in cmd
                if has_l_arg:
                    l_val = cmd[cmd.index("-L") + 1]
                    if l_val == "4,4":
                        stdout = (
                            "hash1 (<owner-foo@google.com> "
                            "2026-06-05 10:00:00 +0000 4)   "
                            "\"foo-pkg\": \"^1.0.0\"\n"
                        )
                    elif l_val == "7,7":
                        stdout = (
                            "hash1 (<owner-bar@google.com> "
                            "2026-06-05 10:00:00 +0000 7)   "
                            "\"bar-pkg\": \"^2.0.0\"\n"
                        )
                    else:
                        stdout = ""
                else:
                    stdout = ""
                return subprocess.CompletedProcess(
                    args=cmd, returncode=0, stdout=stdout, stderr=""
                )

        raise ValueError(f"Unexpected command: {cmd}")

    @patch("subprocess.run")
    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    def test_scan(self, mock_cores, mock_run):
        """Test scanning standard npm packages."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {
            "owner-foo@google.com",
            "owner-bar@google.com",
        }

        scanner = NpmScanner(self.test_dir)
        scanner.date = date(2026, 6, 10)
        results = list(scanner.scan("package.json"))

        self.assertEqual(len(results), 2)

        foo_res = next(r for r in results if r.package == "foo-pkg")
        self.assertEqual(foo_res.source, "package.json")
        self.assertEqual(foo_res.pkg_type, "npm")
        self.assertEqual(
            foo_res.current, PackageVersion("1.0.0", date(2026, 1, 2))
        )
        self.assertEqual(
            foo_res.earliest, PackageVersion("1.1.0", date(2026, 2, 1))
        )
        self.assertEqual(foo_res.tier, 2)
        self.assertEqual(foo_res.owner, "owner-foo@google.com")

        bar_res = next(r for r in results if r.package == "bar-pkg")
        self.assertEqual(bar_res.source, "package.json")
        self.assertEqual(bar_res.pkg_type, "npm")
        self.assertEqual(
            bar_res.current, PackageVersion("2.0.0", date(2026, 2, 2))
        )
        self.assertEqual(
            bar_res.earliest, PackageVersion("2.0.0", date(2026, 2, 2))
        )
        self.assertEqual(bar_res.tier, 2)
        self.assertEqual(bar_res.owner, "owner-bar@google.com")

    @patch("subprocess.run")
    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    def test_scan_pw_web(self, mock_cores, mock_run):
        """Test scanning package.json under pw_web (tier 0)."""
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {
            "owner-foo@google.com",
            "owner-bar@google.com",
        }

        # Create package.json under pw_web directory
        pw_web_dir = os.path.join(self.test_dir, "pw_web")
        os.makedirs(pw_web_dir, exist_ok=True)
        pw_web_package_json = os.path.join(pw_web_dir, "package.json")
        with open(pw_web_package_json, "w") as f:
            f.write(self.package_json_content)

        pw_web_package_lock = os.path.join(pw_web_dir, "package-lock.json")
        with open(pw_web_package_lock, "w") as f:
            f.write(self.package_lock_json_content)

        scanner = NpmScanner(self.test_dir)
        scanner.add_on_device_module('pw_web')
        results = list(scanner.scan(os.path.join("pw_web", "package.json")))

        self.assertEqual(len(results), 2)

        foo_res = next(r for r in results if r.package == "foo-pkg")
        # For pw_web, dependencies should be TIER0_ON_DEVICE (0)
        self.assertEqual(foo_res.tier, 0)

    @patch("subprocess.run")
    def test_npm_not_found(self, mock_run):
        """Test scan when npm command is not found."""
        mock_run.side_effect = FileNotFoundError()
        scanner = NpmScanner(self.test_dir)
        with self.assertRaises(RuntimeError) as ctx:
            list(scanner.scan("package.json"))
        self.assertIn("npm command not found", str(ctx.exception))
        self.assertIn("source activate.sh", str(ctx.exception))

    def test_node_modules_skipped(self):
        """Test that packages inside node_modules are skipped."""
        node_modules_path = os.path.join(
            self.test_dir, "node_modules", "some-pkg", "package.json"
        )
        os.makedirs(os.path.dirname(node_modules_path), exist_ok=True)
        with open(node_modules_path, "w") as f:
            f.write(self.package_json_content)

        scanner = NpmScanner(self.test_dir)
        results = list(
            scanner.scan(
                os.path.join("node_modules", "some-pkg", "package.json")
            )
        )
        self.assertEqual(len(results), 0)


if __name__ == "__main__":
    unittest.main()
