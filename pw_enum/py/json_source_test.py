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
"""Tests for loading and resolving pw_enum JSON sources."""

import json
from pathlib import Path
import tempfile
from typing import Any
import unittest

from pw_enum.json_source import (
    evaluate_expression,
    resolve_sources,
    SourceError,
)
from pw_enum.parse import EnumDescriptor, EnumValue


def _values(enums: tuple[EnumDescriptor, ...]) -> dict[str, int]:
    return {
        f"{e.name}.{v.canonical_name}": v.value for e in enums for v in e.values
    }


class JsonSourceTestBase(unittest.TestCase):
    """Writes JSON sources to a temporary directory."""

    def setUp(self) -> None:
        self._temp_dir = tempfile.TemporaryDirectory()
        self._dir = Path(self._temp_dir.name)

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def _write(self, name: str, data: Any) -> Path:
        path = self._dir / name
        if isinstance(data, str):
            path.write_text(data, encoding="utf-8")
        else:
            path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def _resolve(
        self,
        data: Any,
        deps: tuple[Path, ...] = (),
        transitive_deps: tuple[Path, ...] = (),
    ) -> tuple[EnumDescriptor, ...]:
        path = self._write("test.json", data)
        (result,) = resolve_sources([path], deps, transitive_deps)
        return result

    def _assert_error(self, data: Any, message: str) -> None:
        with self.assertRaisesRegex(SourceError, message):
            self._resolve(data)


def _file(*enums: dict[str, Any], **fields: Any) -> dict[str, Any]:
    return {"namespace": "my.pkg", "enums": list(enums), **fields}


def _enum(name: str, *values: dict[str, Any], **fields: Any) -> dict[str, Any]:
    return {"name": name, "values": list(values), **fields}


