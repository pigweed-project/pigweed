# Copyright 2022 The Pigweed Authors
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
"""Executes a compilation failure test."""

import argparse
import enum
import logging
import os
from pathlib import Path
import re
import shlex
import string
import sys
import subprocess
from typing import Callable, Sequence, TextIO

import pw_cli.log

from pw_compilation_testing.generator import Compiler, Expectation, TestCase

_LOG = logging.getLogger('pw_nc_test')

_RULE_REGEX = re.compile('^rule (?:cxx|.*_cxx)$')
_NINJA_VARIABLE = re.compile('^([a-zA-Z0-9_]+) = ?')


# TODO(hepler): Could do this step just once and output the results.
def find_cc_rule(toolchain_ninja_file: Path) -> str | None:
    """Searches the toolchain.ninja file for the cc rule."""
    cmd_prefix = '  command = '

    found_rule = False

    with toolchain_ninja_file.open() as fd:
        for line in fd:
            if found_rule:
                if line.startswith(cmd_prefix):
                    cmd = line[len(cmd_prefix) :].strip()
                    if cmd.startswith('ccache '):
                        cmd = cmd[len('ccache ') :]
                    return cmd

                if not line.startswith('  '):
                    break
            elif _RULE_REGEX.match(line):
                found_rule = True

    return None


def _parse_ninja_variables(target_ninja_file: Path) -> dict[str, str]:
    variables: dict[str, str] = {}

    with target_ninja_file.open() as fd:
        for line in fd:
            match = _NINJA_VARIABLE.match(line)
            if match:
                variables[match.group(1)] = line[match.end() :].strip()

    return variables


_EXPECTED_GN_VARS = (
    'asmflags',
    'cflags',
    'cflags_c',
    'cflags_cc',
    'cflags_objc',
    'cflags_objcc',
    'defines',
    'include_dirs',
)

_TEST_MACRO = 'PW_NC_TEST_EXECUTE_CASE_'
# Regular expression to find and remove ANSI escape sequences, based on
# https://stackoverflow.com/a/14693789.
_ANSI_ESCAPE_SEQUENCES = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')


def _red(message: str) -> str:
    return f'\033[31;1m{message}\033[0m'


_TITLE_1 = '     NEGATIVE     '
_TITLE_2 = ' COMPILATION TEST '

_BOX_TOP = f'┏{"━" * len(_TITLE_1)}┓'
_BOX_MID_1 = '┃{title}┃ \033[1m{test_name}\033[0m'
_BOX_MID_2 = '┃{title}┃ \033[0m{source}:{line}\033[0m'
_BOX_BOT = f'┻{"━" * (len(_TITLE_1))}┻{"━" * (77 - len(_TITLE_1))}┓'
_FOOTER = '\n' + '━' * 79 + '┛'


def _start_result(
    color: Callable[[str], str], test: TestCase, command: str, out: TextIO
) -> None:
    print(_BOX_TOP, file=out)
    print(
        _BOX_MID_1.format(title=color(_TITLE_1), test_name=test.name()),
        file=out,
    )
    print(
        _BOX_MID_2.format(
            title=color(_TITLE_2), source=test.source, line=test.line
        ),
        file=out,
    )
    print(_BOX_BOT, file=out)
    print(file=out)

    _LOG.debug('Compilation command:\n%s', command)


def _start_failure(test: TestCase, command: str, out: TextIO) -> None:
    _start_result(_red, test, command, out)


class TestResult(enum.Enum):
    PASS = 'PASSED'
    FAIL = 'FAILED'
    SKIP = 'SKIPPED'

    def ok(self) -> bool:
        """True if the test passed or was skipped intentionally."""
        return self is not self.FAIL

    def colorized(self) -> str:
        if self is self.FAIL:
            return '\033[1;31mFAILED\033[0m'  # red FAILED

        if self is self.PASS:
            return '\033[1;32mPASSED\033[0m'  # green PASSED

        return '\033[1;33mSKIPPED\033[0m'  # yellow SKIPPED


