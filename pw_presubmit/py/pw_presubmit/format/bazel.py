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
"""Code formatter plugin for Bazel build files."""

from pathlib import Path
from typing import Dict, Final, Iterable, Iterator, List, Sequence, Tuple

from pw_cli.file_filter import FileFilter
from pw_presubmit.format.core import (
    FileFormatter,
    FormattedFileContents,
    FormatFixStatus,
)


DEFAULT_BAZEL_FILE_PATTERNS = FileFilter(
    endswith=['.bazel', '.bzl'],
    name=['^BUILD$', '^WORKSPACE$'],
)


class BuildifierFormatter(FileFormatter):
    """A formatter that runs ``buildifier`` on files."""

    # These warnings are safe to enable because they can always be auto-fixed.
    DEFAULT_WARNINGS_TO_FIX: Final[Sequence[str]] = (
        'load',
        'native-build',
        'unsorted-dict-items',
    )

    def __init__(
        self, warnings_to_fix: Sequence[str] = DEFAULT_WARNINGS_TO_FIX, **kwargs
    ):
        kwargs.setdefault('mnemonic', 'Bazel')
        kwargs.setdefault('file_patterns', DEFAULT_BAZEL_FILE_PATTERNS)
        super().__init__(**kwargs)
        self.warnings_to_fix = list(warnings_to_fix)

    @staticmethod
    def _detect_file_type(file_path: Path) -> str:
        if file_path.name == 'MODULE.bazel' or file_path.name.endswith(
            '.MODULE.bazel'
        ):
            return 'module'
        if file_path.name == 'WORKSPACE':
            return 'workspace'
        if '.bzl' in file_path.name:
            return 'bzl'
        if 'BUILD' in file_path.name or file_path.suffix == '.bazel':
            return 'build'

        return 'default'

    def _files_by_type(self, paths: Iterable[Path]) -> Dict[str, List[Path]]:
        all_types = (
            'module',
            'workspace',
            'bzl',
            'build',
            'default',
        )
        all_files: Dict[str, List[Path]] = {t: [] for t in all_types}

        for file_path in paths:
            all_files[self._detect_file_type(file_path)].append(file_path)

        return all_files

    def format_file_in_memory(
        self, file_path: Path, file_contents: bytes
    ) -> FormattedFileContents:
        """Uses ``buildifier`` to check the formatting of the requested file.

        The file at ``file_path`` is NOT modified by this check.

        Returns:
            A populated
            :py:class:`pw_presubmit.format.core.FormattedFileContents` that
            contains either the result of formatting the file, or an error
            message.
        """
        proc = self.run_tool(
            'buildifier',
            [
                f'--type={self._detect_file_type(file_path)}',
                '--lint=fix',
                '--warnings=' + ','.join(self.warnings_to_fix),
            ],
            input=file_contents,
        )
        ok = proc.returncode == 0
        return FormattedFileContents(
            ok=ok,
            formatted_file_contents=proc.stdout,
            error_message=None if ok else proc.stderr.decode(),
        )

    def format_file(self, file_path: Path) -> FormatFixStatus:
        """Formats the provided file in-place using ``buildifier``.

        Returns:
            A FormatFixStatus that contains relevant errors/warnings.
        """
        proc = self.run_tool(
            'buildifier',
            [
                f'--type={self._detect_file_type(file_path)}',
                '--lint=fix',
                '--warnings=' + ','.join(self.warnings_to_fix),
                file_path,
            ],
        )
        ok = proc.returncode == 0
        return FormatFixStatus(
            ok=ok,
            error_message=None if ok else proc.stderr.decode(),
        )

    def format_files(
        self, paths: Iterable[Path], keep_warnings: bool = True
    ) -> Iterator[Tuple[Path, FormatFixStatus]]:
        """Uses ``buildifier`` to format the specified files in-place.

        Returns:
            An iterator of ``Path`` and
            :py:class:`pw_presubmit.format.core.FormatFixStatus` pairs for each
            file that was not successfully formatted. If ``keep_warnings`` is
            ``True``, any successful format operations with warnings will also
            be returned.
        """
        sorted_files = self._files_by_type(paths)
        for file_type, type_specific_paths in sorted_files.items():
            if not type_specific_paths:
                continue

            proc = self.run_tool(
                'buildifier',
                [
                    f'--type={file_type}',
                    '--lint=fix',
                    '--warnings=' + ','.join(self.warnings_to_fix),
                    *type_specific_paths,
                ],
            )

            # If there's an error, fall back to per-file formatting to figure
            # out which file has problems.
            if proc.returncode != 0:
                yield from super().format_files(
                    type_specific_paths, keep_warnings
                )
