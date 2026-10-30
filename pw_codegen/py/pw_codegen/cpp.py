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
"""C++ source code generation utilities."""

from __future__ import annotations

from collections.abc import Collection, Iterable, Iterator
import contextlib
import itertools
import re
from typing import NamedTuple

from pw_codegen import OutputFile, snake_to_pascal

# C++ keywords that cannot be used as identifiers in generated C++ code
# (https://eel.is/c++draft/lex.key and
# https://en.cppreference.com/w/cpp/keyword):
KEYWORDS: frozenset[str] = frozenset(
    {
        "alignas",
        "alignof",
        "and",
        "and_eq",
        "asm",
        "atomic_cancel",
        "atomic_commit",
        "atomic_noexcept",
        "auto",
        "bitand",
        "bitor",
        "bool",
        "break",
        "case",
        "catch",
        "char",
        "char8_t",
        "char16_t",
        "char32_t",
        "class",
        "compl",
        "concept",
        "const",
        "consteval",
        "constexpr",
        "constinit",
        "const_cast",
        "continue",
        "contract_assert",
        "co_await",
        "co_return",
        "co_yield",
        "decltype",
        "default",
        "delete",
        "do",
        "double",
        "dynamic_cast",
        "else",
        "enum",
        "explicit",
        "export",
        "extern",
        "false",
        "float",
        "for",
        "friend",
        "goto",
        "if",
        "inline",
        "int",
        "long",
        "mutable",
        "namespace",
        "new",
        "noexcept",
        "not",
        "not_eq",
        "nullptr",
        "operator",
        "or",
        "or_eq",
        "private",
        "protected",
        "public",
        "reflexpr",
        "register",
        "reinterpret_cast",
        "requires",
        "return",
        "short",
        "signed",
        "sizeof",
        "static",
        "static_assert",
        "static_cast",
        "struct",
        "switch",
        "synchronized",
        "template",
        "this",
        "thread_local",
        "throw",
        "true",
        "try",
        "typedef",
        "typeid",
        "typename",
        "union",
        "unsigned",
        "using",
        "virtual",
        "void",
        "volatile",
        "wchar_t",
        "while",
        "xor",
        "xor_eq",
    }
)

