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
"""Python source code generation utilities."""

from __future__ import annotations

from collections.abc import Iterator
import keyword


def is_identifier(name: str) -> bool:
    """Returns whether ``name`` is a valid, non-keyword Python identifier."""
    return name.isidentifier() and not keyword.iskeyword(name)


def string_literal(text: str) -> str:
    """Returns a Python string literal, preferring double quotes."""
    literal = repr(text)
    # repr() uses single quotes unless the text contains them. Prefer double
    # quotes when the text contains neither kind of quote.
    if literal.startswith("'") and '"' not in text and "'" not in text:
        return '"' + literal[1:-1] + '"'
    return literal


def docstring(text: str, indent: str = "") -> Iterator[str]:
    """Yields an indented Python docstring for ``text``.

    Yields nothing when ``text`` is empty. Normalizes ``\\r\\n`` line endings
    and strips trailing newlines on non-blank text before formatting with
    triple double-quotes, falling back to :py:func:`string_literal` when
    ``text`` contains triple quotes, backslashes, leading/trailing quotes or
    horizontal whitespace, or non-printable characters.
    """
    if not text:
        return
    normalized = text.replace("\r\n", "\n")
    if normalized.strip():
        normalized = normalized.rstrip("\n")
    lines = normalized.split("\n")
    if (
        any(s in normalized for s in ('"""', "\\"))
        or normalized.startswith('"')
        or normalized.endswith('"')
        or normalized != normalized.strip()
        or not all(line.isprintable() for line in lines)
    ):
        yield f"{indent}{string_literal(text)}"
        return
    if len(lines) == 1:
        yield f'{indent}"""{lines[0]}"""'
        return
    yield f'{indent}"""{lines[0]}'
    for line in lines[1:]:
        yield f"{indent}{line}".rstrip()
    yield f'{indent}"""'
