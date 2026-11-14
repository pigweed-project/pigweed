# Copyright 2025 The Pigweed Authors
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
"""Tests for pw_build.generated_tests."""

import io
from pathlib import Path
import tempfile
import unittest

import jinja2

from pw_build import generated_tests


class TestContext(unittest.TestCase):
    def test_cc_name(self) -> None:
        ctx = generated_tests.Context('My Group', 1, 2, None)
        self.assertEqual(ctx.cc_name(), 'MyGroup_1')

        ctx = generated_tests.Context('another-group', 1, 1, None)
        self.assertEqual(ctx.cc_name(), 'AnotherGroup')

    def test_py_name(self) -> None:
        ctx = generated_tests.Context('My Group', 1, 2, None)
        self.assertEqual(ctx.py_name(), 'test_my_group_1')

        ctx = generated_tests.Context('Another Group', 1, 1, None)
        self.assertEqual(ctx.py_name(), 'test_another_group')

    def test_ts_name(self) -> None:
        ctx = generated_tests.Context('My Group', 1, 2, None)
        self.assertEqual(ctx.ts_name(), 'my group 1')

        ctx = generated_tests.Context('Another Group', 1, 1, None)
        self.assertEqual(ctx.ts_name(), 'another group')


class TestTestGenerator(unittest.TestCase):
    """Tests for the TestGenerator class."""

    def test_init_valid(self) -> None:
        cases = ['group', 1]
        gen = generated_tests.TestGenerator(cases)
        # pylint: disable=protected-access
        self.assertEqual(gen._cases, {'group': [1]})
        # pylint: enable=protected-access

    def test_init_invalid_empty(self) -> None:
        with self.assertRaises(generated_tests.Error):
            generated_tests.TestGenerator([])

    def test_init_invalid_first_not_group(self) -> None:
        with self.assertRaises(generated_tests.Error):
            generated_tests.TestGenerator([1])

    def test_init_invalid_empty_group_name(self) -> None:
        with self.assertRaises(generated_tests.Error):
            generated_tests.TestGenerator(['', 1])

    def test_python_tests(self) -> None:
        cases = ['group', 1]
        gen = generated_tests.TestGenerator(cases)

        def define_test(_):
            def test(_):
                pass

            return test

        test_cls = gen.python_tests('MyTests', define_test)
        self.assertTrue(issubclass(test_cls, unittest.TestCase))
        self.assertTrue(hasattr(test_cls, 'test_group'))

    def test_cc_tests(self) -> None:
        cases = ['group', 1]

        def cc_gen(ctx):
            yield f'TEST({ctx.cc_name()}) {{}}'

        gen = generated_tests.TestGenerator(
            cases, cc_test=(cc_gen, '#include "header.h"', '// footer')
        )

        output = io.StringIO()
        gen.cc_tests(output)

        expected = '#include "header.h"\nTEST(Group) {}\n\n// footer\n'
        self.assertEqual(output.getvalue(), expected)

    def test_ts_tests(self) -> None:
        cases = ['group', 1]

        def ts_gen(ctx):
            yield f"it('{ctx.ts_name()}', () => {{}});"

        gen = generated_tests.TestGenerator(
            cases, ts_test=(ts_gen, 'describe("tests", () => {', '});')
        )

        output = io.StringIO()
        gen.ts_tests(output)

        expected = 'describe("tests", () => {\nit(\'group\', () => {});\n});\n'
        self.assertEqual(output.getvalue(), expected)


