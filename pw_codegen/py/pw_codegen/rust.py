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
"""Rust source code generation utilities."""

from __future__ import annotations

import math
import struct
from typing import NamedTuple

# Rust strict and reserved keywords across all editions
# (https://doc.rust-lang.org/reference/keywords.html):
KEYWORDS: frozenset[str] = frozenset(
    {
        "Self",
        "abstract",
        "as",
        "async",
        "await",
        "become",
        "box",
        "break",
        "const",
        "continue",
        "crate",
        "do",
        "dyn",
        "else",
        "enum",
        "extern",
        "false",
        "final",
        "fn",
        "for",
        "gen",
        "if",
        "impl",
        "in",
        "let",
        "loop",
        "macro",
        "match",
        "mod",
        "move",
        "mut",
        "override",
        "priv",
        "pub",
        "ref",
        "return",
        "self",
        "static",
        "struct",
        "super",
        "trait",
        "true",
        "try",
        "type",
        "typeof",
        "unsafe",
        "unsized",
        "use",
        "virtual",
        "where",
        "while",
        "yield",
    }
)


def is_identifier(name: str) -> bool:
    """Returns whether ``name`` is a valid, non-keyword Rust identifier."""
    return name.isidentifier() and name != "_" and name not in KEYWORDS


class IntType(NamedTuple):
    """Metadata for a fixed-width Rust integer type."""

    rust_type: str
    min_value: int
    max_value: int


def _int_type(bits: int, signed: bool) -> IntType:
    if signed:
        return IntType(f"i{bits}", -(2 ** (bits - 1)), 2 ** (bits - 1) - 1)
    return IntType(f"u{bits}", 0, 2**bits - 1)


_INT_TYPE_LIST: tuple[IntType, ...] = tuple(
    _int_type(bits, prefix == "i")
    for prefix in ("i", "u")
    for bits in (8, 16, 32, 64, 128)
)

# Fixed-width integer types keyed by canonical name (e.g. "i32", "u64").
INT_TYPES: dict[str, IntType] = {
    info.rust_type: info for info in _INT_TYPE_LIST
}

_ASCII_CTRL_LIMIT = 0x20
_ASCII_DEL = 0x7F
_C1_CTRL_MIN = 0x80
_C1_CTRL_MAX = 0x9F
_SURROGATE_MIN = 0xD800
_SURROGATE_MAX = 0xDFFF

_CHAR_ESCAPES: dict[str, str] = {
    "\\": r"\\",
    "\n": r"\n",
    "\r": r"\r",
    "\t": r"\t",
    "\0": r"\0",
}


def bool_literal(value: bool) -> str:
    """Formats ``value`` as a Rust boolean literal (``true`` or ``false``)."""
    return "true" if value else "false"


def int_literal(value: int, int_type: str) -> str:
    """Formats ``value`` as a typed Rust integer literal.

    Args:
        value: Integer value to format.
        int_type: Fixed-width Rust integer type suffix from
            :py:data:`INT_TYPES` (for example ``"i32"`` or ``"u64"``).

    Raises:
        KeyError: ``int_type`` is not in :py:data:`INT_TYPES`.
        ValueError: ``value`` is out of range for ``int_type``.
    """
    bounds = INT_TYPES[int_type]
    if not bounds.min_value <= value <= bounds.max_value:
        raise ValueError(
            f"Value {value!r} is out of range for {int_type} "
            f"[{bounds.min_value}, {bounds.max_value}]"
        )
    return f"{value}{int_type}"


def i8_literal(value: int) -> str:
    """Formats ``value`` as a Rust ``i8`` literal."""
    return int_literal(value, "i8")


def i16_literal(value: int) -> str:
    """Formats ``value`` as a Rust ``i16`` literal."""
    return int_literal(value, "i16")


def i32_literal(value: int) -> str:
    """Formats ``value`` as a Rust ``i32`` literal."""
    return int_literal(value, "i32")


