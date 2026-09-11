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
"""Universal header integration."""

import os
from dataclasses import dataclass, field
from pathlib import Path
import re
from typing import Any

from docutils import nodes
from jinja2 import Environment
from sphinx.application import Sphinx
from sphinx.environment.adapters.toctree import global_toctree_for_doc

from .utils import format_href

_HEADER_PLACEHOLDER = "<!-- pw-sentinel -->"
_MAX_VISIBLE_HEADER_LINKS = 5


@dataclass
class HeaderLink:
    """Represents a navigation link in the header."""

    title: str
    href: str
    children: list["HeaderLink"] = field(default_factory=list)


def extract_header_nav(app: Sphinx) -> list[HeaderLink]:
    """Crawls the root document toctree and first-level children to build header nav."""
    root_doc = app.config.root_doc
    toctree_node = global_toctree_for_doc(
        app.env,
        root_doc,
        app.builder,
        collapse=False,
        includehidden=True,
        maxdepth=2,
        titles_only=True,
    )
    if not toctree_node or len(toctree_node) == 0:
        return []

    first_child = toctree_node[0]
    if not isinstance(first_child, nodes.bullet_list):
        return []

    top_links: list[HeaderLink] = []

    for list_item in first_child.children:
        if not list_item.children:
            continue
        para = list_item.children[0]
        if not para.children:
            continue
        ref = para.children[0]
        if not isinstance(ref, nodes.reference):
            continue

        title = ref.astext()
        href = format_href(ref.attributes.get("refuri", ""))

        child_links: list[HeaderLink] = []
        for child in list_item.children[1:]:
            if isinstance(child, nodes.bullet_list):
                for sub_item in child.children:
                    if not sub_item.children:
                        continue
                    sub_para = sub_item.children[0]
                    if not sub_para.children:
                        continue
                    sub_ref = sub_para.children[0]
                    if not isinstance(sub_ref, nodes.reference):
                        continue
                    sub_title = sub_ref.astext()
                    sub_href = format_href(sub_ref.attributes.get("refuri", ""))
                    child_links.append(
                        HeaderLink(title=sub_title, href=sub_href)
                    )
                break

        top_links.append(
            HeaderLink(title=title, href=href, children=child_links)
        )

    if len(top_links) <= _MAX_VISIBLE_HEADER_LINKS:
        return top_links

    primary_links = top_links[:_MAX_VISIBLE_HEADER_LINKS]
    more_links = top_links[_MAX_VISIBLE_HEADER_LINKS:]

    dropdown_children: list[HeaderLink] = []
    for link in more_links:
        dropdown_children.append(HeaderLink(title=link.title, href=link.href))

    root_href = format_href(
        app.builder.get_target_uri(root_doc) or "index.html"
    )
    primary_links.append(
        HeaderLink(title="More", href=root_href, children=dropdown_children)
    )

    return primary_links


class HeaderCompiler:
    """Orchestrates header compilation from CSS and JS files."""

    def __init__(
        self,
        integration_dir: Path,
        nav_links: list[HeaderLink] | None = None,
    ) -> None:
        self.integration_dir = integration_dir
        self.nav_links = nav_links or []
        self.env = Environment(trim_blocks=True, lstrip_blocks=True)
        template_path = self.integration_dir / "header.html"
        self.template = self.env.from_string(
            template_path.read_text(encoding="utf-8")
        )

    def compile(self) -> str:
        css_files = [
            "header.css",
            "search.css",
            "theme.css",
            "nav.css",
            "breadcrumbs.css",
        ]
        js_files = ["header.js"]

        css_parts = [
            (self.integration_dir / f).read_text(encoding="utf-8")
            for f in css_files
        ]
        js_parts = [
            (self.integration_dir / f).read_text(encoding="utf-8")
            for f in js_files
        ]

        html = self.template.render(
            style="\n".join(css_parts),
            script="\n".join(js_parts),
            nav_links=self.nav_links,
        )

        lines = [line for line in html.splitlines() if line.strip() != ""]
        return "\n".join(lines) + "\n"


def postprocess(app: Sphinx, exception: Exception | None) -> None:
    """Generates global header, sitewide nav, and breadcrumbs in a single pass."""
    if exception is not None or app.builder.format != "html":
        return

    from .breadcrumbs import (
        build_breadcrumb_trail,
        render_breadcrumbs,
    )
    from .nav import (
        extract_site_nav,
        has_active_descendant,
        inject_site_nav_into_content,
        is_matching_url,
    )

    outdir = Path(app.outdir)
    site_nav_links = extract_site_nav(app)
    header_nav_links = extract_header_nav(app)

    integration_dir = Path(__file__).parent
    compiler = HeaderCompiler(integration_dir, nav_links=header_nav_links)
    header_html = compiler.compile()

    env = Environment(trim_blocks=True, lstrip_blocks=True)
    env.globals["is_matching"] = is_matching_url
    env.globals["has_active_child"] = lambda item, cur: any(
        has_active_descendant(child, cur) for child in item.children
    )

    injected_count = 0
    for root, _, files in os.walk(outdir):
        for file in files:
            if not file.endswith(".html"):
                continue
            path = Path(root) / file
            content = path.read_text(encoding="utf-8")
            if _HEADER_PLACEHOLDER not in content:
                continue

            rel_parts = path.relative_to(outdir).parts
            rel_page_path = str(path.relative_to(outdir))

            # 1. For Sphinx pages (not assets, api, or rustdoc), inject mobile site nav
            if not any(
                part in ("_static", "_sources", "rustdoc", "api")
                for part in rel_parts
            ):
                content = inject_site_nav_into_content(
                    content, rel_page_path, site_nav_links, env
                )

            # 2. Build breadcrumb trail and HTML
            trail = build_breadcrumb_trail(
                rel_page_path, content, site_nav_links
            )
            breadcrumbs_html = render_breadcrumbs(trail)

            # 3. Replace placeholder with header + breadcrumbs
            header_and_breadcrumbs = (
                f"{header_html}\n{breadcrumbs_html}"
                if breadcrumbs_html
                else header_html
            )
            new_content = content.replace(
                _HEADER_PLACEHOLDER, header_and_breadcrumbs
            )
            path.write_text(new_content, encoding="utf-8")
            injected_count += 1

    if injected_count == 0:
        raise RuntimeError(
            f"Failed to inject header: placeholder '{_HEADER_PLACEHOLDER}' not "
            f"found in any HTML files under {outdir}"
        )
