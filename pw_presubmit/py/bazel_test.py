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
"""Tests for private/bazel.py."""

import pathlib
import subprocess
import unittest
from unittest import mock

from pw_presubmit.private import bazel
from pw_presubmit.v2 import Context


class BazelPresubmitTest(unittest.TestCase):
    """Tests for Bazel presubmit integration."""

    def setUp(self):
        bazel._get_non_manual_affected_targets.cache_clear()  # pylint: disable=protected-access
        self.ctx = mock.MagicMock(spec=Context)
        self.ctx.root = pathlib.Path('/workspace')
        self.ctx.all_modified_paths = (
            pathlib.Path('/workspace/foo/bar.py'),
            pathlib.Path('/workspace/foo/baz.cc'),
        )
        self.ctx.paths = self.ctx.all_modified_paths

    @mock.patch('pw_presubmit.private.tools.run_subprocess')
    def test_python_lint(self, mock_run):
        """Test the python linting step."""

        # Mock responses from `bazel query`
        def run_subprocess_side_effect(args, **unused_kwargs):
            cmd = list(args)
            if 'query' in cmd:
                query_expr = cmd[-1]
                if 'except attr("tags", "manual",' in query_expr:
                    # Base query
                    stdout = (
                        "py_library rule //foo:mypy_ok\n"
                        "py_binary rule //foo:mypy_binary\n"
                        "py_library rule //foo:pylint_ok\n"
                    )
                elif 'attr("tags", "nomypy"' in query_expr:
                    # nomypy query
                    stdout = "//foo:pylint_ok\n"
                elif 'attr("tags", "nopylint"' in query_expr:
                    # nopylint query
                    stdout = "//foo:mypy_ok\n" "//foo:mypy_binary\n"
                else:
                    stdout = ""
                return subprocess.CompletedProcess(
                    args=args, returncode=0, stdout=stdout
                )
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=""
            )

        mock_run.side_effect = run_subprocess_side_effect

        bazel.python_lint.run(self.ctx)

        # We expect run_subprocess to be called 5 times:
        # 1. Base query for mypy (which runs because cache is empty)
        # 2. Targeted query for nomypy
        # 3. Build mypy rules
        # 4. Targeted query for nopylint (base query is cached, so not run)
        # 5. Build pylint rules
        self.assertEqual(mock_run.call_count, 5)

        calls = [c[0][0] for c in mock_run.call_args_list]

        # Verify first query is base query
        self.assertIn('query', calls[0])
        self.assertIn('except attr("tags", "manual",', calls[0][-1])

        # Verify second query checks nomypy tag on all rules set
        self.assertIn('query', calls[1])
        self.assertIn('attr("tags", "nomypy", set(', calls[1][-1])

        # Verify first build built mypy targets (excluding //foo:pylint_ok)
        self.assertEqual(calls[2][1], 'build')
        self.assertIn('--config=mypy', calls[2])
        self.assertIn('//foo:mypy_ok', calls[2])
        self.assertIn('//foo:mypy_binary', calls[2])

        # Verify third query checks nopylint tag on all rules set
        self.assertIn('query', calls[3])
        self.assertIn('attr("tags", "nopylint", set(', calls[3][-1])

        # Verify second build built pylint targets (excluding mypy targets)
        self.assertEqual(calls[4][1], 'build')
        self.assertIn('--config=pylint', calls[4])
        self.assertIn('//foo:pylint_ok', calls[4])

    @mock.patch('pw_presubmit.private.tools.run_subprocess')
    def test_clang_tidy(self, mock_run):
        """Test the clang-tidy step."""

        def run_subprocess_side_effect(args, **unused_kwargs):
            cmd = list(args)
            if 'query' in cmd:
                query_expr = cmd[-1]
                if 'except attr("tags", "manual",' in query_expr:
                    stdout = "cc_library rule //foo:cpp_target\n"
                else:
                    stdout = ""
                return subprocess.CompletedProcess(
                    args=args, returncode=0, stdout=stdout
                )
            return subprocess.CompletedProcess(
                args=args, returncode=0, stdout=""
            )

        mock_run.side_effect = run_subprocess_side_effect

        bazel.clang_tidy.run(self.ctx)

        # We expect run_subprocess to be called 3 times:
        # 1. Base query
        # 2. Targeted query for noclangtidy
        # 3. Build clang-tidy target
        self.assertEqual(mock_run.call_count, 3)
        calls = [c[0][0] for c in mock_run.call_args_list]

        # Verify first query is base query
        self.assertIn('query', calls[0])
        self.assertIn('except attr("tags", "manual",', calls[0][-1])

        # Verify second query checks noclangtidy tag
        self.assertIn('query', calls[1])
        self.assertIn('attr("tags", "noclangtidy", set(', calls[1][-1])

        # Verify build built clang-tidy target
        self.assertEqual(calls[2][1], 'build')
        self.assertIn('--config=clang-tidy', calls[2])
        self.assertIn('//foo:cpp_target', calls[2])


if __name__ == '__main__':
    unittest.main()
