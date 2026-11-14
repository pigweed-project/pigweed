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
"""Generates C++ or Rust printf conformance tests from a YAML definition."""

from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Any

import yaml

from pw_build import generated_tests
from pw_codegen import cpp, rust

_INT32_MIN = -(2**31)
_INT32_MAX = 2**31 - 1
_UINT32_MAX = 2**32 - 1
_INT64_MIN = -(2**63)
_INT64_MAX = 2**63 - 1
_UINT64_MAX = 2**64 - 1
_MAX_BYTE_CHAR = 0xFF

_TEST_ID_RE = re.compile(r"^[a-z][a-z0-9_]*$")

_ALLOWED_TOP_LEVEL_KEYS = frozenset({"cases"})
_ALLOWED_CASE_KEYS = frozenset(
    {
        "test_id",
        "format_string",
        "args",
        "expected_output",
        "skip_snprintf",
    }
)


@dataclass(frozen=True)
class NormalizedArg:
    """Represents a single typed argument in a printf test case."""

    kind: str
    value: Any


@dataclass(frozen=True)
class NormalizedTestCase:
    """Represents a normalized printf conformance test case."""

    test_id: str
    format_string: str
    args: list[NormalizedArg]
    expected_output: str
    expected_output_utf8_len: int
    skip_snprintf: str | None
    cc_snprintf_args: tuple[str, ...] = ()
    has_pointer_spec: bool = False
    suppress_wformat: bool = False
    suppress_wformat_zero_length: bool = False


def _require_str(value: Any, label: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"Expected string for {label}, got {value!r}")
    return value


def _require_int_in_range(
    arg: NormalizedArg, min_val: int, max_val: int, context: str
) -> int:
    if arg.kind not in ("signed_int", "unsigned_int"):
        raise ValueError(
            f"{context} expected integer argument, got {arg.kind!r}"
        )
    val = arg.value
    if not isinstance(val, int) or isinstance(val, bool):
        raise ValueError(f"{context} expected int value, got {val!r}")
    if not min_val <= val <= max_val:
        raise ValueError(
            f"{context} value {val} is out of range [{min_val}, {max_val}]"
        )
    return val


def _rust_int_arg(
    variant: str, formatter: Callable[[int], str]
) -> Callable[[Any], str]:
    def _format(val: Any) -> str:
        if not isinstance(val, int) or isinstance(val, bool):
            raise ValueError(f"Expected int for {variant}, got {val!r}")
        return f"TestArg::{variant}({formatter(val)})"

    return _format


def _rust_float_arg(val: Any) -> str:
    if isinstance(val, bool) or not isinstance(val, (int, float)):
        raise ValueError(f"Expected float for FloatingPoint, got {val!r}")
    return f"TestArg::FloatingPoint({rust.f64_literal(val)})"


def _rust_string_arg(val: Any) -> str:
    lit = rust.string_literal(_require_str(val, "string_value"))
    return f"TestArg::StringValue({lit})"


def _rust_char_arg(val: Any) -> str:
    lit = rust.char_literal(_require_str(val, "char_value"))
    return f"TestArg::CharValue({lit})"


_RUST_ARG_FORMATTERS: dict[str, Callable[[Any], str]] = {
    "signed_int": _rust_int_arg("SignedInt", rust.i64_literal),
    "unsigned_int": _rust_int_arg("UnsignedInt", rust.u64_literal),
    "floating_point": _rust_float_arg,
    "string_value": _rust_string_arg,
    "char_value": _rust_char_arg,
    "pointer_value": _rust_int_arg("PointerValue", rust.u64_literal),
}


def rust_arg(arg: NormalizedArg) -> str:
    """Formats a NormalizedArg as a Rust TestArg expression."""
    try:
        formatter = _RUST_ARG_FORMATTERS[arg.kind]
    except KeyError as exc:
        raise ValueError(f"Unsupported argument kind: {arg.kind!r}") from exc
    return formatter(arg.value)


_SIGNED_INT_CASTS: dict[str, tuple[str, str, int, int]] = {
    "": ("int", "int32", _INT32_MIN, _INT32_MAX),
    "hh": ("int", "int32", _INT32_MIN, _INT32_MAX),
    "h": ("int", "int32", _INT32_MIN, _INT32_MAX),
    "l": ("long", "int32", _INT32_MIN, _INT32_MAX),
    "ll": ("long long", "int64", _INT64_MIN, _INT64_MAX),
    "j": ("std::intmax_t", "int64", _INT64_MIN, _INT64_MAX),
    "z": (
        "std::make_signed_t<std::size_t>",
        "int32",
        _INT32_MIN,
        _INT32_MAX,
    ),
    "t": ("std::ptrdiff_t", "int32", _INT32_MIN, _INT32_MAX),
}

