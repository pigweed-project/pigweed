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
"""Private utilities for pw_presubmit."""

from __future__ import annotations

import collections.abc
import os
from pathlib import Path
from typing import Iterable, Iterator, Sequence


def relative_paths(paths: Iterable[Path], start: Path) -> Iterable[Path]:
    """Returns relative Paths calculated with os.path.relpath."""
    for path in paths:
        yield Path(os.path.relpath(path, start))


def flatten(*items) -> Iterator:
    """Yields items from a series of items and nested iterables.

    This function is used to flatten arbitrarily nested lists. str and bytes
    are kept intact.
    """

    for item in items:
        if isinstance(item, collections.abc.Iterable) and not isinstance(
            item, (str, bytes, bytearray)
        ):
            yield from flatten(*item)
        else:
            yield item


def format_time(time_s: float) -> str:
    """Format a time duration as a string."""
    minutes, seconds = divmod(time_s, 60)
    if minutes < 60:
        return f' {int(minutes)}:{seconds:04.1f}'
    hours, minutes = divmod(minutes, 60)
    return f'{int(hours):d}:{int(minutes):02}:{int(seconds):02}'


def make_box(section_alignments: Sequence[str]) -> str:
    indices = [i + 1 for i in range(len(section_alignments))]
    top_sections = '{2}'.join('{1:{1}^{width%d}}' % i for i in indices)
    mid_sections = '{5}'.join(
        '{section%d:%s{width%d}}' % (i, section_alignments[i - 1], i)
        for i in indices
    )
    bot_sections = '{9}'.join('{8:{8}^{width%d}}' % i for i in indices)

    return ''.join(
        [
            '{0}',
            *top_sections,
            '{3}\n',
            '{4}',
            *mid_sections,
            '{6}\n',
            '{7}',
            *bot_sections,
            '{10}',
        ]
    )
