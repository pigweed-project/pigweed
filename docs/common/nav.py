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
"""Sphinx custom site navigation menu generator.

PyData Sphinx Theme (pydata-sphinx-theme) separates top-level header navigation
(startdepth=0) from section sidebar navigation (startdepth=1). On mobile
devices, the default PyData Sphinx Theme navigation presents a confusing UI
and fails to work when JavaScript is disabled.

To solve this, we provide a custom site navigation menu for Sphinx pages on
mobile devices (accessed via the hamburger drawer):
1. We construct the complete recursive sitewide navigation hierarchy directly
   from the root toctree during the Sphinx build using native semantic HTML
   (``<details>``/``<summary>``) so navigation remains fully functional even
   when JavaScript is disabled.
2. We inject this markup into ``#pst-primary-sidebar`` across all Sphinx HTML
   pages (displayed on mobile viewports while desktop continues using PyData's
   native section nav).
3. For deeply nested pages, parent ``<details>`` ancestor elements are
   automatically marked ``open`` at build time, and the active link is tagged
   with ``aria-current="page"`` and ``active`` classes so that the navigation
   tree is expanded to the current page and scrolled into view.
"""

from dataclasses import dataclass, field
import os
from pathlib import Path
import re

from docutils import nodes
from jinja2 import Environment
from sphinx.application import Sphinx
from sphinx.environment.adapters.toctree import global_toctree_for_doc

from .utils import format_href


@dataclass
class NavLink:
    """Represents a navigation item in the sitewide menu."""

    title: str
    href: str
    children: list["NavLink"] = field(default_factory=list)


def _parse_bullet_list(bullet_list: nodes.bullet_list) -> list[NavLink]:
    """Recursively parses docutils bullet_list nodes into NavLink objects."""
    nav_links: list[NavLink] = []
    for list_item in bullet_list.children:
        if not isinstance(list_item, nodes.list_item) or not list_item.children:
            continue
        para = list_item.children[0]
        if not para.children:
            continue
        ref = para.children[0]
        if not isinstance(ref, nodes.reference):
            continue

        title = ref.astext()
        href = format_href(ref.attributes.get("refuri", ""))

        child_links: list[NavLink] = []
        for child in list_item.children[1:]:
            if isinstance(child, nodes.bullet_list):
                child_links = _parse_bullet_list(child)
                break

        nav_links.append(NavLink(title=title, href=href, children=child_links))

    return nav_links


def extract_site_nav(app: Sphinx) -> list[NavLink]:
    """Extracts sitewide navigation sections and child links from root toctree."""
    root_doc = app.config.root_doc
    toctree_node = global_toctree_for_doc(
        app.env,
        root_doc,
        app.builder,
        collapse=False,
        includehidden=True,
        maxdepth=-1,
        titles_only=True,
    )
    if not toctree_node or len(toctree_node) == 0:
        return []

    first_child = toctree_node[0]
    if not isinstance(first_child, nodes.bullet_list):
        return []

    return _parse_bullet_list(first_child)


def _normalize_url_for_matching(url: str) -> str:
    """Normalizes a URL or path for matching against the current page path."""
    url = url.split("?")[0].split("#")[0].replace("\\", "/")
    if url.startswith("https://pigweed.dev/"):
        url = url[len("https://pigweed.dev/") :]
    elif url.startswith("http://pigweed.dev/"):
        url = url[len("http://pigweed.dev/") :]
    url = url.lstrip("/")
    if url.endswith("/index.html"):
        url = url[: -len("/index.html")]
    elif url == "index.html":
        url = ""
    elif url.endswith(".html"):
        url = url[: -len(".html")]
    elif url.endswith("/"):
        url = url[:-1]
    return url


def is_matching_url(link_href: str, current_page_path: str) -> bool:
    """Checks whether a nav link points to the current page."""
    norm_link = _normalize_url_for_matching(link_href)
    norm_page = _normalize_url_for_matching(current_page_path)
    return norm_link == norm_page


def has_active_descendant(link: NavLink, current_page_path: str) -> bool:
    """Recursively checks whether a nav link or any descendant matches current page."""
    if is_matching_url(link.href, current_page_path):
        return True
    return any(
        has_active_descendant(child, current_page_path)
        for child in link.children
    )


