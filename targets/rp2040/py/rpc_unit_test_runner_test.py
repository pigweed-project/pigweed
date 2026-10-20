#!/usr/bin/env python3
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
"""Tests for parse_test_filters in rpc_unit_test_runner."""

import unittest

from rp2040_utils.rpc_unit_test_runner import parse_test_filters


class ParseTestFiltersTest(unittest.TestCase):
    """Tests parse_test_filters parsing and validation logic."""

    def test_empty_filters(self) -> None:
        """Empty input returns no test filters (run all)."""
        self.assertEqual(parse_test_filters([]), [])
        self.assertEqual(parse_test_filters(['']), [])
        self.assertEqual(parse_test_filters(['   ']), [])
        self.assertEqual(parse_test_filters([':']), [])
        self.assertEqual(parse_test_filters(['::']), [])

    def test_run_all_wildcards(self) -> None:
        """Wildcards '*' and '*.*' indicate running all tests."""
        self.assertEqual(parse_test_filters(['*']), [])
        self.assertEqual(parse_test_filters(['*.*']), [])
        # Mixing '*' with other filters still indicates running all tests.
        self.assertEqual(parse_test_filters(['SuiteName', '*']), [])
        self.assertEqual(parse_test_filters(['SuiteName.TestName:*.*']), [])

    def test_specific_test_case(self) -> None:
        """'SuiteName.TestName' selects a single named test case."""
        filters = parse_test_filters(['SuiteA.Test1'])
        self.assertEqual(filters, [('SuiteA', 'Test1')])

    def test_all_tests_in_suite(self) -> None:
        """'SuiteName' or 'SuiteName.*' selects all tests in that suite."""
        filters = parse_test_filters(['SuiteA'])
        self.assertEqual(filters, [('SuiteA', '*')])

        filters = parse_test_filters(['SuiteB.*'])
        self.assertEqual(filters, [('SuiteB', '*')])

    def test_wildcard_suite_any_test(self) -> None:
        """'*.TestName' matches TestName across all suites."""
        filters = parse_test_filters(['*.MyTest'])
        self.assertEqual(filters, [('*', 'MyTest')])

    def test_colon_delimiter(self) -> None:
        """Colons separate multiple filter expressions within a single
        argument."""
        filters = [
            'SuiteA.Test1:SuiteB.*:*.Test3:SuiteC',
        ]
        self.assertEqual(
            parse_test_filters(filters),
            [
                ('SuiteA', 'Test1'),
                ('SuiteB', '*'),
                ('*', 'Test3'),
                ('SuiteC', '*'),
            ],
        )

    def test_multiple_filter_arguments(self) -> None:
        """Multiple flags/arguments are combined."""
        self.assertEqual(
            parse_test_filters(['SuiteA.Test1', 'SuiteB', '*.Test3']),
            [('SuiteA', 'Test1'), ('SuiteB', '*'), ('*', 'Test3')],
        )

    def test_deduplication(self) -> None:
        """Duplicate suites and test cases are deduplicated."""
        self.assertEqual(
            parse_test_filters(['SuiteA:SuiteA', 'SuiteB.Test1:SuiteB.Test1']),
            [('SuiteA', '*'), ('SuiteB', 'Test1')],
        )

    def test_partial_globbing_disallowed(self) -> None:
        """Partial globbing in suite or test names raises ValueError."""
        # Partial test globbing
        with self.assertRaisesRegex(ValueError, 'Partial test name globbing'):
            parse_test_filters(['SuiteName.Test*'])

        with self.assertRaisesRegex(ValueError, 'Partial test name globbing'):
            parse_test_filters(['SuiteName.*Test'])

        with self.assertRaisesRegex(ValueError, 'Partial test name globbing'):
            parse_test_filters(['*.Test*'])

        # Partial suite globbing
        with self.assertRaisesRegex(ValueError, 'Partial suite name globbing'):
            parse_test_filters(['Suite*.TestName'])

        with self.assertRaisesRegex(ValueError, 'Partial suite name globbing'):
            parse_test_filters(['*Suite.TestName'])

        with self.assertRaisesRegex(ValueError, 'Partial suite name globbing'):
            parse_test_filters(['Suite*'])

        with self.assertRaisesRegex(ValueError, 'Partial suite name globbing'):
            parse_test_filters(['*Suite'])

        with self.assertRaisesRegex(ValueError, 'Partial suite name globbing'):
            parse_test_filters(['Sui*te.*'])

    def test_negative_filters_disallowed(self) -> None:
        """Negative filters (using '-') raise ValueError."""
        with self.assertRaisesRegex(
            ValueError, 'Negative filters are not allowed'
        ):
            parse_test_filters(['SuiteName.-TestName'])

        with self.assertRaisesRegex(
            ValueError, 'Negative filters are not allowed'
        ):
            parse_test_filters(['-SuiteName.TestName'])

        with self.assertRaisesRegex(
            ValueError, 'Negative filters are not allowed'
        ):
            parse_test_filters(['-SuiteName'])

        with self.assertRaisesRegex(
            ValueError, 'Negative filters are not allowed'
        ):
            parse_test_filters(['SuiteName:-TestName'])

    def test_invalid_filter_formats(self) -> None:
        """Malformed filters raise ValueError."""
        with self.assertRaisesRegex(ValueError, 'Invalid test filter format'):
            parse_test_filters(['Suite.Test.Extra'])

        with self.assertRaisesRegex(ValueError, 'Invalid test filter format'):
            parse_test_filters(['Suite.'])

        with self.assertRaisesRegex(ValueError, 'Invalid test filter format'):
            parse_test_filters(['.Test'])

        with self.assertRaisesRegex(ValueError, 'Invalid test filter format'):
            parse_test_filters(['*.'])

        with self.assertRaisesRegex(ValueError, 'Invalid test filter format'):
            parse_test_filters(['.*'])


if __name__ == '__main__':
    unittest.main()
