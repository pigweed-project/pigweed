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
import unittest

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


if __name__ == '__main__':
    unittest.main()
