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
"""Loads and resolves language-neutral pw_enum JSON enum definitions."""

from __future__ import annotations

import ast
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
import enum as _enum
import json
import keyword
import operator
from pathlib import Path
import re
from typing import Any, NoReturn

from pw_enum.parse import (
    EnumDescriptor,
    EnumValue,
    ParseError,
    StatusInfo,
    is_ok_name,
)
from pw_codegen import cpp, python

_ENUM_NAME = re.compile(r"^[A-Z][A-Za-z0-9]*$")
_VALUE_NAME = re.compile(r"^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)*$")

_FILE_KEYS = frozenset(("$schema", "namespace", "cc", "enums"))
_FILE_CC_KEYS = frozenset(("namespace",))
_ENUM_KEYS = frozenset(
    ("name", "type", "doc", "values", "status", "allow_alias")
)
_VALUE_KEYS = frozenset(("name", "value", "display_name", "doc", "cc"))
_VALUE_CC_KEYS = frozenset(("name",))
_STATUS_KEYS = frozenset(("name", "ok"))

DEFAULT_INT_TYPE = "int32"

# Limit shifts to keep evaluation bounded; no valid enum value needs more.
_MAX_SHIFT = 64


class SourceError(ParseError):
    """Raised for invalid pw_enum JSON sources."""


def _fail(path: Path | str, context: str, message: str) -> NoReturn:
    location = f"{path}: {context}" if context else str(path)
    raise SourceError(f"{location}: error: {message}")


_REFERENCE = re.compile(r"(?<![\w.])[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*")
_PLACEHOLDER = "_pw_enum_ref_"

_BINARY_OPERATORS: dict[type[ast.operator], Callable[[int, int], int]] = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.LShift: operator.lshift,
    ast.RShift: operator.rshift,
    ast.BitOr: operator.or_,
    ast.BitAnd: operator.and_,
    ast.BitXor: operator.xor,
}

_UNARY_OPERATORS: dict[type[ast.unaryop], Callable[[int], int]] = {
    ast.USub: operator.neg,
    ast.UAdd: operator.pos,
    ast.Invert: operator.invert,
}


def evaluate_expression(expression: str, lookup: Callable[[str], int]) -> int:
    """Evaluates an integer expression that may reference other enumerators.

    Supports integer literals (e.g. ``10``, ``0x1F``, ``0b101``), references
    to enumerators (e.g. ``ALPHA``, ``Other.BETA``, ``my.pkg.Other.BETA``),
    parentheses, unary ``- + ~``, and binary ``+ - * << >> | & ^``.

    Raises:
        ValueError: The expression is invalid.
    """
    references: list[str] = []

    def replace(match: re.Match[str]) -> str:
        ref = match.group(0)
        if keyword.iskeyword(ref):
            return ref
        references.append(ref)
        return f"{_PLACEHOLDER}{len(references) - 1}"

    try:
        tree = ast.parse(
            _REFERENCE.sub(replace, expression.strip()), mode="eval"
        )
    except SyntaxError as e:
        raise ValueError(f"invalid expression {expression!r}") from e
    except (RecursionError, MemoryError):
        raise ValueError(
            f"expression {expression!r} is too deeply nested"
        ) from None

    def evaluate(node: ast.AST) -> int:
        if isinstance(node, ast.Expression):
            return evaluate(node.body)
        if (
            isinstance(node, ast.Constant)
            and isinstance(node.value, int)
            and not isinstance(node.value, bool)
        ):
            return node.value
        if isinstance(node, ast.Name) and node.id.startswith(_PLACEHOLDER):
            return lookup(references[int(node.id[len(_PLACEHOLDER) :])])
        if isinstance(node, ast.UnaryOp) and type(node.op) in _UNARY_OPERATORS:
            return _UNARY_OPERATORS[type(node.op)](evaluate(node.operand))
        if isinstance(node, ast.BinOp) and type(node.op) in _BINARY_OPERATORS:
            lhs = evaluate(node.left)
            rhs = evaluate(node.right)
            if isinstance(node.op, (ast.LShift, ast.RShift)) and not (
                0 <= rhs <= _MAX_SHIFT
            ):
                raise ValueError(
                    f"shift amount {rhs} in {expression!r} must be from 0 to "
                    f"{_MAX_SHIFT}"
                )
            return _BINARY_OPERATORS[type(node.op)](lhs, rhs)

        raise ValueError(
            f"unsupported syntax in expression {expression!r}; only integers, "
            "enumerator references, parentheses, and the operators "
            "- + ~ * << >> | & ^ are supported"
        )

    try:
        return evaluate(tree)
    except RecursionError:
        raise ValueError(
            f"expression {expression!r} is too deeply nested"
        ) from None


