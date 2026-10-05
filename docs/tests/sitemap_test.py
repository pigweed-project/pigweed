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
"""Sitemap tests for pigweed.dev."""

from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

from helpers import (
    get_docs_dir,
    get_html_files,
    is_redirect_page,
)

_SITEMAP_NS = "http://www.sitemaps.org/schemas/sitemap/0.9"
_BASE_URL = "https://pigweed.dev/"

# Utility and source files that exist in the HTML output directory but are
# intentionally excluded from sitemap.xml.
_EXCLUDED_REL_FILES = {
    "genindex.html",
    "py-modindex.html",
    "rustdoc/help.html",
    "rustdoc/settings.html",
    "search.html",
}


class SitemapTest(unittest.TestCase):
    """Sitemap tests."""

    @classmethod
    def setUpClass(cls):
        cls.docs_dir = get_docs_dir()
        cls.sitemap_path = cls.docs_dir / "sitemap.xml"
        cls.tree = ET.parse(cls.sitemap_path)
        cls.root = cls.tree.getroot()
        cls.urls = [
            loc.text
            for loc in cls.root.findall(
                f"{{{_SITEMAP_NS}}}url/{{{_SITEMAP_NS}}}loc"
            )
            if loc.text
        ]
        cls.url_set = set(cls.urls)

    def _resolve_url_to_file(self, url: str) -> Path | None:
        """Resolves a canonical pigweed.dev URL back to its HTML file on disk."""
        if not url.startswith(_BASE_URL):
            return None
        rel = url[len(_BASE_URL) :]
        if rel == "" or rel.endswith("/"):
            for filename in ("docs.html", "index.html"):
                candidate = (
                    (self.docs_dir / rel / filename)
                    if rel
                    else (self.docs_dir / filename)
                )
                if candidate.is_file() and not is_redirect_page(candidate):
                    return candidate
            return None

        candidate = self.docs_dir / rel
        if candidate.is_file():
            return candidate
        return None

    def test_valid_xml_structure(self):
        """Verifies that sitemap.xml has valid XML syntax, Sitemap 0.9 schema
        structure, sorting, and uniqueness."""
        self.assertTrue(
            self.sitemap_path.is_file(),
            f"{self.sitemap_path} does not exist",
        )
        # ET.parse() uses Python's Expat XML parser to validate that the entire
        # file is well-formed XML 1.0 (raising ET.ParseError on any syntax,
        # encoding, unclosed tag, or entity escaping error).
        tree = ET.parse(self.sitemap_path)
        root = tree.getroot()

        # Verify Sitemap 0.9 XSD constraints
        # (https://www.sitemaps.org/schemas/sitemap/0.9/sitemap.xsd):
        # - Root <urlset> element in the Sitemap 0.9 namespace
        # - Between 1 and 50,000 <url> child elements
        # - Each <url> contains only a <loc> child with length 12..2048 chars
        self.assertEqual(root.tag, f"{{{_SITEMAP_NS}}}urlset")
        self.assertEqual(root.attrib, {})
        self.assertGreater(len(root), 0, "Expected non-empty sitemap.xml")
        self.assertLessEqual(
            len(root),
            50000,
            "Sitemap 0.9 spec allows at most 50,000 <url> elements per file",
        )

        for child in root:
            self.assertEqual(child.tag, f"{{{_SITEMAP_NS}}}url")
            self.assertEqual(child.attrib, {})
            self.assertEqual(
                len(child),
                1,
                "Expected each <url> to contain only a single <loc> child",
            )
            loc = child[0]
            self.assertEqual(loc.tag, f"{{{_SITEMAP_NS}}}loc")
            self.assertEqual(loc.attrib, {})
            self.assertIsNotNone(loc.text, "<loc> text must not be None")
            self.assertGreaterEqual(
                len(loc.text),
                12,
                f"Sitemap 0.9 <loc> must be at least 12 characters: {loc.text}",
            )
            self.assertLessEqual(
                len(loc.text),
                2048,
                f"Sitemap 0.9 <loc> must be at most 2048 characters: {loc.text}",
            )

        self.assertEqual(
            self.urls,
            sorted(self.urls),
            "Sitemap URLs must be sorted in ascending order",
        )
        self.assertEqual(
            len(self.urls),
            len(self.url_set),
            "Sitemap URLs must not contain duplicates",
        )

    def test_urls_resolve_to_valid_pages(self):
        """Maps each sitemap URL back to the filesystem and verifies it is a
        real, non-redirect HTML page."""
        unrewritten: list[str] = []
        missing_files: list[str] = []
        redirect_urls: list[str] = []

        for url in self.urls:
            self.assertTrue(
                url.startswith(_BASE_URL),
                f"Expected URL to start with {_BASE_URL}, got: {url}",
            )
            if url.endswith(("/docs.html", "/index.html")):
                unrewritten.append(url)

            resolved = self._resolve_url_to_file(url)
            if resolved is None:
                missing_files.append(url)
            elif is_redirect_page(resolved):
                redirect_urls.append(url)

        self.assertEqual(
            unrewritten,
            [],
            f"Found URLs ending in /docs.html or /index.html that should be "
            f"rewritten to '/':\n" + "\n".join(unrewritten[:20]),
        )
        self.assertEqual(
            missing_files,
            [],
            f"Found {len(missing_files)} sitemap URLs that do not resolve to a "
            f"non-redirect file on disk:\n" + "\n".join(missing_files[:20]),
        )
        self.assertEqual(
            redirect_urls,
            [],
            f"Found {len(redirect_urls)} sitemap URLs that resolve to redirect "
            f"stubs:\n" + "\n".join(redirect_urls[:20]),
        )

    def test_comprehensive_coverage_across_subsites(self):
        """Verifies that Sphinx, Doxygen, and Rustdoc pages are all indexed."""
        representative_urls = [
            # Sphinx root and general pages
            "https://pigweed.dev/",
            "https://pigweed.dev/overview.html",
            "https://pigweed.dev/os/",
            # Sphinx module homepage (/docs.html -> /)
            "https://pigweed.dev/pw_string/",
            # Sphinx page ending in docs.html (not /docs.html) must be preserved
            "https://pigweed.dev/targets/rp2040/target_docs.html",
            # Reference landing page
            "https://pigweed.dev/api/",
            # Doxygen C/C++ API reference pages
            "https://pigweed.dev/api/cc/",
            "https://pigweed.dev/api/cc/modules.html",
            "https://pigweed.dev/api/cc/group__pw__string.html",
            "https://pigweed.dev/api/cc/classpw_1_1_status.html",
            # Rustdoc API reference pages
            "https://pigweed.dev/rustdoc/pigweed/",
            "https://pigweed.dev/rustdoc/pw_status/",
            "https://pigweed.dev/rustdoc/pw_status/enum.Error.html",
        ]
        for expected_url in representative_urls:
            self.assertIn(
                expected_url,
                self.url_set,
                f"Expected representative URL {expected_url} in sitemap.xml",
            )

        # Verify that reverse-resolving all sitemap URLs back to disk covers
        # every non-excluded HTML page returned by helpers.get_html_files().
        resolved_rel_files = {
            resolved.relative_to(self.docs_dir).as_posix()
            for url in self.urls
            if (resolved := self._resolve_url_to_file(url)) is not None
        }
        expected_rel_files = {
            path.relative_to(self.docs_dir).as_posix()
            for path in get_html_files(self.docs_dir, include_redirects=False)
        } - _EXCLUDED_REL_FILES
        expected_rel_files = {
            rel for rel in expected_rel_files if "rustdoc/src/" not in rel
        }

        missing_from_sitemap = sorted(expected_rel_files - resolved_rel_files)
        unexpected_in_sitemap = sorted(resolved_rel_files - expected_rel_files)

        self.assertEqual(
            missing_from_sitemap,
            [],
            f"Found {len(missing_from_sitemap)} HTML pages missing from "
            f"sitemap.xml:\n" + "\n".join(missing_from_sitemap[:20]),
        )
        self.assertEqual(
            unexpected_in_sitemap,
            [],
            f"Found {len(unexpected_in_sitemap)} unexpected pages in "
            f"sitemap.xml:\n" + "\n".join(unexpected_in_sitemap[:20]),
        )

    def test_excludes_redirects_and_non_content_pages(self):
        """Verifies that redirects, rustdoc source views, and utility pages are
        excluded from sitemap.xml."""
        excluded_urls = [
            # Client-side redirect from redirects.json
            "https://pigweed.dev/automated_analysis.html",
            # Rustdoc root redirect (rustdoc/index.html -> pigweed/index.html)
            "https://pigweed.dev/rustdoc/",
            # Rustdoc macro redirect stub
            "https://pigweed.dev/rustdoc/pw_assert/macro.assert!.html",
            # Utility / non-content pages
            "https://pigweed.dev/genindex.html",
            "https://pigweed.dev/py-modindex.html",
            "https://pigweed.dev/search.html",
            "https://pigweed.dev/rustdoc/help.html",
            "https://pigweed.dev/rustdoc/settings.html",
            "https://pigweed.dev/_static/webpack-macros.html",
        ]
        for excluded_url in excluded_urls:
            self.assertNotIn(
                excluded_url,
                self.url_set,
                f"Excluded URL {excluded_url} should not appear in sitemap.xml",
            )

        rustdoc_src_urls = [u for u in self.urls if "/rustdoc/src/" in u]
        self.assertEqual(
            rustdoc_src_urls,
            [],
            f"Rustdoc source pages should not appear in sitemap.xml, got: "
            f"{rustdoc_src_urls[:10]}",
        )


if __name__ == "__main__":
    unittest.main()
