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
"""Generates Pagefind search index."""

import asyncio
from pathlib import Path

from pagefind.index import IndexConfig, PagefindIndex
from sphinx.application import Sphinx

# Selectors excluded from Pagefind indexing to prevent UI/anchor artifacts
# from polluting search results.
_SPHINX_EXCLUDE_SELECTORS = [
    '.headerlink',  # Section permalink symbols (¶)
]

_DOXYGEN_EXCLUDE_SELECTORS = [
    '.anchor',  # Member and section anchor links
]

_RUSTDOC_EXCLUDE_SELECTORS = [
    '#copy-path',  # Item path copy button
    '.doc-anchor',  # Markdown heading anchor links (§)
]

_EXCLUDE_SELECTORS = (
    _SPHINX_EXCLUDE_SELECTORS
    + _DOXYGEN_EXCLUDE_SELECTORS
    + _RUSTDOC_EXCLUDE_SELECTORS
)


async def run_pagefind(outdir: str, srcdir: str):
    config = IndexConfig(
        exclude_selectors=_EXCLUDE_SELECTORS,
        verbose=False,
        keep_index_url=True,
        output_path=f'{outdir}/search',
        force_language='en',
    )
    async with PagefindIndex(config=config) as index:
        await index.add_directory(outdir)


def generate_search_index(
    app: Sphinx, exception: Exception | None = None
) -> None:
    if app.builder.format != 'html':
        return
    asyncio.run(run_pagefind(str(app.outdir), str(app.srcdir)))
