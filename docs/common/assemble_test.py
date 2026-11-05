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
"""Unit tests for top-level site assembly."""

from pathlib import Path
import tempfile
import unittest
from unittest import mock

from docs.common.header import (
    HEADER_NAV_FILENAME,
    HeaderLink,
    save_header_nav,
)
from docs.common.metadata.sync import SyncError
from docs.common import assemble


class AssembleSiteTest(unittest.TestCase):
    """Tests merging and postprocessing Sphinx, Doxygen, and Rustdoc outputs."""

    def test_assemble_injects_headers_merges_and_finishes_site(self) -> None:
        """Injects subsite headers, drops nav JSON, and finishes site."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sphinx_html, doxygen_html = root / "sphinx", root / "doxygen"
            rustdoc_html, sphinx_srcdir = root / "rustdoc", root / "srcdir"
            out_dir = root / "out"
            for d in (sphinx_html, doxygen_html, sphinx_srcdir):
                d.mkdir()
            (rustdoc_html / "pw_status").mkdir(parents=True)

            (sphinx_html / "index.html").write_text(
                "<html><body><h1>Pigweed Home</h1></body></html>",
                encoding="utf-8",
            )
            save_header_nav(
                sphinx_html / HEADER_NAV_FILENAME,
                [HeaderLink(title="Docs", href="https://pigweed.dev/")],
            )
            (doxygen_html / "group__pw__string.html").write_text(
                "<html><body><!-- pw-sentinel -->"
                "<h1>pw_string</h1></body></html>",
                encoding="utf-8",
            )
            (rustdoc_html / "index.html").write_text(
                '<meta http-equiv="refresh" content="0; url=pw_status/" />',
                encoding="utf-8",
            )
            (rustdoc_html / "pw_status" / "index.html").write_text(
                "<html><head><title>pw_status - Rust</title></head>"
                "<body><!-- pw-sentinel --><h1>pw_status</h1></body></html>",
                encoding="utf-8",
            )

            with mock.patch.object(assemble, "check_categories") as mock_check:
                rc = assemble.main(
                    [
                        f"--sphinx-html={sphinx_html}",
                        f"--doxygen-html={doxygen_html}",
                        f"--rustdoc-html={rustdoc_html}",
                        f"--sphinx-srcdir={sphinx_srcdir}",
                        f"--out-dir={out_dir}",
                    ]
                )

            self.assertEqual(rc, 0)
            mock_check.assert_called_once_with(
                sphinx_srcdir, rustdoc_dir=rustdoc_html
            )
            self.assertFalse((out_dir / HEADER_NAV_FILENAME).exists())
            self.assertTrue((out_dir / "index.html").is_file())
            self.assertIn(
                'href="#doc-content"',
                (out_dir / "api/cc/group__pw__string.html").read_text("utf-8"),
            )
            self.assertIn(
                'href="#main-content"',
                (out_dir / "rustdoc/pw_status/index.html").read_text("utf-8"),
            )
            self.assertTrue((out_dir / "search" / "pagefind.js").is_file())

            sitemap = (out_dir / "sitemap.xml").read_text(encoding="utf-8")
            self.assertIn("<loc>https://pigweed.dev/</loc>", sitemap)
            self.assertIn(
                "<loc>https://pigweed.dev/api/cc/group__pw__string.html</loc>",
                sitemap,
            )
            self.assertIn(
                "<loc>https://pigweed.dev/rustdoc/pw_status/</loc>", sitemap
            )
            self.assertNotIn("<loc>https://pigweed.dev/rustdoc/</loc>", sitemap)

    def test_assemble_returns_nonzero_on_category_sync_error(self) -> None:
        """Exits with status 1 when category validation fails."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with mock.patch.object(
                assemble, "check_categories", side_effect=SyncError("failed")
            ):
                rc = assemble.main(
                    [
                        f"--sphinx-html={root}",
                        f"--doxygen-html={root}",
                        f"--rustdoc-html={root}",
                        f"--sphinx-srcdir={root}",
                        f"--out-dir={root / 'out'}",
                    ]
                )
            self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
