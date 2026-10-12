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
        is_ok_name,
        StatusInfo,
    )
except ImportError:  # Support running without installing this Python package.
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from pw_enum.parse import (
        EnumDescriptor,
        EnumValue,
        parse_enums,
        ParseError,
        camel_to_upper_snake,
        is_ok_name,
        StatusInfo,
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


def _generate_to_string(
    enum_desc: EnumDescriptor,
    value_groups: list[list[EnumValue]],
) -> Iterator[str]:
    """Generates the ToString method for an EnumTraits specialization."""
    yield (
        "  [[nodiscard]] static constexpr const char* ToString("
        "enum_type value) {"
    )
    yield "    switch (value) {"
    for group in value_groups:
        label = group[0].cc_name
        name = "|".join(dict.fromkeys(v.name for v in group))
        yield f"      case {enum_desc.cc_full_name}::{label}:"
        yield f'        return "{_escape_cc(name)}";'
    yield "    }"
    yield f'    return "Unknown {enum_desc.cc_full_name} value";'
    yield "  }"


def _generate_enum_traits(
    enum_desc: EnumDescriptor,
    distinct_values: list[EnumValue],
    value_groups: list[list[EnumValue]],
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
    yield ""
    yield from _generate_to_string(enum_desc, value_groups)
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


_STATUS_BASE_MEMBERS = frozenset(
    ("Code", "ok", "code", "Update", "IgnoreError", "str")
)


def _status_factory_name(cc_name: str) -> str | None:
    if cc_name.startswith("k") and len(cc_name) > 1 and cc_name[1].isupper():
        return cc_name[1:]
    return None


def _generate_status_members(
    enum_desc: EnumDescriptor, status: StatusInfo
) -> Iterator[str]:
    """Generates the factory and Is*() functions for a PW_ENUM_STATUS."""
    reserved = _STATUS_BASE_MEMBERS | {status.name}
    defined: set[str] = set()

    def skip_reason_for(fn_name: str) -> str | None:
        if fn_name in reserved:
            return f"{fn_name}() conflicts with a StatusBase member"
        if fn_name in defined:
            return f"{fn_name}() is already defined"
        return None

    # Factories are not generated for the OK code (or its aliases); OK statuses
    # are default constructed.
    ok_values = [
        v.value for v in enum_desc.values if v.cc_name == status.ok_code
    ]
    assert ok_values, f"{status.ok_code} is not in {enum_desc.cc_full_name}"

    for v in enum_desc.values:
        skip_reason: str | None
        fn_name = _status_factory_name(v.cc_name)
        if fn_name is None:
            skip_reason = "not a kEnumName-style name"
        elif v.value == ok_values[0]:
            skip_reason = f"OK code; use {status.name}() instead"
        else:
            skip_reason = skip_reason_for(fn_name)

        yield ""
        if skip_reason is not None:
            yield (
                f"  // PW_ENUM_STATUS: No factory for {v.cc_name}: "
                f"{skip_reason}."
            )
            continue

        assert fn_name is not None
        defined.add(fn_name)
        yield (
            f"  [[nodiscard]] static constexpr {status.name}" f" {fn_name}() {{"
        )
        yield f"    return {status.name}(Code::{v.cc_name});"
        yield "  }"

    # Is*() functions are generated for every code, including OK codes, except
    # for codes named OK, since IsOk() would duplicate ok().
    for v in enum_desc.values:
        fn_name = _status_factory_name(v.cc_name)
        if fn_name is None:
            skip_reason = "not a kEnumName-style name"
        elif is_ok_name(v.cc_name):
            skip_reason = "use ok() instead"
        else:
            fn_name = f"Is{fn_name}"
            skip_reason = skip_reason_for(fn_name)

        yield ""
        if skip_reason is not None:
            yield (
                f"  // PW_ENUM_STATUS: No Is*() function for {v.cc_name}: "
                f"{skip_reason}."
            )
            continue

        assert fn_name is not None
        defined.add(fn_name)
        yield f"  [[nodiscard]] constexpr bool {fn_name}() const {{"
        yield f"    return code() == Code::{v.cc_name};"
        yield "  }"


def _generate_status_class(enum_desc: EnumDescriptor) -> Iterator[str]:
    """Generates the StatusBase-derived class for a PW_ENUM_STATUS."""
    assert enum_desc.status is not None
    status = enum_desc.status
    ok_code = status.ok_code
    ok_expr = (
        f"{enum_desc.name}::{ok_code}"
        if enum_desc.scopes == status.scopes
        else f"{enum_desc.cc_full_name}::{ok_code}"
    )

    if status.scopes:
        yield f"namespace {'::'.join(status.scopes)} {{"

    yield f"class [[nodiscard]] {status.name} final"
    yield f"    : public ::pw::StatusBase<{status.name}, {ok_expr}> {{"
    yield " public:"
    yield f"  constexpr {status.name}() = default;"
    yield f"  explicit constexpr {status.name}(Code code)"
    yield f"      : ::pw::StatusBase<{status.name}, {ok_expr}>(code) {{}}"
    yield from _generate_status_members(enum_desc, status)
    yield "};"

    if status.scopes:
        yield f"}}  // namespace {'::'.join(status.scopes)}"


def _generate_single_enum_footer(enum_desc: EnumDescriptor) -> Iterator[str]:
    value_groups = _group_by_value(enum_desc)
    distinct_values = [group[0] for group in value_groups]

    if enum_desc.status is not None:
        yield from _generate_status_class(enum_desc)
        yield ""

    yield from _generate_macro_defs(enum_desc)
    yield ""
    yield from _generate_enum_traits(enum_desc, distinct_values, value_groups)
    yield ""
    yield from _generate_tokenization(enum_desc, value_groups)


def generate_footer(enums: list[EnumDescriptor]) -> Iterator[str]:
    """Generates the C++ footer with enum traits and tokenization macros."""
    enums_with_values = [enum_desc for enum_desc in enums if enum_desc.values]
    if not enums_with_values:
        return

    if any(e.status is not None for e in enums_with_values):
        yield '#include "pw_status/status_base.h"'
    yield '#include "pw_tokenizer/enum.h"'

    for enum_desc in enums_with_values:
        yield ""
        yield from _generate_single_enum_footer(enum_desc)


_PW_ENUM_MACRO = re.compile(r'\bPW_ENUM(?:_STATUS)?(?=\s*\()')


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
                assert (
                    count > 0
                ), f"Expected PW_ENUM() or PW_ENUM_STATUS() on line {line_num}"
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
