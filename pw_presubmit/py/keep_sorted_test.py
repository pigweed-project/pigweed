#!/usr/bin/env python3
# Copyright 2022 The Pigweed Authors
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
"""Tests for keep_sorted."""

from pathlib import Path
import tempfile
import textwrap
from typing import Sequence
import unittest
from unittest.mock import MagicMock

from pw_presubmit import keep_sorted

# Only include these literals here so keep_sorted doesn't try to reorder later
# test lines.
START = keep_sorted.START
END = keep_sorted.END

# pylint: disable=attribute-defined-outside-init
# pylint: disable=too-many-public-methods


class TestKeepSorted(unittest.TestCase):
    """Test KeepSorted class"""

    def _run(self, contents: str) -> None:
        self.ctx = MagicMock()
        self.ctx.fail = MagicMock()

        with tempfile.TemporaryDirectory() as tempdir:
            path = Path(tempdir) / 'foo'

            with path.open('w') as outs:
                outs.write(contents)

            self.errors: dict[Path, Sequence[str]] = {}

            # pylint: disable=protected-access
            self.sorter = keep_sorted._FileSorter(self.ctx, path, self.errors)

            # pylint: enable=protected-access

            self.sorter.sort()

            # Truncate the file so it's obvious whether write() changed
            # anything.
            with path.open('w') as outs:
                outs.write('')

            self.sorter.write(path)
            with path.open() as ins:
                self.contents = ins.read()

    def assert_errors(self):
        self.assertTrue(self.errors)

    def assert_no_errors(self):
        self.assertFalse(self.errors)

    def test_missing_end(self) -> None:
        with self.assertRaises(keep_sorted.KeepSortedParsingError):
            self._run(f'{START}\n')

    def test_missing_start(self) -> None:
        with self.assertRaises(keep_sorted.KeepSortedParsingError):
            self._run(f'{END}: end\n')

    def test_repeated_start(self) -> None:
        with self.assertRaises(keep_sorted.KeepSortedParsingError):
            self._run(f'{START}\n{START}\n')

    def test_unrecognized_directive(self) -> None:
        with self.assertRaises(keep_sorted.KeepSortedParsingError):
            self._run(f'{START} foo bar baz\n2\n1\n{END}\n')

    def test_repeated_valid_directive(self) -> None:
        with self.assertRaises(keep_sorted.KeepSortedParsingError):
            self._run(f'{START} ignore-case ignore-case\n2\n1\n{END}\n')

    def test_already_sorted(self) -> None:
        self._run(f'{START}\n1\n2\n3\n4\n{END}\n')
        self.assert_no_errors()
        self.assertEqual(self.contents, '')

    def test_not_sorted(self) -> None:
        self._run(f'{START}\n4\n3\n2\n1\n{END}\n')
        self.assert_errors()
        self.assertEqual(self.contents, f'{START}\n1\n2\n3\n4\n{END}\n')

    def test_prefix_sorted(self) -> None:
        self._run(f'foo\nbar\n{START}\n1\n2\n{END}\n')
        self.assert_no_errors()
        self.assertEqual(self.contents, '')

    def test_prefix_not_sorted(self) -> None:
        self._run(f'foo\nbar\n{START}\n2\n1\n{END}\n')
        self.assert_errors()
        self.assertEqual(self.contents, f'foo\nbar\n{START}\n1\n2\n{END}\n')

    def test_suffix_sorted(self) -> None:
        self._run(f'{START}\n1\n2\n{END}\nfoo\nbar\n')
        self.assert_no_errors()
        self.assertEqual(self.contents, '')

    def test_suffix_not_sorted(self) -> None:
        self._run(f'{START}\n2\n1\n{END}\nfoo\nbar\n')
        self.assert_errors()
        self.assertEqual(self.contents, f'{START}\n1\n2\n{END}\nfoo\nbar\n')

    def test_not_sorted_case_sensitive(self) -> None:
        self._run(f'{START}\na\nD\nB\nc\n{END}\n')
        self.assert_errors()
        self.assertEqual(self.contents, f'{START}\nB\nD\na\nc\n{END}\n')

    def test_not_sorted_case_insensitive(self) -> None:
        self._run(f'{START} ignore-case\na\nD\nB\nc\n{END}\n')
        self.assert_errors()
        self.assertEqual(
            self.contents, f'{START} ignore-case\na\nB\nc\nD\n{END}\n'
        )

    def test_remove_dupes(self) -> None:
        self._run(f'{START}\n1\n2\n2\n1\n{END}\n')
        self.assert_errors()
        self.assertEqual(self.contents, f'{START}\n1\n2\n{END}\n')

    def test_allow_dupes(self) -> None:
        self._run(f'{START} allow-dupes\n1\n2\n2\n1\n{END}\n')
        self.assert_errors()
        self.assertEqual(
            self.contents, f'{START} allow-dupes\n1\n1\n2\n2\n{END}\n'
        )

    def test_case_insensitive_dupes(self) -> None:
        self._run(f'{START} ignore-case\na\nB\nA\n{END}\n')
        self.assert_errors()
        self.assertEqual(
            self.contents, f'{START} ignore-case\nA\na\nB\n{END}\n'
        )

    def test_ignored_prefixes(self) -> None:
        self._run(f'{START} ignore-prefix=foo,bar\na\nb\nfoob\nbarc\n{END}\n')
        self.assert_no_errors()

    def test_ignored_longest_prefixes(self) -> None:
        self._run(f'{START} ignore-prefix=1,123\na\n123b\nb\n1c\n{END}\n')
        self.assert_no_errors()

    def test_ignored_prefixes_whitespace(self) -> None:
        self._run(
            f'{START} ignore-prefix=foo,bar\n' f' a\n b\n foob\n barc\n{END}\n'
        )
        self.assert_no_errors()

    def test_ignored_prefixes_insensitive(self) -> None:
        self._run(
            f'{START} ignore-prefix=foo,bar ignore-case\n'
            f'a\nB\nfooB\nbarc\n{END}\n'
        )
        self.assert_no_errors()

    def test_python_comment_marks_sorted(self) -> None:
        self._run(f'# {START}\n1\n2\n# {END}\n')
        self.assert_no_errors()

    def test_python_comment_marks_not_sorted(self) -> None:
        self._run(f'# {START}\n2\n1\n# {END}\n')
        self.assert_errors()
        self.assertEqual(self.contents, f'# {START}\n1\n2\n# {END}\n')

    def test_python_comment_sticky_sorted(self) -> None:
        self._run(f'# {START}\n# A\n1\n2\n# {END}\n')
        self.assert_no_errors()

    def test_python_comment_sticky_not_sorted(self) -> None:
        self._run(f'# {START}\n2\n# A\n1\n# {END}\n')
        self.assert_errors()
        self.assertEqual(self.contents, f'# {START}\n# A\n1\n2\n# {END}\n')

    def test_python_comment_sticky_disabled(self) -> None:
        self._run(f'# {START} sticky-comments=no\n1\n# B\n2\n# {END}\n')
        self.assert_errors()
        self.assertEqual(
            self.contents, f'# {START} sticky-comments=no\n# B\n1\n2\n# {END}\n'
        )

    def test_cpp_comment_marks_sorted(self) -> None:
        self._run(f'// {START}\n1\n2\n// {END}\n')
        self.assert_no_errors()

    def test_cpp_comment_marks_not_sorted(self) -> None:
        self._run(f'// {START}\n2\n1\n// {END}\n')
        self.assert_errors()
        self.assertEqual(self.contents, f'// {START}\n1\n2\n// {END}\n')

    def test_cpp_comment_sticky_sorted(self) -> None:
        self._run(f'// {START}\n1\n// B\n2\n// {END}\n')
        self.assert_no_errors()

    def test_cpp_comment_sticky_not_sorted(self) -> None:
        self._run(f'// {START}\n// B\n2\n1\n// {END}\n')
        self.assert_errors()
        self.assertEqual(self.contents, f'// {START}\n1\n// B\n2\n// {END}\n')

    def test_cpp_comment_sticky_disabled(self) -> None:
        self._run(f'// {START} sticky-comments=no\n1\n// B\n2\n// {END}\n')
        self.assert_errors()
        self.assertEqual(
            self.contents,
            f'// {START} sticky-comments=no\n// B\n1\n2\n// {END}\n',
        )

    def test_custom_comment_sticky_sorted(self) -> None:
        self._run(f'{START} sticky-comments=%\n1\n% B\n2\n{END}\n')
        self.assert_no_errors()

    def test_custom_comment_sticky_not_sorted(self) -> None:
        self._run(f'{START} sticky-comments=%\n% B\n2\n1\n{END}\n')
        self.assert_errors()
        self.assertEqual(
            self.contents, f'{START} sticky-comments=%\n1\n% B\n2\n{END}\n'
        )

    def test_multiline_comment_sticky_sorted(self) -> None:
        self._run(f'# {START}\n# B\n# A\n1\n2\n# {END}\n')
        self.assert_no_errors()

    def test_multiline_comment_sticky_not_sorted(self) -> None:
        self._run(f'# {START}\n# B\n# A\n2\n1\n# {END}\n')
        self.assert_errors()
        self.assertEqual(self.contents, f'# {START}\n1\n# B\n# A\n2\n# {END}\n')

    def test_comment_sticky_sorted_fallback_sorted(self) -> None:
        self._run(f'# {START}\n# A\n1\n# B\n1\n# {END}\n')
        self.assert_no_errors()

    def test_comment_sticky_sorted_fallback_not_sorted(self) -> None:
        self._run(f'# {START}\n# B\n1\n# A\n1\n# {END}\n')
        self.assert_errors()
        self.assertEqual(self.contents, f'# {START}\n# A\n1\n# B\n1\n# {END}\n')

    def test_comment_sticky_sorted_fallback_dupes(self) -> None:
        self._run(f'# {START} allow-dupes\n# A\n1\n# A\n1\n# {END}\n')
        self.assert_no_errors()

    def test_different_comment_sticky_not_sorted(self) -> None:
        self._run(f'# {START} sticky-comments=%\n% A\n1\n# B\n2\n# {END}\n')
        self.assert_errors()
        self.assertEqual(
            self.contents,
            f'# {START} sticky-comments=%\n# B\n% A\n1\n2\n# {END}\n',
        )

    def test_continuation_sorted(self) -> None:
        initial = textwrap.dedent(
            f"""
            # {START}
            baz
             abc
            foo
              bar
            # {END}
            """.lstrip(
                '\n'
            )
        )

        self._run(initial)
        self.assert_no_errors()

    def test_continuation_not_sorted(self) -> None:
        initial = textwrap.dedent(
            f"""
            # {START}
            foo
              bar
            baz
             abc
            # {END}
            """.lstrip(
                '\n'
            )
        )

        expected = textwrap.dedent(
            f"""
            # {START}
            baz
             abc
            foo
              bar
            # {END}
            """.lstrip(
                '\n'
            )
        )

        self._run(initial)
        self.assert_errors()
        self.assertEqual(self.contents, expected)

    def test_indented_continuation_sorted(self) -> None:
        # Intentionally not using textwrap.dedent().
        initial = f"""
        # {START}
        baz
         abc
        foo
          bar
        # {END}""".lstrip(
            '\n'
        )

        self._run(initial)
        self.assert_no_errors()

    def test_indented_continuation_not_sorted(self) -> None:
        # Intentionally not using textwrap.dedent().
        initial = f"""
        # {START}
        foo
          bar
        baz
         abc
        # {END}""".lstrip(
            '\n'
        )

        expected = f"""
        # {START}
        baz
         abc
        foo
          bar
        # {END}""".lstrip(
            '\n'
        )

        self._run(initial)
        self.assert_errors()
        self.assertEqual(self.contents, expected)


