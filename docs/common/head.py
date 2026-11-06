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
"""Universal <head> tag postprocessing across Sphinx, Doxygen, and Rustdoc."""

from html.parser import HTMLParser
import os
from pathlib import Path

# We have a Google Analytics filter (Admin > Data collection and modification >
# Data filters) configured to only accept traffic coming from pigweed.dev, so
# it is safe to hardcode the production Google Analytics ID across all builds
# without local previews or staging sites polluting production analytics data.
# Duplicated in //docs/tests/head_test.py because //docs/tests is a standalone
# Bazel module that does not depend on //docs/common's Sphinx/Pagefind packages.
# LINT.IfChange(google_analytics_id)
_GOOGLE_ANALYTICS_ID = "G-JQJJWFZWFB"
# LINT.ThenChange(//docs/tests/head_test.py:google_analytics_id)
_GTAG_URL = (
    f"https://www.googletagmanager.com/gtag/js?id={_GOOGLE_ANALYTICS_ID}"
)

_HEAD_CONTENT = (
    f'<script async src="{_GTAG_URL}"></script>\n'
    "<script>\n"
    "  window.dataLayer = window.dataLayer || [];\n"
    "  function gtag() { dataLayer.push(arguments); }\n"
    "  gtag('js', new Date());\n"
    f"  gtag('config', '{_GOOGLE_ANALYTICS_ID}');\n"
    "</script>"
)


def _is_charset_meta(tag: str, attrs: list[tuple[str, str | None]]) -> bool:
    """Returns True if the tag is a <meta> character encoding declaration."""
    if tag != "meta":
        return False
    attr_map = {k: (v or "") for k, v in attrs}
    if "charset" in attr_map:
        return True
    return (
        attr_map.get("http-equiv", "").lower() == "content-type"
        and "charset=" in attr_map.get("content", "").lower()
    )


class _HeadFinder(HTMLParser):
    """HTML tokenizer that locates the <head> injection point.

    If the first child element inside <head> is a character-encoding <meta> tag
    (<meta charset="..."> or <meta http-equiv="Content-Type" ...>), locates the
    end of that <meta> tag so it remains the first element in <head> within the
    browser's 1024-byte prescan window; otherwise locates the end of the opening
    <head> tag.
    """

    def __init__(self) -> None:
        super().__init__(convert_charrefs=False)
        self.in_head: bool = False
        self.match: tuple[int, int, int] | None = None
        self.done: bool = False

    def handle_starttag(
        self, tag: str, attrs: list[tuple[str, str | None]]
    ) -> None:
        if self.done:
            return
        if not self.in_head:
            if tag == "head":
                lineno, col = self.getpos()
                raw = self.get_starttag_text() or "<head>"
                self.match = (lineno, col, len(raw))
                self.in_head = True
            elif tag == "body":
                self.done = True
            return

        if _is_charset_meta(tag, attrs):
            lineno, col = self.getpos()
            meta_raw = self.get_starttag_text()
            if meta_raw:
                self.match = (lineno, col, len(meta_raw))
        self.done = True

    def handle_endtag(self, tag: str) -> None:
        if self.in_head and tag == "head":
            self.done = True


def _find_head_insert_pos(content: str) -> int | None:
    """Returns the character offset for injecting common <head> elements."""
    finder = _HeadFinder()
    try:
        pos = 0
        chunk_size = 1024
        while pos < len(content):
            finder.feed(content[pos : pos + chunk_size])
            pos += chunk_size
            if finder.done:
                break
    finally:
        finder.close()
    if finder.match is None:
        return None

    lineno, col, raw_len = finder.match
    idx = 0
    for _ in range(lineno - 1):
        idx = content.index("\n", idx) + 1
    return idx + col + raw_len


def inject_head_into_html(content: str, rel_page_path: str) -> str:
    """Injects common <head> elements after <head> (or leading charset meta)."""
    insert_pos = _find_head_insert_pos(content)
    if insert_pos is None:
        raise RuntimeError(
            f"Failed to inject <head> content: '<head>' tag not found "
            f"in {rel_page_path}"
        )
    return f"{content[:insert_pos]}\n{_HEAD_CONTENT}{content[insert_pos:]}"


def _is_redirect_page(content: str) -> bool:
    """Returns True if the HTML page is an HTTP-equiv refresh redirect stub."""
    head_prefix = content[:1024].lower()
    return (
        'http-equiv="refresh"' in head_prefix
        or "http-equiv='refresh'" in head_prefix
    )


def inject_head(out_dir: Path) -> None:
    """Injects common <head> elements across all HTML pages in out_dir."""
    injected_count = 0

    for root, _, files in os.walk(out_dir):
        for file in files:
            if not file.endswith(".html"):
                continue
            path = Path(root) / file
            rel_parts = path.relative_to(out_dir).parts
            if any(
                part in ("_static", "_sources", "search") for part in rel_parts
            ):
                continue

            content = path.read_text(encoding="utf-8")
            if _is_redirect_page(content):
                continue

            rel_page_path = path.relative_to(out_dir).as_posix()
            new_content = inject_head_into_html(content, rel_page_path)
            path.write_text(new_content, encoding="utf-8")
            injected_count += 1

    if injected_count == 0:
        raise RuntimeError(
            f"Failed to inject <head> content: no HTML pages found under "
            f"{out_dir}"
        )
