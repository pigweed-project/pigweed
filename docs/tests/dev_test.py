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
"""Tests for dev.js (local development and staging preview utilities)."""

import unittest

from helpers import (
    DocsServer,
    get_chromium_executable,
    get_docs_dir,
)
from playwright.sync_api import sync_playwright


class DevTest(unittest.TestCase):
    """Client-side URL rewriter tests."""

    @classmethod
    def setUpClass(cls):
        cls.docs_dir = get_docs_dir()
        cls.server = DocsServer(cls.docs_dir)
        cls.server.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_header_links_rewritten(self):
        """Verifies that top-level header links have absolute pigweed.dev URLs
        rewritten to relative paths."""
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
                self.server.url_for("pw_analog/docs.html"),
                wait_until="domcontentloaded",
            )

            # Brand link in header
            brand_link = page.locator("#pw-header-brand a").first
            brand_link.wait_for(state="attached", timeout=5000)
            href = brand_link.get_attribute("href")
            self.assertIsNotNone(href)
            self.assertEqual(
                href,
                "../index.html",
                f"Brand link should be rewritten to '../index.html', got '{href}'",
            )

            browser.close()

    def test_sidebar_api_links_rewritten(self):
        """Verifies that links inside the primary sidebar (such as C/C++ API
        references) have absolute pigweed.dev URLs rewritten to relative
        paths."""
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
                self.server.url_for("pw_analog/docs.html"),
                wait_until="domcontentloaded",
            )

            # Find C/C++ API reference link in #pst-primary-sidebar / .bd-sidebar-primary
            sidebar_link = page.locator(
                '.bd-sidebar-primary a[href*="group__pw__analog"]'
            ).first
            sidebar_link.wait_for(state="attached", timeout=5000)

            href = sidebar_link.get_attribute("href")
            self.assertIsNotNone(href)
            self.assertFalse(
                href.startswith("https://pigweed.dev/"),
                f"Sidebar link href '{href}' should not start with https://pigweed.dev/",
            )
            self.assertTrue(
                href.startswith("../") or href.startswith("./"),
                f"Sidebar link href '{href}' should be rewritten to relative path",
            )

            browser.close()

    def test_rustdoc_main_content_link_rewritten(self):
        """Verifies that links within main content on Rustdoc pages (e.g.
        'Pigweed Homepage' on rustdoc/pigweed/index.html) have absolute
        pigweed.dev URLs rewritten to relative paths."""
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
                self.server.url_for("rustdoc/pigweed/index.html"),
                wait_until="domcontentloaded",
            )

            # Find 'Pigweed Homepage' link within main content
            link = page.locator('main a:has-text("Pigweed Homepage")').first
            link.wait_for(state="attached", timeout=5000)

            href = link.get_attribute("href")
            self.assertIsNotNone(href)
            self.assertFalse(
                href.startswith("https://pigweed.dev/"),
                f"Rustdoc main content link href '{href}' should not start with https://pigweed.dev/",
            )
            browser.close()

    def test_section_directory_urls_not_truncated(self):
        """Verifies that section directory URLs ending in '/' are rewritten to
        their corresponding index.html without truncating the section path."""
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

            # Test on nested subpage (root is '../')
            page.goto(
                self.server.url_for("pw_bytes/docs.html"),
                wait_until="domcontentloaded",
            )

            results = page.evaluate(
                """() => {
                const testCases = [
                    { input: 'https://pigweed.dev/contributing/', expected: '../contributing/index.html' },
                    { input: 'https://pigweed.dev/contributing/#guidelines', expected: '../contributing/index.html#guidelines' },
                    { input: 'https://pigweed.dev/docs/build/', expected: '../docs/build/index.html' },
                    { input: 'https://pigweed.dev/', expected: '../index.html' },
                    { input: 'https://pigweed.dev/index.html', expected: '../index.html' },
                    { input: 'https://pigweed.dev/search.html?q=test', expected: '../search.html?q=test' },
                ];
                return testCases.map(tc => {
                    const a = document.createElement('a');
                    a.setAttribute('href', tc.input);
                    document.body.appendChild(a);
                    rewriteUrls();
                    const actual = a.getAttribute('href');
                    a.remove();
                    return { input: tc.input, actual, expected: tc.expected };
                });
            }"""
            )

            for res in results:
                self.assertEqual(
                    res["actual"],
                    res["expected"],
                    f"Failed rewrite for {res['input']}: expected {res['expected']}, got {res['actual']}",
                )

            # Test on root page (root is './')
            page.goto(
                self.server.url_for("index.html"),
                wait_until="domcontentloaded",
            )

            root_results = page.evaluate(
                """() => {
                const testCases = [
                    { input: 'https://pigweed.dev/contributing/', expected: './contributing/index.html' },
                    { input: 'https://pigweed.dev/contributing/#guidelines', expected: './contributing/index.html#guidelines' },
                    { input: 'https://pigweed.dev/', expected: './index.html' },
                ];
                return testCases.map(tc => {
                    const a = document.createElement('a');
                    a.setAttribute('href', tc.input);
                    document.body.appendChild(a);
                    rewriteUrls();
                    const actual = a.getAttribute('href');
                    a.remove();
                    return { input: tc.input, actual, expected: tc.expected };
                });
            }"""
            )

            for res in root_results:
                self.assertEqual(
                    res["actual"],
                    res["expected"],
                    f"Failed root rewrite for {res['input']}: expected {res['expected']}, got {res['actual']}",
                )

            browser.close()


if __name__ == "__main__":
    unittest.main()