# Note: Under C/C++ default argument promotions, unsigned char ('hh') and
# unsigned short ('h') promote to signed 'int' (checked by GCC -Wformat).
_UNSIGNED_INT_CASTS: dict[str, tuple[str, str, int]] = {
    "hh": ("int", "int32", _INT32_MAX),
    "h": ("int", "int32", _INT32_MAX),
    "": ("unsigned int", "uint32", _UINT32_MAX),
    "l": ("unsigned long", "uint32", _UINT32_MAX),
    "ll": ("unsigned long long", "uint64", _UINT64_MAX),
    "j": ("std::uintmax_t", "uint64", _UINT64_MAX),
    "z": ("std::size_t", "uint32", _UINT32_MAX),
    "t": ("std::make_unsigned_t<std::ptrdiff_t>", "uint32", _UINT32_MAX),
}

_LENGTH_MODIFIERS = ("hh", "h", "ll", "l", "j", "z", "t", "L")


def _format_cc_specifier_arg(
    test_id: str,
    spec: str,
    length: str,
    flags: str,
    has_precision: bool,
    arg: NormalizedArg,
) -> tuple[str, bool]:
    """Returns (cc_expr, is_pointer_spec) for a single conversion specifier."""
    context = f"Case {test_id!r} '%{length}{spec}'"
    if spec in "di":
        if "#" in flags:
            raise ValueError(f"{context}: '#' flag is undefined for '%{spec}'")
        if length not in _SIGNED_INT_CASTS:
            raise ValueError(f"{context}: invalid length modifier {length!r}")
        cc_type, lit_type, min_v, max_v = _SIGNED_INT_CASTS[length]
        val = _require_int_in_range(arg, min_v, max_v, context)
        return (
            f"static_cast<{cc_type}>({cpp.int_literal(val, lit_type)})",
            False,
        )

    if spec in "ouxX":
        if "#" in flags and spec == "u":
            raise ValueError(f"{context}: '#' flag is undefined for '%u'")
        if length not in _UNSIGNED_INT_CASTS:
            raise ValueError(f"{context}: invalid length modifier {length!r}")
        cc_type, lit_type, max_v = _UNSIGNED_INT_CASTS[length]
        val = _require_int_in_range(arg, 0, max_v, context)
        return (
            f"static_cast<{cc_type}>({cpp.int_literal(val, lit_type)})",
            False,
        )

    if spec in "fFeEaAgG":
        if length not in ("", "l"):
            raise ValueError(f"{context}: unsupported float length {length!r}")
        if (
            arg.kind != "floating_point"
            or isinstance(arg.value, bool)
            or not isinstance(arg.value, (int, float))
        ):
            raise ValueError(
                f"{context} expected floating_point argument, got {arg!r}"
            )
        return cpp.double_literal(arg.value), False

    if spec == "c":
        if length or has_precision or (set(flags) - {"-"}):
            raise ValueError(
                f"{context}: unsupported flags/precision/length for '%c'"
            )
        if arg.kind == "char_value":
            sval = _require_str(arg.value, f"{context} char_value")
            if len(sval) != 1:
                raise ValueError(
                    f"{context}: char_value must be 1 Unicode scalar, "
                    f"got {sval!r}"
                )
            return cpp.char_literal(sval), False
        val = _require_int_in_range(arg, 0, _MAX_BYTE_CHAR, context)
        return f"static_cast<int>({val})", False

    if spec == "s":
        if length or (set(flags) - {"-"}):
            raise ValueError(f"{context}: unsupported flags/length for '%s'")
        if arg.kind != "string_value":
            raise ValueError(
                f"{context} expected string_value, got {arg.kind!r}"
            )
        sval = _require_str(arg.value, f"{context} string_value")
        if "\0" in sval:
            raise ValueError(f"{context}: embedded NUL byte not supported")
        return cpp.string_literal(sval), False

    if spec == "p":
        if length or has_precision or (set(flags) - {"-"}):
            raise ValueError(
                f"{context}: unsupported flags/precision/length for '%p'"
            )
        if arg.kind != "pointer_value":
            raise ValueError(
                f"{context} expected pointer_value, got {arg.kind!r}"
            )
        val = arg.value
        if (
            not isinstance(val, int)
            or isinstance(val, bool)
            or not 0 <= val <= _UINT32_MAX
        ):
            raise ValueError(
                f"{context}: pointer_value {val!r} out of 32-bit portable range"
            )
        expr = (
            f"reinterpret_cast<const void*>("
            f"static_cast<std::uintptr_t>({cpp.int_literal(val, 'uint32')}))"
        )
        return expr, True

    raise ValueError(f"Case {test_id!r}: unsupported specifier '%{spec}'")


