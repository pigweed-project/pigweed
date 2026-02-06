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
"""Runs the local presubmit checks for the Pigweed repository."""

import argparse
import logging
import os
import re
import sys
from typing import Pattern

from pw_cli import log

from pw_presubmit import (
    block_submission,
    cli,
    cpp_checks,
    json_check,
    keep_sorted,
    upstream_checks,
)
from pw_presubmit.install_hook import install_git_hook
from pw_presubmit.presubmit import Programs

_LOG = logging.getLogger('pw_presubmit')

# Paths to completely exclude from presubmit checks.
_EXCLUDE_PATHS = (
    "\\bthird_party/fuchsia/repo",
    "\\bthird_party/perfetto/repo",
    "\\bthird_party/.*\\.json$",
    "\\bpackage\\.lock$",
    "\\bpw_presubmit/py/pw_presubmit/format/test_data/.*",
    "\\bpw_web/log-viewer/package(-lock)?\\.json",
    "\\bpw_web/log-viewer/src/assets/material_symbols_rounded_subset\\.woff2",
    "^patches\\.json$",
)

EXCLUDES = tuple(re.compile(path) for path in _EXCLUDE_PATHS)

# Quick lint and format checks.
QUICK = (
    upstream_checks.commit_message_format,
    upstream_checks.copyright_notice,
    upstream_checks.inclusive_language_check,
    block_submission.presubmit_check,
    cpp_checks.pragma_once,
    # TODO: b/432484923 - Fix this check in Bazel.
    # build.bazel_lint,
    upstream_checks.owners_lint_checks,
    upstream_checks.source_in_bazel_build(),
    upstream_checks.source_in_gn_build(),
    # TODO: b/432484923 - Implement this check in Bazel.
    # javascript_checks.eslint,
    json_check.presubmit_check,
    keep_sorted.presubmit_check,
    upstream_checks.todo_check_with_exceptions,
)


def parse_args() -> dict:
    """Creates an argument parser and parses arguments."""

    parser = argparse.ArgumentParser(description=__doc__)
    cli.add_arguments(parser, Programs(quick=QUICK), 'quick')
    parser.add_argument(
        '--install',
        action='store_true',
        help='Install the presubmit as a Git pre-push hook and exit.',
    )

    return vars(parser.parse_args())


def run(install: bool, exclude: list[Pattern[str]], **presubmit_args) -> int:
    """Entry point for presubmit."""

    if install:
        install_git_hook(
            'pre-push',
            ['./pw', 'presubmit'],
        )
        return 0

    exclude.extend(EXCLUDES)
    return cli.run(exclude=exclude, **presubmit_args)


def main() -> int:
    """Run the presubmit for the Pigweed repository."""
    # Change to working directory if running from Bazel.
    if 'BUILD_WORKING_DIRECTORY' in os.environ:
        os.chdir(os.environ['BUILD_WORKING_DIRECTORY'])

    return run(**parse_args())


if __name__ == '__main__':
    log.install(logging.INFO)
    sys.exit(main())
