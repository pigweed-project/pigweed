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
"""Content UI tests for pigweed.dev."""

import unittest

from helpers import (
    DocsServer,
    get_chromium_executable,
    get_docs_dir,
)
from playwright.sync_api import sync_playwright


class ContentTest(unittest.TestCase):
    """Content UI tests."""

    @classmethod
    def setUpClass(cls):
        cls.docs_dir = get_docs_dir()
        cls.server = DocsServer(cls.docs_dir)
        cls.server.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_bazel_quickstart_card(self):
        """Verifies that clicking the 'Bazel quickstart' card on the homepage
        navigates to https://cs.opensource.google/pigweed/quickstart/bazel.
        """
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
                wait_until="domcontentloaded",
            )

            card = page.locator(".sd-card").filter(
                has=page.locator(".sd-card-title", has_text="Bazel quickstart")
            )
            card.wait_for(state="visible", timeout=5000)
            card.click()
            page.wait_for_load_state("domcontentloaded")

            self.assertEqual(
                page.url,
                "https://cs.opensource.google/pigweed/quickstart/bazel",
            )

            browser.close()


if __name__ == "__main__":
    unittest.main()
