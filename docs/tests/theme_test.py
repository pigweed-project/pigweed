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
"""Theme UI tests for pigweed.dev."""

import unittest

from helpers import (
    DocsServer,
    get_chromium_executable,
    get_docs_dir,
)
from playwright.sync_api import sync_playwright


class ThemeTest(unittest.TestCase):
    """Theme UI tests."""

    @classmethod
    def setUpClass(cls):
        cls.docs_dir = get_docs_dir()
        cls.server = DocsServer(cls.docs_dir)
        cls.server.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_theme_switch_and_persistence(self):
        """Verifies theme switching and persistence across Sphinx, Rustdoc, and
        Doxygen."""
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

            def verify_theme(expected_theme: str):
                page.wait_for_function(
                    "() => document.documentElement.getAttribute('data-theme')"
                    f" === '{expected_theme}'",
                    timeout=5000,
                )
                theme_val = page.evaluate(
                    "document.documentElement.getAttribute('data-theme')"
                )
                self.assertEqual(theme_val, expected_theme)
                btn = page.locator(
                    f"pw-theme button[data-theme-val='{expected_theme}']"
                )
                btn.wait_for(state="visible", timeout=5000)
                self.assertIn("active", btn.get_attribute("class") or "")

            # 1. Start on a Sphinx page
            page.goto(
                self.server.url_for("pw_bytes/docs.html"),
                wait_until="domcontentloaded",
            )

            # 2. Set theme to dark and verify switch
            dark_btn = page.locator("pw-theme button[data-theme-val='dark']")
            dark_btn.wait_for(state="visible", timeout=5000)
            dark_btn.click()
            verify_theme("dark")

            # 3. Navigate to Rustdoc page and verify dark theme persists
            page.goto(
                self.server.url_for("rustdoc/pw_bytes/index.html"),
                wait_until="domcontentloaded",
            )
            verify_theme("dark")

            # 4. Navigate to Doxygen page and verify dark theme persists
            page.goto(
                self.server.url_for("api/cc/group__pw__bytes.html"),
                wait_until="domcontentloaded",
            )
            verify_theme("dark")

            # 5. Set theme to light and verify switch
            light_btn = page.locator("pw-theme button[data-theme-val='light']")
            light_btn.wait_for(state="visible", timeout=5000)
            light_btn.click()
            verify_theme("light")

            # 6. Navigate to a different Doxygen page and verify light theme
            # persists
            page.goto(
                self.server.url_for("api/cc/group__pw__kvs.html"),
                wait_until="domcontentloaded",
            )
            verify_theme("light")

            # 7. Navigate to a different Sphinx page and verify light theme
            # persists
            page.goto(
                self.server.url_for("pw_allocator/docs.html"),
                wait_until="domcontentloaded",
            )
            verify_theme("light")

            browser.close()


if __name__ == "__main__":
    unittest.main()
