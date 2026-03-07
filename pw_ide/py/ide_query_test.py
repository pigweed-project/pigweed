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
"""Tests for ide_query."""

import argparse
import io
import json
import os
from pathlib import Path
import unittest
from unittest import mock

from pw_ide import ide_query


class IdeQueryTest(unittest.TestCase):
    """Tests for ide_query."""

    def setUp(self):
        # pylint: disable=invalid-name
        self.maxDiff = None
        # pylint: enable=invalid-name
        # Mock env vars
        self.env_patcher = mock.patch.dict(
            os.environ, {"BUILD_WORKSPACE_DIRECTORY": "/workspace"}
        )
        self.env_patcher.start()

    def tearDown(self):
        self.env_patcher.stop()

    def test_relativize_path(self):
        """Test _relativize_path logic."""
        # pylint: disable=protected-access
        workspace = Path("/workspace")
        self.assertEqual(
            ide_query._relativize_path("/workspace/foo/bar.cc", workspace),
            Path("foo/bar.cc"),
        )
        self.assertEqual(
            ide_query._relativize_path("/workspace/bar.h", workspace),
            Path("bar.h"),
        )
        # Test path outside workspace (should return absolute)
        self.assertEqual(
            ide_query._relativize_path("/other/path.cc", workspace),
            Path("/other/path.cc"),
        )
        # Test relative path (should return as-is)
        self.assertEqual(
            ide_query._relativize_path("foo/bar.cc", workspace),
            Path("foo/bar.cc"),
        )
        # pylint: enable=protected-access

    def test_main_ok(self):
        """Test successful query."""
        workspace = Path("/workspace")
        compile_commands_dir = workspace / ".compile_commands" / "platform"
        src_file = workspace / "foo/bar.cc"

        def mock_open_side_effect(*args, **kwargs):
            filename = str(args[0])
            if "deps_mapping.json" in filename:
                return mock.mock_open(
                    read_data=json.dumps(
                        {
                            "files": {"foo/bar.cc": ["//foo:bar"]},
                            "targets": {
                                "//foo:bar": {
                                    "label": "//foo:bar",
                                    "srcs": [str(src_file)],
                                    "hdrs": [],
                                    "deps": ["//foo:baz"],
                                },
                                "//foo:baz": {
                                    "label": "//foo:baz",
                                    "srcs": [],
                                    "hdrs": [],
                                    "deps": [],
                                },
                            },
                        }
                    )
                )(*args, **kwargs)
            if "compile_commands.json" in filename:
                return mock.mock_open(
                    read_data=json.dumps(
                        [{"file": str(src_file), "arguments": ["-Ifoo"]}]
                    )
                )(*args, **kwargs)
            return mock.mock_open()(*args, **kwargs)

        # Mock args
        with mock.patch(
            "argparse.ArgumentParser.parse_args",
            return_value=argparse.Namespace(
                file=str(src_file), compile_commands_dir=compile_commands_dir
            ),
        ):
            # Mock file existence
            with mock.patch("pathlib.Path.exists", return_value=True):
                # Mock file open
                with mock.patch(
                    "builtins.open", side_effect=mock_open_side_effect
                ):
                    with mock.patch(
                        "sys.stdout", new_callable=io.StringIO
                    ) as mock_stdout:
                        ide_query.main()
                        output = mock_stdout.getvalue()

        self.assertIn('working_dir: "/workspace"', output)
        self.assertIn('source_file_path: "foo/bar.cc"', output)
        self.assertIn('id: "//foo:bar"', output)
        self.assertIn('source_file_paths: ["foo/bar.cc"]', output)
        self.assertIn('dependency_ids: ["//foo:baz"]', output)
        self.assertIn('compiler_arguments: ["-Ifoo"]', output)

    def test_main_not_found(self):
        """Test file not found."""
        workspace = Path("/workspace")
        compile_commands_dir = workspace / ".compile_commands" / "platform"
        src_file = workspace / "foo/unknown.cc"

        def mock_open_side_effect(*args, **kwargs):
            filename = str(args[0])
            if "deps_mapping.json" in filename:
                return mock.mock_open(
                    read_data=json.dumps({"files": {}, "targets": {}})
                )(*args, **kwargs)
            return mock.mock_open(read_data="[]")(*args, **kwargs)

        with mock.patch(
            "argparse.ArgumentParser.parse_args",
            return_value=argparse.Namespace(
                file=str(src_file), compile_commands_dir=compile_commands_dir
            ),
        ):
            with mock.patch("pathlib.Path.exists", return_value=True):
                with mock.patch(
                    "builtins.open",
                    side_effect=mock_open_side_effect,
                ):
                    with mock.patch(
                        "sys.stdout", new_callable=io.StringIO
                    ) as mock_stdout:
                        ide_query.main()
                        output = mock_stdout.getvalue()

        self.assertIn('code: CODE_NOT_FOUND', output)
        self.assertIn('source_file_path: "foo/unknown.cc"', output)

    def test_local_path_override(self):
        """Test query for a file that is a local path override."""
        workspace = Path("/workspace")
        compile_commands_dir = workspace / ".compile_commands" / "platform"
        # This path represents a file that might be referred to as
        # external/dep/foo.cc but due to local_path_override, it really exists
        # at third_party/dep/foo.cc
        # and merger.py (once fixed) will output it as such.
        src_file = workspace / "third_party/dep/foo.cc"

        def mock_open_side_effect(*args, **kwargs):
            filename = str(args[0])
            if "deps_mapping.json" in filename:
                return mock.mock_open(
                    read_data=json.dumps(
                        {
                            "files": {"third_party/dep/foo.cc": ["//dep:foo"]},
                            "targets": {
                                "//dep:foo": {
                                    "label": "//dep:foo",
                                    "srcs": [str(src_file)],
                                    "hdrs": [],
                                    "deps": [],
                                }
                            },
                        }
                    )
                )(*args, **kwargs)
            if "compile_commands.json" in filename:
                return mock.mock_open(
                    read_data=json.dumps(
                        [
                            {
                                "file": str(src_file),
                                "arguments": ["-Ithird_party/dep"],
                            }
                        ]
                    )
                )(*args, **kwargs)
            return mock.mock_open()(*args, **kwargs)

        with mock.patch(
            "argparse.ArgumentParser.parse_args",
            return_value=argparse.Namespace(
                file=str(src_file), compile_commands_dir=compile_commands_dir
            ),
        ):
            with mock.patch("pathlib.Path.exists", return_value=True):
                with mock.patch(
                    "builtins.open", side_effect=mock_open_side_effect
                ):
                    with mock.patch(
                        "sys.stdout", new_callable=io.StringIO
                    ) as mock_stdout:
                        ide_query.main()
                        output = mock_stdout.getvalue()

        self.assertIn('working_dir: "/workspace"', output)
        self.assertIn('source_file_path: "third_party/dep/foo.cc"', output)
        self.assertIn('id: "//dep:foo"', output)
        # Ensure that even if it was originally an external repo, if it's
        # resolved locally, we get the local path and arguments.
        self.assertIn('compiler_arguments: ["-Ithird_party/dep"]', output)


if __name__ == "__main__":
    unittest.main()
