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
"""Basic types for presubmit checks."""

from __future__ import annotations

import collections
import collections.abc
import copy
import dataclasses
import itertools
from inspect import Parameter, signature
import logging
import time
from pathlib import Path
from typing import (
    Any,
    Callable,
    Iterable,
    Iterator,
    NamedTuple,
    Pattern,
    Protocol,
    Sequence,
)

from pw_cli.plural import plural
from pw_cli.file_filter import FileFilter

from pw_presubmit.private.result import (
    PresubmitFailure,
    PresubmitResult,
)
from pw_presubmit.private.step import Step
from pw_presubmit.private.tools import (
    flatten,
    format_time,
    relative_paths,
)

_LOG = logging.getLogger('pw_presubmit')


class GenericContext(Protocol):
    """Minimal context needed for a presubmit step.

    Use a protocol to avoid circular imports and to prepare for supporting a new
    minimal presubmit context.
    """

    @property
    def paths(self) -> tuple[Path, ...]: ...

    @property
    def output_dir(self) -> Path: ...

    @property
    def failed(self) -> bool: ...


@dataclasses.dataclass
class SubStep:
    name: str | None
    _func: Callable[..., PresubmitResult]
    args: Sequence[Any] = ()
    kwargs: dict[str, Any] = dataclasses.field(default_factory=lambda: {})

    def __call__(self, ctx: GenericContext) -> PresubmitResult:
        if self.name:
            _LOG.info('%s', self.name)
        return self._func(ctx, *self.args, **self.kwargs)


class Check:
    """Wraps a presubmit check function.

    This class consolidates the logic for running and logging a presubmit check.
    It also supports filtering the paths passed to the presubmit check.
    """

    def __init__(
        self,
        check: (  # pylint: disable=redefined-outer-name
            Callable | Iterable[SubStep]
        ),
        path_filter: FileFilter = FileFilter(),
        always_run: bool = True,
        name: str | None = None,
        doc: str | None = None,
    ) -> None:
        # Since Check wraps a presubmit function, adopt that function's name.
        self.name: str = ''
        self.doc: str = ''
        if isinstance(check, Check):
            self.name = check.name
            self.doc = check.doc
        elif callable(check):
            self.name = check.__name__
            self.doc = check.__doc__ or ''

        if name:
            self.name = name
        if doc:
            self.doc = doc

        if not self.name:
            raise ValueError(f'no name for step: {check}')

        self._substeps_raw: Iterable[SubStep]
        if isinstance(check, collections.abc.Iterator):
            self._substeps_raw = check
        else:  # For consistency, make an individual check a substep
            assert callable(check)
            _ensure_is_valid_presubmit_check_function(check)

            self._substeps_raw = iter((SubStep(None, check),))
        self._substeps_saved: Sequence[SubStep] = ()

        self.filter = path_filter
        self.always_run: bool = always_run

        self._is_presubmit_check_object = True

    def substeps(self) -> Sequence[SubStep]:
        """Return the SubSteps of the current step.

        This is where the list of SubSteps is actually evaluated. It can't be
        evaluated in the constructor because the Iterable passed into the
        constructor might not be ready yet.
        """
        if not self._substeps_saved:
            self._substeps_saved = tuple(self._substeps_raw)
        return self._substeps_saved

    def __repr__(self):
        # This returns just the name so it's easy to show the entire list of
        # steps with '--help'.
        return self.name

    def unfiltered(self) -> Check:
        """Create a new check identical to this one, but without the filter."""
        clone = copy.copy(self)
        clone.filter = FileFilter()
        return clone

    def with_filter(
        self,
        *,
        endswith: Iterable[str] = (),
        exclude: Iterable[Pattern[str] | str] = (),
    ) -> Check:
        """Create a new check identical to this one, but with extra filters.

        Add to the existing filter, perhaps to exclude an additional directory.

        Args:
            endswith: Passed through to FileFilter.
            exclude: Passed through to FileFilter.

        Returns a new check.
        """
        return self.with_file_filter(
            FileFilter(endswith=endswith, exclude=exclude)
        )

    def with_file_filter(self, file_filter: FileFilter) -> Check:
        """Create a new check identical to this one, but with extra filters.

        Add to the existing filter, perhaps to exclude an additional directory.

        Args:
            file_filter: Additional filter rules.

        Returns a new check.
        """
        clone = copy.copy(self)
        clone.filter = self.filter.concat(file_filter)
        return clone

    def run(
        self,
        ctx: GenericContext,
        substep: str | None = None,
    ) -> PresubmitResult:
        """Runs the presubmit check on the provided paths."""

        substep_part = f'.{substep}' if substep else ''
        _LOG.debug(
            'Running %s%s on %s',
            self.name,
            substep_part,
            plural(ctx.paths, "file"),
        )

        start_time_s = time.time()
        result: PresubmitResult
        if substep:
            result = self.run_substep(ctx, substep)
        else:
            result = self(ctx)
        time_str = format_time(time.time() - start_time_s)
        _LOG.debug('%s %s', self.name, result.value)

        _LOG.debug('%s duration:%s', self.name, time_str)

        return result

    def _try_call(
        self,
        func: Callable,
        ctx,
        *args,
        **kwargs,
    ) -> PresubmitResult:
        try:
            result = func(ctx, *args, **kwargs)
            if ctx.failed:
                return PresubmitResult.FAIL
            if isinstance(result, PresubmitResult):
                return result
            return PresubmitResult.PASS

        except PresubmitFailure as failure:
            if str(failure):
                _LOG.warning('%s', failure)

            return PresubmitResult.FAIL

        except Exception as _failure:  # pylint: disable=broad-except
            _LOG.exception('Presubmit check %s failed!', self.name)

            return PresubmitResult.FAIL

        except KeyboardInterrupt:
            print()
            return PresubmitResult.CANCEL

    def run_substep(
        self, ctx: GenericContext, name: str | None
    ) -> PresubmitResult:
        for substep in self.substeps():
            if substep.name == name:
                return self._try_call(substep, ctx)

        expected = ', '.join(repr(s.name) for s in self.substeps())
        raise LookupError(f'bad substep name: {name!r} (expected: {expected})')

    def __call__(self, ctx: GenericContext) -> PresubmitResult:
        """Calling a Check calls its underlying substeps directly.

        This makes it possible to call functions wrapped by @filter_paths. The
        prior filters are ignored, so new filters may be applied.
        """
        result: PresubmitResult
        for substep in self.substeps():
            result = self._try_call(substep, ctx)
            if result and result != PresubmitResult.PASS:
                return result
        return PresubmitResult.PASS


