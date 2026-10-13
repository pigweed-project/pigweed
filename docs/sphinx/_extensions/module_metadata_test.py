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
"""Unit tests for TOC link expansion and rewriting in Sphinx extensions."""

from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

from docutils.nodes import Element  # type: ignore[import-untyped]

import module_metadata
import toctree as toctree_ext


def _make_toctree(entries: list[tuple[str | None, str]]) -> Element:
    node = Element()
    node['entries'] = list(entries)
    node['rawentries'] = [title for title, _ in entries if title is not None]
    return node


class ModuleMetadataTocTest(unittest.TestCase):
    """Tests for `module_metadata` TOC link insertion and rearrangement."""

    # pylint: disable=protected-access

    def test_insert_or_append_link_appends_when_no_placeholder(self) -> None:
        """Appends link to the end of the TOC when no placeholder is present."""
        toc = _make_toctree([(None, 'guide'), (None, 'design')])
        module_metadata._insert_or_append_link(
            toc,
            slug='cc-api-ref',
            default_title='C/C++ API reference',
            url='https://pigweed.dev/api/cc/group__pw__foo.html',
        )
        self.assertEqual(
            toc['entries'],
            [
                (None, 'guide'),
                (None, 'design'),
                (
                    'C/C++ API reference',
                    'https://pigweed.dev/api/cc/group__pw__foo.html',
                ),
            ],
        )

    def test_insert_or_append_link_replaces_pw_uri_in_place(self) -> None:
        """Replaces an explicit `pw://<slug>` entry at its exact position."""
        toc = _make_toctree(
            [
                (None, 'guide'),
                (None, 'pw://cc-api-ref'),
                (None, 'design'),
            ]
        )
        module_metadata._insert_or_append_link(
            toc,
            slug='cc-api-ref',
            default_title='C/C++ API reference',
            url='https://pigweed.dev/api/cc/group__pw__foo.html',
        )
        self.assertEqual(
            toc['entries'],
            [
                (None, 'guide'),
                (
                    'C/C++ API reference',
                    'https://pigweed.dev/api/cc/group__pw__foo.html',
                ),
                (None, 'design'),
            ],
        )

    def test_insert_or_append_link_preserves_custom_title(self) -> None:
        """Preserves custom title on an explicit `pw://<slug>` entry."""
        toc = _make_toctree(
            [
                ('Custom C++ Docs', 'pw://cc-api-ref'),
                (None, 'guide'),
            ]
        )
        module_metadata._insert_or_append_link(
            toc,
            slug='cc-api-ref',
            default_title='C/C++ API reference',
            url='https://pigweed.dev/api/cc/group__pw__foo.html',
        )
        self.assertEqual(
            toc['entries'],
            [
                (
                    'Custom C++ Docs',
                    'https://pigweed.dev/api/cc/group__pw__foo.html',
                ),
                (None, 'guide'),
            ],
        )
        self.assertEqual(toc['rawentries'], ['Custom C++ Docs'])

    def test_add_links_expands_explicit_placeholder_for_unlisted_module(
        self,
    ) -> None:
        """Expands `pw://cc-api-ref` even if module is not in allowlist."""
        toc = _make_toctree(
            [
                (None, 'guide'),
                (None, 'pw://cc-api-ref'),
                (None, 'size'),
            ]
        )
        self.assertFalse(
            module_metadata.should_add_doxygen_link('pw_unlisted_test_mod')
        )
        module_metadata.add_links('pw_unlisted_test_mod', toc)
        expected_cc_url = (
            'https://pigweed.dev/api/cc/group__pw__unlisted__test__mod.html'
        )
        expected_src_url = (
            'https://cs.opensource.google/pigweed/pigweed/+/main:'
            'pw_unlisted_test_mod/'
        )
        expected_issues_url = (
            'https://issues.pigweed.dev/issues?q='
            'pw_unlisted_test_mod%20status:open'
        )
        self.assertEqual(
            toc['entries'],
            [
                (None, 'guide'),
                ('C/C++ API reference', expected_cc_url),
                (None, 'size'),
                ('Source code', expected_src_url),
                ('Issues', expected_issues_url),
            ],
        )

    def test_add_links_rearranges_all_standard_slugs(self) -> None:
        """Expands all standard `pw://` slugs in their specified order."""
        toc = _make_toctree(
            [
                (None, 'pw://issues'),
                (None, 'guide'),
                (None, 'pw://source-code'),
                (None, 'pw://cc-api-ref'),
                (None, 'pw://rust-api-ref'),
            ]
        )
        module_metadata.add_links('pw_status', toc)
        expected_issues_url = (
            'https://issues.pigweed.dev/issues?q=pw_status%20status:open'
        )
        expected_src_url = (
            'https://cs.opensource.google/pigweed/pigweed/+/main:pw_status/'
        )
        self.assertEqual(
            toc['entries'],
            [
                ('Issues', expected_issues_url),
                (None, 'guide'),
                ('Source code', expected_src_url),
                (
                    'C/C++ API reference',
                    'https://pigweed.dev/api/cc/group__pw__status.html',
                ),
                (
                    'Rust API reference',
                    'https://pigweed.dev/rustdoc/pw_status',
                ),
            ],
        )


class ToctreeRewriteTest(unittest.TestCase):
    """Tests for `toctree.rewrite_relative_links` HTML post-processing."""

    def test_rewrite_relative_links_by_directory_depth(self) -> None:
        """Rewrites `pw://` hrefs to depth-appropriate relative paths."""
        with tempfile.TemporaryDirectory() as tmp:
            outdir = Path(tmp)
            root_html = outdir / 'index.html'
            mod_html = outdir / 'pw_foo' / 'docs.html'
            nested_html = outdir / 'os' / 'zephyr' / 'backends.html'
            mod_html.parent.mkdir(parents=True)
            nested_html.parent.mkdir(parents=True)

            root_html.write_text(
                '<a href="pw://pw_foo/docs.html">Foo</a>', encoding='utf-8'
            )
            mod_html.write_text(
                '<a href="pw://pw_bar/docs.html">Bar</a>', encoding='utf-8'
            )
            nested_html.write_text(
                '<a href="pw://pw_baz/docs.html">Baz</a>', encoding='utf-8'
            )

            fake_app = SimpleNamespace(outdir=str(outdir))
            toctree_ext.rewrite_relative_links(
                fake_app,  # type: ignore[arg-type]
                None,
            )

            self.assertEqual(
                root_html.read_text(encoding='utf-8'),
                '<a href="pw_foo/docs.html">Foo</a>',
            )
            self.assertEqual(
                mod_html.read_text(encoding='utf-8'),
                '<a href="../pw_bar/docs.html">Bar</a>',
            )
            self.assertEqual(
                nested_html.read_text(encoding='utf-8'),
                '<a href="../../pw_baz/docs.html">Baz</a>',
            )


if __name__ == '__main__':
    unittest.main()
