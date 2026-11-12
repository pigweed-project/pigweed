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
"""Unit tests for //docs/agents/changelog/scripts."""

import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import common
import end_command
import next_command
import start_command

_SAMPLE_CATEGORIES_TOML = """\
[os]
title = "OS abstraction layers"

[core]
title = "Data structures & utilities"

[os_backends]
parent = "os"
title = "Backends"

[zephyr]
parent = "os_backends"
title = "Zephyr"
"""


def _write_sample_workspace(root: Path) -> None:
    """Creates a minimal Pigweed workspace layout for category tests."""
    metadata_dir = root / "docs" / "common" / "metadata"
    metadata_dir.mkdir(parents=True)
    (metadata_dir / "categories.toml").write_text(
        _SAMPLE_CATEGORIES_TOML, encoding="utf-8"
    )
    (root / "PIGWEED_MODULES").write_text(
        "pw_containers\npw_sync\npw_sync_baremetal\npw_sync_zephyr\n",
        encoding="utf-8",
    )
    modules = {
        "pw_containers": ('[category]\nid = "core"\ntoctree = "Containers"\n'),
        "pw_sync": '[category]\nid = "os"\ntoctree = "Sync"\n',
        "pw_sync_baremetal": (
            '[facade]\nmodule = "pw_sync"\ntoctree = "Bare metal"\n'
        ),
        "pw_sync_zephyr": '[category]\nid = "zephyr"\ntoctree = "Sync"\n',
    }
    for mod_name, content in modules.items():
        mod_dir = root / mod_name
        mod_dir.mkdir()
        (mod_dir / "module.toml").write_text(content, encoding="utf-8")


class ChangelogCategoriesTest(unittest.TestCase):
    """Tests canonical category loading, rollup, validation, and ordering."""

    def test_in_tree_categories_toml_loads_in_display_order(self) -> None:
        """Verifies categories.toml loads top-level entries plus rust."""
        categories_path = Path("docs/common/metadata/categories.toml")
        self.assertTrue(categories_path.is_file())
        data = common.load_categories_data(categories_path)
        keys = list(data.keys())
        self.assertEqual(keys[0], "comms")
        self.assertEqual(keys[-1], "rust")
        self.assertNotIn("bluetooth", keys)
        self.assertNotIn("os_backends", keys)
        self.assertEqual(data["rust"]["title"], "Rust")

    def test_top_level_rollup_and_module_resolution(self) -> None:
        """Verifies subcategory rollup and diff-based category suggestions."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_sample_workspace(root)
            with mock.patch.dict(
                os.environ, {"BUILD_WORKSPACE_DIRECTORY": str(root)}
            ):
                categories = common.load_categories_data()
                self.assertEqual(
                    list(categories.keys()), ["os", "core", "rust"]
                )

                module_cats = common.load_module_categories(root)
                self.assertEqual(
                    module_cats,
                    {
                        "pw_containers": "core",
                        "pw_sync": "os",
                        "pw_sync_baremetal": "os",
                        "pw_sync_zephyr": "os",
                    },
                )

                diff_text = (
                    "diff --git a/pw_containers/rust/lib.rs "
                    "b/pw_containers/rust/lib.rs\n"
                    "diff --git a/pw_sync_zephyr/mutex.cc "
                    "b/pw_sync_zephyr/mutex.cc\n"
                )
                self.assertEqual(
                    common.suggest_commit_categories_from_diff(
                        diff_text, module_cats
                    ),
                    ["rust", "core", "os"],
                )

    def test_category_validation_rejects_and_remaps_subcategories(self) -> None:
        """Verifies subcategories are rejected and remapped to top-level IDs."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_sample_workspace(root)
            with mock.patch.dict(
                os.environ, {"BUILD_WORKSPACE_DIRECTORY": str(root)}
            ):
                res = next_command.check_categories_validity(
                    {"stories": {"zephyr": {}, "cpp": {}, "core": {}}},
                    next_command.get_categories(),
                    common.load_raw_categories_data(),
                )
                self.assertFalse(res["valid"])
                self.assertEqual(res["invalid_categories"], ["cpp", "zephyr"])
                self.assertEqual(
                    res["remapped_subcategories"], {"zephyr": "os"}
                )

    def test_end_command_renders_in_canonical_display_order(self) -> None:
        """Verifies end_command emits categories in categories.toml order."""
        categories_map = {
            "os": "OS abstraction layers",
            "core": "Data structures & utilities",
            "rust": "Rust",
        }
        data = {
            "stories": {
                "rust": {
                    "crate": {
                        "score": 800,
                        "title": "New Rust crate",
                        "body": "Added ``pw_foo`` crate.",
                        "highlight": "Use ``pw_foo`` in Rust.",
                        "example": "",
                    }
                },
                "core": {
                    "vec": {
                        "score": 600,
                        "title": "Vector improvements",
                        "body": "``pw::Vector`` is faster.",
                        "highlight": "Speed up ``pw::Vector``.",
                        "example": "",
                    }
                },
                "os": {
                    "sync": {
                        "score": 700,
                        "title": "Mutex improvements",
                        "body": "``pw::sync::Mutex`` is smaller.",
                        "highlight": "Shrink ``pw::sync::Mutex``.",
                        "example": "",
                    }
                },
            }
        }
        rst = end_command.build_rst_for_month(
            "2026", "10", data, categories_map
        )
        os_pos = rst.index("\nOS abstraction layers\n")
        core_pos = rst.index("\nData structures & utilities\n")
        rust_pos = rst.index("\nRust\n")
        self.assertLess(os_pos, core_pos)
        self.assertLess(core_pos, rust_pos)

    def test_start_check_categories_allows_non_alphabetical_display_order(
        self,
    ) -> None:
        """Verifies start_command allows non-alphabetical display order."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_sample_workspace(root)
            resources = root / "docs" / "agents" / "changelog" / "resources"
            resources.mkdir(parents=True)
            (resources / "README.md").write_text("readme\n", encoding="utf-8")
            (resources / "stale.json").write_text("{}\n", encoding="utf-8")
            with mock.patch.dict(
                os.environ, {"BUILD_WORKSPACE_DIRECTORY": str(root)}
            ):
                start_command.check_categories()
                start_command.clean_resources_dir()
                self.assertTrue((resources / "README.md").exists())
                self.assertFalse((resources / "stale.json").exists())

    def test_end_command_orders_story_cls_chronologically(self) -> None:
        """Verifies end_command orders CL links chronologically by date."""
        data = {
            "stories": {
                "core": {
                    "status": {
                        "score": 800,
                        "title": "Custom status types",
                        "body": "Added ``pw::StatusBase``.",
                        "highlight": "Use ``pw::StatusBase``.",
                        "example": "",
                        "commits": {
                            "ffff": {
                                "summary": "first",
                                "date": "2026-10-02 10:00:00-07:00",
                                "url": "https://pwrev.dev/100",
                            },
                            "0000": {
                                "summary": "second",
                                "date": "2026-10-15 10:00:00-07:00",
                                "url": "https://pwrev.dev/200",
                            },
                        },
                    }
                }
            }
        }
        rst = end_command.build_rst_for_month(
            "2026", "10", data, {"core": "Data structures & utilities"}
        )
        self.assertIn(
            "CLs: `1 <https://pwrev.dev/100>`__, `2 <https://pwrev.dev/200>`__",
            rst,
        )


if __name__ == "__main__":
    unittest.main()