@dataclass
class _PendingValue:
    canonical_name: str
    cc_name: str
    display_name: str
    expression: int | str | None
    doc: str
    context: str
    resolved: int | None = None


class _Origin(_enum.Enum):
    """Where an enum comes from, which determines what can reference it."""

    SOURCE = _enum.auto()
    DIRECT_DEP = _enum.auto()
    TRANSITIVE_DEP = _enum.auto()


@dataclass
class _PendingEnum:
    source: Path
    namespace: tuple[str, ...]
    name: str
    scopes: tuple[str, ...]
    underlying_type: str
    doc: str
    context: str
    values: list[_PendingValue] = field(default_factory=list)
    status: tuple[str, str] | None = None  # (status name, OK name)
    allow_alias: bool = False
    origin: _Origin = _Origin.SOURCE

    @property
    def canonical_full_name(self) -> str:
        return ".".join(self.namespace + (self.name,))

    @property
    def cc_full_name(self) -> str:
        return "::" + "::".join(self.scopes + (self.name,))


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate key {key!r}")
        result[key] = value
    return result


def _named_context(data: Any, context: str) -> str:
    """Adds an object's name, if it has one, to an error context."""
    if isinstance(data, dict) and isinstance(name := data.get("name"), str):
        return f"{context} ({name})"
    return context


def _check_object(
    obj: Any, allowed: frozenset[str], path: Path, context: str
) -> dict[str, Any]:
    if not isinstance(obj, dict):
        _fail(path, context, "expected a JSON object")
    unknown = sorted(set(obj) - allowed)
    if unknown:
        _fail(
            path,
            context,
            f"unknown field(s) {', '.join(unknown)}; supported fields are "
            f"{', '.join(sorted(allowed))}",
        )
    return obj


def _get_str(
    obj: dict[str, Any],
    key: str,
    path: Path,
    context: str,
    *,
    required: bool = False,
) -> str:
    if key not in obj:
        if required:
            _fail(path, context, f"missing required field '{key}'")
        return ""
    value = obj[key]
    if not isinstance(value, str):
        _fail(path, context, f"'{key}' must be a string")
    if required and not value:
        _fail(path, context, f"'{key}' cannot be empty")
    return value


def _get_doc(obj: dict[str, Any], path: Path, context: str) -> str:
    doc = _get_str(obj, "doc", path, context)
    try:
        list(cpp.doc_comment(doc))  # Check for trailing slashes
    except ValueError:
        _fail(
            path,
            context,
            "'doc' lines cannot end with a backslash, since it would "
            "continue a C++ comment onto the next line",
        )
    return doc


def _check_namespace_parts(
    parts: tuple[str, ...], message: str, path: Path, context: str
) -> tuple[str, ...]:
    if not parts or not all(cpp.is_identifier(p) for p in parts):
        _fail(path, context, message)
    return parts


def _parse_namespace(text: str, path: Path, context: str) -> tuple[str, ...]:
    return _check_namespace_parts(
        tuple(text.split(".")),
        f"invalid namespace {text!r}; namespaces are dot-separated "
        "identifiers (e.g. my.pkg) that are not C++ reserved words",
        path,
        context,
    )


def _parse_cc_namespace(text: str, path: Path, context: str) -> tuple[str, ...]:
    return _check_namespace_parts(
        tuple(text.removeprefix("::").split("::")),
        f"invalid C++ namespace {text!r}; C++ namespaces are "
        "'::'-separated identifiers (e.g. my::pkg) that are not C++ reserved "
        "words",
        path,
        context,
    )