class TestResolve(JsonSourceTestBase):
    """Tests resolving enumerator values and names."""

    def test_basic(self) -> None:
        result = self._resolve(
            _file(
                _enum(
                    "MyEnum",
                    {"name": "ALPHA"},
                    {"name": "BETA"},
                    {"name": "GAMMA", "value": 10},
                    {"name": "DELTA"},
                    type="uint8",
                    doc="Docs!",
                )
            )
        )
        (enum,) = result
        self.assertEqual(enum.name, "MyEnum")
        self.assertEqual(enum.namespace, ("my", "pkg"))
        self.assertEqual(enum.scopes, ("my", "pkg"))
        self.assertEqual(enum.cc_full_name, "::my::pkg::MyEnum")
        self.assertEqual(enum.underlying_type, "uint8")
        self.assertEqual(enum.doc, "Docs!")
        self.assertEqual(
            enum.values,
            (
                EnumValue("kAlpha", 0, "ALPHA", "ALPHA"),
                EnumValue("kBeta", 1, "BETA", "BETA"),
                EnumValue("kGamma", 10, "GAMMA", "GAMMA"),
                EnumValue("kDelta", 11, "DELTA", "DELTA"),
            ),
        )

    def test_default_type(self) -> None:
        (enum,) = self._resolve(_file(_enum("E", {"name": "A"})))
        self.assertEqual(enum.underlying_type, "int32")

    def test_overrides(self) -> None:
        result = self._resolve(
            _file(
                _enum(
                    "E",
                    {
                        "name": "IO_ERROR",
                        "display_name": "io error",
                        "cc": {"name": "kIOError"},
                        "doc": "An I/O error.",
                    },
                ),
                cc={"namespace": "::other::ns"},
            )
        )
        (enum,) = result
        self.assertEqual(enum.scopes, ("other", "ns"))
        self.assertEqual(enum.namespace, ("my", "pkg"))
        self.assertEqual(enum.cc_full_name, "::other::ns::E")
        self.assertEqual(
            enum.values,
            (
                EnumValue(
                    "kIOError", 0, "io error", "IO_ERROR", "An I/O error."
                ),
            ),
        )

    def test_references_within_and_across_enums(self) -> None:
        result = self._resolve(
            _file(
                _enum(
                    "First",
                    {"name": "A", "value": "Second.Y + 1"},
                    {"name": "B"},
                    {"name": "ALIAS_B", "value": "B"},
                    {"name": "C", "value": "my.pkg.Second.Z << 2"},
                    {"name": "D", "value": "-Second.X"},
                ),
                _enum(
                    "Second",
                    {"name": "X", "value": 3},
                    {"name": "Y"},
                    {"name": "Z", "value": "(Y | 0x11) & ~1"},
                ),
            )
        )
        self.assertEqual(
            _values(result),
            {
                "First.A": 5,
                "First.B": 6,
                "First.ALIAS_B": 6,
                "First.C": 80,
                "First.D": -3,
                "Second.X": 3,
                "Second.Y": 4,
                "Second.Z": 20,
            },
        )

    def test_references_to_dependencies(self) -> None:
        dep_path = self._write(
            "dep.json",
            {
                "namespace": "other.pkg",
                "enums": [_enum("Dep", {"name": "BASE", "value": 100})],
            },
        )
        result = self._resolve(
            _file(_enum("E", {"name": "A", "value": "other.pkg.Dep.BASE + 1"})),
            deps=(dep_path,),
            transitive_deps=(dep_path,),
        )
        self.assertEqual(_values(result), {"E.A": 101})

    def test_dependencies_resolve_their_own_references(self) -> None:
        base = self._write(
            "base.json",
            {
                "namespace": "base",
                "enums": [_enum("Base", {"name": "X", "value": 7})],
            },
        )
        dep = self._write(
            "dep.json",
            {
                "namespace": "dep",
                "enums": [
                    _enum("Dep", {"name": "Y", "value": "base.Base.X * 2"})
                ],
            },
        )
        result = self._resolve(
            _file(_enum("E", {"name": "A", "value": "dep.Dep.Y + 1"})),
            deps=(dep,),
            transitive_deps=(base, dep),
        )
        self.assertEqual(_values(result), {"E.A": 15})

    def test_references_across_sources(self) -> None:
        first = self._write(
            "first.json",
            _file(_enum("First", {"name": "A", "value": "Second.B"})),
        )
        second = self._write(
            "second.json",
            _file(_enum("Second", {"name": "B", "value": 5})),
        )
        result = resolve_sources([first, second])
        self.assertEqual(
            [_values(r) for r in result], [{"First.A": 5}, {"Second.B": 5}]
        )

    def test_long_implicit_chain(self) -> None:
        values = [{"name": f"V{i}"} for i in range(5000)]
        result = self._resolve(_file(_enum("E", *values)))
        self.assertEqual(result[0].values[-1].value, 4999)

    def test_status(self) -> None:
        result = self._resolve(
            _file(
                _enum(
                    "Code",
                    {"name": "OK"},
                    {"name": "FAILED"},
                    status={"name": "MyStatus", "ok": "OK"},
                )
            )
        )
        status = result[0].status
        assert status is not None
        self.assertEqual(status.name, "MyStatus")
        self.assertEqual(status.scopes, ("my", "pkg"))
        self.assertEqual(status.cc_full_name, "::my::pkg::MyStatus")
        self.assertEqual(status.ok_code, "kOk")

    def test_same_version_as_header_enum(self) -> None:
        """JSON and header enums with the same contents share a version."""
        (json_enum,) = self._resolve(
            _file(_enum("MyEnum", {"name": "ALPHA"}, {"name": "BETA"}))
        )
        header_enum = EnumDescriptor(
            name="MyEnum",
            scopes=("my", "pkg"),
            cc_full_name="::my::pkg::MyEnum",
            line=12,
            values=(
                EnumValue(cc_name="kAlpha", value=0, name="ALPHA"),
                EnumValue(cc_name="kBeta", value=1, name="BETA"),
            ),
        )
        self.assertEqual(json_enum.version, header_enum.version)
        self.assertEqual(
            json_enum.cc_versioned_name, header_enum.cc_versioned_name
        )

    def test_explicit_aliases(self) -> None:
        result = self._resolve(
            _file(
                _enum(
                    "E",
                    {"name": "ALIAS_A", "value": "A"},
                    {"name": "A", "value": 1},
                    {"name": "ALIAS_ALIAS_A", "value": " ALIAS_A "},
                    {"name": "QUALIFIED_A", "value": "E.A"},
                    {"name": "FULLY_QUALIFIED_A", "value": "my.pkg.E.A"},
                    {"name": "B"},
                )
            )
        )
        self.assertEqual(
            _values(result),
            {
                "E.ALIAS_A": 1,
                "E.A": 1,
                "E.ALIAS_ALIAS_A": 1,
                "E.QUALIFIED_A": 1,
                "E.FULLY_QUALIFIED_A": 1,
                "E.B": 2,
            },
        )

    def test_allow_alias(self) -> None:
        result = self._resolve(
            _file(
                _enum(
                    "E",
                    {"name": "A", "value": 1},
                    {"name": "B", "value": 2},
                    {"name": "C", "value": 1},
                    {"name": "D"},
                    allow_alias=True,
                )
            )
        )
        self.assertEqual(
            _values(result), {"E.A": 1, "E.B": 2, "E.C": 1, "E.D": 2}
        )

    def test_aliases_may_share_display_names(self) -> None:
        (enum,) = self._resolve(
            _file(
                _enum(
                    "E",
                    {"name": "A", "display_name": "same"},
                    {"name": "B", "value": "A", "display_name": "same"},
                )
            )
        )
        self.assertEqual([v.name for v in enum.values], ["same", "same"])

    def test_display_name_may_contain_spaces_and_unicode(self) -> None:
        (enum,) = self._resolve(
            _file(_enum("E", {"name": "A", "display_name": "très bien!"}))
        )
        self.assertEqual(enum.values[0].name, "très bien!")


