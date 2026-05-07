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
"""Tests for the formatting step."""

from pathlib import Path
import unittest
from tempfile import TemporaryDirectory

from pw_cli.file_filter import FileFilter
from pw_presubmit.format.core import (
    FileFormatter,
    FormattedFileContents,
    FormatFixStatus,
)
from pw_presubmit.format.step import CodeFormatting
from pw_presubmit.v2 import Context


class FakeFileFormatter(FileFormatter):
    """A fake file formatter for testing."""

    FORMAT_MAP = {
        b'foo': b'bar',
        b'bar': b'bar',
    }

    def __init__(self):
        super().__init__(mnemonic='Fake', file_patterns=FileFilter(name=['.*']))

    def format_file_in_memory(
        self, file_path: Path, file_contents: bytes
    ) -> FormattedFileContents:
        error = ''
        formatted = self.FORMAT_MAP.get(file_contents, None)
        if formatted is None:
            error = f'I do not know how to "{file_contents.decode()}".'
        return FormattedFileContents(
            ok=not error,
            formatted_file_contents=formatted if formatted is not None else b'',
            error_message=error,
        )

    def format_file(self, file_path: Path) -> FormatFixStatus:
        orig = file_path.read_bytes()
        formatted = self.FORMAT_MAP.get(orig, None)
        if formatted is None:
            return FormatFixStatus(
                ok=False,
                error_message=f'I do not know how to "{orig.decode()}".',
            )
        file_path.write_bytes(formatted)
        return FormatFixStatus(ok=True, error_message=None)


class TestCodeFormattingStep(unittest.TestCase):
    """Tests for CodeFormatting step."""

    def test_run_passing(self):
        """Test running the formatting step when it passes."""
        with TemporaryDirectory() as tmp:
            p = Path(tmp) / 'a.txt'
            p.write_bytes(b'bar')

            step = CodeFormatting(FakeFileFormatter())
            ctx = Context(
                root=Path(tmp),
                output_dir=Path(tmp),
                paths=(p,),
                all_paths=(p,),
            )

            step.run(ctx)
            self.assertFalse(ctx.failed)

    def test_run_failing(self):
        """Test running the formatting step when it fails."""
        with TemporaryDirectory() as tmp:
            p = Path(tmp) / 'a.txt'
            p.write_bytes(b'foo')

            step = CodeFormatting(FakeFileFormatter())
            ctx = Context(
                root=Path(tmp),
                output_dir=Path(tmp),
                paths=(p,),
                all_paths=(p,),
            )

            step.run(ctx)
            self.assertTrue(ctx.failed)

    def test_fix_succeeds(self):
        """Test fixing the formatting step when it succeeds."""
        with TemporaryDirectory() as tmp:
            p = Path(tmp) / 'a.txt'
            p.write_bytes(b'foo')

            step = CodeFormatting(FakeFileFormatter())
            ctx = Context(
                root=Path(tmp),
                output_dir=Path(tmp),
                paths=(p,),
                all_paths=(p,),
            )

            step.fix(ctx)
            self.assertFalse(ctx.failed)
            self.assertEqual(p.read_bytes(), b'bar')


if __name__ == '__main__':
    unittest.main()