def _check_type_name(name: str, kind: str, path: Path, context: str) -> None:
    if (
        not _ENUM_NAME.match(name)
        or not python.is_identifier(name)
        or not cpp.is_identifier(name)
    ):
        _fail(
            path,
            context,
            f"{kind} name {name!r} must be PascalCase and not a reserved word",
        )


def _parse_value(data: Any, path: Path, context: str) -> _PendingValue:
    context = _named_context(data, context)
    obj = _check_object(data, _VALUE_KEYS, path, context)
    name = _get_str(obj, "name", path, context, required=True)
    if not _VALUE_NAME.match(name):
        _fail(
            path,
            context,
            f"enumerator name {name!r} must be UPPER_SNAKE_CASE "
            "(e.g. MY_VALUE)",
        )

    cc_name = cpp.constant_style(name)
    if "cc" in obj:
        cc = _check_object(obj["cc"], _VALUE_CC_KEYS, path, f"{context}.cc")
        if "name" in cc:
            cc_name = _get_str(cc, "name", path, f"{context}.cc", required=True)
            if not cpp.is_identifier(cc_name):
                _fail(
                    path,
                    f"{context}.cc",
                    f"invalid C++ name {cc_name!r}; must be a C++ identifier "
                    "that is not a reserved word",
                )

    display_name = name
    if "display_name" in obj:
        display_name = _get_str(
            obj, "display_name", path, context, required=True
        )
        if "|" in display_name:
            _fail(
                path,
                context,
                f"display name {display_name!r} cannot contain '|', which "
                "separates the display names of aliases",
            )
        if not display_name.isprintable():
            _fail(
                path,
                context,
                f"display name {display_name!r} must only contain printable "
                "characters (no newlines or other control characters)",
            )

    expression = obj.get("value")
    if expression is not None and (
        isinstance(expression, bool) or not isinstance(expression, (int, str))
    ):
        _fail(
            path,
            context,
            "'value' must be an integer or a string expression",
        )

    return _PendingValue(
        canonical_name=name,
        cc_name=cc_name,
        display_name=display_name,
        expression=expression,
        doc=_get_doc(obj, path, context),
        context=context,
    )


def _parse_enum(
    data: Any,
    path: Path,
    context: str,
    namespace: tuple[str, ...],
    scopes: tuple[str, ...],
) -> _PendingEnum:
    context = _named_context(data, context)
    obj = _check_object(data, _ENUM_KEYS, path, context)
    name = _get_str(obj, "name", path, context, required=True)
    _check_type_name(name, "enum", path, context)

    underlying_type = DEFAULT_INT_TYPE
    if "type" in obj:
        underlying_type = _get_str(obj, "type", path, context, required=True)
        if underlying_type not in cpp.INT_TYPES:
            _fail(
                path,
                context,
                f"unsupported type {underlying_type!r}; supported types are "
                f"{', '.join(cpp.INT_TYPES)}",
            )

    enum = _PendingEnum(
        source=path,
        namespace=namespace,
        name=name,
        scopes=scopes,
        underlying_type=underlying_type,
        doc=_get_doc(obj, path, context),
        context=context,
    )

    if "allow_alias" in obj:
        if not isinstance(obj["allow_alias"], bool):
            _fail(path, context, "'allow_alias' must be a boolean")
        enum.allow_alias = obj["allow_alias"]

    values = obj.get("values")
    if not isinstance(values, list) or not values:
        _fail(path, context, "'values' must be a non-empty list")

    canonical_names: set[str] = set()
    cc_names: set[str] = set()
    for i, value_data in enumerate(values):
        value = _parse_value(value_data, path, f"{context}.values[{i}]")
        if value.canonical_name in canonical_names:
            _fail(
                path,
                value.context,
                f"enumerator {value.canonical_name} is defined multiple times",
            )
        if value.cc_name in cc_names:
            _fail(
                path,
                value.context,
                f"C++ enumerator name {value.cc_name} is used multiple times",
            )
        canonical_names.add(value.canonical_name)
        cc_names.add(value.cc_name)
        enum.values.append(value)

    if "status" in obj:
        status_context = f"{context}.status"
        status = _check_object(
            obj["status"], _STATUS_KEYS, path, status_context
        )
        status_name = _get_str(
            status, "name", path, status_context, required=True
        )
        ok_name = _get_str(status, "ok", path, status_context, required=True)
        _check_type_name(status_name, "status", path, status_context)
        if status_name == name:
            _fail(
                path,
                status_context,
                f"status and enum cannot have the same name ({name})",
            )
        if ok_name not in canonical_names:
            _fail(
                path,
                status_context,
                f"OK code {ok_name!r} is not an enumerator of {name}",
            )
        enum.status = (status_name, ok_name)

    return enum


