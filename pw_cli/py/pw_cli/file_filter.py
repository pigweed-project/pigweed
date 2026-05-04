# Copyright 2024 The Pigweed Authors
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
"""Class for describing file filter patterns."""

from __future__ import annotations

import os
from pathlib import Path
import re
from typing import Pattern, Iterable, Sequence, Collection


def _str_as_value(val) -> Iterable:
    yield from [val] if isinstance(val, str) else val


class FileFilter:
    """Allows checking if a path matches a series of filters.

    Positive filters (e.g. the file name matches a regex) and negative filters
    (path does not match a regular expression) may be applied.
    """

    def __init__(
        self,
        *,
        exclude: Iterable[Pattern[str] | str] = (),
        endswith: Iterable[str] = (),
        name: Iterable[Pattern[str] | str] = (),
        suffix: Iterable[str] = (),
    ) -> None:
        """Creates a FileFilter with the provided filters.

        If a string is passed to any of the filter arguments instead of an
        iterable, it is treated as a single item.

        Args:
            endswith: True if the end of the path is equal to any of the passed
                      strings
            exclude: If any of the passed regular expression match return False.
                     This overrides and other matches.
            name: Regexes to match with file names(pathlib.Path.name). True if
                  the resulting regex matches the entire file name.
            suffix: True if final suffix (as determined by pathlib.Path) is
                    matched by any of the passed str.
        """
        self._exclude = frozenset(re.compile(i) for i in _str_as_value(exclude))
        self._endswith = frozenset(_str_as_value(endswith))
        self._name = frozenset(re.compile(i) for i in _str_as_value(name))
        self._suffix = frozenset(_str_as_value(suffix))

        self._key = (self._exclude, self._endswith, self._name, self._suffix)

    @property
    def exclude(self) -> Collection[Pattern[str]]:
        return self._exclude

    @property
    def endswith(self) -> Collection[str]:
        return self._endswith

    @property
    def name(self) -> Collection[Pattern[str]]:
        return self._name

    @property
    def suffix(self) -> Collection[str]:
        return self._suffix

    def __eq__(self, other) -> bool:
        if not isinstance(other, FileFilter):
            return NotImplemented
        return self._key == other._key

    def __hash__(self) -> int:
        return hash(self._key)

    def matches(self, path: str | Path) -> bool:
        """Returns true if the path matches any filter but not an exclude.

        If no positive filters are specified, any paths that do not match a
        negative filter are considered to match.

        If 'path' is a Path object it is rendered as a posix path (i.e.
        using "/" as the path separator) before testing with 'exclude' and
        'endswith'.
        """

        path = Path(path)
        posix_path = path.as_posix()
        if any(exp.search(posix_path) for exp in self.exclude):
            return False

        # If there are no positive filters set, accept all paths.
        no_filters = not self.endswith and not self.name and not self.suffix

        return (
            no_filters
            or path.suffix in self.suffix
            or any(regex.fullmatch(path.name) for regex in self.name)
            or any(posix_path.endswith(end) for end in self.endswith)
        )

    def filter(self, paths: Sequence[str | Path]) -> Sequence[Path]:
        return [Path(x) for x in paths if self.matches(x)]

    def concat(
        self,
        file_filter: FileFilter | None = None,
        *,
        exclude: Iterable[Pattern | str] = (),
        endswith: Iterable[str] = (),
        name: Iterable[Pattern | str] = (),
        suffix: Iterable[str] = (),
    ) -> FileFilter:
        """Returns a new filter with the combined properties of its args.

        If a string is passed to any of the filter arguments instead of an
        iterable, it is treated as a single item.
        """
        return type(self)(
            exclude=[
                *_str_as_value(exclude),
                *self.exclude,
                *(file_filter.exclude if file_filter else ()),
            ],
            endswith=[
                *_str_as_value(endswith),
                *self.endswith,
                *(file_filter.endswith if file_filter else ()),
            ],
            name=[
                *_str_as_value(name),
                *self.name,
                *(file_filter.name if file_filter else ()),
            ],
            suffix=[
                *_str_as_value(suffix),
                *self.suffix,
                *(file_filter.suffix if file_filter else ()),
            ],
        )


def exclude_paths(
    exclusions: Iterable[Pattern[str]],
    paths: Iterable[Path],
    relative_to: Path | None = None,
) -> Iterable[Path]:
    """Excludes paths based on a series of regular expressions."""
    if relative_to:

        def relpath(path):
            return Path(os.path.relpath(path, relative_to))

    else:

        def relpath(path):
            return path

    for path in paths:
        if not any(e.search(relpath(path).as_posix()) for e in exclusions):
            yield path
