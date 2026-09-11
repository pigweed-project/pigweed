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
"""Breadcrumbs tests for pigweed.dev."""

import html
from pathlib import Path
import re
import unittest

from helpers import (
    get_docs_dir,
    get_html_files,
)


class BreadcrumbsTest(unittest.TestCase):
    """Breadcrumbs tests."""

    @classmethod
    def setUpClass(cls):
        cls.docs_dir = get_docs_dir()
        cls.html_files = get_html_files(cls.docs_dir)

    def test_no_pydata_breadcrumbs(self):
        """Verifies that PyData's .bd-breadcrumbs node is not found in Sphinx HTML files."""
        self.assertGreater(
            len(self.html_files),
            0,
            f"No HTML files found under {self.docs_dir}",
        )
        pages_checked = 0
        files_with_bd_breadcrumbs: list[str] = []
        for html_file in self.html_files:
            rel_path = html_file.relative_to(self.docs_dir).as_posix()
            # Skip Doxygen (api/) and Rustdoc (rustdoc/) pages
            if rel_path.startswith(("api/", "rustdoc/")):
                continue
            pages_checked += 1
            content = html_file.read_text(encoding="utf-8", errors="ignore")
            if "bd-breadcrumbs" in content:
                files_with_bd_breadcrumbs.append(rel_path)

        self.assertGreater(
            pages_checked,
            0,
            "Expected at least one Sphinx HTML page to check",
        )
        self.assertEqual(
            len(files_with_bd_breadcrumbs),
            0,
            f"Found {len(files_with_bd_breadcrumbs)} Sphinx pages containing 'bd-breadcrumbs':\n"
            + "\n".join(files_with_bd_breadcrumbs[:20]),
        )

    def test_homepage_omits_breadcrumbs(self):
        """Verifies that breadcrumbs are omitted on the homepage."""
        index_file = self.docs_dir / "index.html"
        self.assertTrue(index_file.exists(), f"{index_file} does not exist")
        content = index_file.read_text(encoding="utf-8")
        self.assertNotIn("<pw-breadcrumbs", content)

    def test_sphinx_breadcrumbs(self):
        """Verifies breadcrumb trail for a Sphinx documentation page."""
        html_file = self.docs_dir / "pw_tokenizer" / "token_databases.html"
        self.assertTrue(html_file.exists(), f"{html_file} does not exist")
        content = html_file.read_text(encoding="utf-8")

        m_crumbs = re.search(
            r'<pw-breadcrumbs[^>]*>([\s\S]*?)</pw-breadcrumbs>',
            content,
        )
        self.assertIsNotNone(m_crumbs, "Missing <pw-breadcrumbs> in HTML")
        crumbs_html = m_crumbs.group(1)

        items = re.findall(
            r'<li class="pw-breadcrumbs-item">([\s\S]*?)</li>',
            crumbs_html,
        )
        self.assertEqual(
            len(items), 4, f"Expected 4 breadcrumb items, got {len(items)}"
        )

        # 1. First breadcrumb: text is Home, href is https://pigweed.dev/index.html
        m1 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[0])
        self.assertIsNotNone(m1, f"Expected link in item 0: {items[0]}")
        self.assertEqual(m1.group(2).strip(), "Home")
        self.assertEqual(m1.group(1), "https://pigweed.dev/index.html")

        # 2. Second breadcrumb: text is Modules, href is https://pigweed.dev/modules.html
        m2 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[1])
        self.assertIsNotNone(m2, f"Expected link in item 1: {items[1]}")
        self.assertEqual(m2.group(2).strip(), "Modules")
        self.assertEqual(m2.group(1), "https://pigweed.dev/modules.html")

        # 3. Third breadcrumb: text is pw_tokenizer, href is https://pigweed.dev/pw_tokenizer/docs.html
        m3 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[2])
        self.assertIsNotNone(m3, f"Expected link in item 2: {items[2]}")
        self.assertEqual(m3.group(2).strip(), "pw_tokenizer")
        self.assertEqual(
            m3.group(1), "https://pigweed.dev/pw_tokenizer/docs.html"
        )

        # 4. Final breadcrumb: nonclickable text Token databases
        self.assertNotIn(
            "<a ",
            items[3],
            f"Final breadcrumb should not be a link: {items[3]}",
        )
        m4 = re.search(r'<span aria-current="page">([^<]+)</span>', items[3])
        self.assertIsNotNone(
            m4, f"Expected span aria-current='page' in item 3: {items[3]}"
        )
        leaf_text = html.unescape(m4.group(1).strip())
        self.assertEqual(leaf_text, "Token databases")

    def test_doxygen_breadcrumbs(self):
        """Verifies breadcrumb trail for a Doxygen C/C++ API page."""
        html_file = (
            self.docs_dir
            / "api"
            / "cc"
            / "classpw_1_1async2_1_1_value_future_3_01void_01_4.html"
        )
        self.assertTrue(html_file.exists(), f"{html_file} does not exist")
        content = html_file.read_text(encoding="utf-8")

        m_crumbs = re.search(
            r'<pw-breadcrumbs[^>]*>([\s\S]*?)</pw-breadcrumbs>',
            content,
        )
        self.assertIsNotNone(m_crumbs, "Missing <pw-breadcrumbs> in HTML")
        crumbs_html = m_crumbs.group(1)

        items = re.findall(
            r'<li class="pw-breadcrumbs-item">([\s\S]*?)</li>',
            crumbs_html,
        )
        self.assertEqual(
            len(items), 6, f"Expected 6 breadcrumb items, got {len(items)}"
        )

        # 1. First breadcrumb: text is Home, href is https://pigweed.dev/index.html
        m1 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[0])
        self.assertIsNotNone(m1, f"Expected link in item 0: {items[0]}")
        self.assertEqual(m1.group(2).strip(), "Home")
        self.assertEqual(m1.group(1), "https://pigweed.dev/index.html")

        # 2. Second breadcrumb: text is Reference, href is https://pigweed.dev/api/index.html
        m2 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[1])
        self.assertIsNotNone(m2, f"Expected link in item 1: {items[1]}")
        self.assertEqual(m2.group(2).strip(), "Reference")
        self.assertEqual(m2.group(1), "https://pigweed.dev/api/index.html")

        # 3. Third breadcrumb: text is C/C++, href is https://pigweed.dev/api/cc/modules.html
        m3 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[2])
        self.assertIsNotNone(m3, f"Expected link in item 2: {items[2]}")
        self.assertEqual(m3.group(2).strip(), "C/C++")
        self.assertEqual(m3.group(1), "https://pigweed.dev/api/cc/modules.html")

        # 4. Fourth breadcrumb: text is pw_async2, href matches https://pigweed.dev/api/cc/group__pw__async2.html
        m4 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[3])
        self.assertIsNotNone(m4, f"Expected link in item 3: {items[3]}")
        self.assertEqual(m4.group(2).strip(), "pw_async2")
        self.assertEqual(
            m4.group(1), "https://pigweed.dev/api/cc/group__pw__async2.html"
        )

        # 5. Fifth breadcrumb: text is Futures, href matches https://pigweed.dev/api/cc/group__pw__async2__futures.html
        m5 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[4])
        self.assertIsNotNone(m5, f"Expected link in item 4: {items[4]}")
        self.assertEqual(m5.group(2).strip(), "Futures")
        self.assertEqual(
            m5.group(1),
            "https://pigweed.dev/api/cc/group__pw__async2__futures.html",
        )

        # 6. Final breadcrumb: nonclickable text pw::async2::ValueFuture< void > Class Reference
        self.assertNotIn(
            "<a ",
            items[5],
            f"Final breadcrumb should not be a link: {items[5]}",
        )
        m6 = re.search(r'<span aria-current="page">([^<]+)</span>', items[5])
        self.assertIsNotNone(
            m6, f"Expected span aria-current='page' in item 5: {items[5]}"
        )
        leaf_text = html.unescape(m6.group(1).strip())
        self.assertEqual(
            leaf_text, "pw::async2::ValueFuture< void > Class Reference"
        )

    def test_rustdoc_breadcrumbs(self):
        """Verifies breadcrumb trail for a Rustdoc API page."""
        html_file = (
            self.docs_dir / "rustdoc" / "pw_assert" / "macro.assert.html"
        )
        self.assertTrue(html_file.exists(), f"{html_file} does not exist")
        content = html_file.read_text(encoding="utf-8")

        m_crumbs = re.search(
            r'<pw-breadcrumbs[^>]*>([\s\S]*?)</pw-breadcrumbs>',
            content,
        )
        self.assertIsNotNone(m_crumbs, "Missing <pw-breadcrumbs> in HTML")
        crumbs_html = m_crumbs.group(1)

        items = re.findall(
            r'<li class="pw-breadcrumbs-item">([\s\S]*?)</li>',
            crumbs_html,
        )
        self.assertEqual(
            len(items), 5, f"Expected 5 breadcrumb items, got {len(items)}"
        )

        # 1. First breadcrumb: text is Home, href is https://pigweed.dev/index.html
        m1 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[0])
        self.assertIsNotNone(m1, f"Expected link in item 0: {items[0]}")
        self.assertEqual(m1.group(2).strip(), "Home")
        self.assertEqual(m1.group(1), "https://pigweed.dev/index.html")

        # 2. Second breadcrumb: text is Reference, href is https://pigweed.dev/api/index.html
        m2 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[1])
        self.assertIsNotNone(m2, f"Expected link in item 1: {items[1]}")
        self.assertEqual(m2.group(2).strip(), "Reference")
        self.assertEqual(m2.group(1), "https://pigweed.dev/api/index.html")

        # 3. Third breadcrumb: text is Rust, href is https://pigweed.dev/rustdoc/index.html
        m3 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[2])
        self.assertIsNotNone(m3, f"Expected link in item 2: {items[2]}")
        self.assertEqual(m3.group(2).strip(), "Rust")
        self.assertEqual(m3.group(1), "https://pigweed.dev/rustdoc/index.html")

        # 4. Fourth breadcrumb: text is pw_assert, href is https://pigweed.dev/rustdoc/pw_assert/index.html
        m4 = re.search(r'<a\s+href="([^"]+)">([^<]+)</a>', items[3])
        self.assertIsNotNone(m4, f"Expected link in item 3: {items[3]}")
        self.assertEqual(m4.group(2).strip(), "pw_assert")
        self.assertEqual(
            m4.group(1), "https://pigweed.dev/rustdoc/pw_assert/index.html"
        )

        # 5. Final breadcrumb: nonclickable text assert
        self.assertNotIn(
            "<a ",
            items[4],
            f"Final breadcrumb should not be a link: {items[4]}",
        )
        m5 = re.search(r'<span aria-current="page">([^<]+)</span>', items[4])
        self.assertIsNotNone(
            m5, f"Expected span aria-current='page' in item 4: {items[4]}"
        )
        leaf_text = html.unescape(m5.group(1).strip())
        self.assertEqual(leaf_text, "assert")


if __name__ == "__main__":
    unittest.main()
