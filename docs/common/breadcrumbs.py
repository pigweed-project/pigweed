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
"""Universal breadcrumbs generator for Sphinx, Doxygen, and Rustdoc pages."""

from dataclasses import dataclass
from pathlib import Path
import re

from .nav import NavLink, is_matching_url
from .utils import format_href


@dataclass
class Breadcrumb:
    """Represents a single step in a breadcrumb trail."""

    title: str
    href: str | None = None


def find_nav_trail(
    items: list[NavLink], target_rel_path: str
) -> list[NavLink] | None:
    """Recursively searches nav links to find ancestor trail to target_rel_path."""
    for item in items:
        if is_matching_url(item.href, target_rel_path):
            return [item]
        if item.children:
            child_trail = find_nav_trail(item.children, target_rel_path)
            if child_trail is not None:
                return [item] + child_trail
    return None


def extract_page_title(content: str) -> str:
    """Extracts a clean title from HTML content."""
    m_h1 = re.search(r"<h1[^>]*>(.*?)</h1>", content, re.DOTALL)
    if m_h1:
        clean_h1 = re.sub(r"<[^>]+>", "", m_h1.group(1)).strip()
        clean_h1 = clean_h1.rstrip("¶§").strip()
        if clean_h1:
            return clean_h1

    m_title = re.search(r"<title[^>]*>(.*?)</title>", content, re.DOTALL)
    if m_title:
        title_text = re.sub(r"<[^>]+>", "", m_title.group(1)).strip()
        for sep in [" — ", " – ", " - "]:
            if sep in title_text:
                title_text = title_text.split(sep)[0].strip()
        if title_text:
            return title_text

    return "Documentation"


def build_breadcrumb_trail(
    rel_path: str,
    content: str,
    site_nav_links: list[NavLink],
) -> list[Breadcrumb]:
    """Builds a breadcrumb trail for any HTML page across Sphinx, Doxygen, and Rustdoc."""
    norm_path = rel_path.replace("\\", "/").lstrip("/")

    # 1. Homepage: do not display breadcrumbs
    if norm_path in ("index.html", ""):
        return []

    home_crumb = Breadcrumb(title="Home", href="https://pigweed.dev/index.html")
    ref_crumb = Breadcrumb(
        title="Reference", href="https://pigweed.dev/api/index.html"
    )

    # 2. Reference landing page (api/index.html)
    if norm_path == "api/index.html":
        return [home_crumb, Breadcrumb(title="Reference", href=None)]

    # 3. Doxygen C/C++ API pages (under api/cc/)
    if norm_path.startswith("api/cc/"):
        cc_root_href = "https://pigweed.dev/api/cc/modules.html"
        if norm_path in ("api/cc/index.html", "api/cc/modules.html"):
            return [home_crumb, ref_crumb, Breadcrumb(title="C/C++", href=None)]

        trail = [
            home_crumb,
            ref_crumb,
            Breadcrumb(title="C/C++", href=cc_root_href),
        ]

        # Extract all group links from .ingroups .el nodes
        m_ingroups = re.search(
            r'<div class="ingroups"[^>]*>([\s\S]*?)</div>',
            content,
        )
        if m_ingroups:
            ingroups_html = m_ingroups.group(1)
            group_matches = re.findall(
                r'<a\s+class="el"\s+href="([^"]+)"[^>]*>([^<]+)</a>',
                ingroups_html,
            )
            for href, title in group_matches:
                group_file = href.lstrip("./")
                group_href = f"https://pigweed.dev/api/cc/{group_file}"
                trail.append(Breadcrumb(title=title.strip(), href=group_href))

        # Extract title
        m_title = re.search(r'<div class="title">\s*([^<]+)', content)
        if m_title:
            title = m_title.group(1).strip()
        else:
            title = extract_page_title(content)

        trail.append(Breadcrumb(title=title, href=None))
        return trail

    # 4. Rustdoc pages (under rustdoc/)
    if norm_path.startswith("rustdoc/"):
        rust_root_href = "https://pigweed.dev/rustdoc/index.html"
        if norm_path == "rustdoc/index.html":
            return [home_crumb, ref_crumb, Breadcrumb(title="Rust", href=None)]

        trail = [
            home_crumb,
            ref_crumb,
            Breadcrumb(title="Rust", href=rust_root_href),
        ]

        rel_parts = Path(norm_path).parts  # ('rustdoc', '<crate>', ...)
        if len(rel_parts) >= 2:
            crate_name = rel_parts[1]
            crate_href = f"https://pigweed.dev/rustdoc/{crate_name}/index.html"

            if len(rel_parts) == 2 or (
                len(rel_parts) == 3 and rel_parts[2] == "index.html"
            ):
                trail.append(Breadcrumb(title=crate_name, href=None))
                return trail

            trail.append(Breadcrumb(title=crate_name, href=crate_href))

            m_rust = re.search(
                r"<title>([^<]+)\s+in\s+[^<]+-\s*Rust</title>", content
            )
            if m_rust:
                item_title = m_rust.group(1).strip()
            else:
                item_title = extract_page_title(content)

            trail.append(Breadcrumb(title=item_title, href=None))
            return trail

        title = extract_page_title(content)
        trail.append(Breadcrumb(title=title, href=None))
        return trail

    # 4. Sphinx documentation pages
    nav_trail = find_nav_trail(site_nav_links, norm_path)
    if nav_trail:
        trail = [home_crumb]
        for i, item in enumerate(nav_trail):
            is_last = i == len(nav_trail) - 1
            trail.append(
                Breadcrumb(
                    title=item.title,
                    href=None if is_last else item.href,
                )
            )
        return trail

    # Fallback for unlisted Sphinx pages
    title = extract_page_title(content)
    return [home_crumb, Breadcrumb(title=title, href=None)]


def render_breadcrumbs(trail: list[Breadcrumb]) -> str:
    """Renders the HTML for the breadcrumbs component."""
    if not trail:
        return ""

    items_html: list[str] = []
    for i, crumb in enumerate(trail):
        is_last = i == len(trail) - 1
        if is_last or not crumb.href:
            items_html.append(
                f'<li class="pw-breadcrumbs-item">'
                f'<span aria-current="page">{crumb.title}</span>'
                f"</li>"
            )
        else:
            items_html.append(
                f'<li class="pw-breadcrumbs-item">'
                f'<a href="{crumb.href}">{crumb.title}</a>'
                f"</li>"
            )
        if not is_last:
            items_html.append(
                '<li class="pw-breadcrumbs-separator" aria-hidden="true">'
                "<span>/</span>"
                "</li>"
            )

    list_html = "\n    ".join(items_html)
    return (
        f'<pw-breadcrumbs id="pw-breadcrumbs" aria-label="Breadcrumb">\n'
        f'  <ol class="pw-breadcrumbs-list">\n'
        f"    {list_html}\n"
        f"  </ol>\n"
        f"</pw-breadcrumbs>"
    )