class TestKeepSortedStep(unittest.TestCase):
    """Test KeepSorted class as a presubmit step."""

    def test_fix(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            path = Path(tempdir) / 'foo'
            with path.open('w') as outs:
                outs.write(f'{START}\n2\n1\n{END}\n')

            ctx = MagicMock()
            ctx.paths = (path,)
            ctx.output_dir = Path(tempdir)
            ctx.fail = MagicMock()

            step = keep_sorted.KeepSorted()
            step.fix(ctx)

            with path.open() as ins:
                contents = ins.read()

            self.assertEqual(contents, f'{START}\n1\n2\n{END}\n')
            ctx.fail.assert_not_called()

    def test_fix_parsing_error(self) -> None:  # pylint: disable=no-self-use
        with tempfile.TemporaryDirectory() as tempdir:
            path = Path(tempdir) / 'foo'
            with path.open('w') as outs:
                outs.write(f'{START}\n{START}\n{END}\n')

            ctx = MagicMock()
            ctx.paths = (path,)
            ctx.output_dir = Path(tempdir)
            ctx.fail = MagicMock()

            step = keep_sorted.KeepSorted()
            step.fix(ctx)

            ctx.fail.assert_called_once()

    def test_run(self) -> None:  # pylint: disable=no-self-use
        with tempfile.TemporaryDirectory() as tempdir:
            path = Path(tempdir) / 'foo'
            with path.open('w') as outs:
                outs.write(f'{START}\n2\n1\n{END}\n')

            ctx = MagicMock()
            ctx.paths = (path,)
            ctx.output_dir = Path(tempdir)
            ctx.fail = MagicMock()

            step = keep_sorted.KeepSorted()
            step.run(ctx)

            ctx.fail.assert_called()

    def test_run_sorted(self) -> None:  # pylint: disable=no-self-use
        with tempfile.TemporaryDirectory() as tempdir:
            path = Path(tempdir) / 'foo'
            with path.open('w') as outs:
                outs.write(f'{START}\n1\n2\n{END}\n')

            ctx = MagicMock()
            ctx.paths = (path,)
            ctx.output_dir = Path(tempdir)
            ctx.fail = MagicMock()

            step = keep_sorted.KeepSorted()
            step.run(ctx)

            ctx.fail.assert_not_called()

    def test_keep_sorted_context_fail(self) -> None:
        ctx = keep_sorted.KeepSortedContext(paths=(), output_dir=Path('.'))
        ctx.fail("error")
        self.assertTrue(ctx.failed)


if __name__ == '__main__':
    unittest.main()
