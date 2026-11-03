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
from collections.abc import Iterator, Sequence
from pathlib import Path
import re
import sys
from typing import Any

from pw_codegen import camel_to_upper_snake, cpp
from pw_enum.parse import (
    EnumDescriptor,
    EnumValue,
    parse_enums,
    ParseError,
    is_ok_name,
    StatusInfo,
)


def _legacy_camel_to_upper_snake(name: str) -> str:
    s1 = re.sub("(.)([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub("([a-z0-9])([A-Z])", r"\1_\2", s1).upper()


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

    # Temporarily emit legacy double-underscore aliases for downstream
    # consumers while they migrate to the single-underscore names.
    legacy_prefix = _legacy_camel_to_upper_snake("_".join(enum_desc.scopes))
    if legacy_prefix:
        legacy_prefix += "_"
    legacy_macro = legacy_prefix + _legacy_camel_to_upper_snake(enum_desc.name)
    if legacy_macro != macro:
        yield f"#define {legacy_macro}_DOMAIN {macro}_DOMAIN"
        yield f"#define {legacy_macro}_FMT {macro}_FMT"


def _is_contiguous(distinct_values: Sequence[EnumValue]) -> bool:
    """Returns whether the values cover every integer from min to max."""
    span = distinct_values[-1].value - distinct_values[0].value + 1
    return span == len(distinct_values)


def _generate_is_valid(
    enum_desc: EnumDescriptor,
    distinct_values: Sequence[EnumValue],
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
    unique_values: Sequence[tuple[EnumValue, str]],
) -> Iterator[str]:
    """Generates the ToString method for an EnumTraits specialization."""
    yield (
        "  [[nodiscard]] static constexpr const char* ToString("
        "enum_type value) {"
    )
    yield "    switch (value) {"
    for value, display_name in unique_values:
        yield f"      case {enum_desc.cc_full_name}::{value.cc_name}:"
        yield f"        return {cpp.string_literal(display_name)};"
    yield "    }"
    yield f'    return "Unknown {enum_desc.cc_full_name} value";'
    yield "  }"


def _generate_enum_traits(
    enum_desc: EnumDescriptor,
    unique_values: Sequence[tuple[EnumValue, str]],
) -> Iterator[str]:
    """Generates a pw::EnumTraits specialization for one enum."""
    cc_name = enum_desc.cc_full_name
    distinct_values = [v for v, _ in unique_values]
    is_contiguous = _is_contiguous(distinct_values)

    yield cpp.namespace_open("pw")
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
    yield from _generate_to_string(enum_desc, unique_values)
    yield "};"
    yield cpp.namespace_close("pw")


def _generate_tokenization(
    enum_desc: EnumDescriptor,
    unique_values: Sequence[tuple[EnumValue, str]],
) -> Iterator[str]:
    """Generates the pw_tokenizer registration for one enum."""
    yield f"_PW_TOKENIZE_ENUM_DOMAIN({enum_desc.cc_full_name},"
    yield f'                         "{enum_desc.cc_versioned_name}",'
    for i, (value, display_name) in enumerate(unique_values):
        comma = "" if i == len(unique_values) - 1 else ","
        yield (
            f"                         ({value.cc_name}, "
            f"{cpp.string_literal(display_name)}){comma}"
        )
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
        yield cpp.namespace_open("::".join(status.scopes))

    yield f"class [[nodiscard]] {status.name} final"
    yield f"    : public ::pw::StatusBase<{status.name}, {ok_expr}> {{"
    yield " public:"
    yield f"  constexpr {status.name}() = default;"
    yield f"  explicit constexpr {status.name}(Code code)"
    yield f"      : ::pw::StatusBase<{status.name}, {ok_expr}>(code) {{}}"
    yield from _generate_status_members(enum_desc, status)
    yield "};"

    if status.scopes:
        yield cpp.namespace_close("::".join(status.scopes))


def _generate_single_enum_footer(enum_desc: EnumDescriptor) -> Iterator[str]:
    unique_values = enum_desc.unique_values()

    if enum_desc.status is not None:
        yield from _generate_status_class(enum_desc)
        yield ""

    yield from _generate_macro_defs(enum_desc)
    yield ""
    yield from _generate_enum_traits(enum_desc, unique_values)
    yield ""
    yield from _generate_tokenization(enum_desc, unique_values)


def _footer_includes(enums: Sequence[EnumDescriptor]) -> list[str]:
    """Returns the #include lines that the footer for these enums requires."""
    enums_with_values = [enum_desc for enum_desc in enums if enum_desc.values]
    if not enums_with_values:
        return []

    includes = []
    if any(e.status is not None for e in enums_with_values):
        includes.append('#include "pw_status/status_base.h"')
    includes.append('#include "pw_tokenizer/enum.h"')
    return includes


def generate_footer(
    enums: Sequence[EnumDescriptor], *, with_includes: bool = True
) -> Iterator[str]:
    """Generates the C++ footer with enum traits and tokenization macros.

    Args:
        enums: The enums for which to generate the footer.
        with_includes: Whether to emit the footer's ``#include`` lines. If
            false, the caller is responsible for including the headers from
            ``_footer_includes``.
    """
    enums_with_values = [enum_desc for enum_desc in enums if enum_desc.values]
    if not enums_with_values:
        return

    if with_includes:
        yield from _footer_includes(enums_with_values)

    for i, enum_desc in enumerate(enums_with_values):
        if with_includes or i > 0:
            yield ""
        yield from _generate_single_enum_footer(enum_desc)


_PW_ENUM_MACRO = re.compile(r'\bPW_ENUM(?:_STATUS)?(?=\s*\()')


def _generate_enum_definition(enum_desc: EnumDescriptor) -> Iterator[str]:
    assert enum_desc.underlying_type is not None
    cc_type = cpp.INT_TYPES[enum_desc.underlying_type].cc_type
    namespace = "::".join(enum_desc.scopes)

    yield cpp.namespace_open(namespace)
    yield ""
    yield from cpp.doc_comment(enum_desc.doc)
    yield f"enum class {enum_desc.name} : {cc_type} {{"
    for value in enum_desc.values:
        yield from cpp.doc_comment(value.doc, indent="  ")
        literal = cpp.int_literal(value.value, enum_desc.underlying_type)
        yield f"  {value.cc_name} = {literal},"
    yield "};"
    yield ""
    yield cpp.namespace_close(namespace)


def generate_cc_header(
    source: str, enums: Sequence[EnumDescriptor]
) -> Iterator[str]:
    """Generates a complete C++ header for enums from a pw_enum source."""
    yield f"// Generated by pw_enum from {source}. DO NOT EDIT."
    yield "#pragma once"
    yield ""
    yield from cpp.includes(
        ["<cstdint>", '"pw_enum/traits.h"', *_footer_includes(enums)]
    )

    for enum_desc in enums:
        yield ""
        yield from _generate_enum_definition(enum_desc)

    yield ""
    yield "// --- pw_enum generated footer ---"
    yield ""
    yield from generate_footer(enums, with_includes=False)


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
