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
"""Tests for pw_presubmit.rst_checks."""

from __future__ import annotations

import importlib
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from pw_presubmit import rst_checks
from pw_presubmit.v2 import Context

try:
    import docutils  # type: ignore
    import pygments  # type: ignore

    _HAS_DEPS = bool(docutils and pygments)
except ImportError:
    _HAS_DEPS = False


class RstChecksOptionalDepsTest(unittest.TestCase):
    """Tests behavior when optional dependencies are unavailable."""

    def test_missing_optional_deps_skips_gracefully(self) -> None:
        try:
            with mock.patch.dict('sys.modules', {'docutils.core': None}):
                importlib.reload(rst_checks)
                self.assertEqual(rst_checks.check_rst_text('Bad\n-\n'), [])
        finally:
            importlib.reload(rst_checks)


@unittest.skipUnless(_HAS_DEPS, 'docutils and pygments are not installed')
class RstChecksTest(unittest.TestCase):
    """Unit tests for reStructuredText syntax and code-block checks."""

    def test_short_title_overline_and_underline_caught(self) -> None:
        overline = (
            '------------------------\n'
            'Machine-readable ``--json``\n'
            '------------------------\n'
        )
        issues = rst_checks.check_rst_text(overline)
        self.assertTrue(
            any('Title overline too short' in i.message for i in issues),
            issues,
        )

        issues = rst_checks.check_rst_text(
            'Subheading with ``code``\n--------\n'
        )
        self.assertEqual(len(issues), 1)
        self.assertIn('Title underline too short', issues[0].message)

    def test_bullet_list_missing_blank_line_caught(self) -> None:
        issues = rst_checks.check_rst_text(
            '- Item one\n- Item two\nParagraph without blank line.\n'
        )
        self.assertEqual(len(issues), 1)
        self.assertIn(
            'Bullet list ends without a blank line', issues[0].message
        )

    def test_nested_inline_code_inside_bold_and_italic_caught(self) -> None:
        issues = rst_checks.check_rst_text(
            '* **Never return ``FutureOrValue`` from APIs.**\n'
        )
        self.assertEqual(len(issues), 1)
        self.assertIn('bold (**...**)', issues[0].message)

        issues = rst_checks.check_rst_text(
            'Use *either ``foo`` or ``bar``* here.\n'
        )
        self.assertEqual(len(issues), 1)
        self.assertIn('italic (*...*)', issues[0].message)

    def test_console_highlighting_error_and_unknown_lexer_caught(self) -> None:
        """Catches Pygments lexing errors, :force:, and unknown lexers."""
        console_text = """\
CLI Usage
=========

.. code-block:: console

   # Update the active commit's title
   $ ./gh pr push
"""
        issues = rst_checks.check_rst_text(console_text)
        self.assertEqual(len(issues), 1)
        self.assertEqual(issues[0].line, 4)
        self.assertIn(
            'as "console" resulted in a Pygments error at token "\'"',
            issues[0].message,
        )

        forced_text = """\
.. code-block:: console
   :force:

   # Update the active commit's title
   $ ./gh pr push
"""
        self.assertEqual(rst_checks.check_rst_text(forced_text), [])

        unknown_issues = rst_checks.check_rst_text(
            '* First, run::\n\n'
            '     $ echo hi\n\n'
            '  ..  note::\n\n'
            '     .. code-block:: not-a-lexer\n\n'
            '        hello()\n'
        )
        self.assertEqual(len(unknown_issues), 1)
        self.assertEqual(unknown_issues[0].line, 7)
        self.assertIn(
            "Pygments lexer name 'not-a-lexer' is not known",
            unknown_issues[0].message,
        )

    def test_no_false_positives_on_sphinx_constructs_and_comments(self) -> None:
        """Ignores valid Sphinx directives/roles, comments, and :: blocks."""
        text = """\
.. role:: python(code)
   :language: python

.. _module-pw_example:

==========
pw_example
==========
.. pigweed-module::
   :name: pw_example

.. |cb| raw:: html

   <input type="checkbox">

.. |xref| replace:: :ref:`module-pw_example`

.. pw::Status::Ok() note keeping commented-out example out of lexer checks:
   .. code-block:: not-a-lexer-inside-comment

      ignored()

Paragraph before standalone literal marker
::

   indented_literal_code()
   .. code-block:: not-a-lexer-inside-literal

Inline roles :python:`x = 1`, :ref:`module-pw_example`, :doc:`/index`,
:cc:`pw::Status`, :bug:`12345`, |cb|, and |xref|.

.. include:: shared_snippet.rst

.. raw:: html

   <div>ignored</div>

.. mermaid::

   flowchart LR
     A --> B

.. grid:: 1

   .. grid-item-card:: Card Title

      .. code-block:: python
         :caption: Example
         :linenos:
         :emphasize-lines: 1

         x = 1
"""
        self.assertEqual(rst_checks.check_rst_text(text), [])

    def test_custom_roles_do_not_leak_across_documents(self) -> None:
        self.assertEqual(
            len(
                rst_checks.check_rst_text(
                    '.. role:: temp_emph(emphasis)\n\n'
                    'Use :temp_emph:`foo ``ok```.\n'
                )
            ),
            1,
        )
        self.assertEqual(
            rst_checks.check_rst_text('Use :temp_emph:`foo ``ok```.\n'), []
        )

    def test_presubmit_step_integration(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            good = root / 'good.rst'
            good.write_text('Title\n=====\n\nClean prose.\n', encoding='utf-8')
            bad = root / 'bad.rst'
            bad.write_text('- Bullet\nMissing blank line.\n', encoding='utf-8')
            bad_utf8 = root / 'bad_utf8.rst'
            bad_utf8.write_bytes(b'\xff\xfe')

            ok_ctx = Context(
                root=root,
                output_dir=root,
                paths=(good, root),
                all_paths=(good,),
                all_modified_paths=(good,),
            )
            rst_checks.presubmit_check.run(ok_ctx)
            self.assertFalse(ok_ctx.failed)

            fail_ctx = Context(
                root=root,
                output_dir=root,
                paths=(good, bad, bad_utf8),
                all_paths=(good, bad, bad_utf8),
                all_modified_paths=(bad, bad_utf8),
            )
            rst_checks.presubmit_check.run(fail_ctx)
            self.assertTrue(fail_ctx.failed)


if __name__ == '__main__':
    unittest.main()