# C++20 macros (https://en.cppreference.com/w/cpp/symbol_index/macro),
# excluding the following:
# - Function-like macros, which have unambiguous syntax and thus won't
#   conflict with generated symbols.
# - Macros that couldn't be made valid by appending underscores, namely
#   those containing "__" or starting with "_[A-Z]". C++ reserves all such
#   identifiers for the compiler, and appending underscores wouldn't change
#   that.
STDLIB_MACROS: frozenset[str] = frozenset(
    {
        "ATOMIC_BOOL_LOCK_FREE",
        "ATOMIC_CHAR_LOCK_FREE",
        "ATOMIC_CHAR16_T_LOCK_FREE",
        "ATOMIC_CHAR32_T_LOCK_FREE",
        "ATOMIC_CHAR8_T_LOCK_FREE",
        "ATOMIC_FLAG_INIT",
        "ATOMIC_INT_LOCK_FREE",
        "ATOMIC_LLONG_LOCK_FREE",
        "ATOMIC_LONG_LOCK_FREE",
        "ATOMIC_POINTER_LOCK_FREE",
        "ATOMIC_SHORT_LOCK_FREE",
        "ATOMIC_WCHAR_T_LOCK_FREE",
        "BUFSIZ",
        "CHAR_BIT",
        "CHAR_MAX",
        "CHAR_MIN",
        "CLOCKS_PER_SEC",
        "DBL_DECIMAL_DIG",
        "DBL_DIG",
        "DBL_EPSILON",
        "DBL_HAS_SUBNORM",
        "DBL_MANT_DIG",
        "DBL_MAX",
        "DBL_MAX_10_EXP",
        "DBL_MAX_EXP",
        "DBL_MIN",
        "DBL_MIN_10_EXP",
        "DBL_MIN_EXP",
        "DBL_TRUE_MIN",
        "DECIMAL_DIG",
        "E2BIG",
        "EACCES",
        "EADDRINUSE",
        "EADDRNOTAVAIL",
        "EAFNOSUPPORT",
        "EAGAIN",
        "EALREADY",
        "EBADF",
        "EBADMSG",
        "EBUSY",
        "ECANCELED",
        "ECHILD",
        "ECONNABORTED",
        "ECONNREFUSED",
        "ECONNRESET",
        "EDEADLK",
        "EDESTADDRREQ",
        "EDOM",
        "EEXIST",
        "EFAULT",
        "EFBIG",
        "EHOSTUNREACH",
        "EIDRM",
        "EILSEQ",
        "EINPROGRESS",
        "EINTR",
        "EINVAL",
        "EIO",
        "EISCONN",
        "EISDIR",
        "ELOOP",
        "EMFILE",
        "EMLINK",
        "EMSGSIZE",
        "ENAMETOOLONG",
        "ENETDOWN",
        "ENETRESET",
        "ENETUNREACH",
        "ENFILE",
        "ENOBUFS",
        "ENODATA",
        "ENODEV",
        "ENOENT",
        "ENOEXEC",
        "ENOLCK",
        "ENOLINK",
        "ENOMEM",
        "ENOMSG",
        "ENOPROTOOPT",
        "ENOSPC",
        "ENOSR",
        "ENOSTR",
        "ENOSYS",
        "ENOTCONN",
        "ENOTDIR",
        "ENOTEMPTY",
        "ENOTRECOVERABLE",
        "ENOTSOCK",
        "ENOTSUP",
        "ENOTTY",
        "ENXIO",
        "EOF",
        "EOPNOTSUPP",
        "EOVERFLOW",
        "EOWNERDEAD",
        "EPERM",
        "EPIPE",
        "EPROTO",
        "EPROTONOSUPPORT",
        "EPROTOTYPE",
        "ERANGE",
        "EROFS",
        "errno",
        "ESPIPE",
        "ESRCH",
        "ETIME",
        "ETIMEDOUT",
        "ETXTBSY",
        "EWOULDBLOCK",
        "EXDEV",
        "EXIT_FAILURE",
        "EXIT_SUCCESS",
        "FE_ALL_EXCEPT",
        "FE_DFL_ENV",
        "FE_DIVBYZERO",
        "FE_DOWNWARD",
        "FE_INEXACT",
        "FE_INVALID",
        "FE_OVERFLOW",
        "FE_TONEAREST",
        "FE_TOWARDZERO",
        "FE_UNDERFLOW",
        "FE_UPWARD",
        "FILENAME_MAX",
        "FLT_DECIMAL_DIG",
        "FLT_DIG",
        "FLT_EPSILON",
        "FLT_EVAL_METHOD",
        "FLT_HAS_SUBNORM",
        "FLT_MANT_DIG",
        "FLT_MAX",
        "FLT_MAX_10_EXP",
        "FLT_MAX_EXP",
        "FLT_MIN",
        "FLT_MIN_10_EXP",
        "FLT_MIN_EXP",
        "FLT_RADIX",
        "FLT_ROUNDS",
        "FLT_TRUE_MIN",
        "FOPEN_MAX",
        "FP_FAST_FMA",
        "FP_FAST_FMAF",
        "FP_FAST_FMAL",
        "FP_ILOGB0",
        "FP_ILOGBNAN",
        "FP_SUBNORMAL",
        "FP_ZERO",
        "FP_INFINITE",
        "FP_NAN",
        "FP_NORMAL",
        "HUGE_VAL",
        "HUGE_VALF",
        "HUGE_VALL",
        "INFINITY",
        "INT_FAST16_MAX",
        "INT_FAST16_MIN",
        "INT_FAST32_MAX",
        "INT_FAST32_MIN",
        "INT_FAST64_MAX",
        "INT_FAST64_MIN",
        "INT_FAST8_MAX",
        "INT_FAST8_MIN",
        "INT_LEAST16_MAX",
        "INT_LEAST16_MIN",
        "INT_LEAST32_MAX",
        "INT_LEAST32_MIN",
        "INT_LEAST64_MAX",
        "INT_LEAST64_MIN",
        "INT_LEAST8_MAX",
        "INT_LEAST8_MIN",
        "INT_MAX",
        "INT_MIN",
        "INT16_MAX",
        "INT16_MIN",
        "INT32_MAX",
        "INT32_MIN",
        "INT64_MAX",
        "INT64_MIN",
        "INT8_MAX",
        "INT8_MIN",
        "INTMAX_MAX",
        "INTMAX_MIN",
        "INTPTR_MAX",
        "INTPTR_MIN",
        "L_tmpnam",
        "LC_ALL",
        "LC_COLLATE",
        "LC_CTYPE",
        "LC_MONETARY",
        "LC_NUMERIC",
        "LC_TIME",
        "LDBL_DECIMAL_DIG",
        "LDBL_DIG",
        "LDBL_EPSILON",
        "LDBL_HAS_SUBNORM",
        "LDBL_MANT_DIG",
        "LDBL_MAX",
        "LDBL_MAX_10_EXP",
        "LDBL_MAX_EXP",
        "LDBL_MIN",
        "LDBL_MIN_10_EXP",
        "LDBL_MIN_EXP",
        "LDBL_TRUE_MIN",
        "LLONG_MAX",
        "LLONG_MIN",
        "LONG_MAX",
        "LONG_MIN",
        "MATH_ERREXCEPT",
        "math_errhandling",
        "MATH_ERRNO",
        "MB_CUR_MAX",
        "MB_LEN_MAX",
        "NAN",
        "NULL",
        "ONCE_FLAG_INIT",
        "PRId16",
        "PRId32",
        "PRId64",
        "PRId8",
        "PRIdFAST16",
        "PRIdFAST32",
        "PRIdFAST64",
        "PRIdFAST8",
        "PRIdLEAST16",
        "PRIdLEAST32",
        "PRIdLEAST64",
        "PRIdLEAST8",
        "PRIdMAX",
        "PRIdPTR",
        "PRIi16",
        "PRIi32",
        "PRIi64",
        "PRIi8",
        "PRIiFAST16",
        "PRIiFAST32",
        "PRIiFAST64",
        "PRIiFAST8",
        "PRIiLEAST16",
        "PRIiLEAST32",
        "PRIiLEAST64",
        "PRIiLEAST8",
        "PRIiMAX",
        "PRIiPTR",
        "PRIo16",
        "PRIo32",
        "PRIo64",
        "PRIo8",
        "PRIoFAST16",
        "PRIoFAST32",
        "PRIoFAST64",
        "PRIoFAST8",
        "PRIoLEAST16",
        "PRIoLEAST32",
        "PRIoLEAST64",
        "PRIoLEAST8",
        "PRIoMAX",
        "PRIoPTR",
        "PRIu16",
        "PRIu32",
        "PRIu64",
        "PRIu8",
        "PRIuFAST16",
        "PRIuFAST32",
        "PRIuFAST64",
        "PRIuFAST8",
        "PRIuLEAST16",
        "PRIuLEAST32",
        "PRIuLEAST64",
        "PRIuLEAST8",
        "PRIuMAX",
        "PRIuPTR",
        "PRIx16",
        "PRIX16",
        "PRIx32",
        "PRIX32",
        "PRIx64",
        "PRIX64",
        "PRIx8",
        "PRIX8",
        "PRIxFAST16",
        "PRIXFAST16",
        "PRIxFAST32",
        "PRIXFAST32",
        "PRIxFAST64",
        "PRIXFAST64",
        "PRIxFAST8",
        "PRIXFAST8",
        "PRIxLEAST16",
        "PRIXLEAST16",
        "PRIxLEAST32",
        "PRIXLEAST32",
        "PRIxLEAST64",
        "PRIXLEAST64",
        "PRIxLEAST8",
        "PRIXLEAST8",
        "PRIxMAX",
        "PRIXMAX",
        "PRIxPTR",
        "PRIXPTR",
        "PTRDIFF_MAX",
        "PTRDIFF_MIN",
        "RAND_MAX",
        "SCHAR_MAX",
        "SCHAR_MIN",
        "SCNd16",
        "SCNd32",
        "SCNd64",
        "SCNd8",
        "SCNdFAST16",
        "SCNdFAST32",
        "SCNdFAST64",
        "SCNdFAST8",
        "SCNdLEAST16",
        "SCNdLEAST32",
        "SCNdLEAST64",
        "SCNdLEAST8",
        "SCNdMAX",
        "SCNdPTR",
        "SCNi16",
        "SCNi32",
        "SCNi64",
        "SCNi8",
        "SCNiFAST16",
        "SCNiFAST32",
        "SCNiFAST64",
        "SCNiFAST8",
        "SCNiLEAST16",
        "SCNiLEAST32",
        "SCNiLEAST64",
        "SCNiLEAST8",
        "SCNiMAX",
        "SCNiPTR",
        "SCNo16",
        "SCNo32",
        "SCNo64",
        "SCNo8",
        "SCNoFAST16",
        "SCNoFAST32",
        "SCNoFAST64",
        "SCNoFAST8",
        "SCNoLEAST16",
        "SCNoLEAST32",
        "SCNoLEAST64",
        "SCNoLEAST8",
        "SCNoMAX",
        "SCNoPTR",
        "SCNu16",
        "SCNu32",
        "SCNu64",
        "SCNu8",
        "SCNuFAST16",
        "SCNuFAST32",
        "SCNuFAST64",
        "SCNuFAST8",
        "SCNuLEAST16",
        "SCNuLEAST32",
        "SCNuLEAST64",
        "SCNuLEAST8",
        "SCNuMAX",
        "SCNuPTR",
        "SCNx16",
        "SCNx32",
        "SCNx64",
        "SCNx8",
        "SCNxFAST16",
        "SCNxFAST32",
        "SCNxFAST64",
        "SCNxFAST8",
        "SCNxLEAST16",
        "SCNxLEAST32",
        "SCNxLEAST64",
        "SCNxLEAST8",
        "SCNxMAX",
        "SCNxPTR",
        "SEEK_CUR",
        "SEEK_END",
        "SEEK_SET",
        "SHRT_MAX",
        "SHRT_MIN",
        "SIG_ATOMIC_MAX",
        "SIG_ATOMIC_MIN",
        "SIG_DFL",
        "SIG_ERR",
        "SIG_IGN",
        "SIGABRT",
        "SIGFPE",
        "SIGILL",
        "SIGINT",
        "SIGSEGV",
        "SIGTERM",
        "SIZE_MAX",
        "stderr",
        "stdin",
        "stdout",
        "TIME_UTC",
        "TMP_MAX",
        "UCHAR_MAX",
        "UINT_FAST16_MAX",
        "UINT_FAST32_MAX",
        "UINT_FAST64_MAX",
        "UINT_FAST8_MAX",
        "UINT_LEAST16_MAX",
        "UINT_LEAST32_MAX",
        "UINT_LEAST64_MAX",
        "UINT_LEAST8_MAX",
        "UINT_MAX",
        "UINT16_MAX",
        "UINT32_MAX",
        "UINT64_MAX",
        "UINT8_MAX",
        "UINTMAX_MAX",
        "UINTPTR_MAX",
        "ULLONG_MAX",
        "ULONG_MAX",
        "USHRT_MAX",
        "WCHAR_MAX",
        "WCHAR_MIN",
        "WEOF",
        "WINT_MAX",
        "WINT_MIN",
    }
)