class TestErrors(JsonSourceTestBase):  # pylint: disable=too-many-public-methods
    """Tests errors for invalid sources."""

    def test_missing_file(self) -> None:
        missing = self._dir / "missing.json"
        with self.assertRaisesRegex(
            SourceError, r"missing\.json: error: unable to read file"
        ):
            resolve_sources([missing])

    def test_invalid_json(self) -> None:
        self._assert_error("{", "invalid JSON")

    def test_duplicate_keys(self) -> None:
        self._assert_error(
            '{"namespace": "a", "namespace": "b", "enums": []}',
            "duplicate key 'namespace'",
        )

    def test_unknown_field(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "A", "val": 1})),
            r"enums\[0\] \(E\)\.values\[0\] \(A\): error: unknown field",
        )
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "OK"},
                    cc={"status": {"name": "S", "ok": "OK"}},
                )
            ),
            r"enums\[0\] \(E\): error: unknown field\(s\) cc",
        )
        self._assert_error(
            _file(_enum("E", {"name": "OK"}, status={"name": "S", "x": 1})),
            r"enums\[0\] \(E\)\.status: error: unknown field\(s\) x",
        )

    def test_missing_namespace(self) -> None:
        self._assert_error({"enums": []}, "missing required field 'namespace'")

    def test_invalid_namespace(self) -> None:
        self._assert_error(
            {"namespace": "my.class", "enums": [_enum("E", {"name": "A"})]},
            "invalid namespace",
        )

    def test_no_enums(self) -> None:
        self._assert_error(_file(), "'enums' must be a non-empty list")

    def test_no_values(self) -> None:
        self._assert_error(_file(_enum("E")), "'values' must be")

    def test_invalid_enum_name(self) -> None:
        self._assert_error(_file(_enum("my_enum", {"name": "A"})), "PascalCase")

    def test_invalid_enumerator_name(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "kAlpha"})), "UPPER_SNAKE_CASE"
        )

    def test_duplicate_enumerator(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "A"}, {"name": "A"})),
            "defined multiple times",
        )

    def test_duplicate_cc_name(self) -> None:
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "A_B"},
                    {"name": "AB", "cc": {"name": "kAB"}},
                    {"name": "A_B_C", "cc": {"name": "kAB"}},
                )
            ),
            "kAB is used multiple times",
        )

    def test_duplicate_enum(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "A"}), _enum("E", {"name": "B"})),
            "already defined",
        )

    def test_unsupported_type(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "A"}, type="char")), "unsupported type"
        )

    def test_value_out_of_range(self) -> None:
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "A", "value": 255},
                    {"name": "B"},
                    type="uint8",
                )
            ),
            r"value 256 does not fit in uint8",
        )

    def test_bool_value(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "A", "value": True})),
            "must be an integer",
        )

    def test_unknown_reference(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "A", "value": "Missing.X"})),
            "unknown enumerator 'Missing.X'",
        )

    def test_reference_to_transitive_dep(self) -> None:
        base = self._write(
            "base.json",
            {"namespace": "base", "enums": [_enum("Base", {"name": "X"})]},
        )
        with self.assertRaisesRegex(SourceError, "not a direct dependency"):
            self._resolve(
                _file(_enum("E", {"name": "A", "value": "base.Base.X"})),
                transitive_deps=(base,),
            )

    def test_dep_cannot_reference_source(self) -> None:
        dep = self._write(
            "dep.json",
            {
                "namespace": "dep",
                "enums": [_enum("Dep", {"name": "X", "value": "my.pkg.E.A"})],
            },
        )
        with self.assertRaisesRegex(
            SourceError, "dependency cannot reference enumerator"
        ):
            self._resolve(
                _file(
                    _enum(
                        "E",
                        {"name": "A", "value": 1},
                        {"name": "B", "value": "dep.Dep.X"},
                    )
                ),
                deps=(dep,),
            )

    def test_duplicate_sources(self) -> None:
        source = self._write("dup.json", _file(_enum("E", {"name": "A"})))
        with self.assertRaisesRegex(
            SourceError, r"dup\.json: error: duplicate source file"
        ):
            resolve_sources([source, source])

    def test_enum_defined_in_source_and_dep(self) -> None:
        dep = self._write("dep.json", _file(_enum("E", {"name": "A"})))
        with self.assertRaisesRegex(SourceError, "already defined"):
            self._resolve(_file(_enum("E", {"name": "B"})), deps=(dep,))

    def test_circular_reference(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "A", "value": "B"}, {"name": "B"})),
            "circular reference",
        )

    def test_status_unknown_ok(self) -> None:
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "A"},
                    status={"name": "S", "ok": "OK"},
                )
            ),
            "OK code 'OK' is not an enumerator",
        )

    def test_status_ok_name_must_be_ok_code(self) -> None:
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "SUCCESS"},
                    {"name": "OK"},
                    status={"name": "S", "ok": "SUCCESS"},
                )
            ),
            "OK is not the OK code",
        )
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "SUCCESS"},
                    {"name": "OK", "cc": {"name": "kOther"}},
                    status={"name": "S", "ok": "SUCCESS"},
                )
            ),
            "OK is not the OK code",
        )

    def test_status_name_conflicts_with_enum(self) -> None:
        self._assert_error(
            _file(
                _enum("S", {"name": "OK"}),
                _enum(
                    "E",
                    {"name": "OK"},
                    status={"name": "S", "ok": "OK"},
                ),
            ),
            r"enum my\.pkg\.S is already defined",
        )

    def test_status_name_same_as_enum(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "OK"}, status={"name": "E", "ok": "OK"})),
            r"enums\[0\] \(E\)\.status: error: status and enum cannot have "
            r"the same name \(E\)",
        )

    def test_cc_name_conflicts_with_dep(self) -> None:
        dep = self._write("dep.json", _file(_enum("E", {"name": "A"})))
        with self.assertRaisesRegex(
            SourceError, r"C\+\+ name ::my::pkg::E is also defined"
        ):
            self._resolve(
                {
                    "namespace": "other.pkg",
                    "cc": {"namespace": "my::pkg"},
                    "enums": [_enum("E", {"name": "B"})],
                },
                deps=(dep,),
            )

    def test_invalid_cc_namespace(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "A"}), cc={"namespace": "my.pkg"}),
            r"C\+\+ namespaces are '::'-separated identifiers",
        )
        self._assert_error(
            _file(_enum("E", {"name": "A"}), cc={"namespace": "::"}),
            r"invalid C\+\+ namespace '::'",
        )

    def test_invalid_status_name(self) -> None:
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "OK"},
                    status={"name": "my_status", "ok": "OK"},
                )
            ),
            "status name 'my_status' must be PascalCase",
        )

    def test_keyword_enum_name(self) -> None:
        self._assert_error(
            _file(_enum("None", {"name": "A"})),
            "enum name 'None' must be PascalCase and not a reserved word",
        )
        self._assert_error(
            _file(_enum("EOF", {"name": "A"})),
            "enum name 'EOF' must be PascalCase and not a reserved word",
        )
        # pw_protobuf-internal names remain valid in pw_enum.
        (enum,) = self._resolve(_file(_enum("Message", {"name": "A"})))
        self.assertEqual(enum.name, "Message")

    def test_reserved_cc_identifiers(self) -> None:
        self._assert_error(
            {"namespace": "my.errno", "enums": [_enum("E", {"name": "A"})]},
            "not C\\+\\+ reserved words",
        )
        self._assert_error(
            _file(_enum("E", {"name": "A", "cc": {"name": "EOF"}})),
            "not a reserved word",
        )

    def test_implicit_alias(self) -> None:
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "A", "value": 1},
                    {"name": "B", "value": 0},
                    {"name": "C"},  # Implicitly 0 + 1 = 1, colliding with A.
                )
            ),
            r"values\[2\] \(C\): error: value 1 is already used by A; to "
            r"define an alias, set 'value' to 'A', or set 'allow_alias' to "
            r"true on E",
        )

    def test_duplicate_value(self) -> None:
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "A", "value": 1},
                    {"name": "B", "value": 2},
                    {"name": "C", "value": 1},
                )
            ),
            r"values\[2\] \(C\): error: value 1 is already used by A",
        )

    def test_alias_through_expression(self) -> None:
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "A", "value": 1},
                    {"name": "B", "value": "A + 0"},
                )
            ),
            r"values\[1\] \(B\): error: value 1 is already used by A",
        )

    def test_alias_of_other_enum_value(self) -> None:
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "A", "value": "Other.X"},
                    {"name": "B", "value": "Other.Y"},
                ),
                _enum("Other", {"name": "X"}, {"name": "Y", "value": "X"}),
            ),
            r"\(B\): error: value 0 is already used by A",
        )

    def test_allow_alias_must_be_bool(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "A"}, allow_alias="yes")),
            "'allow_alias' must be a boolean",
        )

    def test_duplicate_display_name(self) -> None:
        self._assert_error(
            _file(
                _enum(
                    "E",
                    {"name": "A", "display_name": "same"},
                    {"name": "B", "display_name": "same"},
                )
            ),
            r"values\[1\] \(B\): error: display name 'same' is already used "
            r"by A, which has a different value",
        )

    def test_display_name_matches_other_name(self) -> None:
        self._assert_error(
            _file(
                _enum("E", {"name": "A"}, {"name": "B", "display_name": "A"})
            ),
            "display name 'A' is already used by A",
        )

    def test_display_name_with_pipe(self) -> None:
        self._assert_error(
            _file(_enum("E", {"name": "A", "display_name": "A|B"})),
            r"display name 'A\|B' cannot contain '\|'",
        )

    def test_display_name_with_control_characters(self) -> None:
        for display_name in ("a\nb", "a\tb", "a\0b", "\x1b[31m"):
            with self.subTest(display_name=display_name):
                self._assert_error(
                    _file(
                        _enum("E", {"name": "A", "display_name": display_name})
                    ),
                    "must only contain printable characters",
                )

    def test_doc_ends_with_backslash(self) -> None:
        for doc in ("C:\\", "first\\\nsecond", "trailing space\\ "):
            with self.subTest(doc=doc):
                self._assert_error(
                    _file(_enum("E", {"name": "A", "doc": doc})),
                    r"values\[0\] \(A\): error: 'doc' lines cannot end with "
                    "a backslash",
                )
        self._assert_error(
            _file(_enum("E", {"name": "A"}, doc="C:\\")),
            r"enums\[0\] \(E\): error: 'doc' lines cannot end with",
        )

    def test_doc_with_inner_backslash(self) -> None:
        (enum,) = self._resolve(
            _file(_enum("E", {"name": "A", "doc": "C:\\path\\file"}))
        )
        self.assertEqual(enum.values[0].doc, "C:\\path\\file")


