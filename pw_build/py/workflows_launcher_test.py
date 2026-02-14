# Copyright 2025 The Pigweed Authors
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
"""Tests for the workflows launcher CLI."""

import argparse
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from google.protobuf import json_format
from pw_build.proto import workflows_pb2
from pw_build.workflows.launcher import WorkflowsCli


_EXAMPLE_CONFIG = """
{
  "configs": [
    {
      "name": "bazel_default",
      "description": "Default bazel build configuration (host)",
      "build_type": "bazel",
      "args": [],
      "env": {}
    }
  ],
  "output_specs": [
    {
      "name": "output_one_strip",
      "glob_patterns": [
        "bazel-bin/1*"
      ],
      "strip_prefix": "bazel-bin/"
    },
    {
      "name": "output_two_nostrip",
      "glob_patterns": [
        "bazel-bin/2*"
      ]
    }
  ],
  "builds": [
    {
      "name": "all_host",
      "use_config": "bazel_default",
      "targets": [
        "//..."
      ]
    },
    {
      "name": "all_host_cpp20",
      "build_config": {
        "name": "bazel_default_cpp20",
        "description": "Host C++20 build",
        "build_type": "bazel",
        "args": [
          "--//pw_toolchain/cc:cxx_standard=20"
        ]
      },
      "use_output": ["output_one_strip"],
      "targets": [
        "//..."
      ]
    },
    {
      "name": "docs",
      "use_config": "bazel_default",
      "targets": [
        "//docs"
      ]
    },
    {
      "name": "build_rp2040_tests",
      "build_config":{
        "name": "pico_rp2040",
        "description": "Default Pi Pico build (rp2040)",
        "build_type": "bazel",
        "args": [
          "--config=rp2040",
          "--build_tests_only"
        ]
      },
      "targets": [
        "//..."
      ]
    },
    {
      "name": "build_rp2350_tests",
      "build_config": {
        "name": "pico_rp2350",
        "description": "Default Pi Pico build (rp2350)",
        "build_type": "bazel",
        "args": [
          "--config=rp2350",
          "--build_tests_only"
        ]
      },
      "targets": [
        "//..."
      ]
    }
  ],
  "tools": [
    {
      "name": "format",
      "description": "Find and fix code formatting issues",
      "use_config": "bazel_default",
      "analyzer_friendly_args": ["--check"],
      "target": "@pigweed//:format"
    },
    {
      "name": "owners_lint",
      "description": "Identify OWNERS file issues",
      "use_config": "bazel_default",
      "target": "@pigweed//pw_presubmit/py:owners_lint",
      "type": "ANALYZER"
    }
  ],
  "groups": [
    {
      "name": "presubmit",
      "builds": [
        "all_host",
        "docs",
        "rp2040",
        "rp2350"
      ],
      "analyzers": [
        "format",
        "owners_lint"
      ]
    }
  ]
}
"""


class TestWorkflowsLauncher(unittest.TestCase):
    """Tests for the workflows launcher."""

    @staticmethod
    def _load_config(json_config: str):
        json_msg = json.loads(json_config)
        msg = workflows_pb2.WorkflowSuite()
        json_format.ParseDict(json_msg, msg)
        return WorkflowsCli(msg)

    def test_tool(self):
        cli = self._load_config(_EXAMPLE_CONFIG)
        expected = '\n'.join(
            (
                'name: "format"',
                'description: "Find and fix code formatting issues"',
                'use_config: "bazel_default"',
                'target: "@pigweed//:format"',
                'analyzer_friendly_args: "--check"',
            )
        )
        actual = cli.describe().describe(['format']).strip()
        self.assertEqual(actual, expected)

    def test_describe_multiple(self):
        cli = self._load_config(_EXAMPLE_CONFIG)
        expected = '''name: "format"
description: "Find and fix code formatting issues"
use_config: "bazel_default"
target: "@pigweed//:format"
analyzer_friendly_args: "--check"
---
name: "presubmit"
builds: "all_host"
builds: "docs"
builds: "rp2040"
builds: "rp2350"
analyzers: "format"
analyzers: "owners_lint"'''
        actual = cli.describe().describe(['format', 'presubmit']).strip()
        self.assertEqual(actual, expected)

    def test_group(self):
        cli = self._load_config(_EXAMPLE_CONFIG)
        expected = '\n'.join(
            (
                'name: "presubmit"',
                'builds: "all_host"',
                'builds: "docs"',
                'builds: "rp2040"',
                'builds: "rp2350"',
                'analyzers: "format"',
                'analyzers: "owners_lint"',
            )
        )
        actual = cli.describe().describe(['presubmit']).strip()
        self.assertEqual(actual, expected)

    def test_build(self):
        cli = self._load_config(_EXAMPLE_CONFIG)
        expected = '\n'.join(
            (
                'name: "build_rp2040_tests"',
                'build_config {',
                '  name: "pico_rp2040"',
                '  description: "Default Pi Pico build (rp2040)"',
                '  build_type: "bazel"',
                '  args: "--config=rp2040"',
                '  args: "--build_tests_only"',
                '}',
                'targets: "//..."',
            )
        )
        actual = cli.describe().describe(['build_rp2040_tests']).strip()
        self.assertEqual(actual, expected)

    def test_build_config(self):
        cli = self._load_config(_EXAMPLE_CONFIG)
        expected = '\n'.join(
            (
                'name: "bazel_default"',
                'description: "Default bazel build configuration (host)"',
                'build_type: "bazel"',
            )
        )
        actual = cli.describe().describe(['bazel_default']).strip()
        self.assertEqual(actual, expected)

    def test_empty_config(self):
        cli = self._load_config('{}')
        with self.assertRaises(ValueError):
            cli.describe().describe(['bazel_default']).strip()

    def test_empty_fragment_name(self):
        cli = self._load_config(_EXAMPLE_CONFIG)
        with self.assertRaises(ValueError):
            cli.describe().describe(['']).strip()

    @patch('pw_build.workflows.launcher.WorkflowsManager')
    @patch('pw_build.project_builder.ProjectBuilder')
    def test_artifacts_manifest_generation(self, _, mock_manager_class):
        """Generate artifact manifest and check contents"""
        mock_manager = mock_manager_class.return_value
        # Ensure 'all_host_cpp20' is recognized as a build name.
        mock_manager.get_build_names.return_value = ['all_host_cpp20']
        mock_manager.get_group_names.return_value = []
        mock_manager.create_build_recipes.return_value = []

        # Mock collect_artifacts to return a sample BuildArtifacts.
        artifacts = workflows_pb2.BuildArtifacts()
        artifacts.output_root = 'out/all_host_cpp20'
        group = artifacts.output_groups.add()
        group.matching_files.append('foo.bin')
        mock_manager.collect_artifacts.return_value = artifacts

        cli = self._load_config(_EXAMPLE_CONFIG)

        with tempfile.TemporaryDirectory() as tmp_dir:
            manifest_path = Path(tmp_dir) / 'manifest.textproto'
            args = argparse.Namespace(
                directory=None,
                output_dir=Path('out'),
                extra_arg=None,
                artifacts_manifest=manifest_path,
            )

            plugins = cli.plugins(args)
            build_plugin = next(p for p in plugins if p.name() == 'build')

            # Run the plugin for the build that has use_output in the config.
            build_plugin.run(['all_host_cpp20'])

            # Verify that the manifest file was written with the expected
            # content.
            self.assertTrue(manifest_path.exists())
            content = manifest_path.read_text()
            self.assertIn('foo.bin', content)
            self.assertIn('out/all_host_cpp20', content)


if __name__ == '__main__':
    unittest.main()
