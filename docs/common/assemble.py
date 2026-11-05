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
"""Assembles pigweed.dev from Sphinx, Doxygen, and rustdoc HTML outputs."""

import argparse
import asyncio
from pathlib import Path
import shutil
import sys

from docs.common.header import (
    HEADER_NAV_FILENAME,
    inject_subsite_headers,
    load_header_nav,
)
from docs.common.metadata.sync import SyncError
from docs.common.metadata.validate import check_categories
from docs.common.search import run_pagefind
from docs.sphinx._extensions.sitemap import write_sitemap


def assemble_site(
    sphinx_html: Path,
    doxygen_html: Path,
    rustdoc_html: Path,
    sphinx_srcdir: Path,
    out_dir: Path,
) -> None:
    """Validates categories, injects subsite headers, and finishes site."""
    check_categories(sphinx_srcdir, rustdoc_dir=rustdoc_html)
    nav_links = load_header_nav(sphinx_html / HEADER_NAV_FILENAME)

    shutil.copytree(
        sphinx_html,
        out_dir,
        ignore=shutil.ignore_patterns(HEADER_NAV_FILENAME),
        copy_function=shutil.copyfile,
        dirs_exist_ok=True,
    )
    # Bazel TreeArtifact inputs have 0555 directory permissions under local
    # execution; copytree's copystat copies those bits onto out_dir and api/.
    for directory in (out_dir, out_dir / "api"):
        if directory.is_dir():
            directory.chmod(0o755)
    inject_subsite_headers(
        doxygen_html, out_dir / "api" / "cc", "api/cc", nav_links
    )
    inject_subsite_headers(
        rustdoc_html, out_dir / "rustdoc", "rustdoc", nav_links
    )

    write_sitemap(out_dir)
    asyncio.run(run_pagefind(out_dir))


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sphinx-html", type=Path, required=True)
    parser.add_argument("--doxygen-html", type=Path, required=True)
    parser.add_argument("--rustdoc-html", type=Path, required=True)
    parser.add_argument("--sphinx-srcdir", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    """CLI entrypoint for top-level site assembly."""
    args = _parse_args(argv)
    try:
        assemble_site(
            args.sphinx_html,
            args.doxygen_html,
            args.rustdoc_html,
            args.sphinx_srcdir,
            args.out_dir,
        )
    except SyncError as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
