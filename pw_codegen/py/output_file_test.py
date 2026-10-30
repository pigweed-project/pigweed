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
"""Tests for pw_codegen.output_file."""

from pathlib import Path, PureWindowsPath
import unittest

from pw_codegen.output_file import OutputFile


class OutputFileTest(unittest.TestCase):
    """Tests indentation-aware source buffering."""

    def test_write_and_indent(self) -> None:
        output = OutputFile("hello.c")
        self.assertEqual(output.name(), "hello.c")
        output.write_line("int main(void) {")
        with output.indent():
            output.write_line('printf("Hello, world");')
            output.write_line()
            with output.indent(4):
                output.write_line("return 0;")
        output.write_line("}")

        self.assertEqual(
            output.content(),
            "int main(void) {\n"
            '  printf("Hello, world");\n'
            "\n"
            "      return 0;\n"
            "}\n",
        )

    def test_indent_restored_on_exception(self) -> None:
        output = OutputFile("err.cc")
        with self.assertRaises(RuntimeError):
            with output.indent():
                raise RuntimeError("boom")
        output.write_line("top_level();")
        self.assertEqual(output.content(), "top_level();\n")

    def test_path_and_custom_indent_width(self) -> None:
        output = OutputFile(Path("dir", "file.py"), indent_width=4)
        self.assertEqual(output.name(), "dir/file.py")
        self.assertEqual(
            OutputFile(PureWindowsPath("dir", "file.py")).name(),
            "dir/file.py",
        )
        output.write_line("def fn():")
        with output.indent():
            output.write_line("return 1")
        self.assertEqual(output.content(), "def fn():\n    return 1\n")


if __name__ == "__main__":
    unittest.main()