@dataclasses.dataclass
class FilteredCheck:
    check: Check
    paths: Sequence[Path]
    substep: str | None = None

    @property
    def name(self) -> str:
        return self.check.name

    def run(
        self,
        ctx: GenericContext,
    ):
        return self.check.run(ctx, self.substep)


class PresubmitCheckTrace(NamedTuple):
    ctx: GenericContext
    check: Check | None
    func: str | None
    args: Iterable[Any]
    kwargs: dict[Any, Any]
    call_annotation: dict[Any, Any]

    def __repr__(self) -> str:
        return f'''CheckTrace(
  ctx={self.ctx.output_dir}
  id(ctx)={id(self.ctx)}
  check={self.check}
  args={self.args}
  kwargs={self.kwargs.keys()}
  call_annotation={self.call_annotation}
)'''


def _required_args(function: Callable) -> Iterable[Parameter]:
    """Returns the required arguments for a function."""
    optional_types = Parameter.VAR_POSITIONAL, Parameter.VAR_KEYWORD

    for param in signature(function).parameters.values():
        if param.default is param.empty and param.kind not in optional_types:
            yield param


def _ensure_is_valid_presubmit_check_function(chk: Callable) -> None:
    """Checks if a Callable can be used as a presubmit check."""
    try:
        required_args = tuple(_required_args(chk))
    except (TypeError, ValueError):
        raise TypeError(
            'Presubmit checks must be callable, but '
            f'{chk!r} is a {type(chk).__name__}'
        )

    if len(required_args) != 1:
        raise TypeError(
            f'Presubmit check functions must have exactly one required '
            f'positional argument (the PresubmitContext), but '
            f'{chk.__name__} has {len(required_args)} required arguments'
            + (
                f' ({", ".join(a.name for a in required_args)})'
                if required_args
                else ''
            )
        )


class Program(collections.abc.Sequence):
    """A sequence of presubmit checks; basically a tuple with a name."""

    def __init__(self, name: str, steps: Iterable[Callable]):
        self.name = name

        def ensure_check(step):
            if isinstance(step, Check):
                return step
            if isinstance(step, Step):
                return Check(
                    step.run,
                    name=step.name,
                    doc=step.__doc__ or '',
                    path_filter=step.filter,
                )

            return Check(step)

        self._steps: tuple[Check, ...] = tuple(
            {ensure_check(s): None for s in flatten(steps)}
        )

    def __getitem__(self, i):
        return self._steps[i]

    def __len__(self):
        return len(self._steps)

    def __str__(self):
        return self.name

    def title(self):
        return f'{self.name if self.name else ""} presubmit checks'.strip()

    def filter(
        self, root: Path, paths: Sequence[Path]
    ) -> tuple[FilteredCheck, ...]:
        """Returns a tuple of FilteredChecks for the given paths."""
        rel_paths = tuple(relative_paths(paths, root))
        posix_paths = tuple(p.as_posix() for p in rel_paths)

        filter_to_checks = collections.defaultdict(list)
        for chk in self._steps:
            assert isinstance(
                chk, Check
            ), f'Cannot filter a program containing {type(chk)}'
            filter_to_checks[chk.filter].append(chk)

        checks_to_paths = {}
        for filt, checks in filter_to_checks.items():
            filtered_paths = tuple(
                path
                for path, filter_path in zip(paths, posix_paths)
                if filt.matches(filter_path)
            )

            for chk in checks:
                if filtered_paths or chk.always_run:
                    checks_to_paths[chk] = filtered_paths
                else:
                    _LOG.debug('Skipping "%s": no relevant files', chk.name)

        return tuple(
            FilteredCheck(c, checks_to_paths[c])
            for c in self._steps
            if c in checks_to_paths
        )


class Programs(collections.abc.Mapping):
    """A mapping of presubmit check programs.

    Use is optional. Helpful when managing multiple presubmit check programs.
    """

    def __init__(self, **programs: Sequence):
        """Initializes a name: program mapping from the provided keyword args.

        A program is a sequence of presubmit check functions. The sequence may
        contain nested sequences, which are flattened.
        """
        self._programs: dict[str, Program] = {
            name: Program(name, checks) for name, checks in programs.items()
        }

    def all_steps(self) -> dict[str, Check]:
        return {c.name: c for c in itertools.chain(*self.values())}

    def __getitem__(self, item: str) -> Program:
        return self._programs[item]

    def __iter__(self) -> Iterator[str]:
        return iter(self._programs)

    def __len__(self) -> int:
        return len(self._programs)
