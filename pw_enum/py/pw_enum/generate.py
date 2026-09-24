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
"""Generates shadowed headers with pw_enum footer."""

import argparse
from collections import defaultdict
from collections.abc import Iterator, Sequence
from pathlib import Path
import re
import sys
from typing import Any

try:
    from pw_enum.parse import (
        EnumDescriptor,
        EnumValue,
        parse_enums,
        ParseError,
        camel_to_upper_snake,
    )
except ImportError:  # Support running without installing this Python package.
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from pw_enum.parse import (
        EnumDescriptor,
        EnumValue,
        parse_enums,
        ParseError,
        camel_to_upper_snake,
    )


def _generate_macro_defs(enum_desc: EnumDescriptor) -> Iterator[str]:
    macro_prefix = camel_to_upper_snake("_".join(enum_desc.scopes))
    if macro_prefix:
        macro_prefix += "_"
    macro = macro_prefix + camel_to_upper_snake(enum_desc.name)
    yield f'#define {macro}_DOMAIN "{enum_desc.cc_versioned_name}"'
    yield (
        f'#define {macro}_FMT '
        f'PW_LOG_TOKEN_FMT("{enum_desc.cc_versioned_name}")'
    )


def _group_by_value(enum_desc: EnumDescriptor) -> list[list[EnumValue]]:
    """Groups enumerators by value, in ascending order of value.

    Enumerators that share a value (aliases) end up in the same group, sorted
    by name. Each group is therefore one distinct value of the enum.
    """
    grouped: dict[int, list[EnumValue]] = defaultdict(list)
    for v in sorted(enum_desc.values, key=lambda val: (val.value, val.name)):
        grouped[v.value].append(v)
    return list(grouped.values())


_ESCAPE_MAP = {
    ord("\\"): b"\\\\",
    ord('"'): b"\\\"",
    ord("\n"): b"\\n",
    ord("\r"): b"\\r",
    ord("\t"): b"\\t",
}


def _escape_cc(s: str) -> str:
    """Escapes quotes, backslashes, and control characters for C++ strings."""
    escaped = bytearray()
    for b in s.encode("utf-8"):
        if b in _ESCAPE_MAP:
            escaped.extend(_ESCAPE_MAP[b])
        elif b < 32 or b >= 127:
            escaped.extend(f"\\x{b:02x}".encode("ascii"))
        else:
            escaped.append(b)
    return escaped.decode("ascii")


def _is_contiguous(distinct_values: list[EnumValue]) -> bool:
    """Returns whether the values cover every integer from min to max."""
    span = distinct_values[-1].value - distinct_values[0].value + 1
    return span == len(distinct_values)


def _generate_is_valid(
    enum_desc: EnumDescriptor,
    distinct_values: list[EnumValue],
    is_contiguous: bool,
) -> Iterator[str]:
    """Generates the IsValid overloads for an EnumTraits specialization.

    `is_contiguous` must be the value emitted for `kIsContiguous`. The integer
    overload relies on this: `pw::internal::IsValidInteger` skips the
    per-enumerator check and accepts anything within [kMin, kMax] when
    `kIsContiguous` is true.
    """
    yield "  [[nodiscard]] static constexpr bool IsValid(enum_type value) {"

    if len(distinct_values) == 1:
        yield "    return value == kMin;"
    elif is_contiguous:
        yield "    return value >= kMin && value <= kMax;"
    else:
        # Let the compiler pick the best test for the value set: it lowers a
        # switch to a range check, bit test, or jump table as appropriate.
        yield "    switch (value) {"
        for v in distinct_values:
            yield f"      case {enum_desc.cc_full_name}::{v.cc_name}:"
        yield "        return true;"
        yield "      default:"
        yield "        return false;"
        yield "    }"

    yield "  }"
    yield ""
    yield "  template <typename Integer,"
    yield (
        "            typename = "
        "std::enable_if_t<internal::kIsInteger<Integer>>>"
    )
    yield "  [[nodiscard]] static constexpr bool IsValid(Integer value) {"
    yield "    return internal::IsValidInteger<enum_type>(value);"
    yield "  }"


def _generate_enum_traits(
    enum_desc: EnumDescriptor,
    distinct_values: list[EnumValue],
) -> Iterator[str]:
    """Generates a pw::EnumTraits specialization for one enum."""
    cc_name = enum_desc.cc_full_name
    is_contiguous = _is_contiguous(distinct_values)

    yield "namespace pw {"
    yield "template <>"
    yield f"struct EnumTraits<{cc_name}> {{"
    # Marks this specialization as generated. pw::has_enum_traits_v detects
    # this field, so hand-written specializations are not mistaken for
    # generated ones.
    yield "  static constexpr bool _pw_enum_internal_tag_do_not_use = true;"
    yield ""
    yield f"  using enum_type = {cc_name};"
    yield "  using underlying_type = std::underlying_type_t<enum_type>;"
    yield ""
    yield f'  static constexpr std::string_view kName = "{enum_desc.name}";'
    yield (
        "  static constexpr std::string_view kFullyQualifiedName = "
        f'"{cc_name}";'
    )
    yield (
        "  static constexpr std::string_view kTokenDomain = "
        f'"{enum_desc.cc_versioned_name}";'
    )
    yield ""
    yield (
        "  static constexpr size_t kDistinctValueCount = "
        f"{len(distinct_values)};"
    )
    yield (
        "  static constexpr bool kIsContiguous = "
        f"{str(is_contiguous).lower()};"
    )
    yield ""
    yield (
        "  static constexpr enum_type kMin = "
        f"{cc_name}::{distinct_values[0].cc_name};"
    )
    yield (
        "  static constexpr enum_type kMax = "
        f"{cc_name}::{distinct_values[-1].cc_name};"
    )
    yield ""
    yield (
        "  static constexpr std::array<enum_type, kDistinctValueCount> "
        "kValues = {{"
    )
    for v in distinct_values:
        yield f"      {cc_name}::{v.cc_name},"
    yield "  }};"
    yield ""
    yield from _generate_is_valid(enum_desc, distinct_values, is_contiguous)
    yield "};"
    yield "}  // namespace pw"


