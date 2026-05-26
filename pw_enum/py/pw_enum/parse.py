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
"""Parses C++ enum definitions from .h files using compile-time evaluation."""

from __future__ import annotations

from collections.abc import Iterator, Sequence
from dataclasses import dataclass
from typing import Generic, TypeVar
import ast
import codecs
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile


class ParseError(Exception):
    """Exception raised for errors in parsing .h files."""


@dataclass(frozen=True)
class EnumValue:
    """Represents a single, resolved enumerator."""

    cc_name: str
    value: int
    name: str


_Values = TypeVar('_Values')


@dataclass(frozen=True)
class _ParsedEnum(Generic[_Values]):
    name: str
    scopes: tuple[str, ...]
    cc_full_name: str
    line: int
    values: tuple[_Values, ...]


_ParsedEnums = list[_ParsedEnum[tuple[str, str]]]


@dataclass(frozen=True)
class EnumDescriptor(_ParsedEnum[EnumValue]):
    """Represents a parsed C++ enum definition with evaluated values."""

    @property
    def version(self) -> str:
        """Returns a unique version hash for the enum."""
        sorted_values = sorted(self.values, key=lambda v: (v.value, v.name))
        h = hashlib.sha256()
        h.update(self.cc_full_name.encode())
        for v in sorted_values:
            h.update(str(v.value).encode())
            h.update(v.name.encode())
        return "_pw_enum_" + h.hexdigest()[:12]

    @property
    def cc_versioned_name(self) -> str:
        """Returns the fully qualified versioned C++ name of the enum."""
        return "::" + "::".join(self.scopes + (self.version, self.name))


