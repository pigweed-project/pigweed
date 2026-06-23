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
"""Tests for find_core_owners in pw_fortifier."""
# pylint: disable=protected-access,missing-class-docstring,missing-function-docstring,no-self-use

import os
import unittest
from unittest.mock import patch, mock_open, MagicMock
import subprocess
from io import StringIO
from pw_fortifier.find_core_owners import (
    CoreOwnerFinder,
    run_git,
    parse_owners_file,
    _parse_snippet_arg,
    main,
)


class TestFindCoreOwners(unittest.TestCase):
    """Tests for find_owner utility by mocking git commands."""

    def setUp(self):
        # Find real repo root to construct paths that resolve nicely
        # relative to it. This test file is at
        # pw_fortifier/py/find_core_owners_test.py, so repo root is 2 levels up.
        current_dir = os.path.dirname(os.path.abspath(__file__))
        self.repo_path = os.path.abspath(os.path.join(current_dir, "..", ".."))

    def _mock_subprocess_run(self, cmd, **_kwargs):
        # 1. git log
        if "log" in cmd:
            target_file = os.path.normpath(cmd[-1])
            if target_file == os.path.normpath("module1/file.txt"):
                # Commit 3: LSC, Commit 2: non-LSC (2 modules),
                # Commit 1: non-LSC (1 module)
                stdout = (
                    "hash3 Commit 3 - LSC\n"
                    "hash2 Commit 2 - modify both\n"
                    "hash1 Commit 1 - init\n"
                )
            elif target_file == os.path.normpath("module_target/file.txt"):
                # Both commits are LSC (11 modules)
                stdout = (
                    "hash_lsc_2 Commit 2 - LSC modify\n"
                    "hash_lsc_1 Commit 1 - LSC init\n"
                )
            else:
                stdout = ""
            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout=stdout, stderr=""
            )

        # 2. git show
        if "show" in cmd:
            commit_hash = cmd[-1]

            if commit_hash == "hash1":
                # Commit 1: affects 1 file
                stdout = "module1/file.txt\n"
            elif commit_hash == "hash2":
                # Commit 2: affects 2 files
                stdout = "module1/file.txt\nmodule2/file.txt\n"
            elif commit_hash == "hash3":
                # Commit 3 (LSC): affects 101 files
                stdout = (
                    "module1/file.txt\n"
                    + "\n".join(
                        [f"module_lsc_{i}/file.txt" for i in range(100)]
                    )
                    + "\n"
                )
            elif commit_hash == "hash_lsc_1":
                # LSC init: affects 101 files
                stdout = (
                    "module_target/file.txt\n"
                    + "\n".join(
                        [f"module_lsc_{i}/file.txt" for i in range(100)]
                    )
                    + "\n"
                )
            elif commit_hash == "hash_lsc_2":
                # LSC modify: affects 101 files
                stdout = (
                    "module_target/file.txt\n"
                    + "\n".join(
                        [f"module_lsc_{i}/file.txt" for i in range(100)]
                    )
                    + "\n"
                )
            else:
                stdout = ""

            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout=stdout, stderr=""
            )

        # 3. git blame
        if "blame" in cmd:
            commit_hash = cmd[-3]
            has_l_arg = "-L" in cmd
            # Check if -e is present
            self.assertIn("-e", cmd)
            if commit_hash == "hash2":
                if has_l_arg:
                    l_val = cmd[cmd.index("-L") + 1]
                    if l_val == "1,3":
                        stdout = (
                            "hash2 (<user1@google.com> "
                            "2026-06-05 10:00:00 +0000 1) line 1\n"
                            "hash1 (<user2@google.com> "
                            "2026-06-05 09:00:00 +0000 2) line 2\n"
                            "hash2 (<user1@google.com> "
                            "2026-06-05 10:00:00 +0000 3) line 3\n"
                        )
                    else:
                        stdout = ""
                else:
                    stdout = (
                        "hash2 (<user1@google.com> "
                        "2026-06-05 10:00:00 +0000 1) line 1\n"
                        "hash1 (<user2@google.com> "
                        "2026-06-05 09:00:00 +0000 2) line 2\n"
                        "hash1 (<user2@google.com> "
                        "2026-06-05 09:00:00 +0000 3) line 3\n"
                        "hash1 (<user3@google.com> "
                        "2026-06-05 09:00:00 +0000 4) line 4\n"
                        "hash1 (<user3@google.com> "
                        "2026-06-05 09:00:00 +0000 5) line 5\n"
                        "hash1 (<user3@google.com> "
                        "2026-06-05 09:00:00 +0000 6) line 6\n"
                    )
            else:
                stdout = ""
            return subprocess.CompletedProcess(
                args=cmd, returncode=0, stdout=stdout, stderr=""
            )

        raise ValueError(f"Unexpected git command: {cmd}")

    @patch("subprocess.run")
    def test_find_nonlsc_revision(self, mock_run):
        mock_run.side_effect = self._mock_subprocess_run

        # Should skip Commit 3 (LSC) and return Commit 2 (non-LSC)
        finder = CoreOwnerFinder(root=self.repo_path)
        revision = finder._find_nonlsc_revision("module1/file.txt")
        self.assertIsNotNone(revision)
        self.assertIn("Commit 2 - modify both", revision)

        # Verify call count
        # 1. log
        # 2. show hash3
        # 3. show hash2
        self.assertEqual(mock_run.call_count, 3)

        # Verify it didn't call show for hash1
        called_hashes = [
            call.args[0][-1]
            for call in mock_run.call_args_list
            if "show" in call.args[0]
        ]
        self.assertIn("hash3", called_hashes)
        self.assertIn("hash2", called_hashes)
        self.assertNotIn("hash1", called_hashes)

    @patch("subprocess.run")
    def test_all_lsc_revisions_returns_none(self, mock_run):
        mock_run.side_effect = self._mock_subprocess_run

        # Should skip both commits and return None
        finder = CoreOwnerFinder(root=self.repo_path)
        revision = finder._find_nonlsc_revision("module_target/file.txt")
        self.assertIsNone(revision)

        # Calls expected:
        # 1. log
        # 2. show hash_lsc_2
        # 3. show hash_lsc_1
        self.assertEqual(mock_run.call_count, 3)

    @patch("subprocess.run")
    def test_git_error(self, mock_run):
        mock_run.side_effect = subprocess.CalledProcessError(
            returncode=1, cmd="git"
        )
        result = run_git(["status"], cwd=self.repo_path)
        self.assertEqual(result, [])

    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    @patch("subprocess.run")
    def test_find_core_owner_with_lines(self, mock_run, mock_cores):
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {"user1@google.com", "user2@google.com"}
        finder = CoreOwnerFinder(root=self.repo_path)
        finder.add(os.path.join(self.repo_path, "module1/file.txt"), (1, 3))
        author = finder.find()
        self.assertEqual(author, "user1@google.com")

    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    @patch("subprocess.run")
    def test_find_core_owner_whole_file(self, mock_run, mock_cores):
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {"user1@google.com", "user2@google.com"}
        finder = CoreOwnerFinder(root=self.repo_path)
        finder.add(os.path.join(self.repo_path, "module1/file.txt"))
        author = finder.find()
        self.assertEqual(author, "user2@google.com")

    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    @patch("subprocess.run")
    def test_find_core_owner_any_owner(self, mock_run, mock_cores):
        mock_run.side_effect = self._mock_subprocess_run
        mock_cores.return_value = {"user1@google.com", "user2@google.com"}
        finder = CoreOwnerFinder(root=self.repo_path)
        finder.add(os.path.join(self.repo_path, "module1/file.txt"))
        author = finder.find(any_owner=True)
        self.assertEqual(author, "user3@google.com")

    @patch("pw_fortifier.find_core_owners.parse_owners_file")
    @patch("os.path.exists")
    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    @patch("subprocess.run")
    def test_find_local_owner_fallback(
        self, mock_run, mock_cores, mock_exists, mock_parse
    ):
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout="", stderr=""
        )
        mock_cores.return_value = {"core_user@google.com"}

        target_file = os.path.join(self.repo_path, "module1/submodule/file.txt")
        local_owners_dir = os.path.join(self.repo_path, "module1")
        local_owners_file = os.path.join(local_owners_dir, "OWNERS")

        def _exists(path):
            return path == local_owners_file

        mock_exists.side_effect = _exists

        def _parse(path):
            if path == local_owners_file:
                return ["non_core@google.com", "core_user@google.com"]
            return []

        mock_parse.side_effect = _parse

        finder = CoreOwnerFinder(root=self.repo_path)
        finder.add(target_file)

        author = finder.find()
        self.assertEqual(author, "core_user@google.com")

    @patch("pw_fortifier.find_core_owners.parse_owners_file")
    @patch("os.path.exists")
    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder.core_members")
    @patch("subprocess.run")
    def test_find_local_owner_fallback_any_owner(
        self, mock_run, mock_cores, mock_exists, mock_parse
    ):
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout="", stderr=""
        )
        mock_cores.return_value = {"core_user@google.com"}

        target_file = os.path.join(self.repo_path, "module1/submodule/file.txt")
        local_owners_dir = os.path.join(self.repo_path, "module1")
        local_owners_file = os.path.join(local_owners_dir, "OWNERS")

        def _exists(path):
            return path == local_owners_file

        mock_exists.side_effect = _exists

        def _parse(path):
            if path == local_owners_file:
                return ["non_core@google.com", "core_user@google.com"]
            return []

        mock_parse.side_effect = _parse

        finder = CoreOwnerFinder(root=self.repo_path)
        finder.add(target_file)

        author = finder.find(any_owner=True)
        self.assertEqual(author, "non_core@google.com")

    def test_parse_owners_file(self):
        mock_content = "user1@google.com\nuser2@google.com\n"
        with patch("builtins.open", mock_open(read_data=mock_content)):
            owners = parse_owners_file("some_path")
            self.assertEqual(owners, ["user1@google.com", "user2@google.com"])

    def test_core_members(self):
        mock_content = (
            "# Some header\n"
            "include EXTENDED_OWNERS\n"
            "\n"
            "# Core team members.\n"
            "# Note comment\n"
            "user1@google.com #{ANNOTATION}\n"
            "user2@google.com\n"
            "\n"
            "# Other section\n"
            "per-file MODULE.bazel=file:WORKSPACE_OWNERS\n"
        )
        with patch("builtins.open", mock_open(read_data=mock_content)):
            finder = CoreOwnerFinder(root=self.repo_path)
            members = finder.core_members()
            self.assertEqual(members, {"user1@google.com", "user2@google.com"})

    def test_core_members_no_file(self):
        with patch("builtins.open", side_effect=FileNotFoundError()):
            finder = CoreOwnerFinder(root=self.repo_path)
            with self.assertRaises(FileNotFoundError):
                finder.core_members()

    def test_parse_snippet_arg(self):
        path, lines = _parse_snippet_arg("foo/bar.cc:10-20")
        self.assertEqual(path, "foo/bar.cc")
        self.assertEqual(lines, (10, 20))

        path, lines = _parse_snippet_arg("foo/bar.cc")
        self.assertEqual(path, "foo/bar.cc")
        self.assertIsNone(lines)

        path, lines = _parse_snippet_arg("foo/bar.cc:10")
        self.assertEqual(path, "foo/bar.cc:10")
        self.assertIsNone(lines)

        path, lines = _parse_snippet_arg("C:\\foo\\bar.cc:10-20")
        self.assertEqual(path, "C:\\foo\\bar.cc")
        self.assertEqual(lines, (10, 20))

        path, lines = _parse_snippet_arg("C:\\foo\\bar.cc")
        self.assertEqual(path, "C:\\foo\\bar.cc")
        self.assertIsNone(lines)

    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder")
    def test_main(self, mock_finder_class):
        mock_finder = MagicMock()
        mock_finder.find.return_value = "owner@google.com"
        mock_finder_class.return_value = mock_finder

        with patch("sys.stdout", new_callable=StringIO) as mock_stdout:
            main(["file.cc:10-20", "file2.cc"])

        output = mock_stdout.getvalue()
        self.assertIn("file.cc:10-20: owner@google.com", output)
        self.assertIn("file2.cc: owner@google.com", output)

        mock_finder_class.assert_called_with(root=".")
        mock_finder.add.assert_any_call("file.cc", (10, 20))
        mock_finder.add.assert_any_call("file2.cc", None)
        mock_finder.find.assert_called_with(any_owner=False)

    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder")
    def test_main_any_owner(self, mock_finder_class):
        mock_finder = MagicMock()
        mock_finder.find.return_value = "owner@google.com"
        mock_finder_class.return_value = mock_finder

        with patch("sys.stdout", new_callable=StringIO) as mock_stdout:
            main(["--any", "file.cc"])

        output = mock_stdout.getvalue()
        self.assertIn("file.cc: owner@google.com", output)

        mock_finder_class.assert_called_once_with(root=".")
        mock_finder.add.assert_called_once_with("file.cc", None)
        mock_finder.find.assert_called_once_with(any_owner=True)

    @patch("pw_fortifier.find_core_owners.CoreOwnerFinder")
    def test_main_with_root(self, mock_finder_class):
        mock_finder = MagicMock()
        mock_finder.find.return_value = "owner@google.com"
        mock_finder_class.return_value = mock_finder

        with patch("sys.stdout", new_callable=StringIO):
            main(["-r", "/tmp/mock_root", "file.cc"])

        mock_finder_class.assert_called_once_with(root="/tmp/mock_root")


if __name__ == "__main__":
    unittest.main()