def _check_results(
    test: TestCase,
    command: str,
    process: subprocess.CompletedProcess,
    ok_file: TextIO,
    err_file: TextIO,
) -> TestResult:
    stderr = process.stderr.decode(errors='replace')
    err = lambda *a: print(*a, file=err_file)

    if process.returncode == 0:
        # Check if the test was commented out or disabled.
        if _should_skip_test(command):
            err(
                f'Skipped test {test.name()} since it was excluded by the '
                'preprocessor',
            )
            return TestResult.SKIP

        _start_failure(test, command, err_file)
        err('Compilation succeeded, but it should have failed!')
        err('Update the test code so that it fails to compile.')
        return TestResult.FAIL

    compiler_str = command.split(' ', 1)[0]
    compiler = Compiler.from_command(compiler_str)

    _LOG.debug('%s is %s', compiler_str, compiler)
    expectations: list[Expectation] = [
        e for e in test.expectations if compiler.matches(e.compiler)
    ]

    _LOG.debug(
        '%s: Checking compilation from %s (%s) for %d of %d patterns:',
        test.name(),
        compiler_str,
        compiler,
        len(expectations),
        len(test.expectations),
    )
    for expectation in expectations:
        _LOG.debug('    %s', expectation.pattern.pattern)

    if not expectations:
        _start_failure(test, command, err_file)
        err(
            f'Compilation with {compiler_str} failed, but no PW_NC_EXPECT() '
            f'patterns that apply to {compiler_str} were provided'
        )

        err(f'Compilation output:\n{stderr}')
        err(
            'Add at least one PW_NC_EXPECT("<regex>") or PW_NC_EXPECT_'
            f'{compiler.name}("<regex>") expectation to {test.case}',
        )
        return TestResult.FAIL

    no_color = _ANSI_ESCAPE_SEQUENCES.sub('', stderr)

    failed = [e for e in expectations if not e.pattern.search(no_color)]
    if failed:
        _start_failure(test, command, err_file)
        err(
            f'Compilation with {compiler_str} failed, but the output did not '
            'match the expected patterns.'
        )
        err(
            f'{len(failed)} of {len(expectations)} expected patterns did not '
            'match:',
        )
        err()
        for exp in expectations:
            res = '❌' if exp in failed else '✅'
            err(
                f'  {res} {test.source.name}:{exp.line} {exp.pattern.pattern}',
            )
        err()

        err(f'Compilation output:\n{stderr}')
        err(
            'Update the test so that compilation fails with the '
            'expected output'
        )
        return TestResult.FAIL

    _start_result(lambda a: a, test, command, ok_file)
    print(
        'Compilation failed and the output matched the expected patterns:\n',
        file=ok_file,
    )
    for expect in expectations:
        print(
            f'  ✅ {test.source.name}:{expect.line} {expect.pattern.pattern}',
            file=ok_file,
        )

    return TestResult.PASS


def _should_skip_test(base_command: str) -> bool:
    # Attempt to run the preprocessor while setting the PW_NC_TEST macro to an
    # illegal statement (defined() with no identifier). If the preprocessor
    # passes, the test was skipped.
    preprocessor_cmd = f'{base_command}={shlex.quote("defined()")} -E'

    split_cmd = shlex.split(preprocessor_cmd)
    if "-c" in split_cmd:
        split_cmd.remove("-c")
        preprocessor_cmd = shlex.join(split_cmd)

    process = subprocess.run(
        preprocessor_cmd,
        shell=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    _LOG.debug(
        'Preprocessor command to check if test is enabled returned %d:\n%s',
        process.returncode,
        preprocessor_cmd,
    )
    return process.returncode == 0


def run_test(
    test: TestCase,
    command: str,
    all_tests: Sequence[str],
    ok_file: TextIO,
    err_file: TextIO,
) -> TestResult:
    """Executes a compiler command and checks for expected compilation failure.

    Args:
      test: The TestCase object representing the test to run
      command: The compiler command to run.
      all_tests: list of all test case names in the suite
      ok_file: Where to write output for passed tests
      err_file: Where to write output for failed tests
    """
    test_command = ' '.join(
        [
            command,
            '-DPW_NEGATIVE_COMPILATION_TESTS_ENABLED',
            # Define macros to disable all tests except this one.
            *(f'-D{_TEST_MACRO}{t}=0' for t in all_tests if t != test.case),
            f'-D{_TEST_MACRO}{test.case}',
        ]
    )
    process = subprocess.run(test_command, shell=True, capture_output=True)
    result = _check_results(test, test_command, process, ok_file, err_file)
    print(_FOOTER, file=err_file)
    return result


def _main(
    test: TestCase, toolchain_ninja: Path, target_ninja: Path, all_tests: Path
) -> int:
    """Compiles a compile fail test and returns 1 if compilation succeeds."""
    command = find_cc_rule(toolchain_ninja)

    if command is None:
        _LOG.critical(
            'Failed to find C++ compilation command in %s', toolchain_ninja
        )
        return 2

    variables = {key: '' for key in _EXPECTED_GN_VARS}
    variables.update(_parse_ninja_variables(target_ninja))

    variables['out'] = str(
        target_ninja.parent / f'{target_ninja.stem}.compile_fail_test.out'
    )
    variables['in'] = str(test.source)
    command = string.Template(command).substitute(variables)

    with open(os.devnull, 'w') as ignore_passes:
        if run_test(
            test,
            command,
            all_tests.read_text().splitlines(),
            ignore_passes,
            sys.stderr,
        ).ok():
            return 0

    return 1


def _parse_args() -> dict:
    """Parses command-line arguments."""

    parser = argparse.ArgumentParser(
        description='Emits an error when a facade has a null backend'
    )
    parser.add_argument(
        '--toolchain-ninja',
        type=Path,
        required=True,
        help='Ninja file with the compilation command for the toolchain',
    )
    parser.add_argument(
        '--target-ninja',
        type=Path,
        required=True,
        help='Ninja file with the compilation commands to the test target',
    )
    parser.add_argument(
        '--test-data',
        dest='test',
        required=True,
        type=TestCase.deserialize,
        help='Serialized TestCase object',
    )
    parser.add_argument('--all-tests', type=Path, help='List of all tests')
    return vars(parser.parse_args())


if __name__ == '__main__':
    pw_cli.log.install(level=logging.INFO)
    sys.exit(_main(**_parse_args()))
