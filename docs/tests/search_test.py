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
"""Search UI tests for pigweed.dev."""

import unittest

from helpers import (
    DocsServer,
    get_chromium_executable,
    get_docs_dir,
)
from playwright.sync_api import sync_playwright


class SearchTest(unittest.TestCase):
    """Search UI tests."""

    @classmethod
    def setUpClass(cls):
        cls.docs_dir = get_docs_dir()
        cls.server = DocsServer(cls.docs_dir)
        cls.server.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def _run_search_test(self, viewport: dict[str, int], trigger_selector: str):
        representative_pages = [
            # Sphinx overview page
            "overview.html",
            # Sphinx module subpage
            "pw_string/docs.html",
            # Doxygen group page
            "api/cc/group__pw__string.html",
            # Doxygen class page
            "api/cc/classpw_1_1_status.html",
            # Rustdoc module index
            "rustdoc/pw_status/index.html",
            # Rustdoc enum page
            "rustdoc/pw_status/enum.Error.html",
        ]

        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(viewport=viewport)
            page = context.new_page()

            for rel_path in representative_pages:
                url = self.server.url_for(rel_path)
                page.goto(url, wait_until="networkidle")

                # Open Pagefind search modal
                search_btn = page.locator(trigger_selector).first
                search_btn.wait_for(state="attached", timeout=5000)
                search_btn.click(force=True)

                # Locate search input inside the modal and submit query
                modal = page.locator("pagefind-modal")
                search_input = modal.locator("input").first
                search_input.wait_for(state="visible", timeout=10000)
                search_input.fill("pw_status")

                # Wait for search result fragment to load and link to be visible
                result_link = modal.locator(
                    "a.pf-result-link, a.pf-heading-link"
                ).first
                result_link.wait_for(state="visible", timeout=20000)

                # Get target href of the result link before clicking
                expected_target = result_link.get_attribute("href")
                self.assertIsNotNone(expected_target)

                # Click the search result and verify navigation to expected page
                result_link.click()
                page.wait_for_load_state("domcontentloaded")

                self.assertTrue(
                    page.url.endswith(expected_target.lstrip("./"))
                    or page.url == expected_target,
                    f"Searching from {rel_path} expected navigation to "
                    f"{expected_target}, got {page.url}",
                )

            browser.close()

    def test_search(self):
        """Verifies searching via Pagefind modal on desktop viewport."""
        self._run_search_test(
            viewport={"width": 1280, "height": 800},
            trigger_selector=(
                "#pw-search-desktop button, .pf-trigger-btn, #pw-search-desktop"
            ),
        )

    def test_search_mobile(self):
        """Verifies searching via Pagefind modal on mobile viewport."""
        self._run_search_test(
            viewport={"width": 375, "height": 667},
            trigger_selector="#pw-search-mobile",
        )

    def test_legacy_searchbox_hidden(self):
        """Verifies that legacy Sphinx #searchbox element is always hidden."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            for width in [375, 1024, 1440]:
                context = browser.new_context(
                    viewport={"width": width, "height": 800}
                )
                page = context.new_page()
                page.goto(
                    self.server.url_for("index.html"),
                    wait_until="domcontentloaded",
                )

                searchbox = page.locator("#searchbox")
                if searchbox.count() > 0:
                    self.assertFalse(
                        searchbox.first.is_visible(),
                        f"#searchbox should be hidden at viewport width {width}",
                    )
                context.close()

            browser.close()

    def _get_first_search_excerpt(self, query: str) -> str:
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1280, "height": 800}
            )
            page = context.new_page()
            page.goto(
                self.server.url_for("index.html"),
                wait_until="networkidle",
            )

            # Open Pagefind search modal
            search_btn = page.locator(
                "#pw-search-desktop button, .pf-trigger-btn, #pw-search-desktop"
            ).first
            search_btn.wait_for(state="attached", timeout=5000)
            search_btn.click(force=True)

            # Locate search input inside the modal and submit query
            modal = page.locator("pagefind-modal")
            search_input = modal.locator("input").first
            search_input.wait_for(state="visible", timeout=10000)
            search_input.fill(query)

            # Wait for search result fragment to load
            result_link = modal.locator(
                "a.pf-result-link, a.pf-heading-link"
            ).first
            result_link.wait_for(state="visible", timeout=20000)

            # Retrieve the first result excerpt
            excerpt = modal.locator(".pf-result-excerpt").first
            excerpt.wait_for(state="visible", timeout=20000)
            excerpt_text = excerpt.inner_text()

            browser.close()
            return excerpt_text

    def test_search_excerpt_excludes_skip_link(self):
        """Verifies that the first search result excerpt for
        'Crate pw_log_backend_api' does not include 'Skip to main content'."""
        excerpt_text = self._get_first_search_excerpt(
            "Crate pw_log_backend_api"
        )
        self.assertTrue(
            excerpt_text,
            "Expected search result excerpt to be non-empty",
        )
        self.assertNotIn(
            "Skip to main content",
            excerpt_text,
            f"First search result excerpt should not contain 'Skip to main content', got: {excerpt_text}",
        )

    def test_search_excerpt_excludes_header(self):
        """Verifies that the first search result excerpt for 'Crate pw_base64'
        excludes 'light_mode', ensuring pw-header is excluded from indexing."""
        excerpt_text = self._get_first_search_excerpt("Crate pw_base64")
        self.assertTrue(
            excerpt_text,
            "Expected search result excerpt to be non-empty",
        )
        self.assertNotIn(
            "light_mode",
            excerpt_text,
            f"First search result excerpt should not contain 'light_mode', got: {excerpt_text}",
        )

    def test_search_excerpt_excludes_breadcrumbs(self):
        """Verifies that the first search result excerpt for 'Crate pw_base64'
        excludes 'Home', ensuring breadcrumbs UI is excluded from indexing."""
        excerpt_text = self._get_first_search_excerpt("Crate pw_base64")
        self.assertTrue(
            excerpt_text,
            "Expected search result excerpt to be non-empty",
        )
        self.assertNotIn(
            "Home",
            excerpt_text,
            f"First search result excerpt should not contain 'Home', got: {excerpt_text}",
        )

    def test_search_excerpt_excludes_rustdoc_source(self):
        """Verifies that the first search result excerpt for 'Crate pw_status'
        excludes 'Source', ensuring rustdoc source link UI is excluded."""
        excerpt_text = self._get_first_search_excerpt("Crate pw_status")
        self.assertTrue(
            excerpt_text,
            "Expected search result excerpt to be non-empty",
        )
        self.assertNotIn(
            "Source",
            excerpt_text,
            f"First search result excerpt should not contain 'Source', got: {excerpt_text}",
        )

    def test_search_excerpt_excludes_rustdoc_expand_description(self):
        """Verifies that the first search result excerpt for 'Crate pw_status'
        excludes 'Expand description', ensuring rustdoc hideme button is excluded.
        """
        excerpt_text = self._get_first_search_excerpt("Crate pw_status")
        self.assertTrue(
            excerpt_text,
            "Expected search result excerpt to be non-empty",
        )
        self.assertNotIn(
            "Expand description",
            excerpt_text,
            f"First search result excerpt should not contain 'Expand description', got: {excerpt_text}",
        )


if __name__ == "__main__":
    unittest.main()
