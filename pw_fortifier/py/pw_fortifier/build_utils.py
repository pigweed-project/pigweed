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
"""Build and test utility functions for pw_fortifier."""

from collections.abc import AsyncIterator
import os
import re

from pw_fortifier.bazelisk_utils import run_bazelisk

# TODO: https://pwbug.dev/559267230 - Rework this to decouple it from specific
# Bazel output.
_FAILED_TEST_PATTERNS = (
    re.compile(r'^(?:FAIL|FAILED|TIMEOUT):\s+([^\s()]+)'),
    re.compile(r'^\s*([^\s()]+)(?:\s+\(cached\))?\s+(?:FAILED|TIMEOUT)\b'),
)


async def run_unit_tests(
    cwd: str | os.PathLike[str] = '.',
    target: str | None = None,
) -> AsyncIterator[str]:
    """Runs unit tests using bazelisk and yields names of failed tests.

    Args:
        cwd: Working directory where the command is executed.
        target: Optional Bazel target to test. Defaults to '//...'.

    Yields:
        Names of failed test targets.
    """
    target_arg = target if target is not None else '//...'
    result = await run_bazelisk(['test', target_arg], cwd=cwd)
    output = f'{result.stdout}\n{result.stderr}'
    seen: set[str] = set()
    for line in output.splitlines():
        line = line.strip()
        for pattern in _FAILED_TEST_PATTERNS:
            match = pattern.search(line)
            if match:
                test_name = match.group(1).strip()
                if (
                    test_name.startswith('//')
                    or test_name.startswith('@')
                    or test_name.startswith(':')
                ):
                    if test_name not in seen:
                        seen.add(test_name)
                        yield test_name
                break


# TODO: https://pwbug.dev/559267230 - Rework this too allow downstream projects
# to be able to specify their own presubmit checks.
async def run_presubmit(cwd: str | os.PathLike[str] = '.') -> bool:
    """Runs presubmit checks using bazelisk.

    Args:
        cwd: Working directory where the command is executed.

    Returns:
        True only if the presubmit command succeeds; False otherwise.
    """
    result = await run_bazelisk(['run', '//:pw', '--', 'presubmit'], cwd=cwd)
    return result.returncode == 0
