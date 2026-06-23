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
"""Defines programs and excludes for Pigweed's upstream presubmit."""

from typing import Iterable, Pattern

from pw_presubmit import (
    block_submission,
    cpp_checks,
    json_check,
    keep_sorted,
    upstream_checks,
)
from pw_presubmit.format.step import format_steps
from pw_presubmit.private.step import Step
from pw_presubmit.private.bazel import (
    build_affected_targets,
    test_affected_targets,
    python_lint,
    clang_tidy,
)


# Paths to completely exclude from presubmit checks.
EXCLUDES = (
    "^\\.agents/skills/docs/tests/before\\.rst$",
    "\\bthird_party/fuchsia/repo",
    "\\bthird_party/perfetto/repo",
    "\\bthird_party/.*\\.json$",
    "\\bpackage\\.lock$",
    "\\bpw_presubmit/py/pw_presubmit/format/test_data/.*",
    "\\bpw_web/log-viewer/package(-lock)?\\.json",
    "\\bpw_web/log-viewer/src/assets/material_symbols_rounded_subset\\.woff2",
    "^patches\\.json$",
)

# Quick lint and format checks.
QUICK_COMMON = (
    # commit_message_format could not be easily converted to Step because it
    # needs LUCI context.
    # upstream_checks.commit_message_format,
    upstream_checks.copyright_notice,
    upstream_checks.inclusive_language_check,
    block_submission.presubmit_check,
    cpp_checks.pragma_once,
    # TODO: b/432484923 - Fix this check in Bazel.
    # build.bazel_lint,
    upstream_checks.owners_lint_checks,
    upstream_checks.source_in_gn_build(),
    # TODO: b/432484923 - Implement this check in Bazel.
    # javascript_checks.eslint,
    json_check.presubmit_check,
    keep_sorted.presubmit_check,
    upstream_checks.todo_check_with_exceptions,
)

FULL_COMMON = QUICK_COMMON + (
    upstream_checks.source_in_bazel_build(),
    build_affected_targets,
    test_affected_targets,
    python_lint,
    clang_tidy,
)


def programs(
    format_exclusions: Iterable[Pattern[str] | str] = (),
) -> dict[str, tuple[Step, ...]]:
    """Get Pigweed's Bazel-friendly presubmit programs.

    This is a function to avoid evaluating all programs at import time, which
    might fail in some environments.
    """
    formatting = tuple(format_steps(exclude=format_exclusions))
    return {
        "quick": QUICK_COMMON + formatting,
        "full": FULL_COMMON + formatting,
    }
