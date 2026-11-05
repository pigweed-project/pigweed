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
"""Unit tests for standalone subsite header and breadcrumb injection."""

from pathlib import Path
import tempfile
import unittest

from docs.common.header import (
    HeaderLink,
    inject_subsite_headers,
    load_header_nav,
    save_header_nav,
)


class HeaderSubsiteInjectionTest(unittest.TestCase):
    """Tests HeaderLink serialization and subsite HTML postprocessing."""

    def test_header_nav_json_roundtrip(self) -> None:
        """Serializes and deserializes nested HeaderLink trees."""
        links = [
            HeaderLink(
                title="Docs",
                href="https://pigweed.dev/overview.html",
                children=[
                    HeaderLink(
                        title="Build",
                        href="https://pigweed.dev/build/overview.html",
                    )
                ],
            ),
            HeaderLink(title="Reference", href="https://pigweed.dev/api/"),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            nav_path = Path(tmp) / "_pw_header_nav.json"
            save_header_nav(nav_path, links)
            self.assertEqual(load_header_nav(nav_path), links)

    def test_inject_doxygen_subsite_headers(self) -> None:
        """Injects #doc-content skip link and C/C++ group breadcrumbs."""
        links = [HeaderLink(title="Docs", href="https://pigweed.dev/")]
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "src"
            dst = Path(tmp) / "dst"
            src.mkdir()
            (src / "group__pw__string.html").write_text(
                "<html><body><!-- pw-sentinel -->"
                '<div class="ingroups">'
                '<a class="el" href="group__category__core.html">Core</a>'
                "</div>"
                '<div class="title">pw_string</div>'
                "</body></html>",
                encoding="utf-8",
            )
            (src / "doxygen.css").write_text("body {}", encoding="utf-8")

            inject_subsite_headers(src, dst, "api/cc", links)

            rendered = (dst / "group__pw__string.html").read_text(
                encoding="utf-8"
            )
            self.assertNotIn("<!-- pw-sentinel -->", rendered)
            self.assertIn('<pw-header id="pw-header"', rendered)
            self.assertIn('href="#doc-content"', rendered)
            self.assertIn("https://pigweed.dev/api/cc/modules.html", rendered)
            self.assertIn(
                "https://pigweed.dev/api/cc/group__category__core.html",
                rendered,
            )
            self.assertEqual(
                (dst / "doxygen.css").read_text(encoding="utf-8"), "body {}"
            )

    def test_inject_rustdoc_subsite_preserves_redirect_stubs(self) -> None:
        """Injects #main-content skip link and copies redirect stubs."""
        links = [HeaderLink(title="Docs", href="https://pigweed.dev/")]
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "src"
            dst = Path(tmp) / "dst"
            (src / "pw_status").mkdir(parents=True)
            redirect = (
                '<meta http-equiv="refresh" content="0; url=pigweed/index.html"'
                " />"
            )
            (src / "index.html").write_text(redirect, encoding="utf-8")
            (src / "pw_status" / "enum.Error.html").write_text(
                "<html><head><title>Error in pw_status - Rust</title></head>"
                "<body><!-- pw-sentinel --></body></html>",
                encoding="utf-8",
            )

            inject_subsite_headers(src, dst, "rustdoc", links)

            self.assertEqual(
                (dst / "index.html").read_text(encoding="utf-8"), redirect
            )
            rendered = (dst / "pw_status" / "enum.Error.html").read_text(
                encoding="utf-8"
            )
            self.assertIn('<pw-header id="pw-header"', rendered)
            self.assertIn('href="#main-content"', rendered)
            self.assertIn(
                "https://pigweed.dev/rustdoc/pw_status/index.html", rendered
            )


if __name__ == "__main__":
    unittest.main()
