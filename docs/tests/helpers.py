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
"""Utilities and helpers for documentation tests."""

from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import threading


def get_docs_dir() -> Path:
    """Returns the path to the built HTML documentation directory."""
    raw_path = Path(os.environ["DOCS_PATH"])
    if raw_path.is_absolute():
        docs_path = raw_path
    elif "BUILD_WORKSPACE_DIRECTORY" in os.environ:
        workspace_dir = Path(os.environ["BUILD_WORKSPACE_DIRECTORY"])
        docs_path = (workspace_dir / raw_path).resolve()
    else:
        # Resolve real filesystem path of this file to escape Bazel runfiles
        real_file = Path(os.path.realpath(__file__))
        docs_path = (real_file.parent / raw_path).resolve()

    html_dir = docs_path / "_build" / "html"
    if html_dir.is_dir():
        return html_dir
    if docs_path.is_dir():
        return docs_path

    raise FileNotFoundError(
        f"Documentation directory not found at {html_dir} or {docs_path}. "
        "Please build the documentation first "
        "(e.g. 'bazelisk build //docs/sphinx:docs')."
    )


def is_redirect_page(path: Path) -> bool:
    """Returns True if the HTML page is an HTTP-equiv refresh redirect stub."""
    try:
        with path.open("rb") as f:
            head_bytes = f.read(1024).lower()
            return (
                b'http-equiv="refresh"' in head_bytes
                or b"http-equiv='refresh'" in head_bytes
            )
    except OSError:
        return False


def get_html_files(
    docs_dir: Path | None = None,
    include_redirects: bool = False,
) -> list[Path]:
    """Returns all documentation HTML files, filtering out assets/redirects."""
    directory = docs_dir or get_docs_dir()
    if not directory.exists():
        raise FileNotFoundError(
            f"Documentation directory not found: {directory}"
        )

    files = [
        p
        for p in directory.rglob("*.html")
        if p.is_file()
        and not p.is_symlink()
        and not any(
            part in ("_static", "_sources")
            for part in p.relative_to(directory).parts
        )
    ]

    if not include_redirects:
        files = [p for p in files if not is_redirect_page(p)]

    return files


def get_chromium_executable() -> str:
    """Returns path to the hermetic Chromium binary from Bazel runfiles."""
    rpath = os.environ.get("PYTHON_RUNFILES") or os.environ.get("RUNFILES_DIR")
    if not rpath and os.environ.get("TEST_SRCDIR"):
        rpath = os.environ["TEST_SRCDIR"]

    if rpath:
        runfiles_base = Path(rpath)
        for p in runfiles_base.glob("**/+*playwright_chromium*/**/chrome"):
            if p.is_file() and os.access(p, os.X_OK):
                return str(p)
        for p in runfiles_base.glob("**/playwright_chromium*/**/chrome"):
            if p.is_file() and os.access(p, os.X_OK):
                return str(p)
        mac_cft_pattern = (
            "**/Google Chrome for Testing.app/Contents/MacOS/"
            "Google Chrome for Testing"
        )
        for p in runfiles_base.glob(mac_cft_pattern):
            if p.is_file() and os.access(p, os.X_OK):
                return str(p)

    for p in Path.cwd().glob("**/+*playwright_chromium*/**/chrome"):
        if p.is_file() and os.access(p, os.X_OK):
            return str(p)

    raise FileNotFoundError(
        "Hermetic Chromium binary not found in Bazel runfiles."
    )


class DocsServer:
    """Local HTTP server for serving built documentation during tests."""

    def __init__(self, directory: Path | None = None):
        self.directory = directory or get_docs_dir()
        handler = partial(
            SimpleHTTPRequestHandler,
            directory=str(self.directory),
        )
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(
            target=self.httpd.serve_forever,
            daemon=True,
        )

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()

    def url_for(self, path: str) -> str:
        return f"http://127.0.0.1:{self.port}/{path.lstrip('/')}"
