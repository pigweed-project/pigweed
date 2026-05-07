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
"""Formatting step for pw_presubmit."""

from typing import Iterable, Iterator, Pattern

from pw_build.runfiles_manager import RunfilesManager
from pw_presubmit.format.core import FileFormatter
from pw_presubmit.format.formatters import pigweed_formatters
from pw_presubmit.format.private import cli_support
from pw_presubmit.private.step import Context, Step


class CodeFormatting(Step):
    """Presubmit step that checks and fixes source file formatting."""

    def __init__(
        self,
        formatter: FileFormatter,
        exclude: Iterable[Pattern[str] | str] = (),
    ) -> None:
        super().__init__(exclude=exclude, file_filter=formatter.file_patterns)
        lang = formatter.mnemonic.lower().replace(' ', '_').replace('+', 'p')
        self._name = 'format_' + lang
        self.__doc__ = (
            f'Checks and fixes source file formatting for {formatter.mnemonic}.'
        )
        self._formatter = formatter

    def run(self, ctx: Context) -> None:
        """Run the formatting check."""
        findings = cli_support.check({self._formatter: ctx.paths})

        for diffs in findings.values():
            for diff in diffs:
                if not diff.ok:
                    msg = f'Failed to format: {diff.error_message}'
                    ctx.fail(msg, path=diff.file_path)
                elif diff.diff:
                    ctx.fail(
                        f'Incorrect formatting\n{diff.diff}',
                        path=diff.file_path,
                    )

    def fix(self, ctx: Context) -> None:
        """Apply formatting fixes in place."""
        findings = cli_support.check({self._formatter: ctx.paths})
        if not findings:
            return

        errors = cli_support.fix(findings)
        if errors:
            for path, status in errors.items():
                ctx.fail(f'Failed to format: {status.error_message}', path=path)


def format_steps(
    exclude: Iterable[Pattern[str] | str] = (),
    formatters: Iterable[FileFormatter] | None = None,
) -> Iterator[CodeFormatting]:
    """Yields CodeFormatting steps for each formatter.

    Args:
      exclude: Optional paths to exempt from formattings checks; concatenated
        with other filters.
      formatters: Formatters to use; if None, all available formatters are used.

    Yields:
      CodeFormatting steps for each formatter, filtered for its supported
      extensions.
    """
    if formatters is None:
        formatters = pigweed_formatters(RunfilesManager())

    for formatter in formatters:
        yield CodeFormatting(formatter, exclude=exclude)