class TestJinjaMain(unittest.TestCase):
    """Tests for parse_jinja_args and jinja_main."""

    def test_parse_jinja_args(self) -> None:
        args = generated_tests.parse_jinja_args(
            argv=[
                '--yaml',
                'cases.yaml',
                '--template',
                'cases.h.jinja',
                '--output',
                'cases.h',
            ]
        )
        self.assertEqual(args.yaml, Path('cases.yaml'))
        self.assertEqual(args.template, Path('cases.h.jinja'))
        self.assertEqual(args.output, Path('cases.h'))

    def test_jinja_main_renders_with_default_and_extra_filters(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            yaml_path = tmp / 'cases.yaml'
            template_path = tmp / 'cases.rs.jinja'
            output_path = tmp / 'out' / 'cases.rs'

            yaml_path.write_text('unused: true\n', encoding='utf-8')
            template_path.write_text(
                'const S: &str = {{ name | rust_string }};\n'
                'const N: u32 = {{ count | custom_u32 }};\n',
                encoding='utf-8',
            )

            rc = generated_tests.jinja_main(
                lambda p: {'name': p.name, 'count': 7},
                description='Test generator',
                extra_filters={'custom_u32': lambda v: f'{v}_u32'},
                argv=[
                    '--yaml',
                    str(yaml_path),
                    '--template',
                    str(template_path),
                    '--output',
                    str(output_path),
                ],
            )
            self.assertEqual(rc, 0)
            self.assertEqual(
                output_path.read_text(encoding='utf-8'),
                'const S: &str = "cases.yaml";\nconst N: u32 = 7_u32;\n',
            )

    def test_default_jinja_filters(self) -> None:
        """Verifies all built-in DEFAULT_JINJA_FILTERS produce valid code."""
        filters = generated_tests.DEFAULT_JINJA_FILTERS
        with self.assertRaises(TypeError):
            filters['cc_bool'] = lambda _: 'nope'  # type: ignore[index]
        self.assertEqual(filters['cc_bool'](True), 'true')
        self.assertEqual(filters['cc_bytes'](b'\x01\xff'), '{0x01, 0xff}')
        self.assertEqual(filters['cc_char']('A'), "'A'")
        self.assertEqual(filters['cc_f32'](1.5), '1.5f')
        self.assertEqual(
            filters['cc_f32'](float('nan')),
            'std::numeric_limits<float>::quiet_NaN()',
        )
        self.assertEqual(filters['cc_f64'](1.5), '1.5')
        self.assertEqual(filters['cc_f64'](3), '3.0')
        self.assertEqual(
            filters['cc_f64'](float('nan')),
            'std::numeric_limits<double>::quiet_NaN()',
        )
        self.assertEqual(filters['cc_hex_u32'](0xABCD), '0x0000abcdu')
        self.assertEqual(filters['cc_i64'](-5), 'int64_t{-5}')
        self.assertEqual(filters['cc_opt_u32'](None), 'std::nullopt')
        self.assertEqual(filters['cc_opt_u32'](9), 'uint32_t{9}')
        self.assertEqual(filters['cc_string']('hi'), '"hi"')
        self.assertEqual(filters['cc_u32'](10), 'uint32_t{10}')
        self.assertEqual(
            filters['cc_u64'](2**64 - 1),
            'uint64_t{18446744073709551615ull}',
        )
        self.assertEqual(
            filters['java_bytes'](b'\x01\x7f'),
            'new byte[] {(byte) 0x01, (byte) 0x7f}',
        )
        self.assertEqual(filters['java_string']('hi'), '"hi"')
        self.assertEqual(filters['rust_bool'](False), 'false')
        self.assertEqual(filters['rust_bytes'](b'\x01\x02'), '&[1, 2]')
        self.assertEqual(filters['rust_char']('é'), "'é'")
        self.assertEqual(filters['rust_f32'](2.5), '2.5f32')
        self.assertEqual(filters['rust_f64'](2.5), '2.5f64')
        self.assertEqual(filters['rust_f64'](3), '3.0f64')
        self.assertEqual(filters['rust_hex_u32'](0xABCD), '0x0000abcdu32')
        self.assertEqual(filters['rust_i64'](-5), '-5i64')
        self.assertEqual(filters['rust_opt_u32'](None), 'None')
        self.assertEqual(filters['rust_opt_u32'](9), 'Some(9u32)')
        self.assertEqual(filters['rust_string']('hi'), '"hi"')
        self.assertEqual(filters['rust_u32'](10), '10u32')
        self.assertEqual(
            filters['rust_u64'](2**64 - 1),
            '18446744073709551615u64',
        )
        self.assertEqual(
            filters['ts_bytes'](b'\x01\xfe'),
            'new Uint8Array([0x01, 0xfe])',
        )

    def test_default_jinja_filters_reject_out_of_range_inputs(self) -> None:
        """Verifies DEFAULT_JINJA_FILTERS reject out-of-range numeric values."""
        filters = generated_tests.DEFAULT_JINJA_FILTERS
        for hex_filter in ('cc_hex_u32', 'rust_hex_u32'):
            for bad_hex in (-1, 2**32):
                with self.subTest(filter=hex_filter, bad=bad_hex):
                    with self.assertRaises(ValueError):
                        filters[hex_filter](bad_hex)
        for u32_filter in ('cc_u32', 'rust_u32', 'cc_opt_u32', 'rust_opt_u32'):
            for bad_u32 in (-1, 2**32):
                with self.subTest(filter=u32_filter, bad=bad_u32):
                    with self.assertRaises(ValueError):
                        filters[u32_filter](bad_u32)
        for u64_filter in ('cc_u64', 'rust_u64'):
            for bad_u64 in (-1, 2**64):
                with self.subTest(filter=u64_filter, bad=bad_u64):
                    with self.assertRaises(ValueError):
                        filters[u64_filter](bad_u64)
        for i64_filter in ('cc_i64', 'rust_i64'):
            for bad_i64 in (-(2**63) - 1, 2**63):
                with self.subTest(filter=i64_filter, bad=bad_i64):
                    with self.assertRaises(ValueError):
                        filters[i64_filter](bad_i64)
        for f32_filter in ('cc_f32', 'rust_f32'):
            with self.subTest(filter=f32_filter):
                with self.assertRaises(ValueError):
                    filters[f32_filter](1e300)

    def test_jinja_main_override_and_strict_undefined(self) -> None:
        """Verifies extra_filters override defaults and StrictUndefined runs."""
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            yaml_path = root / 'cases.yaml'
            template_path = root / 'template.jinja'
            output_path = root / 'out.txt'
            yaml_path.write_text('ignored', encoding='utf-8')
            template_path.write_text('{{ x | cc_u32 }}\n', encoding='utf-8')
            rc = generated_tests.jinja_main(
                lambda _: {'x': 5},
                extra_filters={'cc_u32': lambda v: f'custom({v})'},
                argv=[
                    '--yaml',
                    str(yaml_path),
                    '--template',
                    str(template_path),
                    '--output',
                    str(output_path),
                ],
            )
            self.assertEqual(rc, 0)
            self.assertEqual(
                output_path.read_text(encoding='utf-8'), 'custom(5)\n'
            )

            template_path.write_text('{{ missing_var }}\n', encoding='utf-8')
            with self.assertRaises(jinja2.UndefinedError):
                generated_tests.jinja_main(
                    lambda _: {},
                    argv=[
                        '--yaml',
                        str(yaml_path),
                        '--template',
                        str(template_path),
                        '--output',
                        str(output_path),
                    ],
                )


if __name__ == '__main__':
    unittest.main()
