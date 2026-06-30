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
"""Tests for pw_ide Rust project generation."""

import json
import os
from pathlib import Path
import unittest
from unittest import mock

from pyfakefs import fake_filesystem_unittest

from pw_ide import rust


class RustTest(fake_filesystem_unittest.TestCase):
    """Tests for pw_ide rust project generation and config."""

    def setUp(self):
        self.setUpPyfakefs()
        self.workspace_root = Path('/workspace')
        self.platform_dir = self.workspace_root / '.compile_commands' / 'host'
        self.fs.create_dir(self.workspace_root)
        self.fs.create_dir(self.platform_dir)

    def test_generate_ide_config(self):
        """Test generating ide_config.json with and without config."""
        rust.generate_ide_config(
            self.platform_dir,
            ['//pw_kernel/...'],
            rust_config='k_host',
        )
        config_path = self.platform_dir / 'ide_config.json'
        self.assertTrue(config_path.exists())
        with open(config_path, 'r') as f:
            data = json.load(f)

        cmd = data['rust_analyzer_check_override_command']
        self.assertIn('--config=k_host', cmd)
        self.assertIn('//pw_kernel/...', cmd)

    def test_temp_remove_rust_project_backup(self):
        """Test temp_remove_rust_project backs up existing rust-project.json."""
        root_project = self.workspace_root / 'rust-project.json'
        self.fs.create_file(root_project, contents='{"test": true}')

        with rust.temp_remove_rust_project(self.workspace_root, 'host'):
            # While inside context, original file should be renamed to backup
            self.assertFalse(root_project.exists())
            self.assertTrue(
                (self.workspace_root / 'rust-project.json.bak').exists()
            )

        # After exiting context, original file should be restored
        self.assertTrue(root_project.exists())
        self.assertEqual(root_project.read_text(), '{"test": true}')

    def test_process_rust_project_success(self):
        """Test process_rust_project runs Bazel and moves rust-project.json."""

        def mock_run_bazel(_cmd, **_kwargs):
            # Simulate Bazel generating rust-project.json at workspace root
            self.fs.create_file(
                self.workspace_root / 'rust-project.json',
                contents='{"crates": []}',
            )
            return mock.Mock(stdout='')

        with mock.patch.dict(
            os.environ, {'BUILD_WORKING_DIRECTORY': str(self.workspace_root)}
        ):
            rust.process_rust_project(
                platform='host',
                workspace_root=self.workspace_root,
                platform_dir=self.platform_dir,
                rust_targets=['//pw_kernel/...'],
                run_bazel_fn=mock_run_bazel,
                rust_config='k_host',
            )

        # Output rust-project.json should be moved to platform_dir
        self.assertTrue((self.platform_dir / 'rust-project.json').exists())
        # Root rust-project.json should now symlink to platform target
        self.assertTrue(
            (self.workspace_root / 'rust-project.json').is_symlink()
        )


if __name__ == '__main__':
    unittest.main()
