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
"""Global site navigation (<pw-nav>) tests for pigweed.dev."""

import unittest

from helpers import (
    DocsServer,
    get_chromium_executable,
    get_docs_dir,
)
from playwright.sync_api import sync_playwright


class NavTest(unittest.TestCase):
    """Global site navigation (<pw-nav>) tests."""

    @classmethod
    def setUpClass(cls):
        cls.docs_dir = get_docs_dir()
        cls.server = DocsServer(cls.docs_dir)
        cls.server.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_sphinx_global_nav_desktop_initial_state(self):
        """Verifies desktop global navigation (<pw-nav>) visibility across a
        representative sample of Sphinx pages, collapsed initial state on the
        homepage, leaf items without <details>, and link color."""
        representative_pages = [
            # Sphinx homepage
            "index.html",
            # Sphinx top-level guide page
            "overview.html",
            # Sphinx category landing page
            "os/index.html",
            # Sphinx module homepage
            "pw_string/docs.html",
            # Sphinx page that previously configured empty html_sidebars
            "size_optimizations.html",
            # Sphinx target documentation page
            "targets/rp2040/target_docs.html",
        ]
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            for width in (840, 1280):
                context = browser.new_context(
                    viewport={"width": width, "height": 800}
                )
                page = context.new_page()

                for rel_path in representative_pages:
                    page.goto(
                        self.server.url_for(rel_path),
                        wait_until="domcontentloaded",
                    )
                    pw_nav = page.locator("#pw-nav")
                    pw_nav.wait_for(state="visible", timeout=5000)
                    self.assertEqual(
                        page.locator("#pst-primary-sidebar").count(), 0
                    )
                    self.assertEqual(
                        page.locator(
                            "nav[aria-label='Section Navigation']"
                        ).count(),
                        0,
                    )

                    first_top_link = pw_nav.locator(
                        ".pw-nav-tree > li > a.pw-nav-tree-link"
                    ).first
                    self.assertEqual(
                        first_top_link.inner_text().strip(), "Home"
                    )
                    self.assertIn(
                        "index.html", first_top_link.get_attribute("href")
                    )

                    if rel_path == "index.html":
                        self.assertEqual(
                            first_top_link.get_attribute("aria-current"),
                            "page",
                        )
                        self.assertEqual(
                            pw_nav.locator("details[open]").count(),
                            0,
                            "All sections should be collapsed on homepage",
                        )

                        # Leaf top-level section (Reference) should not have <details>
                        reference_item = pw_nav.locator(
                            "li:has(> a[href*='api/index.html'])"
                        )
                        self.assertEqual(
                            reference_item.locator("> details").count(),
                            0,
                            "Leaf section Reference should not have a <details> toggle",
                        )

                        # Verify link colors match universal header links (.pw-nav-link)
                        header_link = page.locator(
                            "#pw-header-nav .pw-nav-link"
                        ).first
                        nav_link = pw_nav.locator(
                            ".pw-nav-tree-link:not([aria-current='page'])"
                        ).first
                        self.assertEqual(
                            nav_link.evaluate(
                                "el => getComputedStyle(el).color"
                            ),
                            header_link.evaluate(
                                "el => getComputedStyle(el).color"
                            ),
                            "pw-nav link color should match universal header link color",
                        )

                context.close()

            browser.close()

    def test_sphinx_global_nav_toggle_vs_link_click(self):
        """Verifies across a representative sample of Sphinx pages that clicking
        empty row space toggles <details> without navigating and clicking the
        link text navigates to the target page."""
        representative_pages = [
            # Sphinx homepage
            "index.html",
            # Sphinx top-level guide page
            "overview.html",
            # Sphinx category landing page
            "os/index.html",
            # Sphinx module homepage
            "pw_string/docs.html",
            # Sphinx module subpage
            "pw_allocator/backends.html",
            # Sphinx target documentation page
            "targets/rp2040/target_docs.html",
        ]
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

            for rel_path in representative_pages:
                page.goto(
                    self.server.url_for(rel_path),
                    wait_until="domcontentloaded",
                )
                pw_nav = page.locator("#pw-nav")
                pw_nav.wait_for(state="visible", timeout=5000)

                contributing_item = pw_nav.locator(
                    ".pw-nav-tree > li:has(> a[href*='contributing/index.html'])"
                )
                contributing_link = contributing_item.locator(
                    "> a.pw-nav-tree-link"
                )
                contributing_details = contributing_item.locator("> details")
                contributing_link.scroll_into_view_if_needed()

                item_box = contributing_item.bounding_box()
                link_box = contributing_link.bounding_box()
                self.assertLess(
                    link_box["width"],
                    item_box["width"] - 28,
                    f"Link box on {rel_path} should wrap only link text, not the entire row width",
                )

                # Click in the empty row space to the right of the link text
                page.mouse.click(
                    link_box["x"] + link_box["width"] + 16,
                    link_box["y"] + link_box["height"] / 2,
                )
                self.assertTrue(
                    contributing_details.evaluate(
                        "el => el.hasAttribute('open')"
                    ),
                    f"Clicking outside link text on {rel_path} should expand the children section",
                )
                self.assertTrue(
                    page.url.endswith(rel_path),
                    f"Clicking outside link text on {rel_path} should not navigate away from page",
                )

                # Click again outside the link text to collapse
                page.mouse.click(
                    link_box["x"] + link_box["width"] + 16,
                    link_box["y"] + link_box["height"] / 2,
                )
                self.assertFalse(
                    contributing_details.evaluate(
                        "el => el.hasAttribute('open')"
                    ),
                    f"Clicking outside link text again on {rel_path} should collapse the children section",
                )

                # Clicking the link text itself navigates to contributing/index.html
                contributing_link.click()
                page.wait_for_url("**/contributing/index.html", timeout=5000)
                pw_nav = page.locator("#pw-nav")
                pw_nav.wait_for(state="visible", timeout=5000)
                active_links = pw_nav.locator("a[aria-current='page']")
                self.assertEqual(
                    active_links.count(),
                    1,
                    f"Expected exactly one aria-current='page' link after navigating from {rel_path}",
                )
                self.assertTrue(
                    pw_nav.locator(
                        ".pw-nav-tree > li:has(> a[href*='contributing/index.html']) > details"
                    ).evaluate("el => el.hasAttribute('open')")
                )

            context.close()
            browser.close()

    def test_sphinx_global_nav_filters_duplicate_self_links(self):
        """Verifies that sections with self-referential toctree entries do not
        render duplicate child links in <pw-nav>."""
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
            pw_nav = page.locator("#pw-nav")
            pw_nav.wait_for(state="visible", timeout=5000)

            for self_href in ("overview.html", "platform.html"):
                section_item = pw_nav.locator(
                    f".pw-nav-tree > li:has(> a[href*='{self_href}'])"
                )
                section_summary = section_item.locator("> details > summary")
                section_summary.click()
                self.assertTrue(
                    section_item.locator("> details").evaluate(
                        "el => el.hasAttribute('open')"
                    )
                )
                self_child_links = section_item.locator(
                    f"> details > ul > li > a[href*='{self_href}']"
                )
                self.assertEqual(
                    self_child_links.count(),
                    0,
                    f"{self_href} submenu should not contain a duplicate self link",
                )

            context.close()
            browser.close()

    def test_sphinx_global_nav_desktop_autoscroll(self):
        """Verifies that <pw-nav> automatically scrolls to bring deeply nested
        active pages into view on desktop load."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            for width in (840, 1280):
                context = browser.new_context(
                    viewport={"width": width, "height": 800}
                )
                page = context.new_page()
                page.goto(
                    self.server.url_for("pw_string/docs.html"),
                    wait_until="domcontentloaded",
                )
                pw_nav = page.locator("#pw-nav")
                pw_nav.wait_for(state="visible", timeout=5000)
                page.wait_for_timeout(300)
                scroll_top = pw_nav.evaluate("el => el.scrollTop")
                self.assertGreater(
                    scroll_top,
                    0,
                    f"Desktop #pw-nav should scroll to pw_string on load (width={width}), got scrollTop={scroll_top}",
                )
                context.close()

            browser.close()

    def test_sphinx_global_nav_no_js_and_keyboard_a11y(self):
        """Verifies that <pw-nav> works when JavaScript is disabled and supports
        keyboard Tab navigation."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )

            # 1. Test with JavaScript disabled
            no_js_context = browser.new_context(
                viewport={"width": 1280, "height": 800},
                java_script_enabled=False,
            )
            no_js_page = no_js_context.new_page()
            no_js_page.goto(
                self.server.url_for("pw_allocator/backends.html"),
                wait_until="domcontentloaded",
            )
            pw_nav = no_js_page.locator("#pw-nav")
            pw_nav.wait_for(state="visible", timeout=5000)

            # Ancestor details are open statically at build time without JS
            allocator_details = pw_nav.locator(
                "li:has(> a[href*='pw_allocator/docs.html']) > details"
            )
            self.assertTrue(
                allocator_details.evaluate("el => el.hasAttribute('open')")
            )

            # Clicking a closed <summary> expands it natively without JS
            overview_details = pw_nav.locator(
                "li:has(> a[href*='overview.html']) > details"
            )
            self.assertFalse(
                overview_details.evaluate("el => el.hasAttribute('open')")
            )
            overview_details.locator("> summary").click()
            self.assertTrue(
                overview_details.evaluate("el => el.hasAttribute('open')")
            )
            no_js_context.close()

            # 2. Test keyboard Tab navigation & Enter/Space disclosure
            kb_context = browser.new_context(
                viewport={"width": 1280, "height": 800}
            )
            kb_page = kb_context.new_page()
            kb_page.goto(
                self.server.url_for("index.html"),
                wait_until="domcontentloaded",
            )
            first_link = kb_page.locator("#pw-nav .pw-nav-tree > li > a").first
            first_link.focus()
            self.assertEqual(
                kb_page.evaluate("document.activeElement.textContent.trim()"),
                "Home",
            )

            # Tab moves from Home (leaf item) to Overview
            kb_page.keyboard.press("Tab")
            self.assertEqual(
                kb_page.evaluate("document.activeElement.textContent.trim()"),
                "Overview",
            )

            # Tab moves to <summary> for Overview
            kb_page.keyboard.press("Tab")
            self.assertEqual(
                kb_page.evaluate(
                    "document.activeElement.getAttribute('aria-label')"
                ),
                "Overview subpages",
            )

            # Because Overview <details> is closed, next Tab skips Overview's children
            # and focuses the next top-level link
            kb_page.keyboard.press("Tab")
            next_text = kb_page.evaluate(
                "document.activeElement.textContent.trim()"
            )
            self.assertNotIn(
                next_text,
                ("Pigweed overview", "Mission & philosophies", "Glossary"),
                "Tab should skip links inside closed <details>",
            )

            # Shift+Tab back to Overview <summary> and press Enter to expand
            kb_page.keyboard.press("Shift+Tab")
            kb_page.keyboard.press("Enter")
            overview_details = kb_page.locator(
                "#pw-nav li:has(> a[href*='overview.html']) > details"
            )
            self.assertTrue(
                overview_details.evaluate("el => el.hasAttribute('open')"),
                "Pressing Enter on focused <summary> should open <details>",
            )

            # Now Tab enters the first child link inside Overview
            kb_page.keyboard.press("Tab")
            child_text = kb_page.evaluate(
                "document.activeElement.textContent.trim()"
            )
            self.assertEqual(child_text, "Mission & philosophies")

            kb_context.close()
            browser.close()

    def test_sphinx_global_nav_mobile_locks_background_scroll(self):
        """Verifies that opening <pw-nav> on mobile prevents the main page
        content from scrolling behind the drawer."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 375, "height": 667}
            )
            page = context.new_page()
            page.goto(
                self.server.url_for("pw_string/docs.html"),
                wait_until="domcontentloaded",
            )

            page.evaluate("window.scrollTo(0, 200)")
            self.assertEqual(page.evaluate("window.scrollY"), 200)

            menu_btn = page.locator("#pw-header-menu")
            menu_btn.wait_for(state="visible", timeout=5000)
            menu_btn.click()

            pw_nav = page.locator("#pw-nav")
            pw_nav.wait_for(state="visible", timeout=5000)
            self.assertEqual(
                page.evaluate("getComputedStyle(document.body).overflow"),
                "hidden",
            )
            self.assertEqual(page.evaluate("window.scrollY"), 200)

            # Verify mobile font size and tap target height are larger than desktop
            self.assertEqual(
                pw_nav.evaluate("el => getComputedStyle(el).fontSize"),
                "15px",
            )
            first_link = pw_nav.locator("a.pw-nav-tree-link").first
            self.assertGreaterEqual(first_link.bounding_box()["height"], 32)

            # Attempt to scroll over the backdrop outside the drawer
            page.mouse.move(350, 300)
            page.mouse.wheel(0, 400)
            page.wait_for_timeout(100)
            self.assertEqual(
                page.evaluate("window.scrollY"),
                200,
                "Main content should not scroll when wheeling over the backdrop",
            )

            # Attempt to scroll past the bottom of <pw-nav>
            page.mouse.move(100, 300)
            page.mouse.wheel(0, 10000)
            page.wait_for_timeout(100)
            self.assertEqual(
                page.evaluate("window.scrollY"),
                200,
                "Main content should not scroll when overscrolling inside <pw-nav>",
            )

            # Close drawer and verify main content can scroll again
            menu_btn.click()
            pw_nav.wait_for(state="hidden", timeout=5000)
            self.assertNotEqual(
                page.evaluate("getComputedStyle(document.body).overflow"),
                "hidden",
            )
            page.mouse.move(200, 300)
            page.mouse.wheel(0, 200)
            page.wait_for_timeout(100)
            self.assertGreater(
                page.evaluate("window.scrollY"),
                200,
                "Main content should scroll again after closing <pw-nav>",
            )

            context.close()
            browser.close()


if __name__ == "__main__":
    unittest.main()
