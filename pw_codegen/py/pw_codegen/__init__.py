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
"""Utilities for generating C++, Java, Python, Rust, and TypeScript code."""

from __future__ import annotations

from collections.abc import Mapping
import re

from pw_codegen.output_file import OutputFile

__all__ = [
    "OutputFile",
    "camel_to_snake",
    "camel_to_upper_snake",
    "snake_to_pascal",
]


def camel_to_snake(name: str) -> str:
    """Converts ``CamelCase`` or ``camelCase`` to ``snake_case``."""
    s1 = re.sub(r"([A-Za-z0-9])([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s1).lower()


def camel_to_upper_snake(name: str) -> str:
    """Converts ``CamelCase`` or ``camelCase`` to ``UPPER_SNAKE_CASE``."""
    return camel_to_snake(name).upper()


def snake_to_pascal(name: str) -> str:
    """Converts ``snake_case`` or ``UPPER_SNAKE_CASE`` to ``PascalCase``."""
    return "".join(part.capitalize() for part in name.split("_"))


def _escape_string_literal(
    data: str, escapes: Mapping[str, str], control_escape: str
) -> str:
    parts = ['"']
    for char in data:
        if char in escapes:
            parts.append(escapes[char])
        elif ord(char) < 0x20 or ord(char) == 0x7F:
            parts.append(control_escape.format(ord(char)))
        else:
            parts.append(char)
    parts.append('"')
    return "".join(parts)
