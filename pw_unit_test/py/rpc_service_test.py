#!/usr/bin/env python3
# Copyright 2021 The Pigweed Authors
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
"""Tests using the callback client for pw_rpc."""

import logging
import unittest
from unittest import mock

from pw_hdlc import rpc
from pw_rpc import testing
from pw_unit_test_proto import unit_test_pb2
from pw_unit_test import (
    run_tests,
    parse_test_filters,
    EventHandler,
    TestCase,
)

# The three suites (Passing, Failing, and DISABLED_Disabled) have these cases.
_CASES = ('Zero', 'One', 'Two', 'DISABLED_Disabled')
_FILE = 'pw_unit_test/test_rpc_server.cc'

PASSING = tuple(TestCase('Passing', case, _FILE) for case in _CASES[:-1])
FAILING = tuple(TestCase('Failing', case, _FILE) for case in _CASES[:-1])
EXECUTED_TESTS = PASSING + FAILING

DISABLED_SUITE = tuple(
    TestCase('DISABLED_Disabled', case, _FILE) for case in _CASES
)

ALL_DISABLED_TESTS = (
    TestCase('Passing', 'DISABLED_Disabled', _FILE),
    TestCase('Failing', 'DISABLED_Disabled', _FILE),
    *DISABLED_SUITE,
)


