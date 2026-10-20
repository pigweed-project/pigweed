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
"""Sphinx global site navigation (<pw-nav>) generator.

Provides a unified, progressive-disclosure sitewide navigation component
(``<pw-nav>``) across all Sphinx documentation pages on both desktop and mobile
viewports:
1. Constructs the complete recursive sitewide navigation hierarchy directly
   from the root toctree during the Sphinx build using native semantic HTML
   (``<details>``/``<summary>``) and CSS so navigation and section disclosure
   remain fully functional even when JavaScript is disabled.
2. Replaces ``<!-- pw-nav-sentinel -->`` across all Sphinx HTML pages. Note that
   ``<pw-nav>`` is only used on Sphinx pages; it is not used on Doxygen
   (``api/cc/``) or rustdoc (``rustdoc/``) pages, which retain their own native
   navigation sidebars.
3. For nested pages, ancestor ``<details>`` elements and the current page's
   ``<details>`` are marked ``open`` at build time, and the active link is
   tagged with ``aria-current="page"`` and ``current active`` classes so the
   navigation tree is expanded to the current page and progressively enhanced
   with auto-scrolling via ``PwNav`` (``nav.js``).
"""

from dataclasses import dataclass, field
import os
from pathlib import Path

from docutils import nodes
from jinja2 import Environment
from sphinx.application import Sphinx
from sphinx.environment.adapters.toctree import global_toctree_for_doc

from .utils import format_href

_NAV_PLACEHOLDER = "<!-- pw-nav-sentinel -->"


@dataclass
class NavLink:
    """Represents a navigation item in the sitewide menu."""

    title: str
    href: str
    children: list["NavLink"] = field(default_factory=list)
    # True when this link is a canonical Sphinx toctree child document; False
    # when it is an external URI or a `pw://` cross-reference (for example, a
    # target-categorized backend listed on `//<facade>/backends.rst` or a
    # category page in `//docs/sphinx/index.rst` whose canonical parent is
    # another page). Because `format_href` rewrites `pw://` links to the same
    # `https://pigweed.dev/...` URLs as internal pages, we track `is_internal`
    # so `has_active_descendant` and `breadcrumbs.find_nav_trail` only match a
    # page's single canonical toctree location.
    is_internal: bool = True


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
        raw_uri = ref.attributes.get("refuri", "")
        is_internal = bool(
            ref.attributes.get(
                "internal",
                not raw_uri.startswith(("pw://", "http://", "https://")),
            )
        )
        href = format_href(raw_uri)

        child_links: list[NavLink] = []
        for child in list_item.children[1:]:
            if isinstance(child, nodes.bullet_list):
                child_links.extend(_parse_bullet_list(child))

        # Filter out self-referential leaf entries produced by `<self>` / `self`
        # in section toctrees so they do not appear as duplicate child links
        # beneath their own parent in the global navigation tree.
        child_links = [
            c
            for c in child_links
            if not (is_matching_url(c.href, href) and not c.children)
        ]

        nav_links.append(
            NavLink(
                title=title,
                href=href,
                children=child_links,
                is_internal=is_internal,
            )
        )

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

    root_href = format_href(
        app.builder.get_target_uri(root_doc) or "index.html"
    )
    nav_links: list[NavLink] = [
        NavLink(title="Home", href=root_href, is_internal=True)
    ]
    for child in toctree_node.children:
        if isinstance(child, nodes.bullet_list):
            nav_links.extend(_parse_bullet_list(child))
    return nav_links


def has_active_descendant(link: NavLink, current_page_path: str) -> bool:
    """Recursively checks whether a nav link or any descendant matches current page."""
    if link.is_internal and is_matching_url(link.href, current_page_path):
        return True
    return any(
        has_active_descendant(child, current_page_path)
        for child in link.children
    )


_SITE_NAV_TEMPLATE = """
{% macro render_items(items, level) %}
  {% for item in items %}
    {% set is_current = item.is_internal and is_matching(item.href, current_page) %}
    {% set has_active = has_active_child(item, current_page) %}
    <li class="pw-nav-tree-item pw-nav-level-{{ level }}{% if item.children %} has-children{% endif %}{% if is_current or has_active %} current active{% endif %}">
      <a class="pw-nav-tree-link{% if is_current %} current active{% endif %}" href="{{ item.href }}"{% if is_current %} aria-current="page"{% endif %}>{{ item.title }}</a>
      {% if item.children %}
      <details{% if is_current or has_active %} open{% endif %}>
        <summary aria-label="{{ item.title }} subpages"></summary>
        <ul class="pw-nav-sublist">
          {{ render_items(item.children, level + 1) }}
        </ul>
      </details>
      {% endif %}
    </li>
  {% endfor %}
{% endmacro %}
<pw-nav id="pw-nav" data-pagefind-ignore="all">
  <nav class="pw-nav-container" aria-label="Documentation">
    <ul class="pw-nav-tree">
      {{ render_items(nav_links, 1) }}
    </ul>
  </nav>
</pw-nav>
""".strip()


def render_site_nav(
    nav_links: list[NavLink],
    current_page: str,
    env: Environment | None = None,
) -> str:
    """Renders the HTML navigation markup for a specific page."""
    if env is None:
        env = Environment(autoescape=True, trim_blocks=True, lstrip_blocks=True)
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
    if _NAV_PLACEHOLDER not in content:
        return content

    site_nav_html = render_site_nav(nav_links, rel_page_path, env)
    return content.replace(_NAV_PLACEHOLDER, site_nav_html, 1)


def inject_site_nav(app: Sphinx) -> None:
    """Injects the generated sitewide navigation menu into all Sphinx HTML pages."""
    outdir = Path(app.outdir)
    nav_links = extract_site_nav(app)
    if not nav_links:
        return

    env = Environment(autoescape=True, trim_blocks=True, lstrip_blocks=True)
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