def camel_to_upper_snake(name: str) -> str:
    """Converts CamelCase to UPPER_SNAKE_CASE."""
    s1 = re.sub("(.)([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub("([a-z0-9])([A-Z])", r"\1_\2", s1).upper()


def _raise_error(msg: str, line: int, path: Path) -> None:
    """Raises ParseError with exact 1-based line number."""
    lines = path.read_text(encoding="utf-8").splitlines()
    line_text = lines[line - 1] if 1 <= line <= len(lines) else ""
    formatted_msg = (
        f"{path.name}:{line}: " f"error: {msg}\n{line:4d} | {line_text}"
    )
    raise ParseError(formatted_msg)


def _filter_flags(flags: Sequence[str], base_cc: str) -> Iterator[str]:
    """Filters compiler flags to prepare for running the preprocessor only."""
    iterator = iter(flags)
    for flag in iterator:
        if flag in ("-o", "-MF", "-MT", "-MQ"):
            try:
                next(iterator)
            except StopIteration:
                pass
            continue
        if flag in ("-c", "-S", "-g", "-MD", "-MMD", "-MP"):
            continue
        if flag == base_cc:
            continue
        yield flag


def _preprocess_header(
    path: Path,
    flags: Sequence[str],
    compiler: str,
    base_cc: str,
) -> str:
    cmd = [
        compiler,
        "-E",
        "-xc++",
        str(path),
        *_filter_flags(flags, base_cc),
        "-w",  # Disable warnings here; they will be surfaced during compilation
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if res.returncode != 0:
        raise ParseError(f"Preprocessing failed:\n{res.stderr}")
    return res.stdout


def _parse_type_name(
    enum_type_name: str,
    current_line: int,
    path: Path,
) -> tuple[str, tuple[str, ...], str]:
    """Parses scopes and name, returning (name, scopes, cc_full_name)."""
    cc_full_name = enum_type_name.strip()
    if not cc_full_name.startswith("::"):
        cc_full_name = "::" + cc_full_name

    type_parts = tuple(p.strip() for p in cc_full_name.split("::") if p.strip())
    if not type_parts:
        _raise_error("Invalid enum name in PW_ENUM", current_line, path)

    if len(type_parts) < 2:
        _raise_error(
            "PW_ENUM requires a fully qualified name with at least one "
            "namespace (e.g. my_ns::MyEnum)",
            current_line,
            path,
        )

    return type_parts[-1], type_parts[:-1], cc_full_name


def _default_display_name(cc_name: str) -> str:
    if cc_name.startswith("k") and len(cc_name) > 1 and cc_name[1].isupper():
        return camel_to_upper_snake(cc_name[1:])

    return cc_name


_ENUM_MARKER = re.compile(
    r'_PW_ENUM_MUST_BE_COMPILED_BY_pw_cc_enum\s*(\(.*?\))\s+_PW_ENUM_MACRO_END',
    re.DOTALL,
)
_LINE_MARKER = re.compile(r'^#\s+(\d+)\s+"([^"]+)"[^\n]*\n', re.MULTILINE)
_IDENTIFIER = re.compile(r'^[a-zA-Z_][a-zA-Z0-9_]*$')


def _parse_enumerators(
    args: Sequence[str],
    line: int,
    path: Path,
) -> Iterator[tuple[str, str]]:
    """Parses enumerators, yielding (cc_name, display_name) tuples."""
    for val in args:
        cc_name = val.strip()

        if '=' in cc_name:
            cc_name, rhs = (p.strip() for p in cc_name.split('=', 1))
            if not (rhs.startswith('"') and rhs.endswith('"')):
                _raise_error(
                    "Custom name for "
                    f"{cc_name} must be a double-quoted string literal: {rhs}",
                    line,
                    path,
                )
            display_name = rhs[1:-1]
            if not display_name:
                _raise_error(
                    f"Custom name for {cc_name} cannot be empty",
                    line,
                    path,
                )
            # Handle escapes within the string literal and check if it's UTF-8.
            try:
                encoded_bytes = display_name.encode("utf-8")
                raw_bytes = codecs.escape_decode(encoded_bytes)[0]
                assert isinstance(raw_bytes, bytes)
                display_name = raw_bytes.decode("utf-8")
            except (UnicodeDecodeError, ValueError) as e:
                _raise_error(
                    f"Custom name for {cc_name} is not valid UTF-8: {e}",
                    line,
                    path,
                )
        else:
            display_name = _default_display_name(cc_name)

        if not _IDENTIFIER.match(cc_name):
            _raise_error(f"Invalid enumerator name: {cc_name}", line, path)

        yield cc_name, display_name


def _find_line_and_file_for_offset(
    source: str,
    offset: int,
) -> tuple[int, Path]:
    """Maps a preprocessed text offset back to the original file and line.

    Parse preprocessor line markers (e.g. `# 123 "file.h"`) to map to the
    original source header path and line. This ensures we only parse macros
    belonging to the current file and ensures errors use accurate line numbers.
    """
    current_line = 0
    current_file = Path()

    text_before = source[:offset]

    last_marker_end = 0
    for m in _LINE_MARKER.finditer(text_before):
        current_line = int(m.group(1))
        current_file = Path(m.group(2))
        last_marker_end = m.end()

    newlines_since_marker = text_before[last_marker_end:].count("\n")
    return current_line + newlines_since_marker, current_file


def _parse_macro_args(args_str: str, line: int, path: Path) -> tuple[str, ...]:
    try:
        val = ast.literal_eval(args_str)
    except SyntaxError as e:
        _raise_error(
            f"Failed to parse macro argument string: {args_str}. Error: {e}",
            line,
            path,
        )
    if not isinstance(val, tuple):
        _raise_error(
            f"Internal PW_ENUM parsing error: expected tuple, got {type(val)}",
            line,
            path,
        )
    return val


def _parse_preprocessed_header(source: str, path: Path) -> _ParsedEnums:
    """Parses the preprocessed output to identify enum macros and lines."""
    enums: _ParsedEnums = []

    for match in _ENUM_MARKER.finditer(source):
        line, file = _find_line_and_file_for_offset(source, match.start())

        if file.resolve() != path.resolve():
            continue

        args = _parse_macro_args(match.group(1).strip(), line, file)

        if not args or not args[0].strip():
            _raise_error("PW_ENUM requires at least an enum name", line, file)

        enum_name, scopes, cc_full_name = _parse_type_name(args[0], line, path)
        enums.append(
            _ParsedEnum(
                name=enum_name,
                scopes=scopes,
                cc_full_name=cc_full_name,
                line=line,
                values=tuple(_parse_enumerators(args[1:], line, path)),
            )
        )

    if not enums:
        raise ParseError(
            f"{path}: pw_cc_enum headers must contain at least one call "
            "to PW_ENUM(); move headers without PW_ENUM() to regular libraries"
        )

    return enums


def _parse_evaluation_output(
    stderr: str,
    enumerator_info: Sequence[tuple[str, str]],
) -> Iterator[EnumValue]:
    """Parses compilation stderr to extract evaluated enum values."""
    eval_values: dict[int, int] = {}
    extractor = re.compile(
        r"PwEnumValueExtractor<\s*(\d+)\s*,\s*([-\dULul]+)\s*>"
    )
    for m in extractor.finditer(stderr):
        t_idx = int(m.group(1))
        eval_values[t_idx] = int(m.group(2).rstrip("ULul"))

    for idx, (cc_name, display_name) in enumerate(enumerator_info):
        if idx not in eval_values:
            raise ParseError(
                f"Failed to evaluate value for {cc_name}!\n{stderr}"
            )
        yield EnumValue(
            cc_name=cc_name,
            value=eval_values[idx],
            name=display_name,
        )


def _generate_extractor_file(path: Path, enums: _ParsedEnums) -> Iterator[str]:
    yield "#pragma GCC system_header"
    yield f'#include "{path.name}"'
    yield "template<int Index, auto V> struct PwEnumValueExtractor;"

    idx = 0
    for enum in enums:
        for cc_name, _ in enum.values:
            yield (
                f"PwEnumValueExtractor<{idx}, +static_cast<"
                f"__underlying_type({enum.cc_full_name})>"
                f"({enum.cc_full_name}::{cc_name})> e_{idx};"
            )
            idx += 1


def _compile_and_evaluate(
    path: Path,
    original_flags: Sequence[str],
    compiler: str,
    enums: _ParsedEnums,
    base_cc: str,
) -> Iterator[EnumValue]:
    """Compiles a generated extractor source file to evaluate enum values."""
    flags = list(original_flags)
    if "clang" in compiler and "-ferror-limit=0" not in flags:
        flags.append("-ferror-limit=0")

    with tempfile.TemporaryDirectory() as td:
        cc_file = Path(td, "extract.cc")
        with cc_file.open("w", encoding="utf-8") as f:
            for line in _generate_extractor_file(path, enums):
                f.write(line)
                f.write("\n")

        cmd = [compiler, "-D_PW_ENUM_GENERATING"]
        cmd.extend([str(cc_file) if f == base_cc else f for f in flags])
        cmd.extend(["-iquote", str(path.parent)])
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)

    flattened_enumerators = [val for enum in enums for val in enum.values]
    return _parse_evaluation_output(proc.stderr, flattened_enumerators)


def parse_enums(
    path: Path,
    compiler: str,
    flags: Sequence[str],
    base_cc: Path,
) -> Iterator[EnumDescriptor]:
    """Parses enums from C++ header file, yielding evaluated EnumDescriptors."""
    base_cc_str = str(base_cc)
    preprocessed = _preprocess_header(path, flags, compiler, base_cc_str)
    parsed_enums = _parse_preprocessed_header(preprocessed, path)
    all_values = _compile_and_evaluate(
        path, flags, compiler, parsed_enums, base_cc_str
    )

    # Read through the flat, ordered list of values and group into their enums.
    for enum in parsed_enums:
        try:
            values = tuple(next(all_values) for _ in enum.values)
        except StopIteration as exc:
            raise ParseError(
                "Internal error: extracted fewer enum values than parsed"
            ) from exc
        yield EnumDescriptor(
            name=enum.name,
            scopes=enum.scopes,
            cc_full_name=enum.cc_full_name,
            line=enum.line,
            values=values,
        )
