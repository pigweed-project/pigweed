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

    def test_rustdoc_ayu_theme_option_hidden(self):
        """Verifies that the ayu theme option is hidden in Rustdoc settings."""
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
                self.server.url_for("rustdoc/settings.html"),
                wait_until="domcontentloaded",
            )

            # Wait for settings to load
            page.wait_for_selector('label[for="theme-dark"]', timeout=5000)
            self.assertTrue(
                page.locator('label[for="theme-dark"]').is_visible()
            )
            self.assertTrue(
                page.locator('label[for="theme-light"]').is_visible()
            )

            # Verify label[for="theme-ayu"] is not shown
            ayu_label = page.locator('label[for="theme-ayu"]')
            self.assertFalse(
                ayu_label.is_visible(),
                "label[for='theme-ayu'] should be hidden in Rustdoc settings",
            )

            browser.close()

    def test_os_color_scheme_preference(self):
        """Verifies that PwTheme respects OS-level prefers-color-scheme when no
        explicit localStorage theme has been set."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )

            # 1. Test OS light mode preference
            context_light = browser.new_context(
                viewport={"width": 1280, "height": 800},
                color_scheme="light",
            )
            page_light = context_light.new_page()
            page_light.goto(
                self.server.url_for("pw_bytes/docs.html"),
                wait_until="domcontentloaded",
            )
            page_light.wait_for_function(
                "() => document.documentElement.getAttribute('data-theme')"
                " === 'light'",
                timeout=5000,
            )
            self.assertEqual(
                page_light.evaluate(
                    "document.documentElement.getAttribute('data-theme')"
                ),
                "light",
                "Theme should default to light when prefers-color-scheme is light",
            )
            light_btn = page_light.locator(
                "pw-theme button[data-theme-val='light']"
            )
            self.assertIn("active", light_btn.get_attribute("class") or "")
            self.assertEqual(light_btn.get_attribute("aria-checked"), "true")
            context_light.close()

            # 2. Test OS dark mode preference
            context_dark = browser.new_context(
                viewport={"width": 1280, "height": 800},
                color_scheme="dark",
            )
            page_dark = context_dark.new_page()
            page_dark.goto(
                self.server.url_for("pw_bytes/docs.html"),
                wait_until="domcontentloaded",
            )
            page_dark.wait_for_function(
                "() => document.documentElement.getAttribute('data-theme')"
                " === 'dark'",
                timeout=5000,
            )
            self.assertEqual(
                page_dark.evaluate(
                    "document.documentElement.getAttribute('data-theme')"
                ),
                "dark",
                "Theme should default to dark when prefers-color-scheme is dark",
            )
            dark_btn = page_dark.locator(
                "pw-theme button[data-theme-val='dark']"
            )
            self.assertIn("active", dark_btn.get_attribute("class") or "")
            self.assertEqual(dark_btn.get_attribute("aria-checked"), "true")
            context_dark.close()

            browser.close()

    def test_localStorage_security_error_graceful_handling(self):
        """Verifies that PwTheme handles localStorage throwing a SecurityError
        without throwing unhandled exceptions or breaking theme initialization.
        """
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1280, "height": 800},
                color_scheme="light",
            )
            # Simulate SecurityError on window.localStorage access
            context.add_init_script(
                """
                Object.defineProperty(window, 'localStorage', {
                    get: function() {
                        throw new DOMException(
                            "The operation is insecure.",
                            "SecurityError"
                        );
                    }
                });
            """
            )
            page = context.new_page()

            # Verify PwTheme on Rustdoc page (which does not use Sphinx scripts)
            rustdoc_errors = []
            page.on("pageerror", lambda err: rustdoc_errors.append(str(err)))

            page.goto(
                self.server.url_for("rustdoc/pw_bytes/index.html"),
                wait_until="domcontentloaded",
            )

            self.assertEqual(
                rustdoc_errors,
                [],
                f"Rustdoc page had uncaught errors: {rustdoc_errors}",
            )

            # Theme should default to system preference (light)
            page.wait_for_function(
                "() => document.documentElement.getAttribute('data-theme')"
                " === 'light'",
                timeout=5000,
            )
            self.assertEqual(
                page.evaluate(
                    "document.documentElement.getAttribute('data-theme')"
                ),
                "light",
            )

            dark_btn = page.locator("pw-theme button[data-theme-val='dark']")
            dark_btn.wait_for(state="visible", timeout=5000)
            dark_btn.click()

            page.wait_for_function(
                "() => document.documentElement.getAttribute('data-theme')"
                " === 'dark'",
                timeout=5000,
            )
            self.assertEqual(
                page.evaluate(
                    "document.documentElement.getAttribute('data-theme')"
                ),
                "dark",
            )

            is_bc_defined = page.evaluate(
                "() => customElements.get('pw-breadcrumbs') !== undefined"
            )
            self.assertTrue(
                is_bc_defined,
                "pw-breadcrumbs custom element should be successfully defined",
            )

            browser.close()

    def _resolve_css_color(self, page, var_name: str) -> str:
        """Resolves a CSS color custom property to its computed rgb() string."""
        return page.evaluate(
            """(varName) => {
                const probe = document.createElement('span');
                probe.style.color = `var(${varName})`;
                document.body.appendChild(probe);
                const resolved = getComputedStyle(probe).color;
                probe.remove();
                return resolved;
            }""",
            var_name,
        )

    def _goto_dark_page(self, page, path: str) -> None:
        page.goto(self.server.url_for(path), wait_until="domcontentloaded")
        page.wait_for_function(
            "() => document.documentElement.getAttribute('data-theme')"
            " === 'dark'",
            timeout=5000,
        )

    def test_rustdoc_dark_surfaces_match_sphinx(self):
        """Verifies Rustdoc dark surfaces, borders, and links match Pigweed's
        dark palette and Sphinx navigation surface."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1280, "height": 800},
                color_scheme="dark",
            )
            page = context.new_page()

            self._goto_dark_page(page, "pw_bytes/docs.html")
            sphinx_styles = page.evaluate(
                """() => {
                    const container = getComputedStyle(
                        document.querySelector('.bd-container')
                    );
                    const nav = getComputedStyle(
                        document.querySelector('#pw-nav')
                    );
                    return {
                        containerBg: container.backgroundColor,
                        navBg: nav.backgroundColor,
                        navBorderRight: nav.borderRightColor,
                    };
                }"""
            )

            self._goto_dark_page(page, "rustdoc/pw_bytes/index.html")
            expected = {
                "primaryBg": self._resolve_css_color(
                    page, "--pw-color-background-primary"
                ),
                "codeBg": self._resolve_css_color(
                    page, "--pw-color-background-code"
                ),
                "linkFocus": self._resolve_css_color(
                    page, "--pw-color-outline-focus"
                ),
                "border": self._resolve_css_color(page, "--pw-color-border"),
            }
            rustdoc_styles = page.evaluate(
                """() => {
                    const body = getComputedStyle(document.body);
                    const sidebar = getComputedStyle(
                        document.querySelector('nav.sidebar')
                    );
                    const code = getComputedStyle(
                        document.querySelector('pre.rust')
                    );
                    const sidebarLink = getComputedStyle(
                        document.querySelector('nav.sidebar .block a')
                    );
                    const headings = getComputedStyle(
                        document.querySelector('.section-header')
                    );
                    return {
                        bodyBg: body.backgroundColor,
                        sidebarBg: sidebar.backgroundColor,
                        sidebarBorder: sidebar.borderRightColor,
                        codeBg: code.backgroundColor,
                        sidebarLink: sidebarLink.color,
                        headingBorder: headings.borderBottomColor,
                    };
                }"""
            )
            self.assertEqual(rustdoc_styles["bodyBg"], expected["primaryBg"])
            self.assertEqual(
                rustdoc_styles["bodyBg"], sphinx_styles["containerBg"]
            )
            self.assertEqual(
                rustdoc_styles["sidebarBg"], sphinx_styles["navBg"]
            )
            self.assertEqual(
                rustdoc_styles["sidebarBorder"], sphinx_styles["navBorderRight"]
            )
            self.assertEqual(rustdoc_styles["codeBg"], expected["codeBg"])
            self.assertEqual(
                rustdoc_styles["sidebarLink"], expected["linkFocus"]
            )
            self.assertEqual(
                rustdoc_styles["headingBorder"], expected["border"]
            )

            browser.close()

    def test_rustdoc_theme_vars_scoped_to_rustdoc(self):
        """Verifies Rustdoc dark-mode overrides do not leak onto Sphinx or
        Doxygen pages."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1280, "height": 800},
                color_scheme="dark",
            )
            page = context.new_page()

            for path in ("pw_bytes/docs.html", "api/cc/group__pw__bytes.html"):
                self._goto_dark_page(page, path)
                vars_on_root = page.evaluate(
                    """() => {
                        const s = getComputedStyle(document.documentElement);
                        return {
                            mainBg: s.getPropertyValue('--main-background-color').trim(),
                            settingsInput: s.getPropertyValue('--settings-input-color').trim(),
                        };
                    }"""
                )
                self.assertEqual(vars_on_root["mainBg"], "", path)
                self.assertEqual(vars_on_root["settingsInput"], "", path)

            self._goto_dark_page(page, "rustdoc/pw_bytes/index.html")
            self.assertEqual(
                self._resolve_css_color(page, "--settings-input-color"),
                self._resolve_css_color(page, "--pw-color-outline-focus"),
            )

            browser.close()

    def test_rustdoc_dark_semantic_and_syntax_colors(self):
        """Verifies Rustdoc preserves distinct semantic item-kind colors and
        aligns code highlighting in dark mode."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1280, "height": 800},
                color_scheme="dark",
            )
            page = context.new_page()

            self._goto_dark_page(page, "rustdoc/pw_bytes/index.html")
            expected = {
                "linkFocus": self._resolve_css_color(
                    page, "--pw-color-outline-focus"
                ),
                "macro": self._resolve_css_color(
                    page, "--pw-color-syntax-keyword"
                ),
                "kw": self._resolve_css_color(
                    page, "--pw-color-syntax-keyword"
                ),
                "str": self._resolve_css_color(
                    page, "--pw-color-syntax-string"
                ),
                "struct": self._resolve_css_color(page, "--type-link-color"),
                "trait": self._resolve_css_color(
                    page, "--pw-color-syntax-number"
                ),
                "fn": self._resolve_css_color(page, "--function-link-color"),
            }
            crate_styles = page.evaluate(
                """() => {
                    const macroLink = getComputedStyle(
                        document.querySelector('dl.item-table a.macro')
                    );
                    const kw = getComputedStyle(
                        document.querySelector('pre.rust .kw')
                    );
                    const str = getComputedStyle(
                        document.querySelector('pre.rust .string')
                    );
                    return {
                        macroLink: macroLink.color,
                        kwColor: kw.color,
                        strColor: str.color,
                    };
                }"""
            )
            self.assertEqual(crate_styles["macroLink"], expected["macro"])
            self.assertEqual(crate_styles["kwColor"], expected["kw"])
            self.assertEqual(crate_styles["strColor"], expected["str"])

            self._goto_dark_page(page, "rustdoc/pw_status/enum.Error.html")
            item_styles = page.evaluate(
                """() => {
                    const docLink = getComputedStyle(
                        document.querySelector('.docblock a')
                    );
                    const assocLink = getComputedStyle(
                        document.querySelector('a.associatedtype')
                    );
                    const structLink = getComputedStyle(
                        document.querySelector('a.struct')
                    );
                    const traitLink = getComputedStyle(
                        document.querySelector('a.trait')
                    );
                    const fnLink = getComputedStyle(
                        document.querySelector('a.fn')
                    );
                    return {
                        docLink: docLink.color,
                        assocLink: assocLink.color,
                        structLink: structLink.color,
                        traitLink: traitLink.color,
                        fnLink: fnLink.color,
                    };
                }"""
            )
            self.assertEqual(item_styles["docLink"], expected["linkFocus"])
            self.assertEqual(item_styles["assocLink"], expected["linkFocus"])
            self.assertEqual(item_styles["structLink"], expected["struct"])
            self.assertEqual(item_styles["traitLink"], expected["trait"])
            self.assertEqual(item_styles["fnLink"], expected["fn"])
            distinct_colors = {
                item_styles["docLink"],
                item_styles["structLink"],
                item_styles["traitLink"],
                item_styles["fnLink"],
                crate_styles["macroLink"],
            }
            self.assertEqual(
                len(distinct_colors),
                5,
                f"Expected distinct semantic link colors, got {item_styles}",
            )

            browser.close()

    def test_rustdoc_settings_dark_and_light_toggle(self):
        """Verifies Rustdoc settings uses dark surface and light mode toggle
        restores the light background."""
        chromium_bin = get_chromium_executable()

        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path=chromium_bin,
                headless=True,
            )
            context = browser.new_context(
                viewport={"width": 1280, "height": 800},
                color_scheme="dark",
            )
            page = context.new_page()

            self._goto_dark_page(page, "rustdoc/settings.html")
            dark_bg = self._resolve_css_color(
                page, "--pw-color-background-primary"
            )
            self.assertEqual(
                page.evaluate(
                    "getComputedStyle(document.body).backgroundColor"
                ),
                dark_bg,
            )

            light_btn = page.locator("pw-theme button[data-theme-val='light']")
            light_btn.wait_for(state="visible", timeout=5000)
            light_btn.click()
            page.wait_for_function(
                "() => document.documentElement.getAttribute('data-theme')"
                " === 'light'",
                timeout=5000,
            )
            light_bg = self._resolve_css_color(
                page, "--pw-color-background-primary"
            )
            self.assertEqual(
                page.evaluate(
                    "getComputedStyle(document.body).backgroundColor"
                ),
                light_bg,
            )

            browser.close()


if __name__ == "__main__":
    unittest.main()
