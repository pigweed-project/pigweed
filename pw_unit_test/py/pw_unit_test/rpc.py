# Copyright 2020 The Pigweed Authors
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
"""Utilities for running unit tests over :ref:`module-pw_rpc`."""

import enum
import abc
from dataclasses import dataclass
import logging
from typing import Iterable, Sequence

from pw_rpc.client import Services
from pw_rpc.callback_client import OptionalTimeout, UseDefault
from pw_unit_test_proto import unit_test_pb2

_LOG = logging.getLogger(__package__)


@dataclass(frozen=True)
class TestCase:
    suite_name: str
    test_name: str
    file_name: str

    def __str__(self) -> str:
        return f'{self.suite_name}.{self.test_name}'

    def __repr__(self) -> str:
        return f'TestCase({str(self)})'


def _test_case(raw_test_case: unit_test_pb2.TestCaseDescriptor) -> TestCase:
    return TestCase(
        raw_test_case.suite_name,
        raw_test_case.test_name,
        raw_test_case.file_name,
    )


@dataclass(frozen=True)
class TestExpectation:
    expression: str
    evaluated_expression: str
    file_name: str
    line_number: int
    success: bool

    def __str__(self) -> str:
        return self.expression

    def __repr__(self) -> str:
        return f'TestExpectation({str(self)})'


class TestCaseResult(enum.IntEnum):
    SUCCESS = unit_test_pb2.TestCaseResult.SUCCESS
    FAILURE = unit_test_pb2.TestCaseResult.FAILURE
    SKIPPED = unit_test_pb2.TestCaseResult.SKIPPED


class EventHandler(abc.ABC):
    @abc.abstractmethod
    def run_all_tests_start(self) -> None:
        """Called before all tests are run."""

    @abc.abstractmethod
    def run_all_tests_end(self, passed_tests: int, failed_tests: int) -> None:
        """Called after the test run is complete."""

    @abc.abstractmethod
    def test_case_start(self, test_case: TestCase) -> None:
        """Called when a new test case is started."""

    @abc.abstractmethod
    def test_case_end(
        self, test_case: TestCase, result: TestCaseResult
    ) -> None:
        """Called when a test case completes with its overall result."""

    @abc.abstractmethod
    def test_case_disabled(self, test_case: TestCase) -> None:
        """Called when a disabled test case is encountered."""

    @abc.abstractmethod
    def test_case_expect(
        self, test_case: TestCase, expectation: TestExpectation
    ) -> None:
        """Called after each expect or assert statement within a test case."""


class LoggingEventHandler(EventHandler):
    """Event handler that logs test events using Google Test format."""

    def run_all_tests_start(self) -> None:
        _LOG.info('[==========] Running all tests.')

    def run_all_tests_end(self, passed_tests: int, failed_tests: int) -> None:
        _LOG.info('[==========] Done running all tests.')
        _LOG.info('[  PASSED  ] %d test(s).', passed_tests)
        if failed_tests:
            _LOG.info('[  FAILED  ] %d test(s).', failed_tests)

    def test_case_start(self, test_case: TestCase) -> None:
        _LOG.info('[ RUN      ] %s', test_case)

    def test_case_end(
        self, test_case: TestCase, result: TestCaseResult
    ) -> None:
        if result == TestCaseResult.SUCCESS:
            _LOG.info('[       OK ] %s', test_case)
        else:
            _LOG.info('[  FAILED  ] %s', test_case)

    def test_case_disabled(self, test_case: TestCase) -> None:
        _LOG.info('Skipping disabled test %s', test_case)

    def test_case_expect(
        self, unused_test_case: TestCase, expectation: TestExpectation
    ) -> None:
        result = 'Success' if expectation.success else 'Failure'
        log = _LOG.info if expectation.success else _LOG.error
        log('%s:%d: %s', expectation.file_name, expectation.line_number, result)
        log('      Expected: %s', expectation.expression)
        log('        Actual: %s', expectation.evaluated_expression)


