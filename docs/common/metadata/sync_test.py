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
"""Unit tests for //docs/common/metadata/sync.py."""

from __future__ import annotations

import copy
from pathlib import Path
import tempfile
from typing import Any
import unittest

from docs.common.metadata import sync

_TEST_CATEGORIES_SCHEMA: dict[str, Any] = {
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "additionalProperties": {
        "type": "object",
        "additionalProperties": False,
        "required": ["title", "description", "homepage"],
        "oneOf": [
            {"required": ["icon"], "not": {"required": ["parent"]}},
            {"required": ["parent"], "not": {"required": ["icon"]}},
        ],
        "properties": {
            "title": {"type": "string", "minLength": 1},
            "icon": {"type": "string", "minLength": 1},
            "description": {"type": "string", "minLength": 1},
            "homepage": {
                "type": "string",
                "pattern": "^//(docs/sphinx|targets)/.+\\.rst$",
            },
            "parent": {"type": "string", "pattern": "^[a-z0-9_]+$"},
            "pin": {
                "type": "array",
                "minItems": 1,
                "uniqueItems": True,
                "items": {"type": "string", "pattern": "^[a-z0-9_]+$"},
            },
        },
    },
}

_TEST_MODULE_SCHEMA: dict[str, Any] = {
    "$schema": "http://json-schema.org/draft-07/schema#",
    "type": "object",
    "additionalProperties": False,
    "required": ["status"],
    "anyOf": [
        {"required": ["category"]},
        {"required": ["facade"]},
    ],
    "properties": {
        "status": {
            "type": "string",
            "enum": [
                "deprecated",
                "experimental",
                "stable",
                "unstable",
            ],
        },
        "category": {
            "type": "object",
            "additionalProperties": False,
            "required": ["id", "toctree"],
            "properties": {
                "id": {"type": "string", "minLength": 1},
                "toctree": {"type": "string", "minLength": 1},
            },
        },
        "facade": {
            "type": "object",
            "additionalProperties": False,
            "required": ["module", "toctree"],
            "properties": {
                "module": {"type": "string", "pattern": "^pw_.*$"},
                "toctree": {"type": "string", "minLength": 1},
            },
        },
        "languages": {
            "type": "array",
            "uniqueItems": True,
            "items": {"type": "string"},
        },
        "size": {"type": "string", "minLength": 1},
        "tagline": {"type": "string", "minLength": 1},
    },
}

_SAMPLE_DATA: dict[str, Any] = {
    "categories": {
        "core": {
            "title": "Core utilities",
            "icon": "gear",
            "description": "Foundational types and helpers",
            "homepage": "//docs/sphinx/core/index.rst",
        },
        "os": {
            "title": "OS abstraction",
            "icon": "cpu",
            "description": "Operating system support.",
            "homepage": "//docs/sphinx/os/index.rst",
        },
    },
    "modules": {
        "pw_alpha": {
            "status": "stable",
            "category": {"id": "core", "toctree": "Alpha"},
            "tagline": "Alpha utilities",
            "languages": ["C++", "Rust"],
        },
        "pw_beta": {
            "status": "experimental",
            "category": {"id": "os", "toctree": "Beta"},
            "tagline": 'Beta "quoted" helpers.',
            "languages": ["Python"],
        },
    },
}


def _make_metadata(data: dict[str, Any] | None = None) -> sync.Metadata:
    resolved = copy.deepcopy(_SAMPLE_DATA if data is None else data)
    return sync.Metadata(
        pigweed_modules=sorted(resolved.get("modules", {}).keys()),
        categories_data=resolved["categories"],
        modules_data=resolved["modules"],
        categories_schema=_TEST_CATEGORIES_SCHEMA,
        module_schema=_TEST_MODULE_SCHEMA,
    )


