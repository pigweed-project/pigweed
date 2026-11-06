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
"""Head tag tests for pigweed.dev."""

from html.parser import HTMLParser
import unittest

from helpers import (
    get_docs_dir,
    get_html_files,
    is_redirect_page,
)

# Duplicated from //docs/common/head.py because //docs/tests is a standalone
# Bazel module that runs against pre-built HTML outputs without depending on
# //docs/common's Sphinx/Pagefind packages.
# LINT.IfChange(google_analytics_id)
_GOOGLE_ANALYTICS_ID = "G-JQJJWFZWFB"
# LINT.ThenChange(//docs/common/head.py:google_analytics_id)

_EXPECTED_GTAG_SRC = (
    f"https://www.googletagmanager.com/gtag/js?id={_GOOGLE_ANALYTICS_ID}"
)
_EXPECTED_GTAG_CONFIG = f"gtag('config', '{_GOOGLE_ANALYTICS_ID}');"


def _is_charset_meta(tag: str, attrs: dict[str, str | None]) -> bool:
    if tag != "meta":
        return False
    if "charset" in attrs:
        return True
    return (
        attrs.get("http-equiv") or ""
    ).lower() == "content-type" and "charset=" in (
        attrs.get("content") or ""
    ).lower()


class _HeadAnalyticsVerifier(HTMLParser):
    """Captures the leading elements inside <head> to verify gtag placement."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=False)
        self.in_head = False
        self.in_script = False
        self.head_elements: list[tuple[str, dict[str, str | None], str]] = []
        self._current_attrs: dict[str, str | None] = {}
        self._current_data: list[str] = []
        self.done = False

    def _target_count(self) -> int:
        if self.head_elements and _is_charset_meta(
            self.head_elements[0][0], self.head_elements[0][1]
        ):
            return 3
        return 2

    def handle_starttag(
        self, tag: str, attrs: list[tuple[str, str | None]]
    ) -> None:
        if self.done:
            return
        if tag == "head":
            self.in_head = True
            return
        if not self.in_head:
            return
        attr_map = dict(attrs)
        if tag == "script":
            self.in_script = True
            self._current_attrs = attr_map
            self._current_data = []
        else:
            self.head_elements.append((tag, attr_map, ""))
            if len(self.head_elements) >= self._target_count():
                self.done = True

    def handle_data(self, data: str) -> None:
        if self.in_script:
            self._current_data.append(data)

    def handle_endtag(self, tag: str) -> None:
        if self.done:
            return
        if tag == "script" and self.in_script:
            self.in_script = False
            self.head_elements.append(
                ("script", self._current_attrs, "".join(self._current_data))
            )
            if len(self.head_elements) >= self._target_count():
                self.done = True
        elif tag == "head":
            self.in_head = False
            self.done = True


def _has_analytics_first_in_head(content: str) -> bool:
    verifier = _HeadAnalyticsVerifier()
    try:
        pos = 0
        while pos < len(content) and not verifier.done:
            verifier.feed(content[pos : pos + 2048])
            pos += 2048
    finally:
        verifier.close()

    offset = 0
    if verifier.head_elements and _is_charset_meta(
        verifier.head_elements[0][0], verifier.head_elements[0][1]
    ):
        offset = 1
    if len(verifier.head_elements) < offset + 2:
        return False
    tag1, attrs1, _ = verifier.head_elements[offset]
    tag2, _, body2 = verifier.head_elements[offset + 1]
    return (
        tag1 == "script"
        and "async" in attrs1
        and attrs1.get("src") == _EXPECTED_GTAG_SRC
        and tag2 == "script"
        and _EXPECTED_GTAG_CONFIG in body2
    )


class HeadTest(unittest.TestCase):
    """Tests for universal <head> postprocessing."""

    @classmethod
    def setUpClass(cls):
        cls.docs_dir = get_docs_dir()
        # `include_redirects=False` filters out HTTP-equiv refresh stubs (such
        # as `rustdoc/index.html`), matching `inject_head()` in
        # `docs/common/head.py`.
        cls.html_files = get_html_files(cls.docs_dir, include_redirects=False)

    def test_google_analytics_in_head(self):
        """Verifies that the Google Analytics tag is placed at the start of
        <head> (immediately after any leading charset <meta> tag) on every
        non-redirect page across Sphinx, Doxygen, and Rustdoc.
        """
        self.assertGreater(
            len(self.html_files),
            0,
            f"No HTML files found under {self.docs_dir}",
        )

        missing_or_misplaced: list[str] = []

        for html_file in self.html_files:
            if is_redirect_page(html_file):
                continue
            rel_path = html_file.relative_to(self.docs_dir)
            content = html_file.read_text(encoding="utf-8", errors="ignore")
            if not _has_analytics_first_in_head(content):
                missing_or_misplaced.append(str(rel_path))

        self.assertEqual(
            len(missing_or_misplaced),
            0,
            f"Found {len(missing_or_misplaced)} pages missing Google Analytics "
            f"at the start of <head>:\n" + "\n".join(missing_or_misplaced[:20]),
        )

    def test_redirect_pages_omit_google_analytics(self):
        """Verifies that HTTP-equiv refresh redirect stubs do not have Google
        Analytics injected.
        """
        all_files = get_html_files(self.docs_dir, include_redirects=True)
        redirect_files = [p for p in all_files if is_redirect_page(p)]
        for redirect_file in redirect_files:
            content = redirect_file.read_text(encoding="utf-8", errors="ignore")
            self.assertNotIn(
                _GOOGLE_ANALYTICS_ID,
                content,
                f"Redirect stub unexpectedly contains Google Analytics: "
                f"{redirect_file.relative_to(self.docs_dir)}",
            )


if __name__ == "__main__":
    unittest.main()
