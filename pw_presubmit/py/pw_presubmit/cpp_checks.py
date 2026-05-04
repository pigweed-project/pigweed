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
"""C++-related checks."""

from __future__ import annotations

import logging
from pathlib import Path
import re
from typing import Callable, Iterable, Iterator, Pattern
import warnings

from pw_presubmit import (
    Check,
    build,
    format_code,
    presubmit_context,
)
from pw_presubmit.presubmit import filter_paths
from pw_presubmit.presubmit_context import PresubmitContext
from pw_presubmit.v2 import Step, Context

_LOG: logging.Logger = logging.getLogger(__name__)


def _fail(ctx, error, path):
    ctx.fail(error, path=path)


def _files_missing_pragma_once(paths: Iterable[Path]) -> Iterator[Path]:
    """Yields files that are missing '#pragma once'."""
    for path in paths:
        with path.open() as file:
            for line in file:
                if line.startswith('#pragma once'):
                    break
            else:
                yield path


class PragmaOnce(Step):
    """Presubmit check that ensures all header files contain '#pragma once'."""

    def __init__(self) -> None:
        super().__init__(
            endswith=format_code.CPP_HEADER_EXTS, exclude=[r'\.pb\.h$']
        )

    def run(self, ctx: Context) -> None:
        for path in _files_missing_pragma_once(ctx.paths):
            _fail(ctx, '#pragma once is missing!', path=path)

    def fix(self, ctx: Context) -> None:
        for path in _files_missing_pragma_once(ctx.paths):
            with path.open() as file:
                lines = file.readlines()

            # Find the first line that does not start with '//'
            # (ignoring leading whitespace)
            insert_idx = 0
            for i, line in enumerate(lines):
                if not line.lstrip().startswith('//'):
                    insert_idx = i
                    break
            else:
                insert_idx = len(lines)

            # Insert #pragma once after the first non-comment line
            lines.insert(insert_idx, '#pragma once\n')

            # Add a blank line after #pragma once if the next line is not empty
            if insert_idx + 1 < len(lines) and lines[insert_idx + 1].strip():
                lines.insert(insert_idx + 1, '\n')

            with path.open('w') as file:
                file.writelines(lines)

    def with_filter(
        self,
        *,
        exclude: Iterable[Pattern[str] | str],
        endswith: Iterable[str] = (),
    ) -> PragmaOnce:
        warnings.warn(
            "with_filter() is deprecated; use concat_filter() instead.",
            DeprecationWarning,
        )
        return self.concat_filter(exclude=exclude, endswith=endswith)


# For backwards compatibility:
pragma_once = PragmaOnce()


def include_guard_check(
    guard: Callable[[Path], str] | None = None,
    allow_pragma_once: bool = True,
) -> Check:
    """Create an include guard check.

    Args:
        guard: Callable that generates an expected include guard name for the
            given Path. If None, any include guard is acceptable, as long as
            it's consistent between the '#ifndef' and '#define' lines.
        allow_pragma_once: Whether to allow headers to use '#pragma once'
            instead of '#ifndef'/'#define'.
    """

    def stripped_non_comment_lines(iterable: Iterable[str]) -> Iterator[str]:
        """Yield non-comment non-empty lines from a C++ file."""
        multi_line_comment = False
        for line in iterable:
            line = line.strip()
            if not line:
                continue
            if line.startswith('//'):
                continue
            if line.startswith('/*'):
                multi_line_comment = True
            if multi_line_comment:
                if line.endswith('*/'):
                    multi_line_comment = False
                continue
            yield line

    def check_path(ctx: PresubmitContext, path: Path) -> None:
        """Check if path has a valid include guard."""

        _LOG.debug('checking %s', path)
        expected: str | None = None
        if guard:
            expected = guard(path)
            _LOG.debug('expecting guard %r', expected)

        with path.open() as ins:
            iterable = stripped_non_comment_lines(ins)
            first_line = next(iterable, '')
            _LOG.debug('first line %r', first_line)

            if allow_pragma_once and first_line.startswith('#pragma once'):
                _LOG.debug('found %r', first_line)
                return

            if expected:
                ifndef_expected = f'#ifndef {expected}'
                if not re.match(rf'^#ifndef {expected}$', first_line):
                    _fail(
                        ctx,
                        'Include guard is missing! Expected: '
                        f'{ifndef_expected!r}, Found: {first_line!r}',
                        path=path,
                    )
                    return

            else:
                match = re.match(
                    r'^#\s*ifndef\s+([a-zA-Z_][a-zA-Z_0-9]*)$',
                    first_line,
                )
                if not match:
                    _fail(
                        ctx,
                        'Include guard is missing! Expected "#ifndef" line, '
                        f'Found: {first_line!r}',
                        path=path,
                    )
                    return
                expected = match.group(1)

            second_line = next(iterable, '')
            _LOG.debug('second line %r', second_line)

            if not re.match(rf'^#\s*define\s+{expected}$', second_line):
                define_expected = f'#define {expected}'
                _fail(
                    ctx,
                    'Include guard is missing! Expected: '
                    f'{define_expected!r}, Found: {second_line!r}',
                    path=path,
                )
                return

            _LOG.debug('passed')

    @filter_paths(endswith=format_code.CPP_HEADER_EXTS, exclude=(r'\.pb\.h$',))
    def include_guard(ctx: PresubmitContext):
        """Check that all header files contain an include guard."""
        ctx.paths = presubmit_context.apply_exclusions(ctx)
        for path in ctx.paths:
            check_path(ctx, path)

    return include_guard


@Check
def asan(ctx: PresubmitContext) -> None:
    """Test with the address sanitizer."""
    build.gn_gen(ctx)
    build.ninja(ctx, 'asan')


@Check
def msan(ctx: PresubmitContext) -> None:
    """Test with the memory sanitizer."""
    build.gn_gen(ctx)
    build.ninja(ctx, 'msan')


@Check
def tsan(ctx: PresubmitContext) -> None:
    """Test with the thread sanitizer."""
    build.gn_gen(ctx)
    build.ninja(ctx, 'tsan')


@Check
def ubsan(ctx: PresubmitContext) -> None:
    """Test with the undefined behavior sanitizer."""
    build.gn_gen(ctx)
    build.ninja(ctx, 'ubsan')


@Check
def runtime_sanitizers(ctx: PresubmitContext) -> None:
    """Test with the address, thread, and undefined behavior sanitizers."""
    build.gn_gen(ctx)
    build.ninja(ctx, 'runtime_sanitizers')


def all_sanitizers():
    # TODO: b/234876100 - msan will not work until the C++ standard library
    # included in the sysroot has a variant built with msan.
    return [asan, tsan, ubsan, runtime_sanitizers]
