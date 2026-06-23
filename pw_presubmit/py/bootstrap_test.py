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
"""Tests for private/bootstrap.py."""

import pathlib
import subprocess
import unittest
from unittest import mock

from pw_presubmit.private import bootstrap
from pw_presubmit.v2 import Context


class BootstrapPresubmitTest(unittest.TestCase):
    """Tests for Bootstrap presubmit integration."""

    def setUp(self):
        self.ctx = mock.MagicMock(spec=Context)
        self.ctx.root = pathlib.Path('/workspace')
        self.ctx.output_dir = pathlib.Path('/workspace/out')
        self.ctx.all_modified_paths = (pathlib.Path('/workspace/foo/bar.cc'),)
        self.ctx.paths = self.ctx.all_modified_paths

    @mock.patch('pw_presubmit.private.tools.run_subprocess')
    def test_bootstrap(self, mock_run):
        """Test the bootstrap step."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout=""
        )

        bootstrap.bootstrap.run(self.ctx)

        mock_run.assert_called_once_with(
            ['pw_env_setup/run.sh', 'bootstrap.sh'],
            cwd=self.ctx.root,
            env=mock.ANY,
        )

    @mock.patch('pw_presubmit.private.tools.run_subprocess')
    def test_cmake_tests(self, mock_run):
        """Test the cmake tests step."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout=""
        )

        bootstrap.cmake_tests.run(self.ctx)

        # We expect run_subprocess to be called 2 times:
        # 1. cmake configure
        # 2. ninja build/test
        self.assertEqual(mock_run.call_count, 2)

        calls = [c[0][0] for c in mock_run.call_args_list]

        # Verify first call is cmake configure with source activate.sh
        self.assertIn('. ./activate.sh', calls[0])
        self.assertIn('cmake -B', calls[0])

        # Verify second call is ninja with source activate.sh
        self.assertIn('. ./activate.sh', calls[1])
        self.assertIn('ninja -C', calls[1])

    @mock.patch('pw_presubmit.private.tools.run_subprocess')
    def test_gn_gen(self, mock_run):
        """Test the gn_gen step."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout=""
        )

        bootstrap.gn_gen.run(self.ctx)

        mock_run.assert_called_once_with(
            '. ./activate.sh && gn gen --check out',
            cwd=self.ctx.root,
            env=mock.ANY,
            shell=True,
        )

    @mock.patch('pw_presubmit.private.tools.run_subprocess')
    def test_gn_ninja_build(self, mock_run):
        """Test the gn_ninja_build step."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout=""
        )

        bootstrap.gn_ninja_build.run(self.ctx)

        mock_run.assert_called_once_with(
            '. ./activate.sh && ninja -C out host_clang_debug stm32f429i',
            cwd=self.ctx.root,
            env=mock.ANY,
            shell=True,
        )

    @mock.patch('pw_presubmit.private.tools.run_subprocess')
    def test_gn_ninja_python(self, mock_run):
        """Test the gn_ninja_python step."""
        mock_run.return_value = subprocess.CompletedProcess(
            args=[], returncode=0, stdout=""
        )

        bootstrap.gn_ninja_python.run(self.ctx)

        mock_run.assert_called_once_with(
            '. ./activate.sh && ninja -C out python.lint python.tests',
            cwd=self.ctx.root,
            env=mock.ANY,
            shell=True,
        )


if __name__ == '__main__':
    unittest.main()