# Identifiers that cannot be used directly in generated C++ code because they
# are C++ keywords or standard-library object-like macros.
RESERVED_WORDS: frozenset[str] = KEYWORDS | STDLIB_MACROS

_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class IntType(NamedTuple):
    """Metadata for a fixed-width C++ integer type."""

    cc_type: str
    min_value: int
    max_value: int


def _int_type(bits: int, signed: bool) -> IntType:
    if signed:
        return IntType(f"int{bits}_t", -(2 ** (bits - 1)), 2 ** (bits - 1) - 1)
    return IntType(f"uint{bits}_t", 0, 2**bits - 1)


# Fixed-width integer types keyed by canonical name (e.g. "int32", "uint64").
INT_TYPES: dict[str, IntType] = {
    f"{prefix}{bits}": _int_type(bits, prefix == "int")
    for prefix in ("int", "uint")
    for bits in (8, 16, 32, 64)
}


def is_identifier(name: str) -> bool:
    """Returns whether ``name`` is a valid, non-reserved C++ identifier."""
    return bool(_IDENTIFIER.match(name)) and name not in RESERVED_WORDS


def fix_identifier(
    name: str,
    extra_reserved: Collection[str] = (),
) -> str:
    """Adjusts ``name`` so it does not collide with C++ reserved words.

    If ``name`` is in :py:data:`RESERVED_WORDS` or ``extra_reserved``, an
    underscore is appended. Otherwise, ``name`` is returned unchanged.

    Note that C++ reserves identifiers containing ``__`` or starting with an
    underscore followed by an uppercase letter for the implementation;
    appending an underscore cannot make those valid, so they are not modified.
    """
    if name in RESERVED_WORDS or name in extra_reserved:
        return f"{name}_"
    return name