def _load_source(path: Path) -> list[_PendingEnum]:
    try:
        with path.open(encoding="utf-8") as file:
            data = json.load(file, object_pairs_hook=_reject_duplicate_keys)
    except OSError as e:
        _fail(path, "", f"unable to read file: {e.strerror or e}")
    except (json.JSONDecodeError, ValueError) as e:
        _fail(path, "", f"invalid JSON: {e}")

    obj = _check_object(data, _FILE_KEYS, path, "")
    _get_str(obj, "$schema", path, "")
    namespace = _parse_namespace(
        _get_str(obj, "namespace", path, "", required=True), path, "namespace"
    )
    scopes = namespace
    if "cc" in obj:
        cc = _check_object(obj["cc"], _FILE_CC_KEYS, path, "cc")
        if "namespace" in cc:
            scopes = _parse_cc_namespace(
                _get_str(cc, "namespace", path, "cc", required=True),
                path,
                "cc.namespace",
            )

    enums = obj.get("enums")
    if not isinstance(enums, list) or not enums:
        _fail(path, "", "'enums' must be a non-empty list")

    return [
        _parse_enum(enum, path, f"enums[{i}]", namespace, scopes)
        for i, enum in enumerate(enums)
    ]


def _find_anchor(values: Sequence[_PendingValue], index: int) -> int:
    """Finds the nearest preceding enumerator with a known or explicit value."""
    for i in range(index, 0, -1):
        if values[i].resolved is not None or values[i].expression is not None:
            return i
    return 0