def _build_cc_snprintf_metadata(
    test_id: str,
    fmt: str,
    args: list[NormalizedArg],
) -> tuple[tuple[str, ...], bool, bool, bool]:
    """Computes typed C++ snprintf arguments and diagnostic flags."""
    if "\0" in fmt:
        raise ValueError(
            f"Case {test_id!r}: embedded NUL byte in format string is not "
            "supported by std::snprintf"
        )

    cc_args: list[str] = []
    has_pointer_spec = False
    suppress_wformat = False
    suppress_wformat_zero_length = fmt == ""

    i = 0
    arg_idx = 0
    n = len(fmt)
    while i < n:
        if fmt[i] != "%":
            i += 1
            continue
        i += 1
        if i < n and fmt[i] == "%":
            i += 1
            continue

        flag_start = i
        while i < n and fmt[i] in "-+ #0":
            i += 1
        flags = fmt[flag_start:i]
        if (
            len(flags) != len(set(flags))
            or ("-" in flags and "0" in flags)
            or ("+" in flags and " " in flags)
        ):
            suppress_wformat = True

        # Width
        if i < n and fmt[i] == "*":
            if arg_idx >= len(args):
                raise ValueError(
                    f"Case {test_id!r}: width '*' missing argument"
                )
            wval = _require_int_in_range(
                args[arg_idx],
                _INT32_MIN + 1,
                _INT32_MAX,
                f"Case {test_id!r} width '*'",
            )
            cc_args.append(
                f"static_cast<int>({cpp.int_literal(wval, 'int32')})"
            )
            arg_idx += 1
            i += 1
        else:
            while i < n and fmt[i].isascii() and fmt[i].isdigit():
                i += 1

        # Precision
        has_precision = False
        if i < n and fmt[i] == ".":
            has_precision = True
            i += 1
            if i < n and fmt[i] == "*":
                if arg_idx >= len(args):
                    raise ValueError(
                        f"Case {test_id!r}: precision '.*' missing argument"
                    )
                pval = _require_int_in_range(
                    args[arg_idx],
                    _INT32_MIN,
                    _INT32_MAX,
                    f"Case {test_id!r} precision '.*'",
                )
                cc_args.append(
                    f"static_cast<int>({cpp.int_literal(pval, 'int32')})"
                )
                arg_idx += 1
                i += 1
            else:
                while i < n and fmt[i].isascii() and fmt[i].isdigit():
                    i += 1

        # Length modifier
        length = ""
        for mod in _LENGTH_MODIFIERS:
            if fmt.startswith(mod, i):
                length = mod
                i += len(mod)
                break

        if i >= n:
            raise ValueError(
                f"Case {test_id!r}: incomplete format specifier at end of "
                "string"
            )
        spec = fmt[i]
        i += 1

        if "0" in flags and has_precision and spec in "diouxX":
            suppress_wformat = True

        if arg_idx >= len(args):
            raise ValueError(
                f"Case {test_id!r}: '%{spec}' requires an argument, "
                "but all arguments were consumed"
            )
        expr, is_ptr = _format_cc_specifier_arg(
            test_id, spec, length, flags, has_precision, args[arg_idx]
        )
        arg_idx += 1
        cc_args.append(expr)
        if is_ptr:
            has_pointer_spec = True

    if arg_idx != len(args):
        raise ValueError(
            f"Case {test_id!r}: {len(args)} argument(s) provided, "
            f"but format string consumed {arg_idx}"
        )

    return (
        tuple(cc_args),
        has_pointer_spec,
        suppress_wformat,
        suppress_wformat_zero_length,
    )


def _validate_test_id(test_id: str, index: int) -> None:
    if not test_id:
        raise ValueError(f"Case at index {index} has empty test_id")
    if (
        not _TEST_ID_RE.match(test_id)
        or "__" in test_id
        or not cpp.is_identifier(test_id)
        or not rust.is_identifier(test_id)
    ):
        raise ValueError(
            f"Case at index {index} has invalid test_id {test_id!r}; expected "
            "non-reserved snake_case identifier"
        )