class MarkersTest(unittest.TestCase):
    """Tests for Markers.split."""

    def test_split_valid(self) -> None:
        text = "before\n.. categories-start\ninside\n.. categories-end\nafter\n"
        before, after = sync.RST_MARKERS.split(text, "//docs/sphinx/test.rst")
        self.assertEqual(before, "before\n")
        self.assertEqual(after, "\nafter\n")

    def test_split_none_raises(self) -> None:
        with self.assertRaisesRegex(sync.SyncError, "is missing"):
            sync.RST_MARKERS.split(None, "//docs/sphinx/test.rst")

    def test_split_missing_markers_raises(self) -> None:
        with self.assertRaisesRegex(sync.SyncError, "missing its"):
            sync.RST_MARKERS.split("no markers here\n", "//docs/sphinx/a.rst")

    def test_split_reversed_markers_raises(self) -> None:
        text = ".. categories-end\n.. categories-start\n"
        with self.assertRaisesRegex(sync.SyncError, "missing its"):
            sync.RST_MARKERS.split(text, "//docs/sphinx/a.rst")


class MetadataValidationTest(unittest.TestCase):
    """Tests for Metadata.validate and helper methods."""

    def test_valid_metadata(self) -> None:
        meta = _make_metadata()
        self.assertEqual(meta.validate(), [])
        self.assertEqual(meta.category_relpath("core"), "core/index.rst")
        self.assertEqual(meta.category_docname("core"), "core/index")
        self.assertEqual(meta.category_modules("os"), ["pw_beta"])
        self.assertTrue(meta.has_category("pw_alpha"))

    def test_schema_validation_error(self) -> None:
        data = copy.deepcopy(_SAMPLE_DATA)
        data["modules"]["pw_alpha"]["status"] = "invalid_status"
        meta = _make_metadata(data)
        issues = meta.validate()
        self.assertEqual(len(issues), 1)
        self.assertIn("//pw_alpha/module.toml", issues[0])
        self.assertIn("at 'status'", issues[0])

    def test_unknown_category(self) -> None:
        data = copy.deepcopy(_SAMPLE_DATA)
        data["modules"]["pw_alpha"]["category"] = {
            "id": "nonexistent",
            "toctree": "Alpha",
        }
        meta = _make_metadata(data)
        issues = meta.validate()
        # "core" also becomes empty because pw_alpha was its only module.
        self.assertTrue(any("nonexistent" in issue for issue in issues))

    def test_empty_category(self) -> None:
        data = copy.deepcopy(_SAMPLE_DATA)
        data["categories"]["empty_category"] = {
            "title": "Empty",
            "icon": "question",
            "description": "Unused category",
            "homepage": "//docs/sphinx/empty/index.rst",
        }
        meta = _make_metadata(data)
        issues = meta.validate()
        self.assertEqual(len(issues), 1)
        self.assertIn("Category 'empty_category'", issues[0])

    def test_duplicate_category_homepage(self) -> None:
        data = copy.deepcopy(_SAMPLE_DATA)
        data["categories"]["os"]["homepage"] = "//docs/sphinx/core/index.rst"
        meta = _make_metadata(data)
        issues = meta.validate()
        self.assertEqual(len(issues), 1)
        self.assertIn("share the homepage", issues[0])

    def test_missing_and_extra_pigweed_modules(self) -> None:
        data = copy.deepcopy(_SAMPLE_DATA)
        meta = sync.Metadata(
            pigweed_modules=["pw_alpha", "pw_gamma"],
            categories_data=data["categories"],
            modules_data=data["modules"],
            categories_schema=_TEST_CATEGORIES_SCHEMA,
            module_schema=_TEST_MODULE_SCHEMA,
        )
        issues = meta.validate()
        self.assertEqual(len(issues), 2)
        self.assertTrue(any("pw_gamma" in issue for issue in issues))
        self.assertTrue(any("pw_beta" in issue for issue in issues))

    def test_facade_validation(self) -> None:
        """Tests validation of the 'facade' field on backend modules."""
        data = copy.deepcopy(_SAMPLE_DATA)
        data["modules"]["pw_beta_basic"] = {
            "status": "stable",
            "facade": {"module": "pw_beta", "toctree": "Basic"},
        }
        data["modules"]["pw_beta_core"] = {
            "status": "stable",
            "facade": {"module": "pw_beta", "toctree": "Core"},
            "category": {"id": "core", "toctree": "Beta"},
        }
        meta = _make_metadata(data)
        self.assertEqual(meta.validate(), [])
        self.assertEqual(meta.facades(), ["pw_beta"])
        self.assertEqual(
            meta.facade_backends("pw_beta"), ["pw_beta_basic", "pw_beta_core"]
        )

        # Undefined facade.
        bad = copy.deepcopy(data)
        bad["modules"]["pw_beta_basic"]["facade"] = {
            "module": "pw_missing",
            "toctree": "Basic",
        }
        self.assertTrue(
            any("not defined" in i for i in _make_metadata(bad).validate())
        )

        # Chained facade (backend pointing to another backend).
        chained = copy.deepcopy(data)
        chained["modules"]["pw_beta_core"]["facade"] = {
            "module": "pw_beta_basic",
            "toctree": "Core",
        }
        self.assertTrue(
            any(
                "itself a backend" in i
                for i in _make_metadata(chained).validate()
            )
        )

        # Overlapping category with facade.
        overlap = copy.deepcopy(data)
        overlap["modules"]["pw_beta_core"]["category"] = {
            "id": "os",
            "toctree": "Beta",
        }
        self.assertTrue(
            any(
                "already on its facade" in i
                for i in _make_metadata(overlap).validate()
            )
        )

    def test_subcategory_validation(self) -> None:
        """Tests validation of parent-child category hierarchies."""
        data = copy.deepcopy(_SAMPLE_DATA)
        # Parent category with no direct modules, only a subcategory.
        data["categories"]["hw"] = {
            "title": "Hardware",
            "icon": "package-dependencies",
            "description": "Hardware support",
            "homepage": "//docs/sphinx/hw/index.rst",
        }
        data["categories"]["rpi"] = {
            "parent": "hw",
            "title": "Raspberry Pi",
            "description": "RP2040 support",
            "homepage": "//targets/rp2040/target_docs.rst",
        }
        data["modules"]["pw_rpi"] = {
            "status": "stable",
            "category": {"id": "rpi", "toctree": "RPI"},
        }
        meta = _make_metadata(data)
        self.assertEqual(meta.validate(), [])
        self.assertEqual(meta.top_level_categories(), ["core", "os", "hw"])
        self.assertEqual(meta.subcategories("hw"), ["rpi"])
        self.assertEqual(meta.category_depth("hw"), 0)
        self.assertEqual(meta.category_depth("rpi"), 1)

        # Undefined parent category.
        bad_parent = copy.deepcopy(data)
        bad_parent["categories"]["rpi"]["parent"] = "nonexistent"
        self.assertTrue(
            any(
                "lists parent 'nonexistent'" in i
                for i in _make_metadata(bad_parent).validate()
            )
        )

        # Circular parent chain.
        cycle = copy.deepcopy(data)
        cycle["categories"]["hw"].pop("icon")
        cycle["categories"]["hw"]["parent"] = "rpi"
        self.assertTrue(
            any(
                "circular parent chain" in i
                for i in _make_metadata(cycle).validate()
            )
        )

        # Excessive nesting depth (> 4 levels below top-level, exceeding H6).
        deep = copy.deepcopy(data)
        prev = "hw"
        for idx in range(1, 6):
            cid = f"sub_{idx}"
            deep["categories"][cid] = {
                "parent": prev,
                "title": f"Sub {idx}",
                "description": f"Level {idx}",
                "homepage": f"//docs/sphinx/{cid}/index.rst",
            }
            prev = cid
        deep["modules"]["pw_deep"] = {
            "status": "stable",
            "category": {"id": "sub_5", "toctree": "Deep"},
        }
        self.assertTrue(
            any(
                "nested too deeply" in i
                for i in _make_metadata(deep).validate()
            )
        )

    def test_pin_ordering_and_validation(self) -> None:
        """Tests pinning modules and subcategories and validating pin items."""
        data = copy.deepcopy(_SAMPLE_DATA)
        data["categories"]["os"]["pin"] = ["pw_zeta", "pw_delta"]
        data["categories"]["zephyr"] = {
            "parent": "os",
            "title": "Zephyr",
            "description": "Zephyr support",
            "homepage": "//docs/sphinx/os/zephyr/index.rst",
        }
        data["categories"]["freertos"] = {
            "parent": "os",
            "title": "FreeRTOS",
            "description": "FreeRTOS support",
            "homepage": "//docs/sphinx/os/freertos/index.rst",
        }
        data["modules"]["pw_delta"] = {
            "status": "stable",
            "category": {"id": "os", "toctree": "Delta"},
        }
        data["modules"]["pw_zeta"] = {
            "status": "stable",
            "category": {"id": "os", "toctree": "Zeta"},
        }
        data["modules"]["pw_zephyr"] = {
            "status": "stable",
            "category": {"id": "zephyr", "toctree": "Zephyr"},
        }
        data["modules"]["pw_freertos"] = {
            "status": "stable",
            "category": {"id": "freertos", "toctree": "FreeRTOS"},
        }
        meta = _make_metadata(data)
        self.assertEqual(meta.validate(), [])
        # Pinned modules appear first in pin order, then remaining sorted.
        self.assertEqual(
            meta.category_modules("os"), ["pw_zeta", "pw_delta", "pw_beta"]
        )
        rendered = sync.render_category_page(meta, "os", None)
        # Direct modules appear before subcategories, with pinned items first.
        self.assertIn(
            "   self\n"
            "   Zeta <../pw_zeta/docs>\n"
            "   Delta <../pw_delta/docs>\n"
            "   Beta <../pw_beta/docs>\n"
            "   Zephyr <zephyr/index>\n"
            "   FreeRTOS <freertos/index>",
            rendered,
        )

        # Pinning a subcategory puts it in the pinned position.
        sub_pinned = copy.deepcopy(data)
        sub_pinned["categories"]["os"]["pin"] = ["freertos", "pw_zeta"]
        meta_sub = _make_metadata(sub_pinned)
        self.assertEqual(meta_sub.validate(), [])
        self.assertEqual(meta_sub.subcategories("os"), ["freertos", "zephyr"])
        rendered_sub = sync.render_category_page(meta_sub, "os", None)
        self.assertIn(
            "   self\n"
            "   FreeRTOS <freertos/index>\n"
            "   Zeta <../pw_zeta/docs>\n"
            "   Beta <../pw_beta/docs>\n"
            "   Delta <../pw_delta/docs>\n"
            "   Zephyr <zephyr/index>",
            rendered_sub,
        )

        # Pinning a module or subcategory that does not belong to the category
        # fails validation.
        bad_pin = copy.deepcopy(data)
        bad_pin["categories"]["os"]["pin"] = ["pw_alpha"]
        self.assertTrue(
            any(
                "pins 'pw_alpha', which is not a direct module or subcategory"
                in i
                for i in _make_metadata(bad_pin).validate()
            )
        )


