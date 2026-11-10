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
"""Formatting library core."""

import abc
from dataclasses import dataclass
import difflib
import logging
from pathlib import Path
from typing import Callable, Iterable, Iterator

from pw_cli.file_filter import FileFilter
from pw_cli.tool_runner import ToolRunner, BasicSubprocessRunner


_LOG: logging.Logger = logging.getLogger(__name__)


def _ensure_newline(orig: str) -> str:
    """Adds a warning and newline to any file without a trailing newline."""

    if orig.endswith('\n'):
        return orig
    return orig + '\nNo newline at end of file\n'


def simple_diff(path: Path, original: str, formatted: str) -> str:
    """Produces a diff of the contents of two files."""

    original = _ensure_newline(original)
    formatted = _ensure_newline(formatted)
    return ''.join(
        difflib.unified_diff(
            original.splitlines(keepends=True),
            formatted.splitlines(keepends=True),
            f'{path}  (original)',
            f'{path}  (reformatted)',
        )
    )


DiffCallback = Callable[[Path, str, str], str]
"""The callback type for producing diffs.

Arugments:
    path: File path of the file being diffed.
    original: The contents of the original file, as a string.
    formatted: The contents of the formatted file, as a string.

Returns:
    A human readable diff as a string.
"""


@dataclass(frozen=True)
class FormattedFileContents:
    """The result of running a code formatter on the contents of a file.

    This type is returned by in-memory formatting check operations.

    Attributes:
        ok: A boolean indicating whether or not formatting was successful.
        formatted_file_contents: The contents of the resulting formatted file
            as bytes.
        error_message: A string containing any errors or warnings produced by
            the formatting process.
    """

    ok: bool
    formatted_file_contents: bytes
    error_message: str | None


@dataclass(frozen=True)
class FormattedDiff:
    """The resulting diff of applying a code formatter to a file.

    Attributes:
        ok: A boolean indicating whether or not formatting was successful.
        diff: The resulting diff of applying code formatting, as a
            human-readable string.
        error_message: A string containing any errors or warnings produced by
            the formatting process.
        file_path: The path of the corresponding file that produced this diff.
    """

    ok: bool
    diff: str
    error_message: str | None
    file_path: Path


@dataclass(frozen=True)
class FormatFixStatus:
    """The status of running a code formatter in-place on a file.

    This type is returned by in-place formatting fix operations.

    Attributes:
        ok: A boolean indicating whether or not formatting was successful.
        error_message: A string containing any errors or warnings produced by
            the formatting process.
    """

    ok: bool
    error_message: str | None