def load_test_cases(yaml_path: Path) -> list[NormalizedTestCase]:
    """Loads and normalizes printf conformance test cases from YAML."""
    raw = yaml.safe_load(yaml_path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError(f"Top-level YAML in {yaml_path} must be a mapping")
    unknown_top = set(raw) - _ALLOWED_TOP_LEVEL_KEYS
    if unknown_top:
        raise ValueError(
            f"Unknown top-level YAML keys in {yaml_path}: {sorted(unknown_top)}"
        )
    raw_cases = raw.get("cases")
    if not isinstance(raw_cases, list):
        raise ValueError(f"'cases' in {yaml_path} must be a list")

    cases: list[NormalizedTestCase] = []
    seen_ids: set[str] = set()
    for index, entry in enumerate(raw_cases):
        if not isinstance(entry, dict):
            raise ValueError(
                f"Case at index {index} must be a mapping, got {entry!r}"
            )
        unknown_case = set(entry) - _ALLOWED_CASE_KEYS
        if unknown_case:
            raise ValueError(
                f"Case at index {index} has unknown keys: "
                f"{sorted(unknown_case)}"
            )
        if "test_id" not in entry:
            raise ValueError(f"Case at index {index} is missing test_id")
        test_id = _require_str(entry["test_id"], f"case[{index}].test_id")
        _validate_test_id(test_id, index)
        if test_id in seen_ids:
            raise ValueError(f"Duplicate test_id in {yaml_path}: {test_id!r}")
        seen_ids.add(test_id)

        for required_key in ("format_string", "expected_output"):
            if required_key not in entry:
                raise ValueError(
                    f"Case {test_id!r} is missing required key {required_key!r}"
                )

        format_string = _require_str(
            entry["format_string"], f"case[{test_id}].format_string"
        )
        expected_output = _require_str(
            entry["expected_output"], f"case[{test_id}].expected_output"
        )

        raw_args = entry.get("args")
        if raw_args is None:
            raw_args = []
        elif not isinstance(raw_args, list):
            raise ValueError(f"Case {test_id}: 'args' must be a list")

        args: list[NormalizedArg] = []
        for raw_arg in raw_args:
            if not isinstance(raw_arg, dict) or len(raw_arg) != 1:
                raise ValueError(
                    f"Case {test_id}: each arg must be a "
                    f"single-key mapping, got {raw_arg!r}"
                )
            kind, val = next(iter(raw_arg.items()))
            if not isinstance(kind, str):
                raise ValueError(
                    f"Case {test_id}: argument kind must be a string, got "
                    f"{kind!r}"
                )
            arg = NormalizedArg(kind=kind, value=val)
            # Validate every argument can be rendered in Rust at load time.
            rust_arg(arg)
            args.append(arg)

        skip_snprintf: str | None = None
        if "skip_snprintf" in entry:
            skip_snprintf = _require_str(
                entry["skip_snprintf"], f"case[{test_id}].skip_snprintf"
            )
            if not skip_snprintf.strip():
                raise ValueError(
                    f"Case {test_id!r}: skip_snprintf reason must be non-empty"
                )

        cc_snprintf_args: tuple[str, ...] = ()
        has_pointer_spec = False
        suppress_wformat = False
        suppress_wformat_zero_length = False
        if skip_snprintf is None:
            (
                cc_snprintf_args,
                has_pointer_spec,
                suppress_wformat,
                suppress_wformat_zero_length,
            ) = _build_cc_snprintf_metadata(test_id, format_string, args)

        cases.append(
            NormalizedTestCase(
                test_id=test_id,
                format_string=format_string,
                args=args,
                expected_output=expected_output,
                expected_output_utf8_len=len(expected_output.encode("utf-8")),
                skip_snprintf=skip_snprintf,
                cc_snprintf_args=cc_snprintf_args,
                has_pointer_spec=has_pointer_spec,
                suppress_wformat=suppress_wformat,
                suppress_wformat_zero_length=suppress_wformat_zero_length,
            )
        )
    return cases


def main() -> int:
    """CLI entry point for generating printf conformance test files."""
    return generated_tests.jinja_main(
        lambda yaml_path: {"cases": load_test_cases(yaml_path)},
        description=__doc__,
        extra_filters={
            "rust_arg": rust_arg,
        },
    )


if __name__ == "__main__":
    sys.exit(main())
