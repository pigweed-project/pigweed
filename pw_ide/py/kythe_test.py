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
"""Tests for pw_ide.kythe."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from pw_ide.kythe import (
    DEFAULT_CORPUS,
    _find_required_headers,
    extract_rust_units,
    extract_single_command,
    find_compilation_databases,
    find_kzip_binary,
    find_rust_projects,
)


class KytheExtractorTest(unittest.TestCase):
    """Tests for Kythe compilation unit extraction."""

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.workspace = Path(self.temp_dir.name)

        # Create sample source and header
        self.src_file = self.workspace / "test.cc"
        self.header_file = self.workspace / "test.h"
        self.transitive_header_file = self.workspace / "transitive.h"

        self.transitive_header_file.write_text(
            "#pragma once\nint getTransitiveValue();\n"
        )
        self.header_file.write_text(
            '#pragma once\n#include "transitive.h"\nint getValue();\n'
        )
        self.src_file.write_text(
            '#include "test.h"\nint getValue() { return 42; }\n'
        )

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_find_compilation_databases(self):
        compdb_path = self.workspace / "compile_commands.json"
        compdb_path.write_text("[]")
        found = find_compilation_databases(self.workspace)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0], compdb_path)

    def test_find_compilation_databases_multiple(self):
        """Tests finding compile_commands.json across multiple locations."""
        root_db = self.workspace / "compile_commands.json"
        root_db.write_text("[]")
        out_db = self.workspace / "out" / "compile_commands.json"
        out_db.parent.mkdir(parents=True)
        out_db.write_text("[]")
        cc_db = (
            self.workspace
            / ".compile_commands"
            / "foo"
            / "compile_commands.json"
        )
        cc_db.parent.mkdir(parents=True)
        cc_db.write_text("[]")
        contest_db = self.workspace / "contest" / "compile_commands.json"
        contest_db.parent.mkdir(parents=True)
        contest_db.write_text("[]")
        test_db = self.workspace / "test" / "compile_commands.json"
        test_db.parent.mkdir(parents=True)
        test_db.write_text("[]")
        tests_db = self.workspace / "tests" / "compile_commands.json"
        tests_db.parent.mkdir(parents=True)
        tests_db.write_text("[]")

        found = find_compilation_databases(self.workspace)
        self.assertIn(root_db.resolve(), found)
        self.assertIn(out_db.resolve(), found)
        self.assertIn(cc_db.resolve(), found)
        self.assertIn(contest_db.resolve(), found)
        self.assertNotIn(test_db.resolve(), found)
        self.assertNotIn(tests_db.resolve(), found)

    def test_find_required_headers_recursive(self):
        headers = _find_required_headers(self.src_file, [], self.workspace)
        self.assertIn(self.header_file.resolve(), headers)
        self.assertIn(self.transitive_header_file.resolve(), headers)

    def test_find_required_headers_cmd_dir_relative(self):
        out_dir = self.workspace / "out"
        out_dir.mkdir()
        module_dir = self.workspace / "pw_module" / "public" / "pw_module"
        module_dir.mkdir(parents=True)
        module_header = module_dir / "header.h"
        module_header.write_text("#pragma once\nint getModuleValue();\n")

        src_with_include = self.workspace / "src_test.cc"
        src_with_include.write_text(
            '#include "pw_module/header.h"\nint func() { return 0; }\n'
        )

        # Include path is relative to out_dir: -I../pw_module/public
        include_dirs = ["../pw_module/public"]
        headers = _find_required_headers(
            src_with_include,
            include_dirs,
            self.workspace,
            cmd_dir=out_dir,
        )
        self.assertIn(module_header.resolve(), headers)

    def test_extract_single_command_missing_file(self):
        out_dir = self.workspace / "out"
        out_dir.mkdir()
        entry = {
            "directory": str(self.workspace),
            "command": "clang++ -c non_existent.cc",
            "file": "non_existent.cc",
        }
        res = extract_single_command(
            entry, 0, out_dir, self.workspace, corpus=DEFAULT_CORPUS
        )
        self.assertIsNone(res)

    def test_find_rust_projects(self):
        """Tests discovering rust-project.json in the workspace root."""
        rp_path = self.workspace / "rust-project.json"
        rp_path.write_text("{}")
        found = find_rust_projects(self.workspace)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0], rp_path.resolve())

    def test_find_rust_projects_compile_commands(self):
        """Tests discovering rust-project.json under .compile_commands."""
        cc_dir = self.workspace / ".compile_commands" / "target"
        cc_dir.mkdir(parents=True)
        rp_path = cc_dir / "rust-project.json"
        rp_path.write_text("{}")
        found = find_rust_projects(self.workspace)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0], rp_path.resolve())

    def test_extract_rust_units_empty_manifest(self):
        """Tests extracting Rust units with an empty crates list."""
        rp_path = self.workspace / "rust-project.json"
        rp_path.write_text(json.dumps({"crates": []}))
        out_dir = self.workspace / "out"
        out_dir.mkdir()
        res = extract_rust_units([rp_path], out_dir, self.workspace)
        self.assertEqual(res, [])

    def test_extract_rust_units_valid_crate(self):
        """Tests extracting a valid Rust compilation unit into a kzip."""
        kzip_bin = find_kzip_binary()
        if not shutil.which(kzip_bin) and not Path(kzip_bin).exists():
            self.skipTest("kzip binary not found")
        crate_dir = self.workspace / "pw_sample"
        crate_dir.mkdir()
        src_file = crate_dir / "lib.rs"
        src_file.write_text("pub fn sample() -> i32 { 42 }")
        rp_path = self.workspace / "rust-project.json"
        rp_path.write_text(
            json.dumps(
                {
                    "crates": [
                        {
                            "display_name": "pw_sample",
                            "root_module": "pw_sample/lib.rs",
                            "edition": "2021",
                            "deps": [],
                            "is_workspace_member": True,
                        }
                    ]
                }
            )
        )
        out_dir = self.workspace / "out"
        out_dir.mkdir()
        res = extract_rust_units(
            [rp_path], out_dir, self.workspace, kzip_bin=kzip_bin
        )
        self.assertEqual(len(res), 1)
        self.assertTrue(res[0].exists())

        # Verify unit metadata via kzip info
        info_res = subprocess.run(
            [kzip_bin, "info", f"-input={res[0]}"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(info_res.returncode, 0)
        info = json.loads(info_res.stdout)
        corpus_data = info.get("corpora", {}).get(DEFAULT_CORPUS, {})
        cu_info = corpus_data.get("language_cu_info", {})
        self.assertEqual(list(cu_info.keys()), ["rust"])

    def test_find_rust_projects_ignores_hidden_directories(self):
        """Tests that rust-project.json in hidden directories is ignored."""
        hidden_dir = self.workspace / ".git" / "subdir"
        hidden_dir.mkdir(parents=True)
        (hidden_dir / "rust-project.json").write_text("{}")
        found = find_rust_projects(self.workspace)
        self.assertEqual(found, [])

    def test_extract_rust_units_malformed_json(self):
        """Tests that malformed rust-project.json is skipped gracefully."""
        rp_path = self.workspace / "rust-project.json"
        rp_path.write_text("{ invalid json")
        out_dir = self.workspace / "out"
        out_dir.mkdir()
        res = extract_rust_units([rp_path], out_dir, self.workspace)
        self.assertEqual(res, [])

    def test_extract_rust_units_filters_external_crates(self):
        """Tests that external crates outside the workspace are excluded."""
        kzip_bin = find_kzip_binary()
        if not shutil.which(kzip_bin) and not Path(kzip_bin).exists():
            self.skipTest("kzip binary not found")
        local_dir = self.workspace / "pw_sample"
        local_dir.mkdir()
        (local_dir / "lib.rs").write_text("pub fn sample() -> i32 { 42 }")

        with tempfile.TemporaryDirectory() as ext_temp:
            ext_dir = Path(ext_temp) / "external_pkg"
            ext_dir.mkdir()
            (ext_dir / "lib.rs").write_text("pub fn ext() -> i32 { 100 }")

            rp_path = self.workspace / "rust-project.json"
            rp_path.write_text(
                json.dumps(
                    {
                        "crates": [
                            {
                                "display_name": "pw_sample",
                                "root_module": "pw_sample/lib.rs",
                                "edition": "2021",
                                "deps": [],
                                "is_workspace_member": True,
                            },
                            {
                                "display_name": "external_pkg",
                                "root_module": str(ext_dir / "lib.rs"),
                                "edition": "2021",
                                "deps": [],
                                "is_workspace_member": False,
                            },
                        ]
                    }
                )
            )
            out_dir = self.workspace / "out"
            out_dir.mkdir()
            res = extract_rust_units(
                [rp_path], out_dir, self.workspace, kzip_bin=kzip_bin
            )
            self.assertEqual(len(res), 1)
            self.assertTrue(res[0].exists())
            self.assertIn("rust_pw_sample_0.kzip", res[0].name)


if __name__ == "__main__":
    unittest.main()