class SphinxRenderTest(unittest.TestCase):
    """Tests for Sphinx rendering functions."""

    def test_render_generated_category_page(self) -> None:
        meta = _make_metadata()
        rendered = sync.render_category_page(meta, "os", None)
        self.assertIn(
            ".. _docs-category-os:\n\n==============\nOS abstraction", rendered
        )
        self.assertIn(
            "-------\nModules\n-------\n"
            "The following table lists all modules related to OS abstraction.",
            rendered,
        )
        # pw_beta has "os" as its category, so it uses a relative path with its
        # category.toctree title.
        self.assertIn("   self\n   Beta <../pw_beta/docs>", rendered)
        # Quotes in CSV cells should be escaped as "".
        self.assertIn('""quoted""', rendered)
        self.assertIn(
            ".. | END OF AUTO-GENERATED SECTION",
            rendered,
        )

    def test_render_backends_page(self) -> None:
        data = copy.deepcopy(_SAMPLE_DATA)
        data["modules"]["pw_beta_basic"] = {
            "status": "stable",
            "facade": {"module": "pw_beta", "toctree": "Basic"},
        }
        data["modules"]["pw_beta_core"] = {
            "status": "stable",
            "facade": {"module": "pw_beta", "toctree": "Core"},
            "category": {"id": "core", "toctree": "Beta"},
        }
        meta = _make_metadata(data)
        rendered = sync.render_backends_page(meta, "pw_beta", None)
        # Uncategorized backend uses relative path with facade toctree title.
        self.assertIn("   Basic <../pw_beta_basic/docs>", rendered)
        # Target-categorized backend uses pw:// link with facade toctree title.
        self.assertIn("   Core <pw://pw_beta_core/docs.html>", rendered)

    def test_render_existing_category_page_preserves_prose(self) -> None:
        meta = _make_metadata()
        existing = (
            ".. _docs-category-os:\n\n"
            "==============\n"
            "OS abstraction\n"
            "==============\n"
            "Custom introductory prose.\n\n"
            ".. toctree::\n"
            "   :hidden:\n\n"
            "   self\n"
            "   guide\n\n"
            ".. categories-start\n"
            "old content\n"
            ".. categories-end\n\n"
            "Custom footer prose.\n"
        )
        rendered = sync.render_category_page(meta, "os", existing)
        self.assertIn("Custom introductory prose.", rendered)
        self.assertIn("-------\nModules\n-------", rendered)
        # Generated toctree omits `self` when a hand-written toctree precedes.
        self.assertNotIn("   self\n   Beta <../pw_beta/docs>", rendered)
        self.assertIn("\n   Beta <../pw_beta/docs>\n", rendered)
        self.assertIn("Custom footer prose.", rendered)

    def test_render_modules_rst(self) -> None:
        meta = _make_metadata()
        existing = (
            ".. _docs-modules:\n\n"
            "============\n"
            "Module index\n"
            "============\n"
            ".. categories-start\n"
            ".. categories-end\n"
        )
        rendered = sync.render_modules_rst(meta, existing)
        self.assertIn('":ref:`module-pw_alpha`"', rendered)
        self.assertIn('":ref:`module-pw_beta`"', rendered)
        self.assertLess(
            rendered.index("module-pw_alpha"),
            rendered.index("module-pw_beta"),
        )
        self.assertNotIn(".. toctree::", rendered)

    def test_render_platform_rst(self) -> None:
        """Tests rendering of //docs/sphinx/platform.rst."""
        data = copy.deepcopy(_SAMPLE_DATA)
        data["categories"]["zephyr"] = {
            "parent": "os",
            "title": "Zephyr",
            "description": "Zephyr RTOS support",
            "homepage": "//docs/sphinx/os/zephyr/index.rst",
        }
        data["modules"]["pw_zephyr"] = {
            "status": "stable",
            "category": {"id": "zephyr", "toctree": "Zephyr"},
        }
        meta = _make_metadata(data)
        existing = (
            ".. _docs-module-guides:\n\n"
            "========\n"
            "Platform\n"
            "========\n"
            ".. categories-start\n"
            ".. categories-end\n"
        )
        rendered = sync.render_platform_rst(
            meta, existing, external_toctree_docnames={"os/index"}
        )
        self.assertIn("--------\nOverview\n--------\n.. grid:: 3", rendered)
        self.assertIn(":octicon:`gear` Core utilities", rendered)
        self.assertIn(":octicon:`cpu` OS abstraction", rendered)
        # Subcategories are excluded from the Overview card grid.
        self.assertNotIn("grid-item-card:: :octicon:`None` Zephyr", rendered)
        self.assertIn("Learn more: :doc:`core/index`", rendered)
        # Subcategory renders as an H3 section under its parent category.
        self.assertIn(
            "Zephyr\n======\nZephyr RTOS support\n\n"
            "Learn more: :doc:`os/zephyr/index`",
            rendered,
        )
        self.assertIn("   core/index\n", rendered)
        self.assertIn("   OS abstraction <pw://os/index.html>\n", rendered)

    def test_render_parent_category_page_without_direct_modules(self) -> None:
        """Tests rendering a parent category landing page with subcategories."""
        data = copy.deepcopy(_SAMPLE_DATA)
        data["categories"]["hw"] = {
            "title": "Hardware",
            "icon": "package-dependencies",
            "description": "Hardware support",
            "homepage": "//docs/sphinx/hw/index.rst",
        }
        data["categories"]["rpi"] = {
            "parent": "hw",
            "title": "Raspberry Pi",
            "description": "RP2040 support",
            "homepage": "//targets/rp2040/target_docs.rst",
        }
        data["modules"]["pw_rpi"] = {
            "status": "stable",
            "category": {"id": "rpi", "toctree": "RPI"},
        }
        meta = _make_metadata(data)
        rendered = sync.render_category_page(
            meta,
            "hw",
            None,
            external_toctree_docnames={"targets/*/target_docs"},
        )
        self.assertIn(
            "------------\nRaspberry Pi\n------------\nRP2040 support\n\n"
            "Learn more: :doc:`/targets/rp2040/target_docs`",
            rendered,
        )
        self.assertNotIn("-------\nModules\n-------", rendered)
        self.assertIn(
            "   self\n"
            "   Raspberry Pi <pw://targets/rp2040/target_docs.html>",
            rendered,
        )
        self.assertNotIn("Modules <modules>", rendered)

        rendered_local = sync.render_category_page(
            meta,
            "hw",
            None,
            external_toctree_docnames=set(),
        )
        self.assertIn(
            "   self\n" "   Raspberry Pi <../targets/rp2040/target_docs>",
            rendered_local,
        )

    def test_render_index_rst(self) -> None:
        meta = _make_metadata()
        existing = (
            "Header\n\n.. categories-start\n.. categories-end\n\nFooter\n"
        )
        rendered = sync.render_index_rst(meta, existing)
        self.assertIn(":octicon:`gear` Core utilities", rendered)
        self.assertIn(":link: core/index", rendered)
        self.assertNotIn(".. toctree::", rendered)
        self.assertIn("Header", rendered)
        self.assertIn("Footer", rendered)

    def test_render_sphinx_build_bazel(self) -> None:
        meta = _make_metadata()
        existing = (
            "srcs = [\n"
            "    # categories-start\n"
            "    # categories-end\n"
            "]\n"
        )
        rendered = sync.render_sphinx_build_bazel(meta, existing)
        self.assertIn('    "core/index.rst",\n', rendered)
        self.assertIn('    "os/index.rst",\n', rendered)
        self.assertIn(
            "    # | END OF AUTO-GENERATED SECTION",
            rendered,
        )


