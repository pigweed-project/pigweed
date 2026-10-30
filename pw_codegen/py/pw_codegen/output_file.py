# Copyright 2020 The Pigweed Authors
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
"""Indentation-aware buffer for generating source files."""

from __future__ import annotations

from collections.abc import Iterator
import contextlib
from pathlib import PurePath


class OutputFile:
    """A buffer to which generated source code is written.

    Example:

    .. code-block:: python

       output = OutputFile("hello.c")
       output.write_line("int main(void) {")
       with output.indent():
           output.write_line('printf("Hello, world");')
           output.write_line("return 0;")
       output.write_line("}")

       print(output.content())

    Produces:

    .. code-block:: c

       int main(void) {
         printf("Hello, world");
         return 0;
       }
    """

    INDENT_WIDTH = 2

    def __init__(
        self,
        filename: str | PurePath,
        indent_width: int = INDENT_WIDTH,
    ) -> None:
        self._filename: str = (
            filename.as_posix() if isinstance(filename, PurePath) else filename
        )
        self._indent_width: int = indent_width
        self._content: list[str] = []
        self._indentation: int = 0

    def write_line(self, line: str = "") -> None:
        """Appends a line to the buffer with the current indentation."""
        if line:
            self._content.append(" " * self._indentation)
            self._content.append(line)
        self._content.append("\n")

    @contextlib.contextmanager
    def indent(self, amount: int | None = None) -> Iterator[None]:
        """Increases the indentation level within the context."""
        step = self._indent_width if amount is None else amount
        self._indentation += step
        try:
            yield
        finally:
            self._indentation -= step

    def name(self) -> str:
        """Returns the output filename."""
        return self._filename

    def content(self) -> str:
        """Returns the accumulated file content."""
        return "".join(self._content)