def fix_enum_value_name(
    name: str,
    extra_reserved: Collection[str] = (),
) -> str:
    """Adjusts an enum value name if its ``UPPER_SNAKE_CASE`` form is reserved.

    Checks ``name.upper()`` against :py:data:`RESERVED_WORDS` and
    ``extra_reserved``, appending an underscore to ``name`` if a collision is
    found. Lowercase or mixed-case entries in :py:data:`RESERVED_WORDS` and
    ``extra_reserved`` (such as ``"int"`` or ``"Message"``) do not match
    because the ``UPPER_SNAKE_CASE`` identifier will not collide with them.
    """
    upper = name.upper()
    if upper in RESERVED_WORDS or upper in extra_reserved:
        return f"{name}_"
    return name


def constant_style(name: str) -> str:
    """Formats a ``snake_case`` or ``UPPER_SNAKE_CASE`` name as a C++ constant.

    Converts ``name`` to Google C++ constant style (``kPascalCase``).
    """
    return "k" + snake_to_pascal(name)


_INT32_MAX = 2**31 - 1


def int_literal(value: int, int_type: str = "int64") -> str:
    """Formats ``value`` as a C++ integer literal suitable for ``int_type``.

    Args:
        value: Integer value to format.
        int_type: Canonical fixed-width integer type name from
            :py:data:`INT_TYPES` (for example ``"int32"`` or ``"uint64"``).

    Raises:
        ValueError: ``int_type`` is unknown or ``value`` is out of range for
            ``int_type``.
    """
    if int_type not in INT_TYPES:
        raise ValueError(
            f"Unsupported integer type {int_type!r}; expected one of "
            f"{', '.join(INT_TYPES)}"
        )
    bounds = INT_TYPES[int_type]
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or not bounds.min_value <= value <= bounds.max_value
    ):
        raise ValueError(
            f"Value {value!r} is out of range for {int_type} "
            f"[{bounds.min_value}, {bounds.max_value}]"
        )

    if 0 <= value <= _INT32_MAX:
        return str(value)
    if int_type.startswith("uint"):
        return f"{value}u" if value <= 0xFFFFFFFF else f"{value}ull"
    if -_INT32_MAX <= value < 0:
        return str(value)
    int64_min = INT_TYPES["int64"].min_value
    if value == int64_min:
        # The literal 9223372036854775808 doesn't fit in signed long long.
        return f"({int64_min + 1}ll - 1)"
    return f"{value}ll"


