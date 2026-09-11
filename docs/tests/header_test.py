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
"""Header tests for pigweed.dev."""

import unittest

from helpers import (
    DocsServer,
    get_chromium_executable,
    get_docs_dir,
    get_html_files,
)
from playwright.sync_api import sync_playwright


class HeaderTest(unittest.TestCase):
    """Header tests."""

    @classmethod
    def setUpClass(cls):
        cls.docs_dir = get_docs_dir()
        cls.html_files = get_html_files(cls.docs_dir)
        cls.server = DocsServer(cls.docs_dir)
        cls.server.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def test_header_present(self):
        """Verifies that <pw-header id="pw-header"> is present on every HTML
        page."""
        self.assertGreater(
            len(self.html_files),
            0,
            f"No HTML files found under {self.docs_dir}",
        )

        missing_header: list[str] = []

        for html_file in self.html_files:
            rel_path = html_file.relative_to(self.docs_dir)
            content = html_file.read_text(encoding="utf-8", errors="ignore")
            if '<pw-header id="pw-header">' not in content:
                missing_header.append(str(rel_path))

        self.assertEqual(
            len(missing_header),
            0,
            f"Found {len(missing_header)} pages missing "
            f'<pw-header id="pw-header">:\n' + "\n".join(missing_header[:20]),
        )

    def test_header_nav_present(self):
        """Verifies that the top navigation bar with links is present."""
        index_file = self.docs_dir / "index.html"
        self.assertTrue(index_file.exists(), f"{index_file} does not exist")
        content = index_file.read_text(encoding="utf-8")
        self.assertIn('<nav id="pw-header-nav"', content)
        self.assertIn('class="pw-nav-item"', content)
        self.assertIn('class="pw-nav-popover"', content)

    def test_header_menu_present(self):
        """Verifies that the mobile hamburger menu button is present."""
        index_file = self.docs_dir / "index.html"
        self.assertTrue(index_file.exists(), f"{index_file} does not exist")
        content = index_file.read_text(encoding="utf-8")
        self.assertIn('id="pw-header-menu"', content)

    def test_mobile_menu_navigation(self):
        """Verifies mobile menu toggle and links across Sphinx subpage,
        Doxygen, and Rustdoc."""
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

            # 1. Sphinx subpage (pw_string/docs.html)
            page.goto(
                self.server.url_for("pw_string/docs.html"),
                wait_until="domcontentloaded",
            )
            menu_btn = page.locator("#pw-header-menu")
            menu_btn.wait_for(state="visible", timeout=5000)
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "false")

            # Click to open Sphinx drawer
            menu_btn.click()
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "true")
            sidebar = page.locator(".bd-sidebar-primary")
            sidebar.wait_for(state="visible", timeout=5000)
            nav_links = sidebar.locator("a")
            self.assertGreater(nav_links.count(), 0)

            # Click backdrop to close
            backdrop = page.locator("#pw-nav-backdrop")
            backdrop.click(force=True)
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "false")

            # 2. Doxygen page (api/cc/group__pw__string.html)
            page.goto(
                self.server.url_for("api/cc/group__pw__string.html"),
                wait_until="domcontentloaded",
            )
            menu_btn = page.locator("#pw-header-menu")
            menu_btn.wait_for(state="visible", timeout=5000)
            menu_btn.click()
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "true")
            doxygen_nav = page.locator("#side-nav")
            doxygen_nav.wait_for(state="visible", timeout=5000)
            self.assertGreater(doxygen_nav.locator("a").count(), 0)

            # 3. Rustdoc page (rustdoc/pw_status/index.html)
            page.goto(
                self.server.url_for("rustdoc/pw_status/index.html"),
                wait_until="domcontentloaded",
            )
            menu_btn = page.locator("#pw-header-menu")
            menu_btn.wait_for(state="visible", timeout=5000)
            menu_btn.click()
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "true")
            rustdoc_sidebar = page.locator("nav.sidebar")
            rustdoc_sidebar.wait_for(state="visible", timeout=5000)
            self.assertGreater(rustdoc_sidebar.locator("a").count(), 0)

            browser.close()

    def test_doxygen_mobile_nav_press_and_scroll(self):
        """Verifies Doxygen navigation drawer on mobile supports clicking
        tree links and scrolling."""
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
                self.server.url_for("api/cc/group__pw__string.html"),
                wait_until="domcontentloaded",
            )
            menu_btn = page.locator("#pw-header-menu")
            menu_btn.wait_for(state="visible", timeout=5000)
            menu_btn.click()

            side_nav = page.locator("#side-nav")
            side_nav.wait_for(state="visible", timeout=5000)

            # Check that nav tree links are clickable and not intercepted by backdrop
            first_link = side_nav.locator("#nav-tree a").first
            first_link.wait_for(state="visible", timeout=5000)
            first_link.click()

            # Verify side-nav is scrollable
            is_scrollable = page.evaluate(
                "() => { const el = document.querySelector('#side-nav'); return el.scrollHeight > el.clientHeight; }"
            )
            self.assertTrue(
                is_scrollable,
                "#side-nav should have scrollable content on mobile",
            )

            browser.close()

    def test_rustdoc_topbar_title_alignment(self):
        """Verifies that the API item title in rustdoc-topbar on mobile is
        left-aligned rather than centered."""
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
                self.server.url_for("rustdoc/pw_status/enum.Error.html"),
                wait_until="domcontentloaded",
            )
            topbar = page.locator("rustdoc-topbar")
            topbar.wait_for(state="visible", timeout=5000)

            h2 = topbar.locator("h2")
            h2.wait_for(state="visible", timeout=5000)

            box = h2.bounding_box()
            self.assertIsNotNone(box)
            # h2 should be aligned near the left edge (< 50px), not centered (~150-180px)
            self.assertLess(
                box["x"],
                50,
                f"Rustdoc topbar title is too far right (x={box['x']})",
            )

            browser.close()

    def test_rustdoc_desktop_layout(self):
        """Verifies that rustdoc-topbar is hidden on desktop viewports and
        sidebar sits as a column to the left of the main content."""
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
                self.server.url_for("rustdoc/pw_status/enum.Error.html"),
                wait_until="domcontentloaded",
            )

            # rustdoc-topbar must be hidden on desktop
            topbar = page.locator("rustdoc-topbar")
            self.assertFalse(
                topbar.is_visible(),
                "rustdoc-topbar should be hidden on desktop viewports",
            )

            # sidebar should be visible and located at left edge
            sidebar = page.locator("nav.sidebar")
            sidebar.wait_for(state="visible", timeout=5000)
            sidebar_box = sidebar.bounding_box()
            self.assertIsNotNone(sidebar_box)
            self.assertEqual(sidebar_box["x"], 0)

            # main content should be located to the right of the sidebar
            main = page.locator("main")
            main.wait_for(state="visible", timeout=5000)
            main_box = main.bounding_box()
            self.assertIsNotNone(main_box)
            self.assertGreaterEqual(main_box["x"], sidebar_box["width"])

            browser.close()

    def test_sphinx_page_toc_fab(self):
        """Verifies floating action button for Sphinx page TOC on mobile."""
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

            # 1. Page with TOC (pw_string/docs.html)
            page.goto(
                self.server.url_for("pw_string/docs.html"),
                wait_until="domcontentloaded",
            )
            fab = page.locator("#pw-toc-fab")
            fab.wait_for(state="visible", timeout=5000)
            self.assertEqual(fab.get_attribute("aria-expanded"), "false")

            # Click FAB to open TOC drawer
            fab.click()
            self.assertEqual(fab.get_attribute("aria-expanded"), "true")
            toc_sidebar = page.locator(".bd-sidebar-secondary")
            toc_sidebar.wait_for(state="visible", timeout=5000)

            # Initial scroll position should be at the top of the page
            self.assertEqual(page.evaluate("window.scrollY"), 0)

            # Click TOC link to close drawer and scroll to target section
            toc_link = toc_sidebar.locator("a[href^='#']").first
            target_href = toc_link.get_attribute("href")
            self.assertIsNotNone(target_href)
            target_id = target_href.lstrip("#")

            toc_link.click()
            self.assertEqual(fab.get_attribute("aria-expanded"), "false")

            # Verify page scrolled down to the target section
            page.wait_for_timeout(300)
            scroll_y = page.evaluate("window.scrollY")
            self.assertGreater(
                scroll_y,
                0,
                f"Page did not scroll after clicking TOC link ({target_href}), got scrollY={scroll_y}",
            )

            # Verify target section element is visible in the viewport
            target_box = page.locator(f"#{target_id}").bounding_box()
            self.assertIsNotNone(target_box)
            self.assertGreaterEqual(target_box["y"], 0)
            self.assertLess(target_box["y"], 667)

            # Re-open and verify backdrop click closes TOC drawer
            fab.click()
            self.assertEqual(fab.get_attribute("aria-expanded"), "true")
            backdrop = page.locator("#pw-nav-backdrop")
            backdrop.click(force=True)
            self.assertEqual(fab.get_attribute("aria-expanded"), "false")

            # 2. Doxygen page -> FAB should not be visible
            page.goto(
                self.server.url_for("api/cc/group__pw__string.html"),
                wait_until="domcontentloaded",
            )
            fab = page.locator("#pw-toc-fab")
            self.assertFalse(
                fab.is_visible(),
                "FAB should not be visible on Doxygen pages",
            )

            # 3. Rustdoc page -> FAB should not be visible
            page.goto(
                self.server.url_for("rustdoc/pw_status/index.html"),
                wait_until="domcontentloaded",
            )
            fab = page.locator("#pw-toc-fab")
            self.assertFalse(
                fab.is_visible(),
                "FAB should not be visible on Rustdoc pages",
            )

            browser.close()

    def test_mobile_drawers_mutual_exclusion_and_escape(self):
        """Verifies mutual exclusivity between primary nav and TOC drawers,
        and verifies Escape key closes active drawers."""
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
            menu_btn = page.locator("#pw-header-menu")
            fab = page.locator("#pw-toc-fab")
            menu_btn.wait_for(state="visible", timeout=5000)
            fab.wait_for(state="visible", timeout=5000)

            # Open primary nav
            menu_btn.click()
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "true")
            self.assertEqual(fab.get_attribute("aria-expanded"), "false")

            # Opening TOC drawer closes primary nav
            fab.click()
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "false")
            self.assertEqual(fab.get_attribute("aria-expanded"), "true")

            # Press Escape to close TOC drawer
            page.keyboard.press("Escape")
            self.assertEqual(fab.get_attribute("aria-expanded"), "false")

            # Open primary nav and press Escape to close
            menu_btn.click()
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "true")
            page.keyboard.press("Escape")
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "false")

            browser.close()

    def test_desktop_header_layout(self):
        """Verifies that #pw-header-nav, #pw-header-search, and #pw-header-tools
        are grouped on the right side of the header on desktop."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1440, "height": 900}
            )
            page = context.new_page()

            page.goto(
                self.server.url_for("index.html"),
                wait_until="domcontentloaded",
            )

            brand = page.locator("#pw-header-brand")
            nav = page.locator("#pw-header-nav")
            search = page.locator("#pw-header-search")
            tools = page.locator("#pw-header-tools")

            brand_box = brand.bounding_box()
            nav_box = nav.bounding_box()
            search_box = search.bounding_box()
            tools_box = tools.bounding_box()

            self.assertIsNotNone(brand_box)
            self.assertIsNotNone(nav_box)
            self.assertIsNotNone(search_box)
            self.assertIsNotNone(tools_box)

            # Check left-to-right order: brand -> nav -> search -> tools
            self.assertLessEqual(
                brand_box["x"] + brand_box["width"], nav_box["x"]
            )
            self.assertLess(nav_box["x"] + nav_box["width"], search_box["x"])
            self.assertLess(
                search_box["x"] + search_box["width"], tools_box["x"]
            )

            # Verify that brand is on the far left and tools is on the far right
            self.assertLess(brand_box["x"], 100)
            self.assertGreater(tools_box["x"] + tools_box["width"], 1440 - 150)

            # Verify that search sits directly adjacent to tools, and nav directly adjacent to search
            self.assertLessEqual(
                tools_box["x"] - (search_box["x"] + search_box["width"]), 24
            )
            self.assertLessEqual(
                search_box["x"] - (nav_box["x"] + nav_box["width"]), 24
            )

            browser.close()

    def test_mobile_header_layout(self):
        """Verifies that hamburger menu and brand are on the left, search and
        tools are on the right, with a gap in the middle on mobile."""
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
                self.server.url_for("index.html"),
                wait_until="domcontentloaded",
            )

            menu = page.locator("#pw-header-menu")
            brand = page.locator("#pw-header-brand")
            tools = page.locator("#pw-header-tools")
            search_mobile = page.locator("#pw-search-mobile")
            theme = page.locator("pw-theme")

            menu_box = menu.bounding_box()
            brand_box = brand.bounding_box()
            tools_box = tools.bounding_box()
            search_box = search_mobile.bounding_box()
            theme_box = theme.bounding_box()

            self.assertIsNotNone(menu_box)
            self.assertIsNotNone(brand_box)
            self.assertIsNotNone(tools_box)
            self.assertIsNotNone(search_box)
            self.assertIsNotNone(theme_box)

            # Left side: menu and brand close to each other on the left
            self.assertLess(menu_box["x"], 30)
            self.assertLess(menu_box["x"] + menu_box["width"], brand_box["x"])
            menu_brand_gap = brand_box["x"] - (
                menu_box["x"] + menu_box["width"]
            )
            self.assertLess(menu_brand_gap, 25)

            # Right side: search and theme close to each other on the right
            self.assertGreater(tools_box["x"] + tools_box["width"], 375 - 40)
            self.assertLess(
                search_box["x"] + search_box["width"], theme_box["x"]
            )
            tools_gap = theme_box["x"] - (search_box["x"] + search_box["width"])
            self.assertLessEqual(tools_gap, 16)

            # Largest gap in the middle between brand and right-side tools
            middle_gap = tools_box["x"] - (brand_box["x"] + brand_box["width"])
            self.assertGreater(middle_gap, menu_brand_gap)
            self.assertGreater(middle_gap, tools_gap)
            self.assertGreater(middle_gap, 40)

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

    def test_sphinx_sitewide_nav_expanded_and_scrolled_on_mobile(self):
        """Verifies that on mobile, the hamburger menu opens the sitewide
        navigation on Sphinx pages, expands ancestors for nested pages, and
        scrolls to the active item."""
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

            # 1. Test deeply nested page on mobile
            page.goto(
                self.server.url_for("pw_allocator/backends.html"),
                wait_until="domcontentloaded",
            )

            menu_btn = page.locator("#pw-header-menu")
            menu_btn.wait_for(state="visible", timeout=5000)
            menu_btn.click()

            sidebar = page.locator("#pst-primary-sidebar")
            sidebar.wait_for(state="visible", timeout=5000)

            # Section nav should be hidden on mobile, sitewide nav visible
            mobile_section_nav = sidebar.locator(
                "nav[aria-label='Section Navigation']"
            )
            mobile_site_nav = sidebar.locator(
                "nav[aria-label='Site Navigation']"
            )
            self.assertFalse(mobile_section_nav.is_visible())
            self.assertTrue(mobile_site_nav.is_visible())

            # Check that the sitewide nav title is present
            title = mobile_site_nav.locator(".bd-links__title")
            title.wait_for(state="visible", timeout=5000)
            self.assertEqual(title.inner_text(), "Site Navigation")

            # Check that ancestor details are open
            allocator_item = mobile_site_nav.locator(
                "li:has(> a[href*='pw_allocator/docs.html'])"
            )
            allocator_details = allocator_item.locator("> details")
            self.assertTrue(
                allocator_details.evaluate("el => el.hasAttribute('open')"),
                "pw_allocator details should be expanded with open attribute",
            )

            # Check that active page has aria-current="page" and is visible
            active_link = mobile_site_nav.locator("a[aria-current='page']")
            active_link.wait_for(state="visible", timeout=5000)
            self.assertIn("Backends", active_link.inner_text())

            # 3. Test scrolling down on a page far down the sitewide navigation
            page.goto(
                self.server.url_for("pw_string/docs.html"),
                wait_until="domcontentloaded",
            )
            menu_btn = page.locator("#pw-header-menu")
            menu_btn.wait_for(state="visible", timeout=5000)
            menu_btn.click()

            sidebar = page.locator("#pst-primary-sidebar")
            sidebar.wait_for(state="visible", timeout=5000)
            string_active = sidebar.locator(
                ".pw-site-nav a[aria-current='page']"
            )
            string_active.wait_for(state="visible", timeout=5000)

            page.wait_for_timeout(300)
            sidebar_scroll_top = sidebar.evaluate("el => el.scrollTop")
            self.assertGreater(
                sidebar_scroll_top,
                0,
                f"Sidebar should have scrolled down to pw_string, got scrollTop={sidebar_scroll_top}",
            )

            # 4. Test homepage mobile sitewide nav
            page.goto(
                self.server.url_for("index.html"),
                wait_until="domcontentloaded",
            )
            menu_btn = page.locator("#pw-header-menu")
            menu_btn.wait_for(state="visible", timeout=5000)
            menu_btn.click()

            sidebar = page.locator("#pst-primary-sidebar")
            sidebar.wait_for(state="visible", timeout=5000)
            title = sidebar.locator(".bd-links__title")
            title.wait_for(state="visible", timeout=5000)
            self.assertEqual(title.inner_text(), "Site Navigation")

            context.close()
            browser.close()

    def test_rustdoc_mobile_nav_in_page_link_closes_menu(self):
        """Verifies that clicking an in-page anchor link (e.g. RefUnwindSafe)
        in the Rustdoc sidebar on mobile closes the navigation drawer and
        restores the hamburger menu icon."""
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
                self.server.url_for(
                    "rustdoc/pw_log_backend_api/enum.LogLevel.html"
                ),
                wait_until="domcontentloaded",
            )

            menu_btn = page.locator("#pw-header-menu")
            menu_btn.wait_for(state="visible", timeout=5000)
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "false")
            icon = menu_btn.locator(".material-symbols-outlined")
            self.assertEqual(icon.inner_text().strip(), "menu")

            # Click hamburger menu to open nav
            menu_btn.click()
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "true")
            self.assertEqual(icon.inner_text().strip(), "menu_open")

            sidebar = page.locator("nav.sidebar")
            sidebar.wait_for(state="visible", timeout=5000)

            # Click RefUnwindSafe link in the sidebar
            link = sidebar.locator('a:has-text("RefUnwindSafe")').first
            link.wait_for(state="visible", timeout=5000)
            link.click()

            # Verify hamburger menu has returned to original closed icon
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "false")
            self.assertEqual(icon.inner_text().strip(), "menu")

            browser.close()


if __name__ == "__main__":
    unittest.main()