class _Resolver:
    """Resolves enumerator values, including references across enums."""

    def __init__(self) -> None:
        # Maps canonical enumerator names to pending enumerators.
        self._symbols: dict[str, tuple[_PendingEnum, int]] = {}
        self._enum_names: dict[str, Path] = {}
        self._cc_names: dict[str, Path] = {}
        self._in_progress: set[tuple[int, int]] = set()

    def add(self, enum: _PendingEnum) -> None:
        """Registers an enum and its enumerators for resolution."""
        full_name = enum.canonical_full_name
        names = [full_name]
        cc_names = [enum.cc_full_name]
        if enum.status:
            status_name = enum.status[0]
            names.append(".".join(enum.namespace + (status_name,)))
            cc_names.append("::" + "::".join(enum.scopes + (status_name,)))

        for name in names:
            if (other := self._enum_names.get(name)) is not None:
                _fail(
                    enum.source,
                    enum.context,
                    f"enum {name} is already defined in {other}",
                )
            self._enum_names[name] = enum.source

        # C++ defines the enum and its status class in the same namespace. This
        # also prevents conflicts in languages like Python, which name the enum
        # after its status, since every enum in a file has the same namespace.
        for cc_name in cc_names:
            if (other := self._cc_names.get(cc_name)) is not None:
                _fail(
                    enum.source,
                    enum.context,
                    f"C++ name {cc_name} is also defined in {other}",
                )
            self._cc_names[cc_name] = enum.source

        for i, value in enumerate(enum.values):
            self._symbols[f"{full_name}.{value.canonical_name}"] = (enum, i)

    def resolve(self, enum: _PendingEnum, index: int) -> int:
        """Returns the value of an enumerator, resolving it if necessary."""
        values = enum.values
        if (resolved := values[index].resolved) is not None:
            return resolved

        # Implicit values are the previous value + 1. Resolve iteratively from
        # the nearest anchor so long runs of implicit values cannot exceed the
        # recursion limit.
        start = _find_anchor(values, index)
        if (base := values[start].resolved) is None:
            base = self._evaluate(enum, start)
            values[start].resolved = base

        for offset, i in enumerate(range(start + 1, index + 1), start=1):
            values[i].resolved = base + offset

        return base + (index - start)

    def _evaluate(self, enum: _PendingEnum, index: int) -> int:
        value = enum.values[index]
        if value.expression is None:
            return 0  # The first enumerator defaults to 0.
        if isinstance(value.expression, int):
            return value.expression

        key = (id(enum), index)
        if key in self._in_progress:
            _fail(
                enum.source,
                value.context,
                f"circular reference while evaluating {value.canonical_name}",
            )

        self._in_progress.add(key)
        try:
            return evaluate_expression(
                value.expression, lambda ref: self._lookup(enum, value, ref)
            )
        except ValueError as e:
            _fail(enum.source, value.context, str(e))
        except RecursionError:
            _fail(
                enum.source,
                value.context,
                f"reference chain is too deep while evaluating "
                f"{value.canonical_name}",
            )
        finally:
            self._in_progress.discard(key)

    def _lookup(
        self, enum: _PendingEnum, value: _PendingValue, reference: str
    ) -> int:
        dots = reference.count(".")
        if dots == 0:
            symbol_name = f"{enum.canonical_full_name}.{reference}"
        elif dots == 1:
            symbol_name = f"{'.'.join(enum.namespace)}.{reference}"
        else:
            symbol_name = reference

        if (symbol := self._symbols.get(symbol_name)) is None:
            _fail(
                enum.source,
                value.context,
                f"unknown enumerator {reference!r}; references must name an "
                "enumerator in the same enum (e.g. ALPHA), an enum in the "
                "same namespace (e.g. Other.ALPHA), or a fully qualified "
                "enum (e.g. my.pkg.Other.ALPHA) from this target or its deps",
            )

        target, index = symbol
        if (
            enum.origin is not _Origin.SOURCE
            and target.origin is _Origin.SOURCE
        ):
            _fail(
                enum.source,
                value.context,
                f"dependency cannot reference enumerator {reference!r} in "
                f"downstream source {target.source}",
            )
        # Like proto_library, sources may only reference enums in their
        # own target or its direct deps.
        if (
            enum.origin is _Origin.SOURCE
            and target.origin is _Origin.TRANSITIVE_DEP
        ):
            _fail(
                enum.source,
                value.context,
                f"{reference} is defined in {target.source}, which is "
                "not a direct dependency; add its pw_enum target to deps",
            )
        return self.resolve(target, index)


def _same_enum_target(
    expression: int | str | None, enum: _PendingEnum
) -> str | None:
    """If expression directly names an enumerator in enum, returns its name."""
    if not isinstance(expression, str):
        return None
    ref = expression.strip()
    for prefix in ("", f"{enum.name}.", f"{enum.canonical_full_name}."):
        if ref.startswith(prefix):
            candidate = ref[len(prefix) :]
            if "." not in candidate:
                return candidate
    return None


def _check_aliases(enum: _PendingEnum) -> None:
    """Requires enumerators that share a value to be intentional aliases.

    Like protobuf, duplicate values are errors unless the enum sets
    allow_alias. Without allow_alias, an enumerator may only share a value if
    its value is the name of another enumerator in the same enum, which makes
    the alias explicit.
    """
    if enum.allow_alias:
        return

    groups: dict[int, list[_PendingValue]] = {}
    for value in enum.values:
        assert value.resolved is not None
        groups.setdefault(value.resolved, []).append(value)

    for group in groups.values():
        names = frozenset(v.canonical_name for v in group)
        originals = [
            v
            for v in group
            if _same_enum_target(v.expression, enum)
            not in (names - {v.canonical_name})
        ]
        if len(originals) > 1:
            first, duplicate = originals[:2]
            _fail(
                enum.source,
                duplicate.context,
                f"value {duplicate.resolved} is already used by "
                f"{first.canonical_name}; to define an alias, set 'value' to "
                f"{first.canonical_name!r}, or set 'allow_alias' to true on "
                f"{enum.name}",
            )


