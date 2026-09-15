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
"""Defines AsyncPath, an asynchronous wrapper around pathlib.Path."""

import asyncio
from collections.abc import AsyncIterator
import functools
import os
from pathlib import Path
from typing import Any, Union

_PURE_PROPERTIES = {
    'anchor',
    'drive',
    'name',
    'parent',
    'parents',
    'parts',
    'root',
    'stem',
    'suffix',
    'suffixes',
}

_PURE_METHODS = {
    'as_posix',
    'as_uri',
    'is_absolute',
    'is_relative_to',
    'is_reserved',
    'joinpath',
    'match',
    'relative_to',
    'resolve',
    'with_name',
    'with_stem',
    'with_suffix',
}

_ITERABLE_METHODS = {
    'glob',
    'iterdir',
    'rglob',
    'walk',
}


class _AsyncIterableResult:
    """An awaitable result that also supports async iteration."""

    def __init__(self, coro: Any) -> None:
        self._coro = coro

    def __await__(self) -> Any:
        return self._coro.__await__()

    async def __aiter__(self) -> AsyncIterator[Any]:
        for item in await self._coro:
            yield item


def _convert_path(arg: Any) -> Any:
    """Unwraps an AsyncPath to a Path instance."""
    if isinstance(arg, AsyncPath):
        return arg.path
    return arg


def _wrap_result(val: Any) -> Any:
    """Wraps Path results back into AsyncPath."""
    if isinstance(val, Path):
        return AsyncPath(val)
    if isinstance(val, list):
        return [_wrap_result(item) for item in val]
    if isinstance(val, tuple):
        return tuple(_wrap_result(item) for item in val)
    return val


@functools.total_ordering
class AsyncPath(os.PathLike[str]):
    """An asynchronous wrapper around pathlib.Path using asyncio executors.

    `AsyncPath` wraps an underlying `pathlib.Path` instance and forwards
    operations:

    * **Pure path operations**: Pure path properties (`name`, `stem`, `suffix`,
      `parent`, `parents`, `parts`, etc.) and pure path methods (`resolve`,
      `with_name`, `with_suffix`, `relative_to`, `/` operator, etc.) operate
      synchronously and return wrapped `AsyncPath` instances where applicable.
    * **Asynchronous I/O operations**: File system operations (`read_text`,
      `write_text`, `read_bytes`, `write_bytes`, `exists`, `is_file`,
      `is_dir`, `stat`, `mkdir`, `unlink`, `rename`, etc.) are executed on an
      asyncio executor thread and must be awaited.
    * **Iterables / generators**: Directory iteration and globbing methods
      (`iterdir`, `glob`, `rglob`, `walk`) return awaitable async iterables
      that can be consumed with `async for` or awaited directly into a list.
    * **PathLike & ordering**: `AsyncPath` implements `os.PathLike[str]` via
      `os.fspath` and supports rich comparisons and sorting.

    Basic use examples (corresponding to `pathlib.Path` basic use):

    Importing the main class:
        >>> from pw_fortifier.async_path import AsyncPath

    Listing subdirectories:
        >>> p = AsyncPath('.')
        >>> [x async for x in p.iterdir() if await x.is_dir()]
        [AsyncPath(PosixPath('docs')), AsyncPath(PosixPath('dist')), ...]
        # Or by awaiting directly:
        >>> [x for x in await p.iterdir() if await x.is_dir()]

    Listing Python source files in this directory tree:
        >>> await p.glob('**/*.py')
        [AsyncPath(PosixPath('setup.py')), ...]
        # Or using async for:
        >>> [x async for x in p.glob('**/*.py')]

    Navigating inside a directory tree:
        >>> p = AsyncPath('/etc')
        >>> q = p / 'init.d' / 'reboot'
        >>> q
        AsyncPath(PosixPath('/etc/init.d/reboot'))
        >>> q.resolve()
        AsyncPath(PosixPath('/etc/rc.d/init.d/halt'))

    Querying path properties:
        >>> await q.exists()
        True
        >>> await q.is_dir()
        False

    Reading file contents:
        >>> await q.read_text()
        '#!/bin/bash\\n'
    """

    path: Path
    """The underlying pathlib.Path instance."""

    def __init__(self, *args: Union[str, Path, 'AsyncPath']) -> None:
        """Initializes AsyncPath from strings, Paths, or other AsyncPaths.

        Args:
            *args: Path components to join into a single path.
        """
        resolved_args = [_convert_path(a) for a in args]
        self.path = Path(*resolved_args)

    def __getattr__(self, name: str) -> Any:
        attr = getattr(self.path, name)

        # Properties can be accessed synchronously.
        if name in _PURE_PROPERTIES:
            if name == 'parents':
                return tuple(AsyncPath(p) for p in attr)
            return _wrap_result(attr)

        # Pure methods don't do any I/O and can be executed synchronously.
        if name in _PURE_METHODS and callable(attr):

            def _run_sync(*args: Any, **kwargs: Any) -> Any:
                converted_args = [_convert_path(a) for a in args]
                converted_kwargs = {
                    k: _convert_path(v) for k, v in kwargs.items()
                }
                return _wrap_result(attr(*converted_args, **converted_kwargs))

            return _run_sync

        if callable(attr):

            # Run other methods async to allow progress while waiting for I/O
            async def _run_async(*args: Any, **kwargs: Any) -> Any:
                converted_args = [_convert_path(a) for a in args]
                converted_kwargs = {
                    k: _convert_path(v) for k, v in kwargs.items()
                }
                call = functools.partial(
                    attr, *converted_args, **converted_kwargs
                )
                loop = asyncio.get_running_loop()

                def _execute() -> Any:
                    res = call()
                    if hasattr(res, '__iter__') and not isinstance(
                        res, (bytes, str, list, tuple, dict, set)
                    ):
                        res = list(res)
                    return res

                res = await loop.run_in_executor(None, _execute)
                return _wrap_result(res)

            # Wrap values returned by generators to allow for async iteration.
            if name in _ITERABLE_METHODS:

                def _run_iterable(*args: Any, **kwargs: Any) -> Any:
                    return _AsyncIterableResult(_run_async(*args, **kwargs))

                return _run_iterable

            return _run_async

        return attr

    def __fspath__(self) -> str:
        """Returns the file system path representation."""
        return str(self.path)

    def __lt__(self, other: Any) -> bool:
        """Orders AsyncPath instances by their underlying path."""
        if isinstance(other, AsyncPath):
            return self.path < other.path
        if isinstance(other, Path):
            return self.path < other
        return NotImplemented

    def __truediv__(self, other: Union[str, Path, 'AsyncPath']) -> 'AsyncPath':
        """Joins paths using the / operator."""
        if isinstance(other, AsyncPath):
            return AsyncPath(self.path / other.path)
        return AsyncPath(self.path / other)

    def __str__(self) -> str:
        return str(self.path)

    def __repr__(self) -> str:
        return f'AsyncPath({self.path!r})'

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, AsyncPath):
            return False
        return self.path == other.path

    def __hash__(self) -> int:
        return hash(self.path)