_STRING_ESCAPES: dict[int, str] = {
    ord(b'"'): r"\"",
    ord(b"\\"): r"\\",
    ord(b"\a"): r"\a",
    ord(b"\b"): r"\b",
    ord(b"\f"): r"\f",
    ord(b"\n"): r"\n",
    ord(b"\r"): r"\r",
    ord(b"\t"): r"\t",
    ord(b"\v"): r"\v",
}


def _string_literal_parts(data: bytes) -> Iterator[str]:
    hex_byte = False
    yield '"'

    for byte in data:
        if byte in _STRING_ESCAPES:
            hex_byte = False
            yield _STRING_ESCAPES[byte]
            continue
        if 0x20 <= byte < 0x7F:
            char = chr(byte)
            if hex_byte and (char.isdigit() or "a" <= char.lower() <= "f"):
                yield '""'  # Split literal so \x doesn't absorb this hex digit.
            hex_byte = False
            yield char
            continue

        hex_byte = True
        yield rf"\x{byte:02x}"

    yield '"'


def string_literal(data: str | bytes) -> str:
    """Returns a quoted C++ string literal for ``bytes`` or a UTF-8 ``str``.

    Uses printable ASCII characters and standard C++ escape sequences where
    possible, falling back to ``\\xNN`` hex escapes for non-ASCII or control
    bytes and splitting adjacent string literals when a hex escape is followed
    by a hexadecimal ASCII character.
    """
    if isinstance(data, str):
        data = data.encode("utf-8")
    return "".join(_string_literal_parts(data))


