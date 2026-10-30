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
"""Tests for pw_codegen.python."""

import ast
import inspect
import unittest

from pw_codegen import python


class PythonCodegenTest(unittest.TestCase):
    """Tests Python identifier checks, string literals, and docstrings."""

    def test_is_identifier(self) -> None:
        self.assertTrue(python.is_identifier("my_module"))
        self.assertTrue(python.is_identifier("MyEnum"))
        self.assertFalse(python.is_identifier(""))
        self.assertFalse(python.is_identifier("bad-name"))
        self.assertFalse(python.is_identifier("123"))
        self.assertFalse(python.is_identifier("None"))
        self.assertFalse(python.is_identifier("def"))

    def test_string_literal(self) -> None:
        self.assertEqual(python.string_literal("hello"), '"hello"')
        self.assertEqual(python.string_literal(""), '""')
        self.assertEqual(
            python.string_literal('Has "quotes"'), '\'Has "quotes"\''
        )
        self.assertEqual(python.string_literal("it's"), '"it\'s"')
        for text in ("plain", 'both \' and "', "line1\nline2", "back\\slash"):
            with self.subTest(text=text):
                self.assertEqual(
                    ast.literal_eval(python.string_literal(text)), text
                )

    def test_docstring(self) -> None:
        self.assertEqual(list(python.docstring("")), [])
        self.assertEqual(
            list(python.docstring("The first value.", "    ")),
            ['    """The first value."""'],
        )
        self.assertEqual(
            list(python.docstring("My enum.\n\nWith multiple lines.", "    ")),
            [
                '    """My enum.',
                "",
                "    With multiple lines.",
                '    """',
            ],
        )
        self.assertEqual(
            list(
                python.docstring(
                    "My enum.\r\n\r\nWith trailing newline.\r\n", "    "
                )
            ),
            [
                '    """My enum.',
                "",
                "    With trailing newline.",
                '    """',
            ],
        )

    def test_docstring_edge_cases(self) -> None:
        cases = (
            '"starts with quote"',
            'Has "quotes"',
            "\n",
            "a\rb",
            'triple """ quotes',
            "has \\ backslash",
            " leading space",
            "trailing space ",
        )
        for doc in cases:
            with self.subTest(doc=doc):
                lines = list(python.docstring(doc, "    "))
                source = "class C:\n" + "\n".join(lines) + "\n"
                cls_node = ast.parse(source).body[0]
                assert isinstance(cls_node, ast.ClassDef)
                self.assertEqual(ast.get_docstring(cls_node, clean=False), doc)
                namespace: dict[str, type] = {}
                exec(  # pylint: disable=exec-used
                    compile(source, "doc.py", "exec"), namespace
                )
                self.assertEqual(
                    inspect.getdoc(namespace["C"]), inspect.cleandoc(doc)
                )


if __name__ == "__main__":
    unittest.main()