_SITE_NAV_TEMPLATE = """
{% macro render_items(items, level) %}
  {% for item in items %}
    {% set is_current = is_matching(item.href, current_page) %}
    {% set has_active = has_active_child(item, current_page) %}
    <li class="toctree-l{{ level }}{% if item.children %} has-children{% endif %}{% if is_current or has_active %} current active{% endif %}">
      <a class="reference internal{% if is_current %} current active{% endif %}" href="{{ item.href }}"{% if is_current %} aria-current="page"{% endif %}>{{ item.title }}</a>
      {% if item.children %}
      <details{% if is_current or has_active %} open{% endif %}>
        <summary><span class="toctree-toggle" role="presentation"><i class="fa-solid fa-chevron-down"></i></span></summary>
        <ul>
          {{ render_items(item.children, level + 1) }}
        </ul>
      </details>
      {% endif %}
    </li>
  {% endfor %}
{% endmacro %}
<nav class="bd-docs-nav bd-links" aria-label="Site Navigation">
  <p class="bd-links__title" role="heading" aria-level="1">Site Navigation</p>
  <div class="bd-toc-item navbar-nav">
    <ul class="nav bd-sidenav">
      {{ render_items(nav_links, 1) }}
    </ul>
  </div>
</nav>
""".strip()


def render_site_nav(
    nav_links: list[NavLink],
    current_page: str,
    env: Environment | None = None,
) -> str:
    """Renders the HTML navigation markup for a specific page."""
    if env is None:
        env = Environment(trim_blocks=True, lstrip_blocks=True)
        env.globals["is_matching"] = is_matching_url
        env.globals["has_active_child"] = lambda item, cur: any(
            has_active_descendant(child, cur) for child in item.children
        )
    template = env.from_string(_SITE_NAV_TEMPLATE)
    return template.render(nav_links=nav_links, current_page=current_page)


def inject_site_nav_into_content(
    content: str,
    rel_page_path: str,
    nav_links: list[NavLink],
    env: Environment,
) -> str:
    """Injects sitewide navigation into the content of a Sphinx HTML page."""
    if 'id="pst-primary-sidebar"' not in content:
        return content

    site_nav_html = render_site_nav(nav_links, rel_page_path, env)
    pattern = r'<div class="sidebar-primary-item">\s*<nav class="bd-docs-nav bd-links"[\s\S]*?</nav>\s*</div>'
    site_nav_item = f'<div class="sidebar-primary-item pw-site-nav">\n{site_nav_html}\n</div>'

    if re.search(pattern, content):
        return re.sub(
            pattern,
            lambda m: f"{m.group(0)}\n{site_nav_item}",
            content,
            count=1,
        )

    target = '<div class="sidebar-header-items sidebar-primary__section">'
    if target in content:
        replacement = f"{target}\n{site_nav_item}"
        return content.replace(target, replacement, 1)

    end_target = '<div class="sidebar-primary-items__end'
    if end_target in content:
        replacement = f"{site_nav_item}\n{end_target}"
        return content.replace(end_target, replacement, 1)

    return content


def inject_site_nav(app: Sphinx) -> None:
    """Injects the generated sitewide navigation menu into all Sphinx HTML pages."""
    outdir = Path(app.outdir)
    nav_links = extract_site_nav(app)
    if not nav_links:
        return

    env = Environment(trim_blocks=True, lstrip_blocks=True)
    env.globals["is_matching"] = is_matching_url
    env.globals["has_active_child"] = lambda item, cur: any(
        has_active_descendant(child, cur) for child in item.children
    )

    for root, _, files in os.walk(outdir):
        for file in files:
            if not file.endswith(".html"):
                continue
            path = Path(root) / file
            rel_parts = path.relative_to(outdir).parts
            is_doxygen = rel_parts[:2] == ("api", "cc")
            # Skip asset directories and external subsites (Doxygen, Rustdoc)
            if (
                any(
                    part in ("_static", "_sources", "rustdoc")
                    for part in rel_parts
                )
                or is_doxygen
            ):
                continue

            content = path.read_text(encoding="utf-8")
            rel_page_path = path.relative_to(outdir).as_posix()
            new_content = inject_site_nav_into_content(
                content, rel_page_path, nav_links, env
            )
            if new_content != content:
                path.write_text(new_content, encoding="utf-8")