def _generate_tokenization(
    enum_desc: EnumDescriptor,
    value_groups: list[list[EnumValue]],
) -> Iterator[str]:
    """Generates the pw_tokenizer registration for one enum."""
    yield f"_PW_TOKENIZE_ENUM_DOMAIN({enum_desc.cc_full_name},"
    yield f'                         "{enum_desc.cc_versioned_name}",'
    for i, group in enumerate(value_groups):
        label = group[0].cc_name
        # Aliases share a token; their names are joined with "|".
        name = "|".join(dict.fromkeys(v.name for v in group))
        comma = "" if i == len(value_groups) - 1 else ","
        yield f'                         ({label}, "{_escape_cc(name)}"){comma}'
    yield ");"


def _generate_single_enum_footer(enum_desc: EnumDescriptor) -> Iterator[str]:
    value_groups = _group_by_value(enum_desc)
    distinct_values = [group[0] for group in value_groups]

    yield from _generate_macro_defs(enum_desc)
    yield ""
    yield from _generate_enum_traits(enum_desc, distinct_values)
    yield ""
    yield from _generate_tokenization(enum_desc, value_groups)


def generate_footer(enums: list[EnumDescriptor]) -> Iterator[str]:
    """Generates the C++ footer with enum traits and tokenization macros."""
    enums_with_values = [enum_desc for enum_desc in enums if enum_desc.values]
    if not enums_with_values:
        return

    yield '#include "pw_tokenizer/enum.h"'

    for enum_desc in enums_with_values:
        yield ""
        yield from _generate_single_enum_footer(enum_desc)


_PW_ENUM_MACRO = re.compile(r'\bPW_ENUM(?=\s*\()')


def _write_generated_header(
    input_path: Path,
    output_path: Path,
    enums: list[EnumDescriptor],
) -> None:
    """Writes the final generated header with replaced macro and the footer."""
    enum_lines = frozenset(enum_desc.line for enum_desc in enums)

    with (
        input_path.open(encoding="utf-8") as infile,
        output_path.open("w", encoding="utf-8", newline="\n") as outfile,
    ):
        for line_num, line in enumerate(infile, 1):
            if line_num in enum_lines:
                line, count = _PW_ENUM_MACRO.subn("_PW_ENUM_GENERATED", line)
                assert count > 0, f"Expected PW_ENUM() on line {line_num}"
            outfile.write(line)

        outfile.write("\n\n// --- pw_enum generated footer ---\n\n")

        for line in generate_footer(enums):
            outfile.write(line)
            outfile.write("\n")


def _generate_header(
    input_path: Path,
    output_path: Path,
    flags: Sequence[str],
    compiler: str,
    base_cc: Path,
) -> None:
    """Generates a shadowed version of the header with a pw_enum footer."""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    descriptors = list(parse_enums(input_path, compiler, flags, base_cc))
    _write_generated_header(input_path, output_path, descriptors)


def main(
    inputs: Sequence[Path],
    outputs: Sequence[Path],
    compiler: str,
    compiler_flags: Path,
    base_cc: Path,
) -> int:
    """Main entry point for the code generator."""
    if len(inputs) != len(outputs):
        print(
            f"Error: Number of inputs ({len(inputs)}) must match number of "
            f"outputs ({len(outputs)})",
            file=sys.stderr,
        )
        return 1

    try:
        flags = compiler_flags.read_text(encoding="utf-8").splitlines()

        for inp, out in zip(inputs, outputs):
            _generate_header(inp, out, flags, compiler, base_cc)
    except ParseError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    return 0


def _parse_args() -> dict[str, Any]:
    """Parses command line arguments."""
    parser = argparse.ArgumentParser(
        description="Generate shadowed headers with pw_enum footer"
    )
    parser.add_argument("inputs", nargs="+", type=Path, help="Input headers")
    parser.add_argument(
        "--outputs", nargs="+", type=Path, help="Output headers", required=True
    )
    parser.add_argument(
        "--compiler",
        type=str,
        help="Compiler to query for builtin includes",
        required=True,
    )
    parser.add_argument(
        "--compiler-flags",
        type=Path,
        help="File containing compilation flags",
        required=True,
    )
    parser.add_argument(
        "--base-cc",
        type=Path,
        help="base.cc placeholder file path",
        required=True,
    )
    return vars(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main(**_parse_args()))