def _check_display_names(enum: _PendingEnum) -> None:
    """Requires enumerators with different values to have distinct names."""
    first_with_name: dict[str, _PendingValue] = {}
    for value in enum.values:
        first = first_with_name.setdefault(value.display_name, value)
        if first.resolved != value.resolved:
            _fail(
                enum.source,
                value.context,
                f"display name {value.display_name!r} is already used by "
                f"{first.canonical_name}, which has a different value; "
                "display names must be unique",
            )


def _finish_enum(enum: _PendingEnum) -> EnumDescriptor:
    int_type = cpp.INT_TYPES[enum.underlying_type]
    values: list[EnumValue] = []
    for value in enum.values:
        assert value.resolved is not None
        if not int_type.min_value <= value.resolved <= int_type.max_value:
            _fail(
                enum.source,
                value.context,
                f"value {value.resolved} does not fit in "
                f"{enum.underlying_type} ({int_type.min_value} to "
                f"{int_type.max_value})",
            )
        values.append(
            EnumValue(
                cc_name=value.cc_name,
                value=value.resolved,
                name=value.display_name,
                canonical_name=value.canonical_name,
                doc=value.doc,
            )
        )

    _check_aliases(enum)
    _check_display_names(enum)

    status = None
    if enum.status is not None:
        status_name, ok_name = enum.status
        ok = next(v for v in values if v.canonical_name == ok_name)
        for v in values:
            if (
                is_ok_name(v.canonical_name) or is_ok_name(v.cc_name)
            ) and v.value != ok.value:
                _fail(
                    enum.source,
                    f"{enum.context}.status",
                    f"{v.canonical_name} is not the OK code ({ok_name}); "
                    "codes named OK must be the OK code since they conflict "
                    "with ok()",
                )
        status = StatusInfo(
            name=status_name,
            scopes=enum.scopes,
            cc_full_name="::" + "::".join(enum.scopes + (status_name,)),
            ok_code=ok.cc_name,
        )

    return EnumDescriptor(
        name=enum.name,
        scopes=enum.scopes,
        cc_full_name=enum.cc_full_name,
        line=0,
        values=tuple(values),
        status=status,
        namespace=enum.namespace,
        underlying_type=enum.underlying_type,
        doc=enum.doc,
    )


def _load_unique(
    paths: Sequence[Path], origin: _Origin, loaded: set[Path]
) -> list[list[_PendingEnum]]:
    result = []
    for path in paths:
        if path in loaded:
            continue
        loaded.add(path)
        enums = _load_source(path)
        for enum in enums:
            enum.origin = origin
        result.append(enums)
    return result


def resolve_sources(
    sources: Sequence[Path],
    deps: Sequence[Path] = (),
    transitive_deps: Sequence[Path] = (),
) -> list[tuple[EnumDescriptor, ...]]:
    """Loads and resolves JSON enum sources.

    Enumerators may reference enumerators in any of the sources or in direct
    dependencies.

    Args:
        sources: The JSON source files to load and resolve.
        deps: JSON sources of direct dependencies, which sources may
            reference.
        transitive_deps: JSON sources of all transitive dependencies, which
            dependencies may reference. May include sources and deps.

    Returns:
        The resolved enums for each source.

    Raises:
        SourceError: A source is invalid.
    """
    loaded_paths: set[Path] = set()
    for path in sources:
        if path in loaded_paths:
            _fail(path, "", "duplicate source file")
        loaded_paths.add(path)

    loaded = [_load_source(path) for path in sources]

    dependencies = _load_unique(deps, _Origin.DIRECT_DEP, loaded_paths)
    dependencies += _load_unique(
        transitive_deps, _Origin.TRANSITIVE_DEP, loaded_paths
    )

    resolver = _Resolver()
    for enums in [*dependencies, *loaded]:
        for enum in enums:
            resolver.add(enum)

    for enums in loaded:
        for enum in enums:
            for i in range(len(enum.values)):
                resolver.resolve(enum, i)

    return [tuple(_finish_enum(enum) for enum in enums) for enums in loaded]