class DoxygenAndRustdocRenderTest(unittest.TestCase):
    """Tests for Doxygen and rustdoc rendering functions."""

    def test_render_doxygen_modules_h_with_subgroups_and_cycle(self) -> None:
        """Tests Doxygen subgroup resolution and cyclic @ingroup handling."""
        meta = _make_metadata()
        with tempfile.TemporaryDirectory() as tmp:
            tagfile = Path(tmp) / "index.tag"
            tagfile.write_text(
                '<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>\n'
                "<tagfile>\n"
                '  <compound kind="group">\n'
                "    <name>pw_alpha_sub</name>\n"
                "    <title>Alpha subgroup</title>\n"
                "    <filename>group__pw__alpha__sub.html</filename>\n"
                '    <class kind="class">pw::Alpha</class>\n'
                "  </compound>\n"
                '  <compound kind="group">\n'
                "    <name>cycle_a</name>\n"
                '    <class kind="class">pw::Cycle</class>\n'
                "  </compound>\n"
                "</tagfile>\n",
                encoding="utf-8",
            )
            existing = (
                "// Header\n\n"
                "// categories-start\n"
                "// categories-end\n\n"
                "/// @defgroup pw_alpha_sub Alpha subgroup\n"
                "/// @ingroup pw_alpha\n"
                "/// @defgroup cycle_a Cycle A\n"
                "/// @ingroup cycle_b\n"
                "/// @defgroup cycle_b Cycle B\n"
                "/// @ingroup cycle_a\n"
            )
            rendered = sync.render_doxygen_modules_h(meta, tagfile, existing)
            self.assertIn(
                "/// @defgroup category_core Core utilities", rendered
            )
            self.assertNotIn(
                "/// @defgroup category_os OS abstraction", rendered
            )
            self.assertIn("/// @defgroup pw_alpha pw_alpha", rendered)
            self.assertIn("/// @ingroup category_core", rendered)
            self.assertNotIn("/// @defgroup pw_beta", rendered)
            self.assertIn(
                "// | END OF AUTO-GENERATED SECTION",
                rendered,
            )

    def test_render_rustdoc_lib_rs(self) -> None:
        meta = _make_metadata()
        with tempfile.TemporaryDirectory() as tmp:
            rustdoc_dir = Path(tmp)
            (rustdoc_dir / "pw_alpha").mkdir()
            (rustdoc_dir / "pw_alpha" / "index.html").write_text(
                "<html></html>", encoding="utf-8"
            )
            existing = (
                "//! Top\n"
                "//! <!-- categories-start -->\n"
                "//! <!-- categories-end -->\n"
            )
            rendered = sync.render_rustdoc_lib_rs(meta, rustdoc_dir, existing)
            self.assertIn("//! ### Core utilities", rendered)
            self.assertIn(
                "//! - [`pw_alpha`](../pw_alpha/index.html) - Alpha utilities.",
                rendered,
            )
            self.assertNotIn("pw_beta", rendered)
            self.assertIn(
                "//! <!-- | END OF AUTO-GENERATED SECTION",
                rendered,
            )


