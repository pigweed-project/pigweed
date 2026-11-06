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
"""Generates and runs negative compilation tests.

If PW_NC_TESTs parse successfully, produces a script that reports the results
when executed.
"""

import argparse
import concurrent.futures
import logging
import io
import os
import sys
from pathlib import Path
from typing import Iterable, NamedTuple, Sequence, TextIO

import pw_cli.log

from pw_compilation_testing.generator import (
    MalformedTestError,
    ParseError,
    TestCase,
    enumerate_tests,
)
from pw_compilation_testing.runner import run_test, TestResult

_LOG = logging.getLogger('pw_nc_test')

# Since each test case in a single .cc file can be parallelized, it's possible
# to spin up e.g. 128 parallel compilations which can cause huge amounts of
# memory consumption in some environments. To ameliorate resource exhaustion,
# this is capped to a practical (albeit somewhat arbitrary) parallelism limit.
# For more context, see https://pwbug.dev/446978848.
_MAX_PARALLELISM = min(os.cpu_count() or 4, 16)


class _TestRun(NamedTuple):
    test: TestCase
    result: TestResult
    output: str


def _execute_test_case(
    test: TestCase,
    sources: dict[Path, Path],
    all_tests: Sequence[str],
) -> _TestRun:
    _LOG.debug('Running test: %s', test.name())
    with io.StringIO() as output:
        result = run_test(
            test, sources[test.source].read_text(), all_tests, output, output
        )
        return _TestRun(test, result, output.getvalue())


def _test_in_parallel(
    sources: dict[Path, Path], tests: Sequence[TestCase]
) -> Iterable[_TestRun]:
    """Runs the tests in parallel and yields _TestRuns as they complete."""
    all_tests = [t.case for t in tests]

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=_MAX_PARALLELISM
    ) as executor:
        for future in concurrent.futures.as_completed(
            executor.submit(_execute_test_case, test, sources, all_tests)
            for test in tests
        ):
            yield future.result()


def _execute_tests_and_report_results(
    name: str,
    sources: dict[Path, Path],
    tests: Sequence[TestCase],
    result_script: TextIO,
) -> None:
    failures = []
    passes = []

    for test_run in _test_in_parallel(sources, tests):
        _LOG.debug('%s %s', test_run.test.name(), test_run.result.value)
        if test_run.result is TestResult.PASS:
            passes.append(test_run)
        elif test_run.result is TestResult.FAIL:
            failures.append(test_run)
            result_script.write('cat <<EOF\n')
            result_script.write(test_run.output)
            result_script.write('EOF\n')

    # Only write passing tests when there are no failures. This keeps passes
    # from polluting the output when there are failures.
    if not failures:
        for test_run in passes:
            result_script.write('cat <<EOF\n')
            result_script.write(test_run.output)
            result_script.write('EOF\n')

    result_script.write('cat <<EOF\n\n')
    result_script.write('═' * 80)
    final = TestResult.FAIL if failures else TestResult.PASS
    result_script.write(
        f'\n\033[1m{name}\033[0m {final.colorized()}: '
        f'{len(passes)} of {len(tests)} PW_NC_TESTs passed\n'
    )
    result_script.write('═' * 80)

    result_script.write('\n\nEOF\n')

    if failures:
        result_script.write('exit 1\n')

    _LOG.debug('%d of %d PW_NC_TESTs passed.', len(passes), len(tests))


def _enumeration_error(title: str, details: str) -> None:
    print('\n\033[31;1mERROR:\033[0m', title, file=sys.stderr)
    print(file=sys.stderr)
    print(details, file=sys.stderr)
    print(file=sys.stderr)


def _main(name: str, results: Path, sources: dict[Path, Path]) -> int:
    """Parses and runs negative compilation tests."""
    pw_cli.log.install(level=logging.INFO)

    try:
        tests = enumerate_tests(name[name.rfind(':') + 1 :], sources.keys())
    except MalformedTestError as err:
        _enumeration_error(f'{name} is invalid.', err.message)
        return 1
    except ParseError as err:
        _enumeration_error(f'Parse error in {name}', err.message)
        return 1

    with results.open('w') as result_script:
        _execute_tests_and_report_results(name, sources, tests, result_script)

    # Pass/fail is recorded when the results script runs.
    return 0


def _parse_args() -> dict:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True, help='NC test build label')
    parser.add_argument(
        '--results',
        required=True,
        type=Path,
        help='Path to write script that reports the test results',
    )
    parser.add_argument(
        'source_and_invocation_file_pairs',
        type=Path,
        nargs='+',
        help='Space-separated pairs of source and invocation file',
    )
    args = parser.parse_args()
    if len(args.source_and_invocation_file_pairs) % 2 != 0:
        parser.error(
            'The source file and its invocation file must be passed in pairs'
        )

    args.sources = dict(zip(*[iter(args.source_and_invocation_file_pairs)] * 2))
    del args.source_and_invocation_file_pairs
    return vars(args)


if __name__ == '__main__':
    sys.exit(_main(**_parse_args()))
