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
"""Fails the Sphinx build if the docs are out of sync with metadata TOML files.

//docs/common/__init__.py runs `validate_categories` on Sphinx's
`build-finished` event. The checks are the same ones that
`bazelisk run //docs/common/metadata:sync -- --check` runs, applied to the
Sphinx source tree that Bazel assembles, which contains a copy of every file
that sync.py manages.
"""

from pathlib import Path

from sphinx.application import Sphinx
from sphinx.errors import SphinxError

from docs.common.metadata.sync import DocsTree, Metadata, check, format_report


def validate_categories(app: Sphinx, exception: Exception | None) -> None:
    """Sphinx `build-finished` listener that checks the docs categorization."""
    if exception is not None:
        return

    srcdir = Path(app.srcdir)
    metadata_dir = Path(__file__).resolve().parent
    meta = Metadata.load(srcdir, metadata_dir=metadata_dir)
    issues = meta.validate()
    if not issues:
        issues = check(DocsTree.for_sphinx_srcdir(srcdir), meta)
    if issues:
        raise SphinxError(format_report(issues))