class SyncAndCheckIntegrationTest(unittest.TestCase):
    """End-to-end tests for check() and sync()."""

    def test_sync_and_check_roundtrip(self) -> None:
        """Tests that sync() updates all managed files and check() passes."""
        meta = _make_metadata()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sphinx_dir = root / "docs" / "sphinx"
            sphinx_dir.mkdir(parents=True)

            (sphinx_dir / "index.rst").write_text(
                "Home\n====\n\n.. categories-start\n.. categories-end\n",
                encoding="utf-8",
            )
            (sphinx_dir / "platform.rst").write_text(
                "Platform\n========\n.. categories-start\n.. categories-end\n",
                encoding="utf-8",
            )
            (sphinx_dir / "modules.rst").write_text(
                "Module index\n============\n"
                ".. categories-start\n.. categories-end\n",
                encoding="utf-8",
            )
            build_bazel = sphinx_dir / "BUILD.bazel"
            build_bazel.write_text(
                "srcs = [\n"
                "        # categories-start\n"
                "        # categories-end\n"
                "]\n",
                encoding="utf-8",
            )

            # Create a stale category page.
            stale = sphinx_dir / "old_category" / "index.rst"
            stale.parent.mkdir(parents=True)
            stale.write_text(
                ".. _docs-category-old:\n\n"
                "============\nOld category\n============\n"
                ".. categories-start\n.. categories-end\n",
                encoding="utf-8",
            )

            doxygen_h = root / "docs" / "doxygen" / "modules.h"
            doxygen_h.parent.mkdir(parents=True)
            doxygen_h.write_text(
                "// Header\n\n// categories-start\n// categories-end\n",
                encoding="utf-8",
            )
            tagfile = root / "index.tag"
            tagfile.write_text("<tagfile></tagfile>\n", encoding="utf-8")

            rust_lib = root / "pw_rust" / "lib.rs"
            rust_lib.parent.mkdir(parents=True)
            rust_lib.write_text(
                "//! Crate\n"
                "//! <!-- categories-start -->\n"
                "//! <!-- categories-end -->\n",
                encoding="utf-8",
            )
            rustdoc_dir = root / "rustdoc"
            rustdoc_dir.mkdir()

            tree = sync.DocsTree(
                sphinx_dir=sphinx_dir,
                doxygen_modules_h=doxygen_h,
                doxygen_tagfile=tagfile,
                rust_lib_rs=rust_lib,
                rustdoc_dir=rustdoc_dir,
                sphinx_build_bazel=build_bazel,
            )

            # Before sync, check() should report missing and stale files, and
            # sync() should raise SyncError on the stale category page.
            initial_issues = sync.check(tree, meta)
            self.assertTrue(
                any(
                    "old_category/index.rst" in issue
                    for issue in initial_issues
                )
            )
            with self.assertRaisesRegex(
                sync.SyncError, "old_category/index.rst"
            ):
                sync.sync(tree, meta)

            # Once the stale page is removed, sync() brings everything up to
            # date.
            stale.unlink()
            changed = sync.sync(tree, meta)
            self.assertTrue(changed)

            # After sync, check() and a second sync() should both be clean.
            self.assertEqual(sync.check(tree, meta), [])
            self.assertEqual(sync.sync(tree, meta), [])


if __name__ == "__main__":
    unittest.main()
