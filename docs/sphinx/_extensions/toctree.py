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
"""Sphinx extension to rewrite relative URLs in the generated HTML files."""

import os
from pathlib import Path
from sphinx.application import Sphinx


def rewrite_relative_links(app: Sphinx, exception: Exception | None) -> None:
    if exception is not None:
        return

    outdir = Path(app.outdir)
    for root, _, files in os.walk(outdir):
        for file in files:
            if file.endswith('.html'):
                path = Path(root) / file
                rel_depth = Path(root).relative_to(outdir)
                depth = len(rel_depth.parts)
                rel_path = '../' * depth

                content = path.read_text(encoding='utf-8')
                if 'pw://' in content:
                    new_content = content.replace('pw://', rel_path)
                    path.write_text(new_content, encoding='utf-8')


def setup(app: Sphinx) -> dict[str, bool]:
    app.connect('build-finished', rewrite_relative_links)
    return {
        'parallel_read_safe': True,
        'parallel_write_safe': True,
    }
