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
"""A step in a Presubmit program."""

from __future__ import annotations

from abc import ABC, abstractmethod
import copy
import dataclasses
import logging
from pathlib import Path
import re
from typing import Callable, Iterable, Pattern, TypeVar


from pw_cli.file_filter import FileFilter
from pw_presubmit.private.result import Failure


def _camel_to_snake(name: str) -> str:
    """Translates CamelCase to snake_case."""
    s1 = re.sub('(.)([A-Z][a-z]+)', r'\1_\2', name)
    return re.sub('([a-z0-9])([A-Z])', r'\1_\2', s1).lower()


_LOG = logging.getLogger("pw_presubmit")


@dataclasses.dataclass(frozen=True)
class Context:
    """Context passed to presubmit checks.

    This object provides information about the repository root, the files being
    processed, and the output directory for the check. It also tracks failures
    reported by the check.

    Checks use this object to find files to process and to report failures via
    the `fail()` method.
    """

    # Do NOT add any additional properties to this object.
    root: Path
    output_dir: Path
    paths: tuple[Path, ...]
    all_paths: tuple[Path, ...]
    # _failures is mutable despite the class being frozen.
    _failures: list[Failure] = dataclasses.field(default_factory=list)

    def fail(
        self,
        description: str,
        path: Path | None = None,
        line: int | None = None,
        *,
        exception: Exception | None = None,
    ) -> None:
        """Add a failure to this presubmit step.

        Args:
            description: A description of the failure.
            path: The file path where the failure occurred.
            line: The line number where the failure occurred.
            exception: An unexpected exception that caused the failure. A
              stack trace will be logged.
        """
        failure = Failure(description, path, line)
        _LOG.warning('%s', failure, exc_info=exception)
        self._failures.append(failure)

    @property
    def failed(self) -> bool:
        return bool(self._failures)


_T = TypeVar('_T', bound='Step')


class Step(ABC):
    """A single step in a Presubmit run.

    A step with no filters always runs, regardless of which files are included
    in the presubmit.
    """

    def __init__(
        self,
        *,
        exclude: Iterable[Pattern[str] | str] = (),
        endswith: Iterable[str] = (),
        match_name: Iterable[Pattern[str] | str] = (),
        suffix: Iterable[str] = (),
        file_filter: FileFilter = FileFilter(),
    ) -> None:
        """If no filters are specified, the step applies to all files."""
        self._filter = file_filter.concat(
            exclude=exclude, endswith=endswith, name=match_name, suffix=suffix
        )
        self._name = _camel_to_snake(type(self).__name__)

    @property
    def name(self) -> str:
        return self._name

    @abstractmethod
    def run(self, ctx: Context) -> None:
        """Runs the presubmit check."""

    def fix(  # pylint: disable=no-self-use,unused-argument
        self, ctx: Context
    ) -> None:
        """Attempt to fix failures automatically.

        The fix function should report success / failure exactly the same as the
        run function. To signal that it was unable to fix the issue, it should
        throw an exception or call `ctx.fail`. The base implementation raises
        NotImplementedError.
        """
        raise NotImplementedError

    @property
    def filter(self) -> FileFilter:
        """Returns the FileFilter for this step."""
        return self._filter

    def replace_filter(
        self: _T,
        *,
        exclude: Iterable[Pattern[str] | str] = (),
        endswith: Iterable[str] = (),
        match_name: Iterable[Pattern[str] | str] = (),
        suffix: Iterable[str] = (),
        file_filter: FileFilter = FileFilter(),
    ) -> _T:
        """Make a copy of this step with a replaced FileFilter."""
        clone = copy.deepcopy(self)
        clone._filter = file_filter.concat(  # pylint: disable=protected-access
            exclude=exclude, endswith=endswith, name=match_name, suffix=suffix
        )
        return clone

    def concat_filter(
        self: _T,
        *,
        exclude: Iterable[Pattern[str] | str] = (),
        endswith: Iterable[str] = (),
        match_name: Iterable[Pattern[str] | str] = (),
        suffix: Iterable[str] = (),
        file_filter: FileFilter = FileFilter(),
    ) -> _T:
        """Make a copy of this step with concatenated FileFilter rules."""
        clone = copy.deepcopy(self)
        clone._filter = self._filter.concat(  # pylint: disable=protected-access
            exclude=exclude, endswith=endswith, name=match_name, suffix=suffix
        ).concat(file_filter)
        return clone


def step(
    *,
    name: str | None = None,
    fix: Callable[[Context], bool] | None = None,
    exclude: Iterable[Pattern[str] | str] = (),
    endswith: Iterable[str] = (),
    match_name: Iterable[Pattern[str] | str] = (),
    suffix: Iterable[str] = (),
    file_filter: FileFilter = FileFilter(),
) -> Callable[[Callable[[Context], None]], Step]:
    """Decorator to create a Step from a function."""
    fix_func = fix  # Avoid shadowing in class body

    def decorator(func: Callable[[Context], None]) -> Step:
        class _DecoratedStep(Step):
            def __init__(self) -> None:
                super().__init__(
                    exclude=exclude,
                    endswith=endswith,
                    match_name=match_name,
                    suffix=suffix,
                    file_filter=file_filter,
                )
                self.__doc__ = func.__doc__
                if name is None:
                    self._name = _camel_to_snake(func.__name__)
                else:
                    self._name = name

            def run(self, ctx: Context) -> None:
                func(ctx)

            if fix_func is not None:

                def fix(  # pylint: disable=unused-argument
                    self, ctx: Context
                ) -> None:
                    fix_func(ctx)

        return _DecoratedStep()

    return decorator


@dataclasses.dataclass(frozen=True)
class FilteredStep:
    step: Step
    paths: tuple[Path, ...]

    @property
    def name(self) -> str:
        return self.step.name
