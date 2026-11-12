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

    def test_doxygen_sidebar_width_and_theme_color(self):
        """Verifies Doxygen 272px default width, saved width persistence, and
        light-mode active link color on dark OS."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1280, "height": 840},
                color_scheme="dark",
            )
            context.add_init_script(
                "localStorage.setItem('theme', 'light');"
                "localStorage.setItem('mode', 'light');"
            )
            page = context.new_page()
            page.goto(
                self.server.url_for("api/cc/group__pw__varint.html"),
                wait_until="networkidle",
            )
            nav_tree = page.locator("#nav-tree")
            nav_tree.wait_for(state="visible", timeout=5000)
            selected = nav_tree.locator(".item.selected .label a")
            selected.wait_for(state="visible", timeout=5000)

            self.assertEqual(round(nav_tree.bounding_box()["width"]), 272)
            self.assertEqual(
                round(page.locator("#doc-content").bounding_box()["x"]), 278
            )
            self.assertEqual(
                selected.evaluate("el => getComputedStyle(el).color"),
                "rgb(42, 90, 223)",
            )

            page.evaluate("localStorage.setItem('doxygen_width', '310')")
            page.reload(wait_until="networkidle")
            self.assertEqual(round(nav_tree.bounding_box()["width"]), 310)

            context.close()
            browser.close()

    def test_doxygen_chevron_column_alignment(self):
        """Verifies that visible Doxygen disclosure chevrons sit to the right of
        labels in a single vertical column."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1280, "height": 840}
            )
            page = context.new_page()
            page.goto(
                self.server.url_for("api/cc/group__pw__varint.html"),
                wait_until="networkidle",
            )
            nav_tree = page.locator("#nav-tree")
            nav_tree.locator(".item.selected .label a").wait_for(
                state="visible", timeout=5000
            )

            arrow_rights = page.evaluate(
                """() => {
                const items = Array.from(
                    document.querySelectorAll('#nav-tree .item:has(> a)')
                ).filter(el => el.offsetParent !== null);
                return items.map(item => {
                    const label = item.querySelector(':scope > .label');
                    const arrow = item.querySelector(':scope > a > .arrow');
                    return {
                        labelRight: label.getBoundingClientRect().right,
                        arrowLeft: arrow.getBoundingClientRect().left,
                        arrowRight: Math.round(arrow.getBoundingClientRect().right),
                    };
                });
            }"""
            )
            self.assertGreater(len(arrow_rights), 5)
            for row in arrow_rights:
                self.assertLessEqual(row["labelRight"], row["arrowLeft"])
            self.assertEqual(
                len({row["arrowRight"] for row in arrow_rights}), 1
            )

            context.close()
            browser.close()

    def test_doxygen_row_toggle_and_keyboard(self):
        """Verifies keyboard Enter/Space expansion preserves focus and advances
        Tab into the first child, and clicking empty row space toggles without
        navigating."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1280, "height": 840}
            )
            page = context.new_page()
            nav_tree = page.locator("#nav-tree")

            # 1. Keyboard Enter on unvisited Modules preserves focus and Tabs to first child
            page.goto(
                self.server.url_for("api/cc/index.html"),
                wait_until="networkidle",
            )
            modules_item = nav_tree.locator(
                ".item:has(> .label > a:text-is('Modules'))"
            )
            modules_link = modules_item.locator("> .label > a")
            modules_link.wait_for(state="visible", timeout=5000)
            modules_link.focus()

            page.keyboard.press("Tab")
            self.assertEqual(
                page.evaluate(
                    "document.activeElement.getAttribute('aria-label')"
                ),
                "Modules subpages",
            )
            page.keyboard.press("Enter")
            nav_tree.locator(
                "a[aria-label='Modules subpages'][aria-expanded='true']"
            ).wait_for(state="attached", timeout=5000)
            self.assertEqual(
                page.evaluate(
                    "document.activeElement.getAttribute('aria-label')"
                ),
                "Modules subpages",
            )
            self.assertTrue(
                page.evaluate(
                    "document.activeElement.matches(':focus-visible')"
                )
            )

            page.keyboard.press("Tab")
            self.assertEqual(
                page.evaluate("document.activeElement.textContent.trim()"),
                "Async & concurrency",
            )

            # 2. Keyboard Space on async-loaded category preserves focus too
            page.keyboard.press("Tab")
            self.assertEqual(
                page.evaluate(
                    "document.activeElement.getAttribute('aria-label')"
                ),
                "Async & concurrency subpages",
            )
            page.keyboard.press(" ")
            nav_tree.locator(
                "a[aria-label='Async & concurrency subpages'][aria-expanded='true']"
            ).wait_for(state="attached", timeout=5000)
            self.assertEqual(
                page.evaluate(
                    "document.activeElement.getAttribute('aria-label')"
                ),
                "Async & concurrency subpages",
            )
            page.keyboard.press("Tab")
            self.assertEqual(
                page.evaluate("document.activeElement.textContent.trim()"),
                "pw_async",
            )

            # 3. Clicking empty row space toggles without navigating
            page.goto(
                self.server.url_for("api/cc/group__pw__varint.html"),
                wait_until="networkidle",
            )
            nav_tree.locator(".item.selected .label a").wait_for(
                state="visible", timeout=5000
            )
            hals_item = nav_tree.locator(
                ".item:has(> .label > a:text-is('HALs'))"
            )
            hals_link = hals_item.locator("> .label > a")
            hals_link.scroll_into_view_if_needed()
            link_box = hals_link.bounding_box()
            page.mouse.click(
                link_box["x"] + link_box["width"] + 16,
                link_box["y"] + link_box["height"] / 2,
            )
            nav_tree.locator(
                "a[aria-label='HALs subpages'][aria-expanded='true']"
            ).wait_for(state="attached", timeout=5000)
            self.assertTrue(page.url.endswith("api/cc/group__pw__varint.html"))

            context.close()
            browser.close()

    def test_doxygen_cpp_namespace_shortening_and_collision_guard(self):
        """Verifies leading lowercase C++ namespaces are stripped with full
        hover tooltips while colliding sibling names stay intact."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1280, "height": 840}
            )
            page = context.new_page()
            nav_tree = page.locator("#nav-tree")

            page.goto(
                self.server.url_for(
                    "api/cc/classpw_1_1unit__test_1_1_google_test_style_event_handler.html"
                ),
                wait_until="networkidle",
            )
            selected = nav_tree.locator(".item.selected .label a")
            selected.wait_for(state="visible", timeout=5000)
            self.assertEqual(
                selected.inner_text().strip(), "GoogleTestStyleEventHandler"
            )
            self.assertEqual(
                selected.get_attribute("title"),
                "pw::unit_test::GoogleTestStyleEventHandler",
            )
            self.assertEqual(
                nav_tree.evaluate("el => el.scrollWidth - el.clientWidth"), 0
            )

            page.goto(
                self.server.url_for("api/cc/classpw_1_1thread_1_1_thread.html"),
                wait_until="networkidle",
            )
            nav_tree.locator(".item.selected .label a").wait_for(
                state="visible", timeout=5000
            )
            thread_labels = nav_tree.locator(".item .label a").all_inner_texts()
            self.assertIn("pw::Thread", thread_labels)
            self.assertIn("pw::thread::Thread", thread_labels)

            context.close()
            browser.close()

    def test_doxygen_no_js_hides_empty_sidebar(self):
        """Verifies that when JavaScript is disabled, the unpopulated Doxygen
        sidebar stays hidden and does not cover main page content."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1280, "height": 840},
                java_script_enabled=False,
            )
            page = context.new_page()
            page.goto(
                self.server.url_for("api/cc/group__pw__varint.html"),
                wait_until="domcontentloaded",
            )

            self.assertFalse(page.locator("#side-nav").is_visible())
            title_box = page.locator(".headertitle .title").bounding_box()
            self.assertGreaterEqual(title_box["x"], 8)

            context.close()
            browser.close()

    def test_doxygen_mobile_drawer_toggle_and_in_page_link(self):
        """Verifies that expanding a Doxygen section on mobile keeps the drawer
        open while clicking an in-page anchor link closes it."""
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
                self.server.url_for("api/cc/group__pw__varint.html"),
                wait_until="networkidle",
            )

            menu_btn = page.locator("#pw-header-menu")
            menu_btn.wait_for(state="visible", timeout=5000)
            menu_btn.click()

            side_nav = page.locator("#side-nav")
            side_nav.wait_for(state="visible", timeout=5000)
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "true")

            # Expanding pw_varint subpages keeps the mobile drawer open
            varint_toggle = side_nav.locator(
                "a[aria-label='pw_varint subpages']"
            )
            varint_toggle.scroll_into_view_if_needed()
            varint_toggle.click()
            format_link = side_nav.locator(".label a:text-is('Format')").first
            format_link.wait_for(state="visible", timeout=5000)
            self.assertTrue(side_nav.is_visible())

            # Clicking in-page Format anchor closes the mobile drawer
            format_link.click()
            side_nav.wait_for(state="hidden", timeout=5000)
            self.assertEqual(menu_btn.get_attribute("aria-expanded"), "false")

            context.close()
            browser.close()

    def test_doxygen_mobile_drawer_matches_sphinx(self):
        """Verifies Doxygen and Sphinx mobile drawers share 75vw slide-in width,
        dimmed right-side backdrop, scroll lock, and tap-outside dismissal."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            widths = []
            for rel_path, drawer_sel, scroll_sel in (
                ("pw_string/docs.html", "#pw-nav", "body"),
                ("api/cc/group__pw__varint.html", "#side-nav", "#doc-content"),
            ):
                context = browser.new_context(
                    viewport={"width": 375, "height": 667}
                )
                page = context.new_page()
                page.goto(
                    self.server.url_for(rel_path),
                    wait_until="networkidle",
                )
                drawer = page.locator(drawer_sel)
                menu_btn = page.locator("#pw-header-menu")
                menu_btn.wait_for(state="visible", timeout=5000)

                # Closed drawer rests off-screen to the left with transform transition
                self.assertFalse(drawer.is_visible())
                closed_x = page.evaluate(
                    "sel => Math.round(document.querySelector(sel).getBoundingClientRect().x)",
                    drawer_sel,
                )
                self.assertLess(closed_x, -200)
                self.assertIn(
                    "transform",
                    drawer.evaluate(
                        "el => getComputedStyle(el).transitionProperty"
                    ),
                )

                # Opening slides drawer to x=0 at 75vw width with dimmed backdrop on right
                menu_btn.click()
                drawer.wait_for(state="visible", timeout=5000)
                page.wait_for_function(
                    "sel => Math.round(document.querySelector(sel).getBoundingClientRect().x) === 0",
                    arg=drawer_sel,
                    timeout=5000,
                )
                box = drawer.bounding_box()
                self.assertLessEqual(box["width"], 350)
                self.assertEqual(round(box["width"]), round(375 * 0.75))
                widths.append(round(box["width"]))

                backdrop = page.locator("#pw-nav-backdrop")
                self.assertTrue(backdrop.is_visible())
                self.assertEqual(
                    backdrop.evaluate(
                        "el => getComputedStyle(el).backgroundColor"
                    ),
                    "rgba(0, 0, 0, 0.4)",
                )
                self.assertEqual(
                    page.evaluate(
                        "() => document.elementFromPoint(340, 300)?.id"
                    ),
                    "pw-nav-backdrop",
                )
                self.assertEqual(
                    page.locator(scroll_sel).evaluate(
                        "el => getComputedStyle(el).overflowY"
                    ),
                    "hidden",
                )

                # Clicking exposed dimmed content on the right closes the drawer
                page.mouse.click(340, 300)
                drawer.wait_for(state="hidden", timeout=5000)
                self.assertEqual(
                    menu_btn.get_attribute("aria-expanded"), "false"
                )
                context.close()

            self.assertEqual(widths[0], widths[1])
            browser.close()


if __name__ == "__main__":
    unittest.main()
