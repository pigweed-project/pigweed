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
"""Sphinx extension to unify Sphinx, Doxygen, and Rustdoc UIs."""

from typing import Any

from docutils import nodes  # type: ignore[import-untyped]
from sphinx.application import Sphinx
from sphinx.util import logging

from docs.common.metadata.validate import validate_categories
from .header import postprocess
from .search import generate_search_index

_LOG = logging.getLogger(__name__)


def _on_doctree_read(app: Sphinx, doctree: nodes.document) -> None:
    del app
    for node in doctree.findall(
        lambda n: isinstance(n, (nodes.strong, nodes.emphasis))
    ):
        text = node.astext()
        if "``" in text:
            _LOG.warning(
                "reStructuredText does not support nested inline code (``...``) "
                "inside %s (**...** or *...*): %r. Close the bold/italic span "
                "before opening ``...``.",
                node.tagname,
                text,
                location=node,
            )


def _on_build_finished(app: Sphinx, exception: Exception | None) -> None:
    if exception is not None or app.builder.format != "html":
        return

    validate_categories(app, exception)
    postprocess(app, exception)
    generate_search_index(app, exception)


def setup(app: Sphinx) -> dict[str, Any]:
    app.connect("doctree-read", _on_doctree_read)
    app.connect("build-finished", _on_build_finished)
    return {
        "version": "0.1",
        "parallel_read_safe": True,
        "parallel_write_safe": True,
    }