def doc_comment(doc: str, indent: str = "") -> Iterator[str]:
    """Yields ``///`` C++ documentation comment lines for ``doc``.

    Raises:
        ValueError: A line in ``doc`` ends with a backslash, which would splice
            the following line of generated C++ code into the comment.
    """
    for raw_line in doc.splitlines():
        line = f"{indent}/// {raw_line}".rstrip()
        if line.endswith("\\"):
            raise ValueError(
                f"Documentation line {line.strip()!r} cannot end with a "
                "backslash"
            )
        yield line


def byte_array(
    data: bytes,
    *,
    bytes_per_line: int | None = None,
    indent: str = "  ",
) -> str:
    """Formats ``data`` as a C++ ``std::byte`` brace-enclosed initializer.

    When ``bytes_per_line`` is ``None`` (the default), returns a single-line
    initializer such as ``"{std::byte{0x01}, std::byte{0x02}}"`` (or ``"{}"``
    when ``data`` is empty). When ``bytes_per_line`` is positive and ``data``
    is non-empty, formats a multiline initializer with up to ``bytes_per_line``
    elements per indented line.
    """
    if bytes_per_line is not None and bytes_per_line <= 0:
        raise ValueError(
            f"bytes_per_line must be positive; got {bytes_per_line}"
        )
    if not data:
        return "{}"

    elements = [f"std::byte{{0x{byte:02x}}}" for byte in data]
    if bytes_per_line is None:
        return f"{{{', '.join(elements)}}}"

    it = iter(elements)
    lines: list[str] = []
    while chunk := tuple(itertools.islice(it, bytes_per_line)):
        lines.append(f"{indent}{', '.join(chunk)},")
    return "{\n" + "\n".join(lines) + "\n}"


def includes(headers: Iterable[str]) -> list[str]:
    """Deduplicates, groups, and sorts C++ ``#include`` directives.

    Each item in ``headers`` may be either a bare include target
    (``"<cstdint>"`` or ``'"pw_foo/bar.h"'``) or a full ``#include`` line.
    System headers (``<...>``) are emitted first in sorted order, followed by
    local headers (``"..."``) in sorted order, with a blank line separating the
    two groups when both are non-empty.
    """
    system: set[str] = set()
    local: set[str] = set()
    for raw in headers:
        target = raw.strip()
        if target.startswith("#include"):
            target = target[len("#include") :].strip()
        if len(target) > 2 and target.startswith("<") and target.endswith(">"):
            system.add(f"#include {target}")
        elif (
            len(target) > 2 and target.startswith('"') and target.endswith('"')
        ):
            local.add(f"#include {target}")
        else:
            raise ValueError(
                f"Invalid C++ #include target {raw!r}; expected <...> or "
                '"..."'
            )

    lines = sorted(system)
    if system and local:
        lines.append("")
    lines.extend(sorted(local))
    return lines


def _clean_namespace(name: str) -> str:
    clean = name.removeprefix("::")
    if not clean:
        raise ValueError(f"Namespace name {name!r} cannot be empty")
    return clean


def namespace_open(name: str) -> str:
    """Returns the opening line ``namespace <name> {`` for a C++ namespace."""
    return f"namespace {_clean_namespace(name)} {{"


def namespace_close(name: str) -> str:
    """Returns ``}  // namespace <name>`` for a C++ namespace."""
    return f"}}  // namespace {_clean_namespace(name)}"


@contextlib.contextmanager
def namespace(
    output: OutputFile,
    name: str | None,
    *,
    padding: bool = True,
) -> Iterator[None]:
    """Wraps generated lines in ``namespace <name> { ... }``.

    If ``name`` is ``None``, ``""``, or ``"::"``, nothing is written so callers
    can unconditionally wrap declarations whose namespace is optional.
    """
    clean = name.removeprefix("::") if name else ""
    if not clean:
        yield
        return

    if padding:
        output.write_line()
    output.write_line(namespace_open(clean))
    if padding:
        output.write_line()
    yield
    if padding:
        output.write_line()
    output.write_line(namespace_close(clean))