class RpcIntegrationTest(unittest.TestCase):
    """Calls RPCs on an RPC server through a socket."""

    test_server_command: tuple[str, ...] = ()
    port: int

    def setUp(self) -> None:
        self._context = rpc.HdlcRpcLocalServerAndClient(
            self.test_server_command, self.port, [unit_test_pb2]
        )
        self.rpcs = self._context.client.channel(1).rpcs
        self.handler = mock.NonCallableMagicMock(spec=EventHandler)

    def tearDown(self) -> None:
        self._context.close()

    def test_run_tests_default_handler(self) -> None:
        with self.assertLogs(logging.getLogger('pw_unit_test'), 'INFO') as logs:
            self.assertFalse(run_tests(self.rpcs))

        for test in EXECUTED_TESTS:
            self.assertTrue(any(str(test) in log for log in logs.output), test)

    def test_run_tests_calls_test_case_start(self) -> None:
        self.assertFalse(run_tests(self.rpcs, event_handlers=[self.handler]))

        self.handler.test_case_start.assert_has_calls(
            [mock.call(case) for case in EXECUTED_TESTS], any_order=True
        )

    def test_run_tests_calls_test_case_end(self) -> None:
        self.assertFalse(run_tests(self.rpcs, event_handlers=[self.handler]))

        calls = [
            mock.call(
                case,
                (
                    unit_test_pb2.SUCCESS
                    if case.suite_name == 'Passing'
                    else unit_test_pb2.FAILURE
                ),
            )
            for case in EXECUTED_TESTS
        ]
        self.handler.test_case_end.assert_has_calls(calls, any_order=True)

    def test_run_tests_calls_test_case_disabled(self) -> None:
        self.assertFalse(run_tests(self.rpcs, event_handlers=[self.handler]))

        self.handler.test_case_disabled.assert_has_calls(
            [mock.call(case) for case in ALL_DISABLED_TESTS], any_order=True
        )

    def test_passing_tests_only(self) -> None:
        self.assertTrue(
            run_tests(
                self.rpcs,
                test_suites=['Passing'],
                event_handlers=[self.handler],
            )
        )
        calls = [mock.call(case, unit_test_pb2.SUCCESS) for case in PASSING]
        self.handler.test_case_end.assert_has_calls(calls, any_order=True)

    def test_disabled_tests_only(self) -> None:
        self.assertTrue(
            run_tests(
                self.rpcs,
                test_suites=['DISABLED_Disabled'],
                event_handlers=[self.handler],
            )
        )

        self.handler.test_case_start.assert_not_called()
        self.handler.test_case_end.assert_not_called()
        self.handler.test_case_disabled.assert_has_calls(
            [mock.call(case) for case in DISABLED_SUITE], any_order=True
        )

    def test_failing_tests(self) -> None:
        self.assertFalse(
            run_tests(
                self.rpcs,
                test_suites=['Failing'],
                event_handlers=[self.handler],
            )
        )
        calls = [mock.call(case, unit_test_pb2.FAILURE) for case in FAILING]
        self.handler.test_case_end.assert_has_calls(calls, any_order=True)

    def test_single_test_case_only(self) -> None:
        self.assertTrue(
            run_tests(
                self.rpcs,
                test_filters=[('Passing', 'One')],
                event_handlers=[self.handler],
            )
        )
        target_case = TestCase('Passing', 'One', _FILE)
        self.handler.test_case_start.assert_called_once_with(target_case)
        self.handler.test_case_end.assert_called_once_with(
            target_case, unit_test_pb2.SUCCESS
        )

    def test_multiple_test_cases(self) -> None:
        self.assertTrue(
            run_tests(
                self.rpcs,
                test_filters=[('Passing', 'Zero'), ('Passing', 'Two')],
                event_handlers=[self.handler],
            )
        )
        expected = [
            TestCase('Passing', 'Zero', _FILE),
            TestCase('Passing', 'Two', _FILE),
        ]
        self.handler.test_case_end.assert_has_calls(
            [mock.call(case, unit_test_pb2.SUCCESS) for case in expected],
            any_order=True,
        )
        self.assertEqual(self.handler.test_case_end.call_count, 2)

    def test_union_suite_and_test_cases(self) -> None:
        self.assertFalse(
            run_tests(
                self.rpcs,
                test_suites=['Passing'],
                test_filters=[('Failing', 'One')],
                event_handlers=[self.handler],
            )
        )
        expected_passing = [
            mock.call(case, unit_test_pb2.SUCCESS) for case in PASSING
        ]
        expected_failing = [
            mock.call(TestCase('Failing', 'One', _FILE), unit_test_pb2.FAILURE)
        ]
        self.handler.test_case_end.assert_has_calls(
            expected_passing + expected_failing, any_order=True
        )
        self.assertEqual(
            self.handler.test_case_end.call_count, len(PASSING) + 1
        )

    def test_non_existent_test_case(self) -> None:
        record = run_tests(
            self.rpcs,
            test_filters=[('NonExistentSuite', 'NonExistentTest')],
            event_handlers=[self.handler],
        )
        self.assertTrue(record.all_tests_passed())
        self.assertEqual(len(record.passing_tests), 0)
        self.assertEqual(len(record.failing_tests), 0)
        self.handler.test_case_start.assert_not_called()
        self.handler.test_case_end.assert_not_called()

    def test_test_filter_and_test_case_objects(self) -> None:
        filter_obj = unit_test_pb2.TestFilter(
            suite_name='Passing', test_name='Zero'
        )
        test_case_obj = TestCase('Passing', 'Two', _FILE)
        self.assertTrue(
            run_tests(
                self.rpcs,
                test_filters=[filter_obj, test_case_obj],
                event_handlers=[self.handler],
            )
        )
        expected = [
            TestCase('Passing', 'Zero', _FILE),
            TestCase('Passing', 'Two', _FILE),
        ]
        self.handler.test_case_end.assert_has_calls(
            [mock.call(case, unit_test_pb2.SUCCESS) for case in expected],
            any_order=True,
        )
        self.assertEqual(self.handler.test_case_end.call_count, 2)

    def test_filter_without_suite_name(self) -> None:
        self.assertFalse(
            run_tests(
                self.rpcs,
                test_filters=[unit_test_pb2.TestFilter(test_name='One')],
                event_handlers=[self.handler],
            )
        )
        expected = [
            mock.call(TestCase('Passing', 'One', _FILE), unit_test_pb2.SUCCESS),
            mock.call(TestCase('Failing', 'One', _FILE), unit_test_pb2.FAILURE),
        ]
        self.handler.test_case_end.assert_has_calls(expected, any_order=True)
        self.assertEqual(self.handler.test_case_end.call_count, 2)


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


def _main(
    test_server_command: list[str], port: int, unittest_args: list[str]
) -> None:
    RpcIntegrationTest.test_server_command = tuple(test_server_command)
    RpcIntegrationTest.port = port
    unittest.main(argv=unittest_args)


if __name__ == '__main__':
    _main(**vars(testing.parse_test_server_args()))
