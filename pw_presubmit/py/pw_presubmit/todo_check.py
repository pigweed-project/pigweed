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
"""Check the formatting of TODOs."""

import logging
from pathlib import Path
import re
from typing import Iterable, Pattern, Sequence

from pw_presubmit.presubmit import filter_paths
from pw_presubmit.presubmit_context import PresubmitContext

_LOG: logging.Logger = logging.getLogger(__name__)

EXCLUDE: Sequence[str] = (
    # Metadata
    r'^docker/tag$',
    r'\byarn.lock$',
    # Data files
    r'\.bin$',
    r'\.csv$',
    r'\.elf$',
    r'\.gif$',
    r'\.jpg$',
    r'\.json$',
    r'\.png$',
    r'\.svg$',
    r'\.xml$',
)

# todo-check: disable
# pylint: disable=line-too-long
BUGS_ONLY = re.compile(
    r'(?:\bTODO\(b/\d+(?:, ?b/\d+)*\).*\w)|'
    r'(?:\bTODO: b/\d+(?:, ?b/\d+)* - )|'
    r'(?:\bTODO: https://issues\.(?:pigweed|fuchsia)\.dev/(?:issues/)?\d+ - )|'
    r'(?:\bTODO: https://issues\.chromium\.org/(?:issues/)?\d+ - )|'
    r'(?:\bTODO: https://(?:(?:pw|fx)bug\.dev|crbug\.com)/\d+ - )|'
    r'(?:\bTODO: (?:(?:pw|fx)bug\.dev|crbug\.com)/\d+ - )|'
    r'(?:\bTODO: <(?:(?:pw|fx)bug\.dev|crbug\.com)/\d+> - )|'
    r'(?:\bTODO: https://github\.com/[a-zA-Z0-9][-a-zA-Z0-9]*/[a-zA-Z][-_a-zA-Z0-9\.]*/issues/\d+[ ]-[ ])'
)
BUGS_OR_USERNAMES = re.compile(
    r"""
(?:  # Legacy style.
    \bTODO\(
        (?:b/\d+|[a-z]+)  # Username or bug.
        (?:,[ ]?(?:b/\d+|[a-z]+))*  # Additional usernames or bugs.
    \)
.*\w  # Explanation.
)|
(?:  # New style.
    \bTODO:[ ]
    (?:
        b/\d+|  # Bug.
        https://(?:(?:pw|fx)bug\.dev|crbug\.com)(?:/issues)?/\d+| # Short URL
        (?:(?:pw|fx)bug\.dev|crbug\.com)(?:/issues)?/\d+| # Even shorter URL
        <(?:(?:pw|fx)bug\.dev|crbug\.com)/\d+>| # Markdown compatible
        https://issues\.(?:pigweed|fuchsia)\.dev(?:/issues)?/\d+| # Fully qualified bug for rustdoc
        https://issues.chromium.org/issues/\d+| # Fully qualified bug for rustdoc
        # Username@ with optional domain.
        [a-z]+@(?:[a-z][-a-z0-9]*(?:\.[a-z][-a-z0-9]*)+)?
    )
    (?:,[ ]?  # Additional.
        (?:
            b/\d+|  # Bug.
            # Username@ with optional domain.
            [a-z]+@(?:[a-z][-a-z0-9]*(?:\.[a-z][-a-z0-9]*)+)?
        )
    )*
[ ]-[ ].*\w  # Explanation.
)|
(?:  # Fuchsia style.
    \bTODO\(
        (?:fxbug\.dev/\d+|[a-z]+)  # Username or bug.
        (?:,[ ]?(?:fxbug\.dev/\d+|[a-z]+))*  # Additional usernames or bugs.
    \)
.*\w  # Explanation.
)|
(?:  # GitHub issues. No usernames allowed.
    \bTODO:[ ]
    (?:
        https://github\.com/[a-zA-Z0-9][-a-zA-Z0-9]*/[a-zA-Z][-_a-zA-Z0-9\.]*/issues/\d+
    )
[ ]-[ ].*\w  # Explanation.
)
    """,
    re.VERBOSE,
)
# pylint: enable=line-too-long

_TODO_OR_FIXME = re.compile(r'(\bTODO\b)|(\bFIXME\b)')
# todo-check: enable

# If seen, ignore this line and the next.
_IGNORE = 'todo-check: ignore'

# Ignore a whole section. Please do not change the order of these lines.
_DISABLE = 'todo-check: disable'
_ENABLE = 'todo-check: enable'


def _process_file(ctx: PresubmitContext, todo_pattern: re.Pattern, path: Path):
    with path.open() as ins:
        _LOG.debug('Evaluating path %s', path)
        enabled = True
        prev = ''

        try:

            for i, line in enumerate(ins, 1):
                if _DISABLE in line:
                    enabled = False
                elif _ENABLE in line:
                    enabled = True

                if not enabled or _IGNORE in line or _IGNORE in prev:
                    prev = line
                    continue

                if _TODO_OR_FIXME.search(line):
                    if not todo_pattern.search(line):
                        # todo-check: ignore
                        ctx.fail(f'Bad TODO on line {i}:', path)
                        ctx.fail(f'    {line.strip()}')
                        ctx.fail('Prefer this format in new code:')
                        # todo-check: ignore
                        ctx.fail(
                            '    TODO: https://pwbug.dev/12345 - More context.'
                        )

                prev = line

        except UnicodeDecodeError:
            # File is not text, like a gif.
            _LOG.debug('File %s is not a text file', path)


def create(
    todo_pattern: re.Pattern = BUGS_ONLY,
    exclude: Iterable[Pattern[str] | str] = EXCLUDE,
):
    """Create a todo_check presubmit step that uses the given pattern."""

    @filter_paths(exclude=exclude)
    def todo_check(ctx: PresubmitContext):
        """Check that TODO lines are valid."""  # todo-check: ignore
        for path in ctx.paths:
            _process_file(ctx, todo_pattern, path)

    return todo_check