@dataclass(frozen=True)
class TestRecord:
    """Records test results."""

    passing_tests: tuple[TestCase, ...]
    failing_tests: tuple[TestCase, ...]
    disabled_tests: tuple[TestCase, ...]

    def all_tests_passed(self) -> bool:
        return not self.failing_tests

    def __bool__(self) -> bool:
        return self.all_tests_passed()


def parse_test_filters(
    filters: Sequence[str],
) -> list[tuple[str, str]]:
    """Parses test filter strings into test filters.

    Supported formats:
      - 'SuiteName.TestName': named test case
      - 'SuiteName.*': all tests in suite
      - 'SuiteName': all tests in suite (no '.' assumes suite)
      - '*.TestName': named test case across all suites
      - '*.*' or '*': all tests (run everything)
      - Colons (':') can be used to delimit multiple filters.

    Partial globbing (e.g. 'Suite*.Test' or 'Suite.Test*') and negative
    filters (e.g. 'Suite.-Test' or '-Suite') are not allowed.
    """
    test_filters: list[tuple[str, str]] = []
    run_all = False

    for filter_arg in filters:
        for pattern in filter_arg.split(':'):
            pattern = pattern.strip()
            if not pattern:
                continue

            if '-' in pattern:
                raise ValueError(
                    f"Negative filters are not allowed: '{pattern}'"
                )

            if pattern in ('*', '*.*'):
                run_all = True
                continue

            if '.' in pattern:
                if pattern.count('.') != 1:
                    raise ValueError(f"Invalid test filter format: '{pattern}'")
                suite, test = pattern.split('.')
                if not suite or not test:
                    raise ValueError(f"Invalid test filter format: '{pattern}'")

                if suite == '*' and test == '*':
                    run_all = True
                elif suite == '*':
                    if '*' in test:
                        raise ValueError(
                            "Partial test name globbing is not allowed: "
                            f"'{pattern}'"
                        )
                    test_filters.append(('*', test))
                elif test == '*':
                    if '*' in suite:
                        raise ValueError(
                            "Partial suite name globbing is not allowed: "
                            f"'{pattern}'"
                        )
                    test_filters.append((suite, '*'))
                else:
                    if '*' in suite:
                        raise ValueError(
                            "Partial suite name globbing is not allowed: "
                            f"'{pattern}'"
                        )
                    if '*' in test:
                        raise ValueError(
                            "Partial test name globbing is not allowed: "
                            f"'{pattern}'"
                        )
                    test_filters.append((suite, test))
            else:
                suite = pattern
                if '*' in suite:
                    raise ValueError(
                        "Partial suite name globbing is not allowed: "
                        f"'{pattern}'"
                    )
                test_filters.append((suite, '*'))

    if run_all:
        return []

    return list(dict.fromkeys(test_filters))


def _convert_to_proto_test_filters(
    test_suites: Iterable[str],
    test_filters: Iterable[
        TestCase | unit_test_pb2.TestFilter | tuple[str, str] | str
    ],
) -> list[unit_test_pb2.TestFilter]:
    proto_test_filters: list[unit_test_pb2.TestFilter] = [
        unit_test_pb2.TestFilter(suite_name=suite) for suite in test_suites
    ]
    for item in test_filters:
        if isinstance(item, unit_test_pb2.TestFilter):
            proto_test_filters.append(item)
        elif isinstance(item, TestCase):
            proto_test_filters.append(
                unit_test_pb2.TestFilter(
                    suite_name=item.suite_name,
                    test_name=item.test_name,
                )
            )
        elif isinstance(item, tuple):
            proto_test_filters.append(
                unit_test_pb2.TestFilter(
                    suite_name=item[0],
                    test_name=item[1] if len(item) > 1 else None,
                )
            )
        elif isinstance(item, str):
            for suite, test in parse_test_filters([item]):
                proto_test_filters.append(
                    unit_test_pb2.TestFilter(
                        suite_name=suite,
                        test_name=test if test != '*' else None,
                    )
                )
        else:
            raise TypeError(f'Unsupported test filter type: {type(item)}')
    return proto_test_filters


