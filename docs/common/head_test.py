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
"""Unit tests for docs/common/head.py."""

from pathlib import Path
import tempfile
import unittest

from docs.common.head import inject_head, inject_head_into_html


_EXPECTED_GTAG_SCRIPT = (
    '<script async src="https://www.googletagmanager.com/'
    'gtag/js?id=G-JQJJWFZWFB"></script>'
)


class HeadUnitTest(unittest.TestCase):
    """Tests for HTML <head> tag discovery and injection."""

    def test_ignores_head_inside_html_comment(self) -> None:
        html = (
            "<!-- note that `<head>` blah blah... -->\n"
            "<!DOCTYPE html>\n"
            "<html>\n"
            "<head>\n"
            "  <title>Test</title>\n"
            "</head>\n"
            "<body></body>\n"
            "</html>\n"
        )
        result = inject_head_into_html(html, "index.html")
        self.assertTrue(
            result.startswith(
                "<!-- note that `<head>` blah blah... -->\n"
                "<!DOCTYPE html>\n"
                "<html>\n"
                f"<head>\n{_EXPECTED_GTAG_SCRIPT}"
            )
        )

    def test_supports_head_with_attributes_and_multiline_and_charset_meta(
        self,
    ) -> None:
        html = (
            '<!DOCTYPE html><html data-note="<head>">\n'
            "<head\n"
            '  lang="en">\n'
            "<meta charset='utf-8'><title>Page</title>"
            "</head><body></body></html>"
        )
        result = inject_head_into_html(html, "page.html")
        self.assertIn(
            "<head\n"
            '  lang="en">\n'
            f"<meta charset='utf-8'>\n{_EXPECTED_GTAG_SCRIPT}",
            result,
        )

    def test_preserves_leading_content_type_charset_meta(self) -> None:
        html = (
            "<head>\n"
            '<meta http-equiv="Content-Type" '
            'content="text/xhtml;charset=UTF-8"/>\n'
            '<meta http-equiv="X-UA-Compatible" content="IE=11"/>\n'
            "</head><body></body>"
        )
        result = inject_head_into_html(html, "api/cc/index.html")
        self.assertTrue(
            result.startswith(
                "<head>\n"
                '<meta http-equiv="Content-Type" '
                'content="text/xhtml;charset=UTF-8"/>\n'
                f"{_EXPECTED_GTAG_SCRIPT}"
            )
        )

    def test_raises_when_head_missing_or_only_in_body(self) -> None:
        html = "<html><body><!-- <head> --><p>Hello</p></body></html>"
        with self.assertRaises(RuntimeError):
            inject_head_into_html(html, "broken.html")

    def test_inject_head_walks_directory_and_skips_assets_and_redirects(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp)
            (out_dir / "_static").mkdir()
            (out_dir / "_static" / "asset.html").write_text(
                "<div>no head</div>", encoding="utf-8"
            )
            (out_dir / "redirect.html").write_text(
                '<meta http-equiv="refresh" content="0; url=index.html">',
                encoding="utf-8",
            )
            (out_dir / "index.html").write_text(
                "<html><head><title>Home</title></head><body></body></html>",
                encoding="utf-8",
            )

            inject_head(out_dir)

            self.assertIn(
                "G-JQJJWFZWFB",
                (out_dir / "index.html").read_text(encoding="utf-8"),
            )
            self.assertNotIn(
                "G-JQJJWFZWFB",
                (out_dir / "redirect.html").read_text(encoding="utf-8"),
            )
            self.assertEqual(
                (out_dir / "_static" / "asset.html").read_text("utf-8"),
                "<div>no head</div>",
            )

    def test_inject_head_raises_when_empty(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(RuntimeError):
                inject_head(Path(tmp))


if __name__ == "__main__":
    unittest.main()