def i64_literal(value: int) -> str:
    """Formats ``value`` as a Rust ``i64`` literal."""
    return int_literal(value, "i64")


def i128_literal(value: int) -> str:
    """Formats ``value`` as a Rust ``i128`` literal."""
    return int_literal(value, "i128")


def u8_literal(value: int) -> str:
    """Formats ``value`` as a Rust ``u8`` literal."""
    return int_literal(value, "u8")


def u16_literal(value: int) -> str:
    """Formats ``value`` as a Rust ``u16`` literal."""
    return int_literal(value, "u16")


def u32_literal(value: int) -> str:
    """Formats ``value`` as a Rust ``u32`` literal."""
    return int_literal(value, "u32")


def u64_literal(value: int) -> str:
    """Formats ``value`` as a Rust ``u64`` literal."""
    return int_literal(value, "u64")


def u128_literal(value: int) -> str:
    """Formats ``value`` as a Rust ``u128`` literal."""
    return int_literal(value, "u128")


def _float_literal(value: int | float, float_type: str) -> str:
    try:
        fval = float(value)
    except OverflowError as exc:
        raise ValueError(f"Value {value!r} overflows {float_type}") from exc
    if math.isnan(fval):
        return f"{float_type}::NAN"
    if math.isinf(fval):
        if fval > 0:
            return f"{float_type}::INFINITY"
        return f"{float_type}::NEG_INFINITY"
    if float_type == "f32":
        try:
            struct.pack("<f", fval)
        except OverflowError as exc:
            raise ValueError(
                f"Value {value!r} is out of range for {float_type}"
            ) from exc
    return f"{repr(fval)}{float_type}"


def f32_literal(value: int | float) -> str:
    """Formats ``value`` as a Rust ``f32`` literal or constant.

    Args:
        value: Integer or floating-point value to format.

    Raises:
        ValueError: ``value`` overflows ``f32``.
    """
    return _float_literal(value, "f32")


def f64_literal(value: int | float) -> str:
    """Formats ``value`` as a Rust ``f64`` literal or constant.

    Args:
        value: Integer or floating-point value to format.

    Raises:
        ValueError: ``value`` overflows ``f64``.
    """
    return _float_literal(value, "f64")


def _escape_char(ch: str, quote: str) -> str:
    if ch == quote:
        return "\\" + quote
    if ch in _CHAR_ESCAPES:
        return _CHAR_ESCAPES[ch]
    code = ord(ch)
    if _SURROGATE_MIN <= code <= _SURROGATE_MAX:
        raise ValueError(
            f"UTF-16 surrogate U+{code:04X} is not a valid Rust scalar value"
        )
    if code < _ASCII_CTRL_LIMIT or code == _ASCII_DEL:
        return rf"\x{code:02x}"
    if _C1_CTRL_MIN <= code <= _C1_CTRL_MAX:
        return rf"\u{{{code:x}}}"
    return ch


def string_literal(data: str) -> str:
    """Formats ``data`` as a quoted Rust string literal.

    Raises:
        ValueError: ``data`` contains a UTF-16 surrogate.
    """
    escaped = "".join(_escape_char(ch, '"') for ch in data)
    return f'"{escaped}"'


def char_literal(data: str) -> str:
    """Formats a single Unicode scalar value as a Rust ``char`` literal.

    Raises:
        ValueError: ``data`` does not have length 1 or is a UTF-16 surrogate.
    """
    if len(data) != 1:
        raise ValueError(
            f"Rust char literal must be 1 scalar value, got {data!r}"
        )
    return f"""'{_escape_char(data[0], "'")}'"""


def byte_slice(data: bytes | bytearray | memoryview) -> str:
    """Formats ``data`` as a Rust byte slice literal (e.g. ``&[1, 2, 3]``)."""
    return f"&[{', '.join(str(b) for b in data)}]"