class FileChecker(abc.ABC):
    """Abstract class for a code format check tool.

    This class does not have the ability to apply formatting to files, and
    instead only allows in-memory checks to produce expected resulting diffs.

    Attributes:
        file_patterns: A :py:class:`pw_cli.file_filter.FileFilter` that
            describes what kind of files this check applies to.
        mnemonic: A human-readable description of the kind of checker this is
            (e.g. "C and C++", "Bazel", "Python (black)").
        tool_runner: The :py:class:`pw_cli.tool_runner.ToolRunner` to use
            when calling out to subprocesses.
        diff_tool: The :py:data:`pw_presubmit.format.core.DiffCallback` to use
            when producing formatting diffs.
    """

    def __init__(
        self,
        *,
        file_patterns: FileFilter,
        mnemonic: str,
        tool_runner: ToolRunner = BasicSubprocessRunner(),
        diff_tool: DiffCallback = simple_diff,
    ):
        self.file_patterns = file_patterns
        self.mnemonic = mnemonic
        # Always call `self.run_tool` rather than `subprocess.run`, as it allows
        # injection of tools and other environment-specific handlers.
        self.run_tool = tool_runner
        self.diff_tool = diff_tool

    @abc.abstractmethod
    def format_file_in_memory(
        self, file_path: Path, file_contents: bytes
    ) -> FormattedFileContents:
        """Returns the formatted version of a file as in-memory bytes.

        ``file_path`` and ``file_content`` represent the same file. Both are
        provided for convenience. Use ``file_path`` if you can do so without
        modifying the file, or use ``file_contents`` if the formatting tool
        provides a mechanism for formatting the file by piping it to stdin.

        Any subprocess calls should be initiated with ``self.run_tool()`` to
        enable testing and injection of tools and tool wrappers.

        **WARNING**: A :py:class:`pw_presubmit.format.core.FileChecker` must
        **never** modify the file at``file_path``.

        Returns:
            A populated
            :py:class:`pw_presubmit.format.core.FormattedFileContents` that
            contains either the result of formatting the file, or an error
            message.
        """

    def get_formatting_diff(
        self, file_path: Path, dry_run: bool = False
    ) -> FormattedDiff | None:
        """Returns a diff comparing a file to its formatted version.

        If ``dry_run`` is ``True``, the diff will always be ``None``.

        Returns:
            ``None`` if there is no difference after formatting **OR** if
            ``dry_run`` is enabled. Otherwise, a
            :py:class:`pw_presubmit.format.core.FormattedDiff` is returned
            containing either a diff or an error.
        """
        original = file_path.read_bytes()

        formatted = self.format_file_in_memory(file_path, original)

        if not formatted.ok:
            return FormattedDiff(
                diff='',  # Don't try to diff.
                ok=False,
                file_path=file_path,
                error_message=formatted.error_message,
            )

        if dry_run:
            return None

        # No difference found.
        if formatted.formatted_file_contents == original:
            return None

        return FormattedDiff(
            diff=self.diff_tool(
                file_path,
                original.decode(errors='replace'),
                formatted.formatted_file_contents.decode(errors='replace'),
            ),
            file_path=file_path,
            error_message=formatted.error_message,
            ok=True,
        )

    def get_formatting_diffs(
        self, paths: Iterable[Path], dry_run: bool = False
    ) -> Iterator[FormattedDiff]:
        """Checks the formatting of many files without modifying them.

        This method may be overridden to optimize for formatters that allow
        checking multiple files in a single invocation, though you may need
        to do additional parsing to produce diffs or error messages associated
        with each file path.

        Returns:
            An iterator of :py:class:`pw_presubmit.format.core.FormattedDiff`
            objects for each file with identified formatting issues.
        """

        for path in paths:
            diff = self.get_formatting_diff(path, dry_run)
            if diff is not None:
                yield diff


class FileFormatter(FileChecker):
    """Abstract class for a code format fix tool."""

    def __init__(self, **kwargs):
        super().__init__(**kwargs)

    @abc.abstractmethod
    def format_file(self, file_path: Path) -> FormatFixStatus:
        """Formats the provided file in-place.

        Any subprocess calls should be initiated with ``self.run_tool()`` to
        enable testing and injection of tools and tool wrappers.

        Returns:
            A FormatFixStatus that contains relevant errors/warnings.
        """

    def format_files(
        self, paths: Iterable[Path], keep_warnings: bool = True
    ) -> Iterator[tuple[Path, FormatFixStatus]]:
        """Formats the provided files and fixes them in-place.

        All files must be updated to contain the formatted version. If errors
        are encountered along the way, they should be collected and returned as
        a dictionary that maps the path of the file to a string that
        describes the errors encountered while processing that file.

        Any subprocess calls should be initiated with ``self.run_tool()`` to
        enable testing and injection of tools and tool wrappers.

        This method may be overridden to optimize for formatters that allow
        formatting multiple files in a single invocation, though you may need
        to do additional parsing to associate error messages with the paths of
        the files that produced them.

        Returns:
            An iterator of ``Path`` and
            :py:class:`pw_presubmit.format.core.FormatFixStatus` pairs for each
            file that was not successfully formatted. If ``keep_warnings`` is
            ``True``, any successful format operations with warnings will also
            be returned.
        """

        for file_path in paths:
            status = self.format_file(file_path)
            if not status.ok or (status.error_message and keep_warnings):
                yield (file_path, status)