def run_tests(
    rpcs: Services,
    report_passed_expectations: bool = False,
    test_suites: Iterable[str] = (),
    test_filters: Iterable[
        TestCase | unit_test_pb2.TestFilter | tuple[str, str] | str
    ] = (),
    event_handlers: Iterable[EventHandler] = (LoggingEventHandler(),),
    timeout_s: OptionalTimeout = UseDefault.VALUE,
) -> TestRecord:
    """Runs unit tests on a device over :ref:`module-pw_rpc`.

    Calls each of the provided event handlers as test events occur, and returns
    ``True`` if all tests pass.
    """
    proto_test_filters = _convert_to_proto_test_filters(
        test_suites, test_filters
    )

    unit_test_service = rpcs.pw.unit_test.UnitTest  # type: ignore[attr-defined]
    request = unit_test_service.Run.request(
        report_passed_expectations=report_passed_expectations,
        test_filters=proto_test_filters,
    )
    call = unit_test_service.Run.invoke(request, timeout_s=timeout_s)
    test_responses = iter(call)

    # Read the first response, which must be a test_run_start message.
    try:
        first_response = next(test_responses)
    except StopIteration:
        _LOG.error(
            'The "test_run_start" message was dropped! UnitTest.Run '
            'concluded with %s.',
            call.status,
        )
        raise

    if not first_response.HasField('test_run_start'):
        raise ValueError(
            'Expected a "test_run_start" response from pw.unit_test.Run, '
            'but received a different message type. A response may have been '
            'dropped.'
        )

    for event_handler in event_handlers:
        event_handler.run_all_tests_start()

    passing_tests: list[TestCase] = []
    failing_tests: list[TestCase] = []
    disabled_tests: list[TestCase] = []

    for response in test_responses:
        if response.HasField('test_run_start'):
            for event_handler in event_handlers:
                event_handler.run_all_tests_start()
        elif response.HasField('test_run_end'):
            for event_handler in event_handlers:
                event_handler.run_all_tests_end(
                    response.test_run_end.passed, response.test_run_end.failed
                )
            assert len(passing_tests) == response.test_run_end.passed
            assert len(failing_tests) == response.test_run_end.failed
            test_record = TestRecord(
                passing_tests=tuple(passing_tests),
                failing_tests=tuple(failing_tests),
                disabled_tests=tuple(disabled_tests),
            )
        elif response.HasField('test_case_start'):
            raw_test_case = response.test_case_start
            current_test_case = _test_case(raw_test_case)
            for event_handler in event_handlers:
                event_handler.test_case_start(current_test_case)
        elif response.HasField('test_case_end'):
            result = TestCaseResult(response.test_case_end)
            for event_handler in event_handlers:
                event_handler.test_case_end(current_test_case, result)
            if result == TestCaseResult.SUCCESS:
                passing_tests.append(current_test_case)
            else:
                failing_tests.append(current_test_case)
        elif response.HasField('test_case_disabled'):
            raw_test_case = response.test_case_disabled
            current_test_case = _test_case(raw_test_case)
            for event_handler in event_handlers:
                event_handler.test_case_disabled(current_test_case)
            disabled_tests.append(current_test_case)
        elif response.HasField('test_case_expectation'):
            raw_expectation = response.test_case_expectation
            expectation = TestExpectation(
                raw_expectation.expression,
                raw_expectation.evaluated_expression,
                raw_expectation.file_name,
                raw_expectation.line_number,
                raw_expectation.success,
            )
            for event_handler in event_handlers:
                event_handler.test_case_expect(current_test_case, expectation)
    # pylint: disable=used-before-assignment
    return test_record
