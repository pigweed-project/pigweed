# Copyright 2024 The Pigweed Authors
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
"""Tests for the config_file module."""

import json
from pathlib import Path
import tempfile
import unittest

from pw_env_setup import config_file


class TestConfigFile(unittest.TestCase):
    """Tests for loading pigweed.json configurations."""

    def test_default_load_returns_dict(self):
        config = config_file.load()
        self.assertIsInstance(config, dict)

    def test_config_load_with_workspace_directory(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            json_file = Path(tmp_dir) / 'pigweed.json'
            json_file.write_text(json.dumps({'pw': {'custom_key': 'test_val'}}))

            config = config_file.load(
                env={'BUILD_WORKSPACE_DIRECTORY': str(tmp_dir)}
            )
            self.assertEqual(config, {'pw': {'custom_key': 'test_val'}})

    def test_config_load_with_project_root(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            json_file = Path(tmp_dir) / 'pigweed.json'
            json_file.write_text(json.dumps({'pw': {'root_key': 'root_val'}}))

            config = config_file.load(env={'PW_PROJECT_ROOT': str(tmp_dir)})
            self.assertEqual(config, {'pw': {'root_key': 'root_val'}})

    def test_config_load_missing_returns_empty(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            config = config_file.load(
                env={'BUILD_WORKSPACE_DIRECTORY': str(tmp_dir)}
            )
            self.assertEqual(config, {})

    def test_config_load_variable_substitution(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            json_file = Path(tmp_dir) / 'pigweed.json'
            payload = {'pw': {'path': '$pw_env{BUILD_WORKSPACE_DIRECTORY}/foo'}}
            json_file.write_text(json.dumps(payload))

            config = config_file.load(
                env={'BUILD_WORKSPACE_DIRECTORY': str(tmp_dir)}
            )
            self.assertEqual(config, {'pw': {'path': f'{tmp_dir}/foo'}})


if __name__ == '__main__':
    unittest.main()
