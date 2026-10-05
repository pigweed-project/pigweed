# Copyright 2024 The Pigweed Authors
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
"""Generates a sitemap for pigweed.dev."""


from pathlib import Path

from sphinx.application import Sphinx

# Directories excluded from the sitemap.
_EXCLUDE_DIRS = {
    '_sources',  # Raw reST source files
    '_sphinx_design_static',  # sphinx-design static assets
    '_static',  # Sphinx static assets
    'search',  # Pagefind search index assets
}

# Path patterns (relative to outdir) excluded from the sitemap if found
# anywhere in the relative path.
_EXCLUDE_PATTERNS = {
    'rustdoc/src/',  # Rustdoc verbatim source code pages
}

# Specific files (relative to outdir) excluded from the sitemap.
_EXCLUDE_FILES = {
    'genindex.html',  # Unused Sphinx general index
    'py-modindex.html',  # Unused Sphinx Python module index
    'rustdoc/help.html',  # Rustdoc keyboard shortcuts help UI
    'rustdoc/settings.html',  # Rustdoc settings UI
    'search.html',  # Unused legacy Sphinx search page
}


def is_redirect_page(path: Path) -> bool:
    """Returns True if the HTML page is an HTTP-equiv refresh redirect stub."""
    try:
        with path.open('rb') as f:
            head_bytes = f.read(1024).lower()
            return (
                b'http-equiv="refresh"' in head_bytes
                or b"http-equiv='refresh'" in head_bytes
            )
    except OSError:
        # An OSError can occur if a file disappears while the output directory
        # is being scanned (e.g. during a live-reload build) or has unreadable
        # permissions. Default to False (not a redirect) rather than failing the
        # entire Sphinx build.
        return False


def should_include(path: Path, outdir: Path) -> bool:
    """Returns True if the HTML file should be included in the sitemap."""
    if not path.is_file() or path.is_symlink():
        return False
    rel_path = path.relative_to(outdir)
    if any(part in _EXCLUDE_DIRS for part in rel_path.parts):
        return False
    rel_posix = rel_path.as_posix()
    if rel_posix in _EXCLUDE_FILES or any(
        pattern in rel_posix for pattern in _EXCLUDE_PATTERNS
    ):
        return False
    if is_redirect_page(path):
        return False
    return True


def build_url(rel_path: str) -> str:
    if not rel_path.endswith('.html'):
        rel_path = f'{rel_path}.html'
    url = f'https://pigweed.dev/{rel_path.lstrip("/")}'
    # The pigweed.dev production server redirects pages that end with
    # `…/docs.html` to `…/`. E.g. `https://pigweed.dev/pw_string/docs.html`
    # redirects to `https://pigweed.dev/pw_string/`. The latter is the version
    # that should be recorded in the sitemap for optimal SEO. b/386257958
    #
    # Be careful not to clobber other files that end in `docs.html` such
    # as `https://pigweed.dev/targets/rp2040/target_docs.html`.
    #
    # The server also redirects pages that end in `…/index.html`.
    redirects = [
        '/docs.html',
        '/index.html',
    ]
    for pattern in redirects:
        if url.endswith(pattern):
            end = url.rfind(pattern)
            url = url[0:end]
            url += '/'
    return url


def generate_sitemap(app: Sphinx, exception: Exception | None) -> None:
    if exception is not None or app.builder.format != 'html':
        return
    outdir = Path(app.outdir)
    urls = [
        build_url(path.relative_to(outdir).as_posix())
        for path in outdir.rglob('*.html')
        if should_include(path, outdir)
    ]
    urls.sort()
    sitemap = '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
    for url in urls:
        sitemap += f'  <url><loc>{url}</loc></url>\n'
    sitemap += '</urlset>\n'
    (outdir / 'sitemap.xml').write_text(sitemap, encoding='utf-8')


def setup(app: Sphinx) -> dict[str, bool]:
    app.connect('build-finished', generate_sitemap)
    return {
        'parallel_read_safe': True,
        'parallel_write_safe': True,
    }
