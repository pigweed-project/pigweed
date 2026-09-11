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
from pathlib import Path
from typing import Any

from jinja2 import Environment
from sphinx.application import Sphinx

_HEADER_PLACEHOLDER = "<!-- pw-sentinel -->"


class HeaderCompiler:
    """Orchestrates header compilation from CSS and JS files."""

    def __init__(self, integration_dir: Path) -> None:
        self.integration_dir = integration_dir
        self.env = Environment(trim_blocks=True, lstrip_blocks=True)
        template_path = self.integration_dir / "header.html"
        self.template = self.env.from_string(
            template_path.read_text(encoding="utf-8")
        )

    def compile(self) -> str:
        css_files = ["header.css", "search.css", "theme.css"]
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
        )

        lines = [line for line in html.splitlines() if line.strip() != ""]
        return "\n".join(lines) + "\n"


def postprocess(app: Sphinx, exception: Exception | None) -> None:
    """Generates global header and injects it into all generated HTML."""
    if exception is not None or app.builder.format != "html":
        return

    integration_dir = Path(__file__).parent
    compiler = HeaderCompiler(integration_dir)
    header_html = compiler.compile()

    outdir = Path(app.outdir)
    injected_count = 0
    for root, _, files in os.walk(outdir):
        for file in files:
            if file.endswith(".html"):
                path = Path(root) / file
                content = path.read_text(encoding="utf-8")
                if _HEADER_PLACEHOLDER in content:
                    new_content = content.replace(
                        _HEADER_PLACEHOLDER, header_html
                    )
                    path.write_text(new_content, encoding="utf-8")
                    injected_count += 1

    if injected_count == 0:
        raise RuntimeError(
            f"Failed to inject header: placeholder '{_HEADER_PLACEHOLDER}' not "
            f"found in any HTML files under {outdir}"
        )