class TestExpressions(unittest.TestCase):
    """Tests enumerator value expressions."""

    @staticmethod
    def _eval(expression: str) -> int:
        return evaluate_expression(
            expression, {"A": 3, "x.y.B": 5, "my.in.B": 9}.__getitem__
        )

    def test_literals(self) -> None:
        self.assertEqual(self._eval("42"), 42)
        self.assertEqual(self._eval("0x1F"), 31)
        self.assertEqual(self._eval("0b101"), 5)
        self.assertEqual(self._eval("1_000"), 1000)
        self.assertEqual(self._eval("  7 "), 7)

    def test_operators(self) -> None:
        self.assertEqual(self._eval("-(A + 1) * 2"), -8)
        self.assertEqual(self._eval("1 << A | x.y.B ^ 1"), 12)
        self.assertEqual(self._eval("~0 & 0xF0 >> 4"), 15)
        self.assertEqual(self._eval("my.in.B + 1"), 10)

    def test_invalid(self) -> None:
        for expression in (
            "1.5",
            "A / 2",
            "f(1)",
            "'a'",
            "1 if A else 2",
            "True",
            "False",
            "None",
            "A and 1",
            "not A",
            "A[",
        ):
            with self.subTest(expression=expression):
                with self.assertRaises(ValueError):
                    self._eval(expression)

    def test_shift_limit(self) -> None:
        with self.assertRaisesRegex(ValueError, "shift amount"):
            self._eval("1 << 1000000")

    def test_deeply_nested_expression(self) -> None:
        for expression in ("1 + " * 1600 + "1", "-" * 1500 + "1"):
            with self.subTest(expression=expression[:16]):
                with self.assertRaisesRegex(ValueError, "too deeply nested"):
                    self._eval(expression)


if __name__ == "__main__":
    unittest.main()
